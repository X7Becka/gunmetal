#pragma once

// One player driven through the fixed-tick slice (`M6.5.1`, slice 1).
//
// `SimPlayer` is the smallest thing that turns a scripted *driving plan* into
// player motion and shots on top of `GameRuntime`.  It owns no physics of its
// own: every tick goes through the already proven controller (`player/controller.h`)
// and through the already proven weapon seam (`GameRuntime::update_player_weapon`,
// the `0x4453d0` tick of `game/trigger.h`).  Nothing in the runtime is modified
// and no new collision, movement or weapon rule is introduced here.
//
// One tick is exactly:
//
//   1. `yaw += segment.turn * config.dt` (radians per second, so `turn` is an
//      angular velocity, not an angle);
//   2. `gm::step(old_state, input, runtime.collision(), config, config.dt)` with
//      `input` built from `forward`/`strafe`/`jump`/`crouch` (`player/controller.h`);
//      a rejected step (`StepResult::ok == false`) returns `false` and leaves
//      the state untouched;
//   3. `runtime.set_player_eyes({eye_position(state)})`, so the AI and the
//      player-vitals list of the runtime follow the player
//      (`GameRuntime::set_player_eyes`, `runtime.h`);
//   4. with `segment.fire`: `runtime.update_player_weapon(input, eye)` with
//      `aim_direction = (cos(yaw), sin(yaw), 0)` and `trigger_edge` on the
//      first tick of the series (`tick_index == 0`), `trigger_held` afterwards
//      (`PlayerWeaponInput`, `game/trigger.h`; the runtime call is `0x4453d0`).
//
// The module is deterministic in the same sense as the controller: `double`
// only, no clock, no RNG, no platform state, fixed order of operations (the
// engine is built with `-ffp-contract=off`).  Two runs of the same plan and the
// same starting state are bit-identical.
//
// Scope: one player, feet point from the caller, no damage to the player, no
// death, no respawn, no keyboard/mouse input, no rendering.  See
// `docs/SIM_PLAYER.md` for the API walk-through, the numbers of the test and
// the list of what is still missing for the CLI (`--sim-drive`).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/chassis_loadout.h" // ChassisSpec
#include "game/runtime.h"         // GameRuntime
#include "game/trigger.h"         // PlayerWeaponInput, PlayerWeaponTick
#include "player/controller.h"    // PlayerState, ControllerConfig, StepResult
#include "render/collide.h"       // Vec3d

