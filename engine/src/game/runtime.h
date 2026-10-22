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
//   2c. player fire opt-in: one `0x4453d0` weapon tick from the input
//                  installed with `set_player_weapon_input()`, then
//                  `spawn_player_shot()` for a fired projectile
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
// once until the next `tick()`.  The context that selects the blast branch and
// the self-hit guard is either one for the whole fleet or one per result record
// (`std::span<const ProjectileBlastContext>`, indexed like the results), so a
// fleet with mixed owners keeps every round's own source ids.
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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/events.h"
#include "game/projectile.h"
#include "game/trigger.h"
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
    /// Opt-in player weapon phase: `tick_once()` consumes the input installed
    /// by `set_player_weapon_input()` (one `0x4453d0` weapon tick) and spawns
    /// the round through `spawn_player_shot()`.  `false` (default) leaves the
    /// seam to the caller: call `update_player_weapon()` explicitly when the
    /// immediate result is needed.
    bool player_weapons_in_tick = false;
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
    /// Result records that blast (bounce / non-replayed expiry) but had no
    /// context entry in the per-record overload, so they were skipped instead
    /// of being blasted with an invented owner.  A `proximity` record is not
    /// counted: whether it triggers is decided by the missing context's
    /// `allow_proximity`.
    std::size_t skipped_no_context = 0;
    /// Result records that would blast but carry no event id for their trigger
    /// (`collide_event == 0` on a bounce, `expire_event == 0` on an expiry,
    /// `destroy_event == 0` on an RGM destroy) in the `EventBlastRegistry`
    /// overloads.  Nothing is applied for them.
    std::size_t skipped_no_event = 0;
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
    ///
    /// The switch table is replaced wholesale, so the staged `Trigger Switch`
    /// (type 2) records of the old table are dropped with it: they address the
    /// table by index and would otherwise fire the *new* entry at the same
    /// index.  Every other staged record (thing create/expire/destroy, blast
    /// generates, text) does not depend on the table and is kept.
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

    /// ``0x44fc30`` aimed half: cast *direction* from player *player*'s eye
    /// (`set_player_eyes`) through the collision world and let the world's
    /// ``kind 0x101`` switches match the hit polygon.  The engine's ray length
    /// is ``0x40c38800`` = 10 000 units, passed here as *max_distance*.
    ///
    /// On a hit the ``(sector, object, polygon)`` triple comes from the
    /// triangle record (``RayHit::triangle``), exactly the three ints
    /// ``0x43c51f`` returns; *source* is the player index, as ``0x479810``
    /// takes it.  Fired presses are mirrored into the staging queue like
    /// `press_switch`, so `last_events()` carries the type-2 records.  Returns
    /// the number of switches that fired (0 for no players, a zero/NaN
    /// direction, a miss or no matching switch).
    std::size_t aim_switch(std::size_t player, const Vec3d& direction,
                           double max_distance = 10000.0);

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

    // --- player weapon seam -------------------------------------------------

    /// Install the player's weapon tables and loadout (docs/WEAPONS_CPP.md
    /// §3.6).  `set_player_weapons(table)` uses `table.initial()` (the save's
    /// ammo/mask/selected slot); the two-argument form overrides it.  The
    /// charge accumulator (`0x4453d0`'s `+0x2fa8/+0x2fa0`) starts zeroed.
    /// Replacing the loadout clears the pending input and the last result.
    void set_player_weapons(Weapons table);
    void set_player_weapons(Weapons table, Arsenal arsenal);
    /// Drop the tables/loadout/charge state; `reset()` also does this.
    void clear_player_weapons();
    bool has_player_weapons() const { return player_weapons_.has_value(); }
    /// The installed tables, or nullptr when none were set.
    const Weapons* player_weapons() const {
        return player_weapons_ ? &*player_weapons_ : nullptr;
    }
    Arsenal& player_arsenal() { return player_arsenal_; }
    const Arsenal& player_arsenal() const { return player_arsenal_; }
    ChargeState& player_charge() { return player_charge_; }
    const ChargeState& player_charge() const { return player_charge_; }

    /// Projectile template for the player FIRE path: the same `Thing` shape
    /// `spawn_player_shot()` expects (movement-type 6 projectile).  Its `id`
    /// is replaced per round from the runtime id pool (`0x10000+`).
    /// `owner_player` feeds the self-hit guard of `apply_projectile_blasts*`.
    void set_player_projectile(Thing projectile, int owner_player = 0,
                               double level_gravity = 0.0);
    void clear_player_projectile();
    const std::optional<Thing>& player_projectile() const {
        return player_projectile_template_;
    }
    int player_owner() const { return player_owner_; }
    double player_level_gravity() const { return player_level_gravity_; }

    /// Install the input of the next tick for the opt-in phase
    /// (`config().player_weapons_in_tick`).  `tick_once()` consumes it exactly
    /// once, right after the AI phase and before the projectile step, so the
    /// round appears in that tick's `last_projectile_results()` and its create
    /// event is dispatched by the same tick.  Without a new input a following
    /// tick does not repeat the trigger.
    void set_player_weapon_input(PlayerWeaponInput input, Vec3d origin);
    bool has_player_weapon_input() const {
        return pending_player_input_.has_value();
    }

    /// One explicit player weapon tick (`0x4453d0`): slot request -> charge
    /// state machine -> `fire()` -> `spawn_player_shot()`.  This is the same
    /// body the opt-in tick phase runs; it returns the result immediately and
    /// keeps it for `last_player_weapon_tick()`.  Never builds anything.
    PlayerWeaponTick update_player_weapon(const PlayerWeaponInput& input,
                                          Vec3d origin);
    /// Result of the last explicit call or of the last tick phase; empty
    /// (`action.slot == -1`) when neither ran yet.
    const PlayerWeaponTick& last_player_weapon_tick() const {
        return last_player_weapon_tick_;
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
    /// **Legacy for AI rounds.**  The original picks the blast by the event id
    /// the round carries, not by a weapon record; use the
    /// `EventBlastRegistry` overloads below for AI/generic projectiles.  This
    /// form stays for the player FIRE path, where `fire()` already resolved
    /// the blasts into `Weapon::impact_blasts`/`expire_blasts`.
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

    /// The same bridge with one context per result record: `contexts[i]`
    /// describes `last_projectile_results()[i]` (the spawn-slot order, i.e.
    /// `ProjectileTickRecord::index`, so the owner of every round is the one
    /// its own spawn carries).  This is the mixed-owner form: a single
    /// context can only name one `source_player`/`source_thing`, so the
    /// self-hit guard of the other owners would skip the wrong target.
    ///
    /// The span is index-aligned but does not have to be complete: a record
    /// whose index is outside `contexts` is not blasted (no owner is
    /// invented) and is counted in `ProjectileBlastBatch::skipped_no_context`
    /// when it would have blasted.  Marks, vitals spans, `only_new` and the
    /// event posting are exactly those of the single-context overload, and the
    /// two forms share the per-generation marks.
    ProjectileBlastBatch apply_projectile_blasts(
        const Weapon& weapon, std::span<const ProjectileBlastContext> contexts,
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

    /// The owned-vitals form of the per-record overload above: `contexts[i]`
    /// is the context of `last_projectile_results()[i]`, records outside the
    /// span are not blasted and are counted in `skipped_no_context`.
    ProjectileBlastBatch apply_projectile_blasts_to_players(
        const Weapon& weapon, std::span<const ProjectileBlastContext> contexts,
        std::span<ThingVitals> thing_vitals = {}, bool only_new = true);

    /// Event-driven blast bridge: the same per-generation marks and target
    /// arrays as the `Weapon` forms above, but the blast branch is selected by
    /// the *event id the round itself carries*, not by a `Weapon`.
    ///
    /// `Projectiles::tick_results()` records the spawn's `collide_event`
    /// (`THING_TYPES+0x1c4`), `expire_event` (`+0x1ec`) and `destroy_event`
    /// (`+0x1f8`).  A bounce looks up `collide_event`, a non-replayed expiry
    /// `expire_event`, an RGM destroy `destroy_event`; a record without an id
    /// is counted in `skipped_no_event` and applies nothing.  This is the
    /// original path (`0x4752d0`/`0x4703a0` post the id, the entity sub-script
    /// runs `CAUSE_BLAST_RADIUS_DAMAGE`), and it is the only correct one for
    /// AI rounds: `AiWeaponShot::event_id` is the proximity `GENERATE` id
    /// (`THING_TYPES+0x21c`), not a `DEFINE_WEAPONS` slot.
    ///
    /// The registry is caller-owned
    /// (`EventBlastRegistry::add_from_weapons(weapons, level)`) because the
    /// runtime does not retain the `.sbt`; vitals, marks, `only_new` and the
    /// posted hit events are identical to the `Weapon` overloads.
    ProjectileBlastBatch apply_projectile_blasts(
        const EventBlastRegistry& registry,
        const ProjectileBlastContext& context,
        std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
        bool only_new = true);

    ProjectileBlastBatch apply_projectile_blasts(
        const EventBlastRegistry& registry,
        std::span<const ProjectileBlastContext> contexts,
        std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
        bool only_new = true);

    ProjectileBlastBatch apply_projectile_blasts_to_players(
        const EventBlastRegistry& registry,
        const ProjectileBlastContext& context,
        std::span<ThingVitals> thing_vitals = {}, bool only_new = true);

    ProjectileBlastBatch apply_projectile_blasts_to_players(
        const EventBlastRegistry& registry,
        std::span<const ProjectileBlastContext> contexts,
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

    /// Player FIRE path: turn an accepted `fire()` result into a live round.
    ///
    /// `fire()` resolves direction/spread/speed (including the `0x4453d0`
    /// charge) but stays pure; this is the runtime seam that pairs it with the
    /// FIRE sub-script's THING_TYPES projectile (`0x46ea40` -> `0x46d8b0`).
    /// `projectile_thing` supplies gravity/damping/shape/radius/hits/lifetime
    /// and event ids (`docs/THING_FIELDS.md`); its `id` becomes the spawn
    /// `thing_id` and is auto-assigned from the runtime id pool when negative
    /// (`0x10000+`, never clashes with level things).  `owner_player` is the
    /// player index used by the self-hit guard of `apply_projectile_blasts*`.
    ///
    /// Returns the spawn index (`Projectiles::spawn`) or -1 when the shot was
    /// not fired, was hitscan, had no usable speed, or the id was already live.
    /// The create event goes through the runtime queue exactly like
    /// `spawn_projectile()`.
    int spawn_player_shot(const Weapon& weapon, const ShotResult& shot,
                          const Thing& projectile_thing, int owner_player,
                          double level_gravity = 0.0);
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
    /// Shared body of the `apply_projectile_blasts()` forms: exactly one of
    /// `weapon`/`registry` is non-null and selects the blast source.  A
    /// non-null `shared` means one context for the whole fleet, otherwise
    /// `per_record[i]` is the context of result record `i` and records outside
    /// the span are skipped.
    ProjectileBlastBatch apply_projectile_blasts_impl(
        const Weapon* weapon, const EventBlastRegistry* registry,
        const ProjectileBlastContext* shared,
        std::span<const ProjectileBlastContext> per_record,
        std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
        bool only_new);

    GameRuntimeConfig config_;
    WorldState world_;
    Things things_;
    Ai ai_;
    Projectiles projectiles_;
    EventRuntime events_;
    CollisionWorld collision_;
    std::optional<Thing> ai_projectile_template_;
    /// Player weapon seam state (docs/WEAPONS_CPP.md §3.6).  The tables and
    /// loadout are level state (dropped by `reset()`); the projectile template
    /// is caller configuration and survives like `ai_projectile_template_`.
    std::optional<Weapons> player_weapons_;
    Arsenal player_arsenal_{};
    ChargeState player_charge_{};
    std::optional<Thing> player_projectile_template_;
    int player_owner_ = 0;
    double player_level_gravity_ = 0.0;
    std::optional<PlayerWeaponInput> pending_player_input_;
    std::optional<Vec3d> pending_player_origin_;
    PlayerWeaponTick last_player_weapon_tick_{};
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

    // Scratch target arrays of the blast bridge: rebuilt from `Things` on each
    // `apply_projectile_blasts()` call but kept between calls, so a steady
    // state call does not allocate them.  `blast_thing_alive_` is a raw block
    // because `ProjectileBlastTargets::thing_alive` is a `span<const bool>`
    // and `std::vector<bool>` has no contiguous `bool` storage.  They are
    // derived data, like `target_*` above: `reset()` keeps the capacity.
    std::vector<Vec3d> blast_thing_positions_;
    std::unique_ptr<bool[]> blast_thing_alive_;
    std::size_t blast_thing_alive_capacity_ = 0;

    EventDispatchResult last_events_;
};

} // namespace gm
