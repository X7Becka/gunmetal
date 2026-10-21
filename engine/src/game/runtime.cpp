#include "game/runtime.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace gm {

namespace {

bool fail(std::string* out, std::string message) {
    if (out != nullptr) {
        *out = std::move(message);
    }
    return false;
}

void clear_error(std::string* out) {
    if (out != nullptr) {
        out->clear();
    }
}

/// Mirror of the trigger table documented on `apply_projectile_blast()`: the
/// bridge has to know whether a record is a blast before calling it, because
/// the call cannot distinguish "no trigger" from "triggered, nothing in range".
bool blast_trigger(ProjectileStatus status, bool already_spent,
                   bool allow_proximity) {
    switch (status) {
    case ProjectileStatus::bounced:
        return true;
    case ProjectileStatus::expired:
        // A spent state replays `expired` on every step; only the first report
        // carries the expiry blast.
        return !already_spent;
    case ProjectileStatus::proximity:
        return allow_proximity;
    case ProjectileStatus::flying:
    case ProjectileStatus::destroyed:
        return false;
    }
    return false;
}

} // namespace

GameRuntime::GameRuntime(GameRuntimeConfig config) : config_(std::move(config)) {}

bool GameRuntime::build(const SbtLevel& level, const VmState& vm, const SbwLevel& geometry,
                        std::string* error) {
    clear_error(error);
    reset();
    std::string local;
    if (!build_world(level, geometry, &local)) {
        reset();
        return fail(error, "world: " + local);
    }
    if (!build_things(level, vm, &local)) {
        reset();
        return fail(error, "things: " + local);
    }
    return true;
}

bool GameRuntime::build_world(const SbtLevel& level, const SbwLevel& geometry,
                              std::string* error) {
    clear_error(error);
    // Build into temporaries so a rejected level leaves the previous world and
    // the installed switch table untouched.
    WorldState next_world;
    std::string local;
    if (!next_world.build(level, geometry, &local)) {
        return fail(error, local.empty() ? "world build failed" : local);
    }
    CollisionWorld next_collision;
    if (!next_collision.build(geometry, &local)) {
        return fail(error, "collision: " + local);
    }
    world_ = std::move(next_world);
    collision_ = std::move(next_collision);
    world_built_ = true;
    // The old table may carry ids of a different level; the dispatcher has no
    // clear(), so replace it wholesale before installing the new records.
    // Staged type-2 records point into the old table by index, so they are
    // dropped with it instead of being dispatched against the new one.
    events_.queue().clear();
    events_.dispatcher() = EventDispatcher{};
    if (config_.install_switches) {
        events_.install_switches(world_.switches());
    }
    return true;
}

bool GameRuntime::build_things(const SbtLevel& level, const VmState& vm,
                               std::string* error) {
    clear_error(error);
    Things next = Things::load(level, vm, config_.things);
    if (!next.ok()) {
        return fail(error, next.error().empty() ? "thing build failed" : next.error());
    }
    things_ = std::move(next);
    things_built_ = true;
    attach_ai();
    return true;
}

std::size_t GameRuntime::attach_ai() {
    ai_ = Ai(config_.ai);
    if (!config_.build_ai) {
        ai_attached_ = false;
        return 0;
    }
    ai_attached_ = true;
    if (!player_eyes_.empty()) {
        ai_.set_players(player_eyes_);
    }
    return ai_.attach(things_);
}

bool GameRuntime::set_player_eyes(std::span<const Vec3d> eyes) {
    if (eyes.size() > static_cast<std::size_t>(kAiMaxPlayers)) {
        return false;
    }
    for (const Vec3d& eye : eyes) {
        if (!finite(eye)) {
            return false;
        }
    }
    player_eyes_.assign(eyes.begin(), eyes.end());
    if (ai_attached_) {
        if (player_eyes_.empty()) {
            ai_.clear_players();
        } else {
            ai_.set_players(player_eyes_);
        }
    }
    return true;
}

void GameRuntime::clear_players() {
    player_eyes_.clear();
    target_player_radii_.clear();
    if (ai_attached_) {
        ai_.clear_players();
    }
}

bool GameRuntime::press_switch(int switch_id, std::int16_t source) {
    if (!world_.interact(switch_id)) {
        return false;
    }
    switch_source_ = source;
    // `WorldState::tick()` drops the unposted frame log, so mirror the record
    // now.  A full queue is fine: the world remembers the pending tail.
    post_switch_events(events_, world_, source);
    return true;
}

void GameRuntime::set_ai_projectile_template(Thing projectile) {
    ai_projectile_template_ = std::move(projectile);
}

void GameRuntime::clear_ai_projectile_template() {
    ai_projectile_template_.reset();
}

int GameRuntime::take_ai_projectile_id() {
    const int id = next_ai_projectile_id_;
    ++next_ai_projectile_id_;
    return id;
}

