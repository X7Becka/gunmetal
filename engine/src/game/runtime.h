#pragma once

// Minimal headless fixed-tick vertical slice: one owner for the runtime
// subsystems of `game/`, `world/` and `res/`.
//
// `GameRuntime` owns
//
//   * `WorldState`    (doors, switches, lifts of one level, `world/doors.h`)
//   * `Things`        (thing runtime instances, `world/things.h`)
//   * `Ai`            (thing AI scaffold, `world/ai.h`)
//   * `Projectiles`   (movement-type 6/8 rounds, `game/projectile.h`)
//   * `EventRuntime`  (staging queue + switch table + text registry,
//                      `game/events.h`)
//   * `CollisionWorld` (the `.sbw` geometry the AI and the projectiles query)
//
// and steps them in one documented order.  The configuration is a value object
// (`GameRuntimeConfig`) copied into the runtime: there are no globals, no
// renderer, no input and no SDL.  `main.cpp` is deliberately not a caller yet;
// the slice exists so the layers can be pinned by `gm_game_state_tests`
// (docs/RUNTIME_CPP.md).
//
// One fixed tick is exactly:
//
//   1. things      `Things::tick(1, queue)`
//   2. AI          `Ai::sync_vitals()` + `Ai::tick(collision, 1/91 s)`
//   2b. AI shots   opt-in: drain `Ai::take_weapon_shots()` and spawn one
//                  `Projectiles` round per accepted shot (create events go to
//                  the same queue)
//   3. projectiles `Projectiles::tick_results(1, targets, collision, queue)`
//   4. doors/world `WorldState::tick(1)`
//   5. events      pending switch tail + `EventRuntime::pump(now)`
//
// `tick(n)` runs that body exactly `n` times; there is no second pass over any
// subsystem.  The events a phase posts are dispatched once, at the end of the
// same tick, and the queue is drained, so a following tick cannot replay them.
// Ordering is deterministic: every subsystem walks its own insertion-ordered
// vector, there is no clock read, no RNG and no map iteration.
//
// Blast damage is **not** a tick phase: the weapon that a round carries is
// chosen by the caller, so `GameRuntime` cannot pick it.
// `apply_projectile_blasts()` is the explicit bridge over
// `last_projectile_results()` (see the method and docs/RUNTIME_CPP.md §7).  It
// is idempotent per tick generation: every result record is blasted at most
// once until the next `tick()`.
//
// The runtime can also own the player gauges: `set_player_vitals()` stores one
// `Vitals` per `set_player_eyes()` point, and
// `apply_projectile_blasts_to_players()` blasts those without any caller-side
// vitals span.  Ownership is optional (`player_vitals()` empty): a blast then
// counts the player hit but applies no damage, the `apply_blast_damage()` rule.
//
// Lifetime: `Ai` keeps a pointer to the owned `Things`, and the projectile
// target spans point into the runtime's scratch arrays.  The class therefore
// deletes copy/move; build and use it in place.
//
// Switch input goes through `press_switch()` rather than `world().interact()`:
// `WorldState::tick()` drops the unposted part of its frame-local event log, so
// a press must be mirrored into the queue immediately.  `press_switch()` posts
// the type-2 record at once; when the queue is full the world keeps the pending
// tail (`WorldState::switch_post_blocked_`) and the events phase of the next
// tick retries it without duplicating the records already queued.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/events.h"
#include "game/projectile.h"
#include "render/collide.h"
#include "res/sbt.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "world/ai.h"
#include "world/doors.h"
#include "world/things.h"

