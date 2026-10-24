#pragma once

// Chassis physics (`M6.1.1`): mass, top speed, linear drag and the synthetic
// suspension as a standalone, deterministic module.  Nothing here is wired
// into `player/controller.*` yet; the controller keeps its own synthetic
// `gravity = 700` and its own position integrator.
//
// Proven / model split (details and numbers: docs/CHASSIS_PHYSICS.md):
//
//   * `max_speed` is the steady state of the original velocity filter
//     `v <- k*v + dv` (`0x444476`..`0x4444d5`, docs/PLAYER_GHIDRA.md §4):
//     `v_inf = dv/(1-k)` units per tick, i.e. `op4*91/(1-k)` units per second
//     with `dv = op4` (`0x81bd70`).  It is a plateau, not a clamp.
//   * `drag` is the continuous form of the proven per-tick retention
//     `k` (op20 of the record = `0x81bfb8`, the 4th of the eight doubles at
//     record `+0x48` copied to `0x81bfa0`..: 0.9 LIGHT/CHEVY, 0.92
//     MEDIUM/STEALTH, 0.96 HEAVY; verified on 616/616 records of 36 levels by
//     `test_chassis_physics`).  `drag = -91*ln(k)` (MODEL form; the original
//     multiplies the velocity by k once per tick, it has no quadratic drag).
//   * `mass` is op0 (`0x81c378`, the record's mass *limit*), used as the
//     inertia (MODEL: the original's integrator never reads mass, so it only
//     scales forces and cancels out of the trajectories).
//   * `accel`, `suspension_k`, `suspension_d`, `ride_height` are MODEL: the
//     record's suspension block (`+0x48`..`+0x84`) is copied verbatim and has
//     no decoded consumer (docs/CHASSIS.md §3/§7), and the only scalar gravity
//     in the game is the synthetic `ControllerConfig::gravity`.
//
// Integrator: fixed 91 Hz (`0x44db80`, `dt = 1/91`), semi-implicit Euler.  The
// damping of the planar channel is implicit (`v <- (v + a*dt)/(1 + drag*dt)`),
// so it is stable for any `dt`; the spring is explicit and `step()` refuses
// `dt*sqrt(suspension_k) >= 2`.  Position uses the new velocity.  Double only,
// fixed order of operations, no clock: two runs of the same inputs are
// bit-identical (`-ffp-contract=off` is set by engine/CMakeLists.txt).
//
// Frames: the engine world frame of `.sbw` (X right, Y forward, Z up), the
// same one `player/controller.h` uses.  `position` is the feet/contact point,
// `velocity` is in world units per second.
//
// Not modelled: collisions and ground contact (so `velocity.z` free-falls),
// tyre friction/slip, engine curve, body roll/pitch, per-wheel suspension, and
// the record's `op1` weight penalty (that is `ChassisMotion`/the controller).

#include <cstddef>
#include <optional>
#include <string>

#include "game/chassis_loadout.h" // ChassisSpec
#include "render/collide.h"       // Vec3d and the double vector helpers