std::size_t GameRuntime::bridge_ai_shots() {
    if (!ai_projectile_template_.has_value()) {
        // No template: keep the queue so a later tick (after the template is
        // set) can still spawn these shots.  Nothing is dropped silently.
        return 0;
    }
    std::size_t spawned = 0;
    // `spawn_ai_shots()` would hand the same template id to every shot, and
    // `Projectiles::spawn()` rejects a live duplicate, so the bridge allocates
    // a fresh runtime id per round and calls the single-shot factory instead.
    for (const AiWeaponShot& shot : ai_.take_weapon_shots()) {
        Thing projectile = *ai_projectile_template_;
        projectile.id = take_ai_projectile_id();
        ProjectileSpawn spawn;
        if (!ai_shot_projectile_spawn(shot, &projectile, -1, spawn)) {
            continue; // no usable speed: dropped, like `spawn_ai_shots()`
        }
        if (projectiles_.spawn(std::move(spawn), &events_.queue()) >= 0) {
            ++spawned;
        }
    }
    return spawned;
}

void GameRuntime::tick(std::uint64_t ticks) {
    for (std::uint64_t i = 0; i < ticks; ++i) {
        tick_once();
    }
}

void GameRuntime::tick_once() {
    // 1. Things: age, lifetime, expire events (posted once per thing).
    things_.tick(1, &events_.queue());

    // 2. AI: mirror thing vitals first (a thing killed by damage dies here),
    //    then one fixed 1/91 s state step.
    if (ai_attached_) {
        ai_.sync_vitals();
        ai_.tick(collision_, kAiTickSeconds);
    }

    // 2b. AI shots -> projectiles (opt-in).  The queue is drained exactly once
    //     per tick, after the AI step and before the projectile step, so a
    //     round spawned here already appears in this tick's
    //     `last_projectile_results()` and its create event is dispatched by
    //     this tick's events phase.  With the bridge off (or no template) the
    //     queue is left untouched: the caller owns it.
    if (config_.ai_shots_to_projectiles) {
        bridge_ai_shots();
    }

    // 3. Projectiles: one `tick_results` pass, with the queue, so every event
    //    is posted inside the loop exactly once.  The returned records are
    //    moved into `last_projectile_results_`, not rebuilt.
    const ProjectileTargets targets = projectile_targets();
    last_projectile_results_ =
        projectiles_.tick_results(1, targets, collision_, &events_.queue());
    // New generation: every record may be blasted once by the bridge.
    blasted_results_.assign(last_projectile_results_.size(), 0);

    // 4. Doors/lifts: one fixed world step.
    world_.tick(1);

    // 5. Events: retry a switch tail a full queue left pending, then dispatch
    //    the whole staging buffer once.  `pump` drains it, so the next tick
    //    cannot replay these records.
    post_switch_events(events_, world_, switch_source_);
    const std::uint32_t now = static_cast<std::uint32_t>(clock_);
    last_events_ = events_.pump(now);
    ++clock_;
}