namespace gm {

/// First runtime id handed out to a projectile spawned by the AI bridge
/// (`GameRuntime::set_ai_projectile_template`).  The ids are allocated from
/// this base upwards, so they never collide with the small `Things` instance
/// ids (0..n-1) of the same runtime.  The template's own `id` is not used.
inline constexpr int kAiProjectileRuntimeIdBase = 0x10000;

/// Value configuration of a `GameRuntime`.  Only plain values: `AiConfig`,
/// `ThingsOptions` and the build/selection flags below.  The defaults build AI
/// agents for every absolute-position thing and install the level's
/// `ADD_SWITCH` table.
struct GameRuntimeConfig {
    /// AI tunables (docs/AI_CPP.md).  The runtime additionally mirrors thing
    /// vitals into the agents once per tick (`Ai::sync_vitals`).
    AiConfig ai{};
    /// Thing build options used by `Things::load` (docs/THINGS_CPP.md).
    ThingsOptions things{};
    /// Build the AI agent list when things are built / re-attached.
    bool build_ai = true;
    /// Install `WorldState::switches()` into the event dispatcher table after
    /// a world build, so a type-2 record resolves to a GENERATE event.
    bool install_switches = true;
    /// Feed `Things` into `ProjectileTargets` so the proximity table of a
    /// projectile can see things.  Off leaves the thing spans empty.
    bool projectiles_use_things = true;
    /// Radius used for the player entries of `ProjectileTargets`
    /// (`ProjectileSpawn::proximity`).  The engine has no per-player collision
    /// radius here; 0 keeps the players at the eye points as points.
    double player_radius = 0.0;
    /// Opt-in bridge from the AI shot queue to `Projectiles`.  When `true` and
    /// a template is set (`set_ai_projectile_template()`), every tick drains
    /// `Ai::take_weapon_shots()` right after the AI phase and spawns one round
    /// per accepted shot; the spawn's create event goes to the runtime queue.
    /// `false` (default) spawns nothing and leaves the queue to the caller.
    /// The shots themselves only exist with `ai.use_weapon_shots` (which
    /// requires `ai.use_weapon_table`).
    bool ai_shots_to_projectiles = false;
};

/// One `apply_projectile_blasts()` call.  `damage` merges the applied blasts in
/// result-record order (then blast order), so `damage.hits.size()` is the total
/// number of target hits and `damage.chain_reaction` is the OR over them.
struct ProjectileBlastBatch {
    BlastDamageResult damage;
    /// Result records whose blast branch actually ran (a trigger, even when
    /// nothing was inside the radius), i.e. the number of blasts applied.
    std::size_t blasts = 0;
    /// Result records skipped because this generation was already blasted
    /// (`only_new = true` only); flying/destroyed/replayed records are not
    /// counted here.
    std::size_t already_applied = 0;
    /// Blast event records the queue accepted (0 for `event == 0` hits and for
    /// a full queue, which drops the record like every other runtime post).
    std::size_t posted_events = 0;
};

/// Headless owner of one level's runtime.  Build it with `build()` or the two
/// partial builders, drive it with `tick()`/`press_switch()`, read the result
/// through the const accessors.
class GameRuntime {
public:
    explicit GameRuntime(GameRuntimeConfig config = {});

    // The AI points into the owned `Things` and the target scratch arrays are
    // referenced by spans during a tick, so the object must not be relocated.
    GameRuntime(const GameRuntime&) = delete;
    GameRuntime& operator=(const GameRuntime&) = delete;
    GameRuntime(GameRuntime&&) = delete;
    GameRuntime& operator=(GameRuntime&&) = delete;

    // --- build --------------------------------------------------------------

    /// Rebuild everything from a parsed level, the VM state built for it and
    /// the level geometry: clears the runtime first, then builds the collision
    /// world, the world state (doors/switches/lifts), the thing runtime, the
    /// AI agents and the switch table.  On failure the runtime is left reset
    /// and `error` describes the first rejected part.
    bool build(const SbtLevel& level, const VmState& vm, const SbwLevel& geometry,
               std::string* error = nullptr);

    /// Build the collision world + `WorldState` + switch table only.  Things
    /// and AI are not touched except that the switch table is reinstalled.
    /// Returns false (with `error`) when the geometry or the world records are
    /// rejected.
    bool build_world(const SbtLevel& level, const SbwLevel& geometry,
                     std::string* error = nullptr);

