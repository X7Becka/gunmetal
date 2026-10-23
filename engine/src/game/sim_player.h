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
// Projectile slice (`M6.5.1`, slice 2): with `set_projectile()` installed the
// same tick also drives the runtime's own projectile phase, so a fired round
// flies, hits the level geometry and blasts through the weapon-keyed bridge
// (`GameRuntime::apply_projectile_blasts`).  No new physics: the round is
// stepped by `Projectiles::tick_results` inside `GameRuntime::tick_once`, and
// the damage goes through `apply_weapon_blast`/`apply_blast_damage`.  Without
// `set_projectile()` the tick is exactly the slice-1 tick: no runtime tick, no
// blasts, every old behaviour unchanged.
//
// Damage slice (`M6.5.1`, slice 3): the player owns its gauges (`Vitals`,
// save hundredths) and installs them on the runtime with
// `GameRuntime::set_player_vitals` before the blasts, so the same weapon-keyed
// bridge (`apply_projectile_blasts_to_players`) damages the player too -- a
// point-blank round hurts its own shooter.  After every projectile tick the
// gauges are read back from `GameRuntime::player_vitals()`, the decrease is
// added to `damage_taken()`, and `armor <= 0` is death (the original
// `0x4536e0`/`0x453b50` rule).  `respawn()` restores the starting gauges and
// the starting feet/yaw; the death and respawn rules are in
// `docs/SIM_DAMAGE.md`.
//
// Regeneration slice (`M6.1.3`): the optional `set_regen()` rule runs one
// `apply_regen()` step after the gauges are read back from the runtime, so
// shields/armor can recover in the tick.  It is **off by default** (no rule =
// every old test and plan keeps the damage-only behaviour), `damage_taken()`
// is never reduced by it, death stays `armor <= 0`, and the rule/tick model is
// the pure `game/regen.h` one (the original's two `DEFINE_REGENERATION_MARKER`
// doubles have no reader, see `docs/REGEN.md` §3).
//
// Platform carry slice (`M6.4.1`, slice 5, docs/PLATFORM_CARRY.md): the
// optional `set_platform_carry(true)` rule makes a moving panel really carry a
// player who stands on it.  It is **off by default**, so every old plan and
// test keeps the slice-1 tick bit for bit.
//
// The rule is one extra step per tick and it adds no physics of its own:
//
//   1. before the controller step, if the previous tick remembered a panel
//      (`platform_`), look the same triangle up in
//      `CollisionWorld::dynamic_triangles()` (the dynamic set `set_dynamic`
//      publishes, docs/DOOR_COLLISION.md) and take `delta = centroid_now -
//      centroid_then`.  A triangle that is gone, swapped or turned (a swing
//      leaf) is forgotten: only a pure translation is carried.  The player is
//      moved by `delta` **before** `gm::step`, so the controller starts the
//      tick on the panel's current pose and its own ground contact keeps the
//      capsule resting there.  If the moved capsule overlaps *static* geometry
//      (a cabin driven into a wall/ceiling) the transfer is cancelled and the
//      player keeps the old position -- the documented rule of the original is
//      unknown, this is the safe one;
//   2. after the step, the ground probe of the controller (a downward sphere
//      sweep, `player/controller.cpp::ground_contact`) is repeated here with
//      the hit kept: `SphereHit::dynamic` names the panel, its triangle gives
//      the identity `(sector, object, source_poly)` and the pose `centroid`
//      for the next tick.  No dynamic hit under the feet -> no panel, the next
//      tick carries nothing.
//
// Limits (docs/PLATFORM_CARRY.md §4): translation only (no rotating cabins, no
// swing leaves), a stand-on (floor-like) contact only (a leaf pushing the
// capsule sideways is not modelled), one panel at a time, no shaking or
// multiple stacked panels.
//
// Scope: one player, feet point from the caller, no death animation/corpse/
// sound, no keyboard/mouse input, no rendering.  See `docs/SIM_PLAYER.md`
// (§1) for the slice-1 walk-through, `docs/SIM_PROJECTILE.md` for the
// projectile API, `docs/SIM_DAMAGE.md` for damage/death/respawn,
// `docs/REGEN.md` for the regeneration rule and `docs/PLATFORM_CARRY.md` for
// the moving-panel rule, their numbers and their limits.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/chassis_loadout.h" // ChassisSpec
#include "game/regen.h"           // RegenRule, RegenState
#include "game/runtime.h"         // GameRuntime, ProjectileBlastBatch
#include "game/trigger.h"         // PlayerWeaponInput, PlayerWeaponTick
#include "game/weapons.h"         // Vitals
#include "player/controller.h"    // PlayerState, ControllerConfig, StepResult
#include "render/collide.h"       // Vec3d
#include "world/things.h"         // Thing

