#pragma once

// Deterministic fixed-tick player controller (a vertical capsule) on top of
// the collision primitives in `render/collide.h`.
//
// Frame: the engine world frame of `.sbw` (X right, Y forward, Z up), the same
// one docs/CAMERA.md and docs/PLAYER.md use.  `PlayerState::pos` is the *feet*
// point (the bottom centre of the capsule); the camera sits at
// `pos.z + eye_height`, which is the proven first-person formula
// `camera_z = spawn_z + player[0x2F50]` (docs/PLAYER.md §4).
//
// Determinism: this file and controller.cpp use `double` only, never read the
// clock or any platform state, and keep a fixed order of operations (sphere 0,
// 1, 2; triangles in build order; sequential slides).  One call to `step()`
// advances exactly `ControllerConfig::dt` seconds, 1/91 s by default (the HUD
// playback tick, docs/HUD_PLAYER.md).  With `-ffp-contract=off` (set by
// engine/CMakeLists.txt) two runs of the same inputs are bit-identical.
//
// The capsule is approximated by three spheres (feet, middle, head) swept with
// `CollisionWorld::sphere_cast`.  That is a union-of-spheres capsule: it is
// exact on floors, walls and ceilings away from the rounded caps, and the
// rounded bottom is what produces the skin gap used for ground detection.
//
// See docs/PLAYER_CONTROLLER.md for the formulas, the proven/hypothesis split
// of the constants and the goldens.

#include <string>

#include "render/collide.h" // Vec3d, CollisionWorld