    /// Build the thing runtime with `Things::load` and (re)attach the AI.
    /// Returns false (with `error`) for an invalid thing build; the previous
    /// thing runtime is kept then.
    bool build_things(const SbtLevel& level, const VmState& vm,
                      std::string* error = nullptr);

    /// Rebuild the agent list from the current thing runtime.  Call this after
    /// spawning/removing things with `spawn_thing()`/`remove_thing()`: the AI
    /// snapshots the thing list in `attach()`.  Returns the agent count.
    std::size_t attach_ai();

    // --- input seams --------------------------------------------------------

    /// Players' eye points for the AI (1..8, world units).  An empty span
    /// means "no players"; more than eight entries or a non-finite value is
    /// rejected and leaves the previous set untouched.  Never builds anything.
    ///
    /// The owned player vitals (`set_player_vitals()`) are index-aligned with
    /// this list, so a change of the player count drops them; re-setting the
    /// same count keeps the gauges.  `player_vitals()` therefore has either the
    /// same size as the eye list or is empty.
    bool set_player_eyes(std::span<const Vec3d> eyes);

    /// Replace the owned player vitals (0..8 entries, save units: integer
    /// hundredths).  The size must equal the `set_player_eyes()` list -- a
    /// mismatch (including a non-empty span before any eyes were set) is
    /// rejected and leaves both lists untouched.  These gauges never feed `Ai`
    /// (the scaffold has no player gauges); they are the targets of
    /// `apply_projectile_blasts_to_players()`.
    bool set_player_vitals(std::span<const Vitals> vitals);
    /// Owned player vitals, index-aligned with the `set_player_eyes()` points;
    /// empty when none were set.  The span is invalidated by the next
    /// `set_player_eyes()`/`set_player_vitals()`/`clear_players()`/`reset()`.
    std::span<const Vitals> player_vitals() const { return player_vitals_; }
    /// Mutable view of the same entries: contents may be edited in place, the
    /// size stays tied to the player list.  Same lifetime as the const view.
    std::span<Vitals> player_vitals() { return player_vitals_; }

    /// Drop the eye list and the owned player vitals.
    void clear_players();

    /// Press switch `switch_id` (index into `world().switches()`) and mirror it
    /// into the event queue at once.  A full queue leaves the record pending in
    /// the world; the next tick's events phase retries it.  Returns false for
    /// an unknown switch (the world is then untouched).
    bool press_switch(int switch_id, std::int16_t source = 0);

    // --- AI shot template ---------------------------------------------------

    /// Set the projectile template of the AI->projectile bridge: a `Thing`
    /// filled like a movement-type 6 projectile type (speed/gravity/damping/
    /// shape/radius/hits/lifetime/events, see `docs/PROJECTILE_CPP.md`).  The
    /// template is plain runtime state, not a global.
    ///
    /// Every spawned round gets a fresh runtime id from
    /// `kAiProjectileRuntimeIdBase` upwards instead of `Thing::id`, so several
    /// AI rounds can be alive at once (`Projectiles::spawn` rejects duplicate
    /// `thing_id`s and its reuse semantics stay untouched).  A template with no
    /// usable speed yields no spawns; the shot is dropped.
    void set_ai_projectile_template(Thing projectile);
    /// Remove the template; the bridge then leaves the AI queue alone.
    void clear_ai_projectile_template();
    /// The template, if one was set; `reset()` keeps it (it belongs to the
    /// configuration side of the runtime, like `config()`).
    const std::optional<Thing>& ai_projectile_template() const {
        return ai_projectile_template_;
    }

    // --- driving ------------------------------------------------------------

    /// Run exactly `ticks` fixed 91 Hz steps in the order documented above.
    /// `ticks == 0` does nothing.  Each step dispatches its own events, so
    /// `last_events()`/`last_projectile_results()` describe the last step only.
    void tick(std::uint64_t ticks = 1);

    // --- owned state --------------------------------------------------------