namespace gm {

/// Starting gauges of a campaign LIGHT chassis, in save hundredths
/// (`docs/DAMAGE.md` §7: `shield_max = 8000`, `armor_max = 10000`, plates
/// `8000/8000/8000/8000`; `docs/PLAYER_FIELDS.md` §11: `saved0` has armor
/// `10000/10000` and shields `8000` x4).  `mult_plate`/`mult_hull` are the
/// documented defaults `1.0` (`0x452cd0`, docs/DAMAGE.md §2.3).
inline constexpr Vitals kLightPlayerVitals{
    10000, 10000, 8000, {8000, 8000, 8000, 8000}, 1.0, 1.0, 0, 0, 0};

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
/// (`ammo()` reads it, and `clear_projectile()` removes the installed template
/// through it), so the runtime must outlive the `SimPlayer` or the pointer must
/// not be used after the runtime is gone.
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
    /// the controller rejects the step or the runtime rejects the eye point /
    /// the gauges; `error` then explains why and `state()` is unchanged by the
    /// failed tick.  A dead player's tick returns `true` without doing
    /// anything (`docs/SIM_DAMAGE.md` §4).
    bool tick(GameRuntime& runtime, const DriveSegment& segment, int tick_index,
              std::string* error = nullptr);

    /// Install the projectile template of the player FIRE path and switch the
    /// player into **projectile mode**.
    ///
    /// A thin wrapper around `GameRuntime::set_player_projectile(projectile,
    /// owner_player, 0.0)`: the template is the same `Thing` shape the FIRE
    /// sub-script hands to `0x46ea40` (movement type 6; `id < 0` is replaced by
    /// the runtime id pool per round).  The template is stored here and
    /// installed on the runtime in the next `tick()` (before the first one it
    /// is simply kept), so `set_projectile()` may be called right after
    /// `init()` -- the documented use -- or between ticks.
    ///
    /// Projectile mode changes the per-tick contract in exactly one way: the
    /// tick also advances the runtime (`GameRuntime::tick(1)`), because the
    /// round spawn and the projectile step must happen in the same tick.  The
    /// caller must therefore not drive `runtime.tick()` itself while the
    /// player is in projectile mode.  Without this call the tick is the slice-1
    /// tick (no runtime tick), so old plans are unaffected.
    ///
    /// The template is **not validated** (movement type, radius, hits and
    /// events are passed through, like `set_player_projectile`): a template
    /// that is not movement type 6 is still accepted by the runtime and simply
    /// flies as an ordinary round.  See `docs/SIM_PROJECTILE.md` §5.
    void set_projectile(const Thing& projectile, int owner_player = 0);
    /// Drop the template and leave projectile mode (counters stay as they
    /// are).  When this player installed the template on a runtime, the
    /// runtime's `clear_player_projectile()` is called too, so a following
    /// `fire` really spawns nothing.
    void clear_projectile();
    /// True once `set_projectile()` installed a template.
    bool has_projectile() const { return projectile_.has_value(); }

    /// Set the **starting** gauges (save hundredths, `Vitals` of
    /// `game/weapons.h`).  They become the gauges of the current life and are
    /// installed on the runtime with `GameRuntime::set_player_vitals()` on the
    /// next `tick()` (the runtime keeps one gauge per `set_player_eyes()` point,
    /// and that point only exists after the first tick).
    ///
    /// Validation follows the save invariants of `docs/PLAYER_FIELDS.md` §11:
    /// `armor_max > 0`, `0 <= armor <= armor_max`, `shield_max >= 0`,
    /// `0 <= shields[i] <= shield_max`, finite non-negative
    /// `mult_plate`/`mult_hull`.  A violation returns `false` with a one-line
    /// reason in `error` and leaves the previous gauges untouched.  May be
    /// called before or after `init()`; it never touches the runtime directly,
    /// so mixing gauges between runtimes is impossible.
    bool set_vitals(const Vitals& vitals, std::string* error = nullptr);