namespace gm {

/// Kinematic state of one player.  All distances are world units, velocities
/// are world units per second.
struct PlayerState {
    Vec3d pos{};            ///< feet point: bottom centre of the capsule
    Vec3d vel{};            ///< world units / second
    double yaw = 0.0;       ///< camera yaw in radians (docs/CAMERA.md §6)
    double height = 10.0;   ///< current capsule height (crouch aware)
    double eye_height = 10.0; ///< current eye height above `pos`
    bool on_ground = false; ///< contact with a floor from the previous tick
    bool crouching = false; ///< `height < ControllerConfig::height`
};

/// Player intent for one tick.  `forward`/`strafe` are in the player frame and
/// are clamped to [-1, 1]; `+forward` is along `yaw`, `+strafe` is to the right
/// (the camera `right` axis of docs/CAMERA.md).
struct Input {
    double forward = 0.0;
    double strafe = 0.0;
    bool jump = false;
    bool crouch = false;
};

/// Fixed tick rate of the original game (91 ticks = 1 s, `0x44db80`).
constexpr double kPlayerTickHz = 91.0;

/// The horizontal velocity of the original integrator, in world units per tick.
/// The original keeps `player+0x81bd90/0x81bd98` in that unit; the loop is
/// `0x444476`..`0x4444d5` (docs/PLAYER_GHIDRA.md §4).
struct TickVelocity {
    double vx = 0.0;
    double vy = 0.0;
};

/// Chassis speed model of the original (`0x47a980`, `0x4524d0`, `0x443690`).
///
/// `max_speed` is the `op4` field of `DEFINE_PLAYER_TYPES` (`0x81bd70`) in
/// world units *per tick*; `weight_penalty` is `0x81bd80`; `health_factor` is
/// `0x81bf88`; `retention` is the per-tick retention `k` of `0x81bfb8`.
/// The values of the shipped LIGHT chassis are in docs/FORMATS.md §12.3.
struct ChassisMotion {
    double max_speed = 1.70;       ///< op4 (`0x81bd70`), world units per tick
    double weight_penalty = 0.003; ///< `0x81bd80`, per unit of excess weight
    double weight = 0.0;           ///< `0x81c374`
    double weight_limit = 0.0;     ///< `0x81c378`
    double health_factor = 1.0;    ///< `0x81bf88`
    double retention = 0.9;        ///< k (`0x81bfb8`); LIGHT 0.9 (save data)
};

/// `0x4524d0`: the speed after the weight penalty, clamped to 0.05.
/// `1.7 - 0.003 * (615 - 500) = 1.355` for the shipped LIGHT save.
double weight_limited_speed(const ChassisMotion& chassis);

/// `0x443690`: the nominal speed the integrator uses, world units per tick
/// (`weight_limited_speed * health_factor`).
double nominal_speed(const ChassisMotion& chassis);

/// One tick of the original integrator (`0x444478`): `v <- v*k + step`.
/// `step` is the per-tick input delta, already divided by the batch length
/// (the original divides it at `0x44444c`/`0x44445e`).
TickVelocity integrate_velocity(const TickVelocity& velocity, double retention,
                                 const TickVelocity& step);

/// The result of a batch of ticks: the final velocity and the displacement
/// the original accumulates (`local_38`/`local_40`, `0x4444a3`/`0x4444bb`).
struct IntegrationResult {
    TickVelocity velocity{};
    double displacement_x = 0.0;
    double displacement_y = 0.0;
};

/// A batch of `ticks` ticks of the original integrator.  `ticks <= 0` returns
/// the input velocity and a zero displacement.
IntegrationResult integrate_batch(const TickVelocity& velocity, double retention,
                                   const TickVelocity& step, int ticks);

/// The per-tick input delta of the original (`0x443690`).
///
/// The original keeps one counter per action in a 31 dword block
/// (`0x5a49f0` for player one, `0x5a4958` for player two); the counter of an
/// action is incremented once per tick while the action is held
/// (`FUN_0044bdc0`) and the block is copied into the player struct every
/// frame (`FUN_0044d4c0`).  The movement uses two *differences* of those
/// counters:
///
///   `forward_ticks` = `player+0x81c3ac` - `player+0x81c3b0`
///   `strafe_ticks`  = `player+0x81c388` - `player+0x81c38c`
///
/// and the nominal speed, projected on the heading:
///
///   dx = (sin(h)*v)*strafe - (cos(h)*v)*forward
///   dy = (cos(h)*v)*strafe + (sin(h)*v)*forward
///
/// divided by the batch length.  The components are the original's local
/// frame (the matrix has a negative determinant, i.e. it mirrors the axis).
TickVelocity input_delta(double heading, double nominal, int forward_ticks,
                         int strafe_ticks, int ticks);

/// The vertical channel of the same loop: `vz <- 0.85*vz + 0.15*pitch`
/// (`0x4a2368`/`0x4a2360`, the look angle is clamped to `+-pi/12`).
double integrate_vertical(double vz, double pitch);

/// Tunables.  Defaults are a LIGHT-chassis-sized capsule; the provenance of
/// every number is in docs/PLAYER_CONTROLLER.md §4.
struct ControllerConfig {
    double radius = 3.0;          ///< LIGHT approximation; the original uses SOB object radius (obj+0x60)
    double height = 10.0;         ///< standing capsule height
    double eye_height = 10.0;     ///< eye above the feet (LIGHT CHASSIS = 10.0)
    double gravity = 700.0;       ///< synthetic; the original uses a 0x80f840-scaled suspension, not a scalar g
    double accel = 10.0;          ///< ground acceleration, 1/s
    double friction = 8.0;        ///< ground friction, 1/s
    double max_speed = 1.70 * 91.0; ///< 154.7: LIGHT op4 (units/tick) * 91 ticks/s.
                                   ///< This is the *nominal* of the chassis, not
                                   ///< a clamp: the original has no speed limit,
                                   ///< its steady state is `step/(1-k)`.
    double retention = 0.9;       ///< k of the chassis (`0x81bfb8`), the per-tick
                                   ///< share of the velocity the original keeps
    double step_height = 3.0;     ///< max step-up and step-down snap
    double dt = 1.0 / 91.0;       ///< fixed tick length (seconds)
    double jump_speed = 200.0;    ///< take-off vertical speed
    double air_control = 0.35;    ///< fraction of `accel` while airborne
    double crouch_scale = 0.7;    ///< crouched height / standing height
    double max_dt = 0.25;         ///< larger `dt` is a caller error
    double contact_skin = 1e-6;   ///< push-out gap kept by depenetration
    int max_slides = 4;           ///< collide-and-slide iterations per substep
    int max_substeps = 32;        ///< hard cap on substeps per tick
};

/// The chassis numbers of the C++ config, converted into the units of the
/// original (`max_speed` is stored in units/second there, `op4` is stored in
/// units per tick).
ChassisMotion chassis_from_config(const ControllerConfig& config);

/// Outcome of one tick.  On `ok == false` `error` explains why and `state` is
/// the unchanged input state; the controller never throws and never mutates
/// the world.
struct StepResult {
    bool ok = true;
    std::string error;
    PlayerState state{};
    bool hit_wall = false;
    bool hit_floor = false;
    bool hit_ceiling = false;
    bool stepped_up = false;
    bool stepped_down = false;
    bool jumped = false;
    int substeps = 0;
    int slides = 0;
};

/// Initial state for `feet` and `yaw`, with the capsule height and eye height
/// taken from `config`.
PlayerState make_state(const Vec3d& feet, double yaw, const ControllerConfig& config = {});

/// Camera point for rendering: `pos + (0, 0, eye_height)`.
Vec3d eye_position(const PlayerState& state);

/// Advance exactly `config.dt` seconds.  Inputs are validated first: NaN/inf
/// anywhere, a non-positive or too large `dt`, a non-positive radius/height or
/// a capsule thinner than its diameter are reported as a clean error.
StepResult step(const PlayerState& state, const Input& input, const CollisionWorld& world,
                const ControllerConfig& config = {});

} // namespace gm