    WorldState& world() { return world_; }
    const WorldState& world() const { return world_; }
    Things& things() { return things_; }
    const Things& things() const { return things_; }
    Ai& ai() { return ai_; }
    const Ai& ai() const { return ai_; }
    Projectiles& projectiles() { return projectiles_; }
    const Projectiles& projectiles() const { return projectiles_; }
    EventRuntime& events() { return events_; }
    const EventRuntime& events() const { return events_; }
    const CollisionWorld& collision() const { return collision_; }
    const GameRuntimeConfig& config() const { return config_; }

    /// Fixed-tick counter: `0` before the first tick, incremented once per
    /// executed step.  The event dispatcher is called with the pre-increment
    /// value, so the first tick dispatches at `now == 0`.
    std::uint64_t clock() const { return clock_; }

    bool has_world() const { return world_built_; }
    bool has_things() const { return things_built_; }
    bool has_ai() const { return ai_attached_; }

    // --- last tick results --------------------------------------------------

    /// One record per stored projectile, in spawn order, exactly as returned by
    /// the single `Projectiles::tick_results` call of the last step.  The
    /// records are moved in, not rebuilt, and are empty event vectors for a
    /// step without projectiles.  Valid until the next `tick()`/`reset()`.
    std::span<const ProjectileTickRecord> last_projectile_results() const {
        return last_projectile_results_;
    }

    /// The `event_dispatch` result of the last step (`ok == false` only for a
    /// corrupt queue, which the runtime never builds itself).  Empty but valid
    /// after a step that queued nothing.  Valid until the next `tick()/reset()`.
    const EventDispatchResult& last_events() const { return last_events_; }

    // --- projectile -> blast bridge ----------------------------------------

    /// Apply the selected weapon's impact/expire blasts for the projectile
    /// results of the last step and post the blast events through the runtime
    /// queue.  This is the explicit weapon seam: the runtime does not choose a
    /// weapon and does not call this from `tick()`.
    ///
    /// Only trigger statuses are blasted: `bounced`, `expired` (unless the
    /// record is a replay, `already_spent`) and `proximity` only when
    /// `context.allow_proximity` (the same contract as
    /// `apply_projectile_blast()`); `flying`/`destroyed` never.  `expired` is
    /// forwarded as the blast branch, so `expired == true` selects
    /// `Weapon::expire_blasts` and a bounce selects `Weapon::impact_blasts`.
    ///
    /// Targets come from the runtime: the player positions are the
    /// `set_player_eyes()` points, so `player_vitals[i]` must describe player
    /// `i` of that list; the thing positions are `things().things()` in
    /// insertion order (a source-relative thing has no world point and is
    /// marked not alive), so `thing_vitals[i]` must describe the thing at that
    /// index -- the same order the projectile target table uses.  A vitals span
    /// may be shorter than the position span; the extra hits are still counted,
    /// only their damage is skipped (existing `apply_blast_damage()` rule).  The
    /// bridge writes only into those spans: call `store_thing_vitals()` yourself
    /// to copy the result into `Things`.
    ///
    /// Every applied hit with a non-zero `BlastHit::event` is posted as a type-3
    /// record at the blast centre (`extra = 0`); the per-target coordinates of
    /// the original hit path are not proven here.
    ///
    /// Idempotence: each record of the current `tick_results()` generation is
    /// blasted at most once.  A second call without a new tick applies nothing
    /// (`already_applied` counts the skipped records); the marks are cleared by
    /// the next `tick()`, `reset()`, `clear_events()` or `clear_projectiles()`,
    /// so a new bounce or expiry blasts again.  `only_new = false` re-applies
    /// the current generation (e.g. after the caller changed its vitals
    /// spans), but a replayed expiry (`already_spent`) still never blasts.
    ProjectileBlastBatch apply_projectile_blasts(
        const Weapon& weapon, const ProjectileBlastContext& context,
        std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
        bool only_new = true);