    /// Install (or replace) the optional regeneration rule of this player
    /// (`game/regen.h`, `docs/REGEN.md`).  Regeneration is **off** until this
    /// is called, so a plan that never touches it keeps the slice-3 behaviour
    /// exactly.
    ///
    /// The rule is configuration: it survives `init()` and `respawn()`, while
    /// the per-life clock (`RegenState`) is reset by both.  Validation is
    /// `regen_rule_error()`: a negative/oversized `delay_ticks`, a
    /// negative/non-finite/oversized `rate_per_tick` or a rule with neither
    /// pool enabled returns `false` with a one-line text and leaves the
    /// previous rule untouched.
    bool set_regen(const RegenRule& rule, std::string* error = nullptr);
    /// Drop the rule (back to the default: no regeneration) and reset the
    /// clock.
    void clear_regen();
    /// True once `set_regen()` installed a rule.
    bool has_regen() const { return regen_rule_.has_value(); }
    /// The installed rule, or nullptr when regeneration is off.
    const RegenRule* regen_rule() const {
        return regen_rule_ ? &*regen_rule_ : nullptr;
    }
    /// Clock/carry of the installed rule (zero-initialised before the first
    /// tick and after `set_vitals()`/`respawn()`).
    const RegenState& regen_state() const { return regen_state_; }

    /// Restore the state of `init()`: feet, yaw and the **starting** gauges
    /// (`set_vitals()` or `kLightPlayerVitals`), `alive()` from
    /// `armor > 0` (true for the default gauges), one more `deaths()`.  The
    /// gauges are re-installed on the runtime with the next `tick()`; the
    /// projectile template and the ammo of the runtime stay as they are,
    /// `tick_index()` keeps counting (no clock reset, so a plan stays
    /// deterministic).  Returns `false` with a text before `init()`.
    bool respawn(std::string* error = nullptr);

    /// True while `armor > 0`; false from the tick that killed the player until
    /// `respawn()`.  A dead player's `tick()` is a successful no-op (no motion,
    /// no shot, no runtime tick), so the state, the tick counter and the gauges
    /// stop changing -- the documented behaviour of this slice.
    bool alive() const { return alive_; }
    /// Number of ticks (`tick_index()` of the killing tick, 1-based) of the
    /// last death; `-1` before the first death.  Kept after `respawn()` as the
    /// previous death's tick.
    int death_tick() const { return death_tick_; }
    /// Successful `respawn()` calls over the life of this player.
    int deaths() const { return deaths_; }
    /// Total gauge loss over the life of this player, in save hundredths:
    /// every decrease of `armor` plus every decrease of the four shields,
    /// summed over the projectile ticks.  Window refreshes and respawns of the
    /// gauges do not subtract from it.
    int damage_taken() const { return damage_taken_; }
    /// Current gauges (a copy; the runtime-owned span is not exposed).  Before
    /// the first tick this is the starting set, after each projectile tick it is
    /// `GameRuntime::player_vitals()[0]` -- the same values the original
    /// `0x453b50` writes into `+0x3244..+0x3260`.
    const Vitals& vitals() const { return vitals_; }

    // --- moving panel carry (slice 5, M6.4.1, docs/PLATFORM_CARRY.md) --------

    /// Turn the carry rule on/off.  **Off by default**: a player that never
    /// calls this behaves exactly like slice 4 (the lift cabin passes through
    /// the standing player, `dz = -39.3` in `test_lift_collision`), so old
    /// plans and goldens are untouched.  The flag is configuration like
    /// `set_regen()`: it survives `init()`/`respawn()`; only the remembered
    /// panel is per-life.
    void set_platform_carry(bool enabled);
    /// True while the carry rule is on.
    bool platform_carry() const { return platform_carry_; }
    /// True when the *last successful tick* found a dynamic floor triangle
    /// under the feet and remembered it.  Always false while the rule is off
    /// (no probe is made then).  A jump, a step off the panel or a panel that
    /// disappeared clears it on the tick that notices.
    bool on_platform() const { return platform_.has_value(); }
    /// Displacement the player was moved by on the last tick (`0,0,0` when
    /// nothing was carried, when the rule is off or when the transfer was
    /// cancelled by a static overlap).  This is the panel's own `delta` of that
    /// tick, so `platform_delta()` summed over the run equals the panel travel.
    Vec3d platform_delta() const { return platform_delta_; }