namespace gm {

/// Largest accepted `ticks` of one `DriveSegment` (one segment is at most
/// ~3 hours of game time at 91 Hz; anything larger is a typo, not a plan).
inline constexpr int kDriveMaxTicks = 1000000;

/// One straight stretch of a driving plan: the same input for `ticks` ticks.
///
/// `forward`/`strafe` are the controller's player-frame axes in `[-1, 1]`
/// (clamped by `gm::step`), `turn` is yaw angular velocity in radians per
/// second (`+` = towards +Y from +X, the `docs/CAMERA.md` §3 yaw), `fire`
/// holds the trigger, `jump`/`crouch` press the corresponding action.
struct DriveSegment {
    int ticks = 0;          ///< 1..kDriveMaxTicks
    double forward = 0.0;   ///< forward axis, -1..1
    double strafe = 0.0;    ///< right axis, -1..1
    double turn = 0.0;      ///< yaw velocity, radians / second
    bool fire = false;      ///< hold the trigger for the whole segment
    bool jump = false;      ///< hold jump
    bool crouch = false;    ///< hold crouch
};

/// Parse a driving script into segments.
///
/// Format: segments separated by `;`, each `<key>=<value>:<ticks>` for the
/// numeric keys (`forward`, `strafe`, `turn`) and `<key>:<ticks>`,
/// `<key>=<ticks>` or `<key>=<value>:<ticks>` for the boolean ones (`fire`,
/// `jump`, `crouch`; a missing value means `true`, a finite non-zero value
/// means `true`).  The compact `jump=5` spelling (ticks after `=`, no value)
/// is the one the documented example plan uses.
///
///     forward=1:900;turn=0.5:300;fire:200;jump=5
///
/// Spaces, tabs and line breaks around the fields are ignored.  `ticks` must
/// be an integer in `1..kDriveMaxTicks`.  An empty script, an empty segment,
/// an unknown key, a numeric key without a value or without ticks, a malformed
/// or non-finite number and an out-of-range `ticks` all return `std::nullopt`
/// and a one-line reason in `error`; the parser never throws.
std::optional<std::vector<DriveSegment>> parse_drive(std::string_view script,
                                                     std::string* error = nullptr);

/// One scripted player on top of one `GameRuntime`.
///
/// Build it with `init()`, drive it with one `tick()` per fixed tick.  The
/// class keeps a non-owning pointer to the runtime it was last ticked with
/// (only `ammo()` needs it), so the runtime must outlive the `SimPlayer` or the
/// pointer must not be used after the runtime is gone.
class SimPlayer {
public:
    /// Configure the controller from `chassis` and place the player at `feet`
    /// with `yaw` (radians).
    ///
    /// The `ControllerConfig` is built from the chassis only:
    ///   * `eye_height` = `chassis.eye_height` (`0x81bf90`/`player+0x2f50`);
    ///   * `max_speed` = `chassis.max_speed` (the `op4` field, world units per
    ///     tick) times `kPlayerTickHz` (91), the same conversion as
    ///     `chassis_from_config()`;
    ///   * `retention` = `chassis_motion(chassis).retention` (`+0x81bfb8`).
    /// The capsule radius/height and every other tunable keep the
    /// `ControllerConfig` defaults (LIGHT-sized capsule, `docs/PLAYER_CONTROLLER.md`).
    ///
    /// Returns `false` with a text for a non-finite `feet`/`yaw`, an eye height
    /// that is not finite and positive, or a non-finite/negative `max_speed`.
    bool init(const Vec3d& feet, double yaw, const ChassisSpec& chassis,
              std::string* error = nullptr);

    /// Advance exactly one fixed tick (1/91 s).
    ///
    /// `tick_index` is the index of this tick inside the segment (`0` on the
    /// first tick of a series, which is what turns `segment.fire` into a
    /// trigger *edge*; later ticks only hold it).  The caller runs
    /// `0..segment.ticks-1`.  Returns `false` when the player was not
    /// initialised, a segment value is not finite, `tick_index` is negative,
    /// the controller rejects the step or the runtime rejects the eye point;
    /// `error` then explains why and `state()` is unchanged by the failed tick.
    bool tick(GameRuntime& runtime, const DriveSegment& segment, int tick_index,
              std::string* error = nullptr);

    /// Current player state (feet point, velocity, yaw, capsule height).
    const PlayerState& state() const { return state_; }
    /// Current yaw in radians; equals `state().yaw`.
    double yaw() const { return state_.yaw; }
    /// Accepted shots over the life of this player (one per `fired()` action).
    int shots_fired() const { return shots_fired_; }
    /// Ammo of weapon slot `slot` in the runtime's loadout
    /// (`runtime.player_arsenal().ammo[slot]`); `0` when no runtime is known
    /// yet or `slot` is outside `0..kWeaponSlots-1`.
    std::uint32_t ammo(int slot) const;
    /// Number of successful ticks executed so far (0 after `init()`).
    int tick_index() const { return ticks_done_; }
    /// The controller configuration built by `init()`.
    const ControllerConfig& config() const { return config_; }
    /// Result of the last step (kept even when it failed).
    const StepResult& last_step() const { return last_step_; }
    /// Result of the last weapon tick; a default value (slot -1) until a
    /// segment with `fire` ran.
    const PlayerWeaponTick& last_weapon() const { return last_weapon_; }
    /// True once `init()` succeeded.
    bool initialized() const { return initialized_; }

private:
    ControllerConfig config_{};
    PlayerState state_{};
    StepResult last_step_{};
    PlayerWeaponTick last_weapon_{};
    const GameRuntime* runtime_ = nullptr;
    int ticks_done_ = 0;
    int shots_fired_ = 0;
    bool initialized_ = false;
};

} // namespace gm