ProjectileTargets GameRuntime::projectile_targets() {
    ProjectileTargets targets;
    targets.player_positions = std::span<const Vec3d>(player_eyes_);
    target_player_radii_.assign(player_eyes_.size(), config_.player_radius);
    targets.player_radii = target_player_radii_;

    if (!config_.projectiles_use_things) {
        return targets;
    }

    const std::vector<Thing>& list = things_.things();
    const std::size_t count = list.size();
    target_thing_positions_.resize(count);
    target_thing_radii_.resize(count);
    target_thing_alive_.resize(count);
    target_thing_flags2_.resize(count);
    target_thing_movement_type_.resize(count);
    target_thing_has_object_.resize(count);
    target_thing_owner_.resize(count);
    target_thing_parent_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Thing& thing = list[i];
        const bool absolute = thing.position.has_value();
        target_thing_positions_[i] =
            absolute ? Vec3d{static_cast<double>((*thing.position)[0]),
                             static_cast<double>((*thing.position)[1]),
                             static_cast<double>((*thing.position)[2])}
                     : Vec3d{};
        target_thing_radii_[i] = thing.radius;
        // A source-relative thing has no world point in this slice, so it can
        // never be a proximity target; the slot is kept for index stability.
        target_thing_alive_[i] = (thing.alive && absolute) ? std::uint8_t{1}
                                                           : std::uint8_t{0};
        // Thing carries no runtime flags2 word; 0 means "not the 0x20000 dead
        // bit", which is already covered by `alive`.
        target_thing_flags2_[i] = 0;
        target_thing_movement_type_[i] = thing.movement_type;
        target_thing_has_object_[i] = thing.model.empty() ? 0 : 1;
        target_thing_owner_[i] = thing.owner;
        target_thing_parent_[i] = thing.parent;
    }
    targets.thing_positions = target_thing_positions_;
    targets.thing_radii = target_thing_radii_;
    targets.thing_alive = target_thing_alive_;
    targets.thing_flags2 = target_thing_flags2_;
    targets.thing_movement_type = target_thing_movement_type_;
    targets.thing_has_object = target_thing_has_object_;
    targets.thing_owner = target_thing_owner_;
    targets.thing_parent = target_thing_parent_;
    return targets;
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts(
    const Weapon& weapon, const ProjectileBlastContext& context,
    std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
    bool only_new) {
    ProjectileBlastBatch batch;
    const std::size_t count = last_projectile_results_.size();
    // Defensive: the marks are sized by `tick_once()`; rebuild them if a
    // caller cleared the result behind the runtime's back.
    if (blasted_results_.size() != count) {
        blasted_results_.assign(count, 0);
    }
    if (count == 0) {
        return batch;
    }

    // Target points: the runtime owns the positions (player eyes and the
    // world points of its things), so the caller only supplies vitals at the
    // same indices.  `ProjectileBlastTargets::thing_alive` needs a contiguous
    // `bool` array (index-stable like `things().things()`, dead and
    // source-relative things marked absent), which `std::vector<bool>` cannot
    // provide.
    const std::vector<Thing>& list = things_.things();
    const std::size_t thing_count = list.size();
    std::vector<Vec3d> thing_positions(thing_count);
    std::unique_ptr<bool[]> thing_alive(new bool[thing_count]);
    for (std::size_t i = 0; i < thing_count; ++i) {
        const Thing& thing = list[i];
        const bool absolute = thing.position.has_value();
        thing_positions[i] =
            absolute ? Vec3d{static_cast<double>((*thing.position)[0]),
                             static_cast<double>((*thing.position)[1]),
                             static_cast<double>((*thing.position)[2])}
                     : Vec3d{};
        thing_alive[i] = thing.alive && absolute;
    }

    ProjectileBlastTargets targets;
    targets.player_positions = player_eyes_;
    targets.player_vitals = player_vitals;
    targets.thing_positions = thing_positions;
    targets.thing_alive = std::span<const bool>(thing_alive.get(), thing_count);
    targets.thing_vitals = thing_vitals;

    for (std::size_t i = 0; i < count; ++i) {
        const ProjectileTickRecord& record = last_projectile_results_[i];
        if (!blast_trigger(record.status, record.already_spent,
                           context.allow_proximity)) {
            continue;
        }
        if (blasted_results_[i] != 0) {
            if (only_new) {
                ++batch.already_applied;
                continue;
            }
        }

        BlastDamageResult one;
        apply_projectile_blast(
            weapon, projectile_result_of(record),
            record.status == ProjectileStatus::expired, context, targets, &one);

        // One record per affected target with a non-zero blast event.  The
        // original per-target coordinates are not proven here, so the record
        // is posted at the blast centre (the post-step projectile position).
        for (const AppliedBlastHit& hit : one.hits) {
            if (hit.hit.event == 0) {
                continue;
            }
            if (events_.queue().post_generate(
                    static_cast<std::int32_t>(hit.hit.event), 0,
                    record.position.x, record.position.y, record.position.z,
                    0)) {
                ++batch.posted_events;
            }
        }

        batch.damage.chain_reaction =
            batch.damage.chain_reaction || one.chain_reaction;
        batch.damage.hits.insert(batch.damage.hits.end(), one.hits.begin(),
                                 one.hits.end());
        blasted_results_[i] = 1;
        ++batch.blasts;
    }
    return batch;
}

int GameRuntime::spawn_thing(Thing thing) {
    const int id = things_.spawn(std::move(thing), &events_.queue());
    if (id >= 0) {
        things_built_ = true;
    }
    return id;
}

bool GameRuntime::remove_thing(int id) {
    return things_.remove(id, &events_.queue());
}

int GameRuntime::spawn_projectile(ProjectileSpawn spawn) {
    return projectiles_.spawn(std::move(spawn), &events_.queue());
}

bool GameRuntime::remove_projectile(int thing_id) {
    return projectiles_.remove(thing_id, &events_.queue());
}

void GameRuntime::reset() {
    world_ = WorldState{};
    things_ = Things{};
    ai_ = Ai(config_.ai);
    projectiles_ = Projectiles{};
    events_ = EventRuntime{};
    collision_.clear();
    player_eyes_.clear();
    switch_source_ = 0;
    clock_ = 0;
    // The template belongs to the configuration side and is kept (like
    // `config()`); the id counter restarts with the empty projectile fleet.
    next_ai_projectile_id_ = kAiProjectileRuntimeIdBase;
    world_built_ = false;
    things_built_ = false;
    ai_attached_ = false;
    last_projectile_results_.clear();
    blasted_results_.clear();
    last_events_ = EventDispatchResult{};
}

void GameRuntime::clear_events() {
    events_.queue().clear();
    world_.clear_events();
    ai_.clear_events();
    last_projectile_results_.clear();
    blasted_results_.clear();
    last_events_ = EventDispatchResult{};
}

void GameRuntime::clear_projectiles() {
    projectiles_ = Projectiles{};
    last_projectile_results_.clear();
    blasted_results_.clear();
}

} // namespace gm