namespace gm {

/// Fixed tick of the original (`0x44db80`: 91 ticks = 1 s).
inline constexpr double kChassisTickHz = 91.0;
/// Default step length, seconds.
inline constexpr double kChassisDt = 1.0 / kChassisTickHz;
/// Largest accepted `dt` (same cap as `ControllerConfig::max_dt`).
inline constexpr double kChassisMaxDt = 0.25;

/// The retention `k` used when the caller has no record at hand: the code's
/// fallback `0.92` (`0x47ab6a`), not LIGHT's saved `0.9`.
inline constexpr double kChassisRetentionFallback = 0.92;

/// Signature field of the retention `k` (`0x81bfb8`, op20).  It is one of the
/// eight doubles at record `+0x48` and is *not* part of `ChassisSpec`, which
/// carries only the nine fields the original applies.
inline constexpr std::size_t kChassisRetentionField = 20;
/// First and last signature fields of that eight-double block (op17..op24).
inline constexpr std::size_t kChassisEightDoublesFirst = 17;
inline constexpr std::size_t kChassisEightDoublesCount = 8;

/// MODEL defaults of the suspension, shared by every chassis (no decoded
/// per-chassis value exists).
inline constexpr double kChassisSuspensionK = 90.0; ///< 1/s^2: omega = 9.4868 rad/s
inline constexpr double kChassisSuspensionD = 8.0;  ///< 1/s: damping ratio 0.4216
inline constexpr double kChassisRideHeight = 3.0;   ///< world units (LIGHT radius approx.)

/// Physical parameters of one chassis.  Provenance of every field is in the
/// header comment; MODEL marks a value the data does not determine.
struct ChassisParams {
    double mass = 500.0;         ///< op0 `0x81c378`; inertia (MODEL mapping)
    double max_speed = 1547.0;   ///< PROVEN plateau: `op4*91/(1-k)` units/s
    double drag = 9.58780692486219; ///< PROVEN k -> `-91*ln(k)` damping, 1/s (MODEL form)
    double accel = 14832.33731276181; ///< MODEL `drag*max_speed`, units/s^2
    double suspension_k = kChassisSuspensionK; ///< MODEL, 1/s^2 (per unit mass)
    double suspension_d = kChassisSuspensionD; ///< MODEL, 1/s (per unit mass)
    double ride_height = kChassisRideHeight;   ///< MODEL loaded equilibrium, world units
    double gravity = 700.0;      ///< synthetic `ControllerConfig::gravity`; the original
                                 ///< scales a suspension term by `0x80f840` (op 96) instead
};

/// One chassis' live state.  `suspension_offset` is the compression of the
/// (single, synthetic) spring measured from the loaded equilibrium: `0` at
/// `ride_height`, positive when compressed.
struct ChassisState {
    Vec3d position{};                ///< feet/contact point, world units
    Vec3d velocity{};                ///< world units per second
    double suspension_offset = 0.0;  ///< metres, 0 = ride_height
    double suspension_velocity = 0.0;///< d(offset)/dt, metres per second
};

/// Map a record to physical parameters.  `retention` is the record's `k`
/// (op20); pass `kChassisRetentionFallback` when it is not available.
/// Fails (with a text) for a retention outside `(0,1)`, a non-positive mass or
/// speed, or a non-finite input.
std::optional<ChassisParams> chassis_params(const ChassisSpec& spec, double retention,
                                            std::string* error = nullptr);

/// Deterministic chassis body: planar drive + linear drag + one synthetic
/// suspension oscillator, plus the ballistic vertical channel.
class ChassisPhysics {
public:
    /// Validate and install `params`.  On failure the previous parameters are
    /// kept and `error` names the offending field.  Also enables `step()`.
    bool configure(const ChassisParams& params, std::string* error = nullptr);

    bool configured() const { return configured_; }
    const ChassisParams& params() const { return params_; }

    /// Advance one step of `dt` seconds (call with `kChassisDt`).
    ///
    /// `input.x`/`input.y` are the thrust axes in world space, clamped to unit
    /// length; `input.z` is ignored (the drive is planar).  Refused (and the
    /// state left *bit-identical*) for a non-positive/too large/non-finite
    /// `dt`, a non-finite state or input, an unconfigured object, or a step
    /// too long for the explicit spring (`dt*sqrt(suspension_k) >= 2`).
    bool step(ChassisState& state, double dt, Vec3d input,
              std::string* error = nullptr) const;

    /// Planar speed `sqrt(vx^2 + vy^2)`, world units per second.  The vertical
    /// channel is ballistic and is not part of the chassis speed.
    double speed(const ChassisState& state) const;

    /// Spring-damper force along +Z (up) applied to a body of `params().mass`
    /// at `offset`/`offset_velocity`:
    /// `+mass * (suspension_k * offset + suspension_d * offset_velocity)`.
    /// Positive offset (compression) pushes up.
    double suspension_force(double offset, double offset_velocity) const;

    /// Body height above the contact plane at the current compression:
    /// `ride_height - suspension_offset`.
    double body_height(const ChassisState& state) const;

private:
    ChassisParams params_{};
    bool configured_ = false;
};

} // namespace gm