    /// Current player state (feet point, velocity, yaw, capsule height).
    const PlayerState& state() const { return state_; }
    /// Current yaw in radians; equals `state().yaw`.
    double yaw() const { return state_.yaw; }
    /// Accepted shots over the life of this player (one per `fired()` action).
    int shots_fired() const { return shots_fired_; }
    /// Shots that really produced a round: every weapon tick whose
    /// `PlayerWeaponTick::spawned >= 0` (hitscan and rejected shots do not
    /// count).  `0` when neither the player nor the runtime carries a
    /// projectile template.
    int projectiles_spawned() const { return projectiles_spawned_; }
    /// Accumulated `ProjectileBlastBatch::damage.hits.size()` over every blast
    /// the player's rounds applied.  One record per blast-target pair, so a
    /// two-blast rocket inside both radii counts several hits for one target;
    /// player entries are counted too, and since slice 3 they also apply real
    /// damage (`vitals()`/`damage_taken()`, `docs/SIM_DAMAGE.md`).
    int blast_hits() const { return blast_hits_; }
    /// Number of **distinct** thing slots that took blast damage (a damageable
    /// thing -- `ThingVitals::max_hp > 0` -- inside a blast radius).  This is
    /// what a test checks against `Thing::health`/`Thing::alive`.
    int things_damaged() const { return things_damaged_; }
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
    /// Install the stored template on `runtime` (once per runtime, before the
    /// weapon seam so the first shot already spawns a round).
    void install_projectile(GameRuntime& runtime);
    /// Install the current gauges on `runtime` when the runtime has none of
    /// ours yet (after `init()`/`set_vitals()`/`respawn()` or a runtime switch).
    bool install_vitals(GameRuntime& runtime, std::string* error);
    /// Apply the blasts of the current `last_projectile_results()` generation
    /// once, count them and write the result back into `Things`.
    void apply_blasts(GameRuntime& runtime);
    /// Move `state` by the delta of the remembered panel (before the
    /// controller step).  A missing/swapped/turned panel forgets it; a move
    /// that would overlap static geometry is cancelled.  Updates `platform_`
    /// to the pose just observed and `platform_delta_`.
    void carry_platform(const CollisionWorld& world, PlayerState& state);
    /// Ground probe after the step: remember the dynamic triangle under the
    /// feet, or forget the previous panel.
    void detect_platform(const CollisionWorld& world);

    /// The panel the player stood on at the end of the last tick.
    struct PlatformRef {
        std::uint32_t index = 0;       ///< into `dynamic_triangles()`
        std::uint32_t sector = 0;
        std::uint32_t object = 0;
        std::uint32_t source_poly = 0; ///< polygon inside its object
        Vec3d centroid{};              ///< pose of the triangle last tick
        Vec3d normal{};                ///< unit face normal last tick
        Vec3d data_normal{};           ///< authored normal last tick
    };

    ControllerConfig config_{};
    PlayerState state_{};
    StepResult last_step_{};
    PlayerWeaponTick last_weapon_{};
    const GameRuntime* runtime_ = nullptr;
    int ticks_done_ = 0;
    int shots_fired_ = 0;
    bool initialized_ = false;

    // Starting pose (slice 3): `respawn()` returns here.
    Vec3d start_feet_{};
    double start_yaw_ = 0.0;

    // Gauges (slice 3).  `start_vitals_` is the configuration (set_vitals or
    // the LIGHT default), `vitals_` the current life.  `vitals_runtime_` is the
    // runtime whose `player_vitals_` currently holds `installed_vitals_`; the
    // dirty flag forces a re-install after `init()`/`set_vitals()`/`respawn()`.
    Vitals start_vitals_ = kLightPlayerVitals;
    Vitals vitals_ = kLightPlayerVitals;
    const GameRuntime* vitals_runtime_ = nullptr;
    bool vitals_dirty_ = false;
    bool alive_ = true;
    int death_tick_ = -1;
    int deaths_ = 0;
    int damage_taken_ = 0;

    // Regeneration (slice 4, `M6.1.3`).  `regen_rule_` is configuration and
    // survives re-init/respawn; `regen_state_` is the current life's clock and
    // is reset by both.
    std::optional<RegenRule> regen_rule_{};
    RegenState regen_state_{};

    // Moving panel carry (slice 5, `M6.4.1`, docs/PLATFORM_CARRY.md).  The flag
    // is configuration (survives init/respawn); the remembered panel and the
    // last delta belong to the current life.
    bool platform_carry_ = false;
    std::optional<PlatformRef> platform_{};
    Vec3d platform_delta_{};

    // Projectile mode (slice 2).  `projectile_runtime_` is nulled by
    // `set_projectile()`/`clear_projectile()`/`init()`, so the template reaches
    // whichever runtime the player ticks with next.
    std::optional<Thing> projectile_{};
    int projectile_owner_ = 0;
    GameRuntime* projectile_runtime_ = nullptr; ///< last runtime we installed on
    int projectiles_spawned_ = 0;
    int blast_hits_ = 0;
    int things_damaged_ = 0;
    /// Index-aligned flags of the thing slots already counted in
    /// `things_damaged_`; sized like `runtime.things().things()`.
    std::vector<std::uint8_t> damaged_things_{};
};

} // namespace gm