    /// The same bridge, with the runtime-owned player vitals: `player_vitals_`
    /// (see `set_player_vitals()`) is blasted instead of a caller span, so a
    /// runtime configured once with eyes + vitals needs no vitals plumbing.
    /// Thing vitals stay caller-owned (the runtime does not shadow `Things`);
    /// an empty span counts thing hits but applies no thing damage, the usual
    /// `apply_blast_damage()` rule.
    ///
    /// Both entry points share the per-generation marks, so mixing them cannot
    /// double-blast a record: a second call of either kind without a new
    /// `tick()` applies nothing (`already_applied` counts the skipped records).
    ProjectileBlastBatch apply_projectile_blasts_to_players(
        const Weapon& weapon, const ProjectileBlastContext& context,
        std::span<ThingVitals> thing_vitals = {}, bool only_new = true);

    // --- spawn --------------------------------------------------------------

    /// Append a thing, assign an id when `thing.id < 0` and post its create
    /// event.  Returns the id or -1 for a duplicate explicit id.  The AI is not
    /// re-attached (call `attach_ai()`).
    int spawn_thing(Thing thing);
    /// Mark a thing dead and post its destroy event.  The record stays in
    /// `things().things()` (stable indices).
    bool remove_thing(int id);

    /// Spawn a projectile and post its create event.  Returns the spawn index
    /// (`Projectiles::spawn`) or -1 for a duplicate `thing_id`.
    int spawn_projectile(ProjectileSpawn spawn);
    /// Mark a projectile dead and post its destroy event; the slot stays in
    /// `projectiles().list()` and reports `destroyed` from then on.
    bool remove_projectile(int thing_id);

    // --- clear --------------------------------------------------------------

    /// Drop everything and start from scratch with the same configuration:
    /// empty world/things/AI/projectiles/events/collision, no players (eyes and
    /// vitals), clock 0.  Does not touch `config()`.
    void reset();

    /// Drop the queued records, the world switch mirror, the AI event log and
    /// the last-tick results.  The text registry is a display log with its own
    /// frame deadlines and stays; use `reset()` to drop it too.  The clock is
    /// not reset.
    void clear_events();

    /// Drop the projectile fleet and the last projectile result.
    void clear_projectiles();

private:
    void tick_once();
    /// Drain the AI shot queue through `ai_shot_projectile_spawn()` with a
    /// fresh runtime id per shot.  Returns the number of spawned rounds.
    std::size_t bridge_ai_shots();
    /// Next unique projectile id of this runtime (see
    /// `kAiProjectileRuntimeIdBase`).
    int take_ai_projectile_id();
    ProjectileTargets projectile_targets();

    GameRuntimeConfig config_;
    WorldState world_;
    Things things_;
    Ai ai_;
    Projectiles projectiles_;
    EventRuntime events_;
    CollisionWorld collision_;
    std::optional<Thing> ai_projectile_template_;
    std::vector<Vec3d> player_eyes_;
    /// One gauge set per eye point (same size or empty); see
    /// `set_player_vitals()`.
    std::vector<Vitals> player_vitals_;
    std::int16_t switch_source_ = 0;
    std::uint64_t clock_ = 0;
    int next_ai_projectile_id_ = kAiProjectileRuntimeIdBase;
    bool world_built_ = false;
    bool things_built_ = false;
    bool ai_attached_ = false;

    // Flat derived arrays for `ProjectileTargets`; rebuilt from `Things` every
    // tick (capacity is kept, no deep copies of the thing records).
    std::vector<double> target_player_radii_;
    std::vector<Vec3d> target_thing_positions_;
    std::vector<double> target_thing_radii_;
    std::vector<std::uint8_t> target_thing_alive_;
    std::vector<std::uint32_t> target_thing_flags2_;
    std::vector<int> target_thing_movement_type_;
    std::vector<int> target_thing_has_object_;
    std::vector<int> target_thing_owner_;
    std::vector<int> target_thing_parent_;

    std::vector<ProjectileTickRecord> last_projectile_results_;
    /// One byte per `last_projectile_results_` record: 1 when the current
    /// generation was already blasted.  Cleared by every new `tick_results`
    /// assignment and by the explicit clears.
    std::vector<std::uint8_t> blasted_results_;
    EventDispatchResult last_events_;
};

} // namespace gm
