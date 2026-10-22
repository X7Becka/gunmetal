#include "game/runtime.h"

#include <array>
#include <cmath>
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

/// Copy the staged records of `queue` into a queue of the same kind, dropping
/// the `Trigger Switch` (type 2) records.  `build_world()` replaces the switch
/// table wholesale; a staged type-2 record addresses that table by index, so
/// keeping it would fire the new level's entry at the same index.  Every other
/// record (thing create/expire/destroy, blast generates, text) does not depend
/// on the table and has to survive a world rebuild.
///
/// A corrupt staging buffer parses as `ok == false` with no events; the result
/// is then empty, i.e. the old "drop the queue" behaviour for a buffer the
/// dispatcher would reject as a whole anyway.
EventQueue without_switch_records(const EventQueue& queue) {
    EventQueue filtered(queue.overflow_check());
    const EventParseResult parsed = EventQueue::parse(queue.bytes());
    if (!parsed.ok) {
        return filtered;
    }
    for (const Event& event : parsed.events) {
        if (event.type == EventType::trigger_switch) {
            continue;
        }
        filtered.post(event);
    }
    return filtered;
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
    // dropped with it instead of being dispatched against the new one.  Every
    // other staged record (thing create/expire/destroy, blast generates, text)
    // does not address the table and survives the rebuild: `build_world()`
    // runs on a live runtime that may already have posted world events.
    events_.queue() = without_switch_records(events_.queue());
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
    const std::size_t previous_count = player_eyes_.size();
    player_eyes_.assign(eyes.begin(), eyes.end());
    // The vitals are index-aligned with this list; a different player count
    // makes the old gauges describe the wrong players, so they are dropped
    // instead of being silently re-mapped.
    if (player_eyes_.size() != previous_count) {
        player_vitals_.clear();
    }
    if (ai_attached_) {
        if (player_eyes_.empty()) {
            ai_.clear_players();
        } else {
            ai_.set_players(player_eyes_);
        }
    }
    return true;
}

bool GameRuntime::set_player_vitals(std::span<const Vitals> vitals) {
    if (vitals.size() > static_cast<std::size_t>(kAiMaxPlayers)) {
        return false;
    }
    // Index alignment is the whole contract of the owned vitals: a span of a
    // different size is rejected before anything is written, so a rejected
    // call cannot corrupt the previous gauges or the eye list.
    if (vitals.size() != player_eyes_.size()) {
        return false;
    }
    player_vitals_.assign(vitals.begin(), vitals.end());
    return true;
}

void GameRuntime::clear_players() {
    player_eyes_.clear();
    player_vitals_.clear();
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

std::size_t GameRuntime::aim_switch(std::size_t player, const Vec3d& direction,
                                    double max_distance) {
    if (player >= player_eyes_.size()) {
        return 0;
    }
    const double length = std::sqrt(direction.x * direction.x +
                                    direction.y * direction.y +
                                    direction.z * direction.z);
    if (!(length > 0.0) || !std::isfinite(length)) {
        return 0;
    }
    const Vec3d unit{direction.x / length, direction.y / length, direction.z / length};
    const auto hit = collision_.raycast(player_eyes_[player], unit, max_distance);
    if (!hit.has_value() || hit->triangle == nullptr) {
        return 0;
    }
    PolygonHit polygon;
    polygon.sector = hit->triangle->sector;
    polygon.object = hit->triangle->object;
    polygon.polygon = hit->triangle->source_poly;
    polygon.distance = hit->t;
    // 0x44fc30 has no `0x428399` object gate on the aim path.
    polygon.object_flags = 0;
    const std::size_t fired =
        world_.poll_aim(polygon, static_cast<std::int16_t>(player));
    if (fired > 0) {
        post_switch_events(events_, world_, static_cast<std::int16_t>(player));
    }
    return fired;
}

GameRuntime::ImpactResult GameRuntime::resolve_hitscan_impact(
    std::size_t player, const ShotResult& shot) {
    ImpactResult out;
    if (!shot.ok || !shot.fired || !shot.hit) {
        return out;
    }
    if (player_weapons_.has_value()) {
        const Weapon* weapon = player_weapons_->at(shot.slot);
        if (weapon != nullptr && weapon->hitscan &&
            post_weapon_impact(events_.queue(), *weapon, shot,
                               static_cast<std::uint32_t>(player))) {
            out.impact_event = true;
        }
    }
    PolygonHit polygon;
    polygon.sector = shot.sector;
    polygon.object = shot.object;
    polygon.polygon = shot.polygon;
    polygon.distance = shot.distance;
    // 0x44fad0 compares only the triple; the object gate belongs to the blast
    // path (`0x44fb30`), so the impact passes zero flags.
    polygon.object_flags = 0;
    out.switches = world_.trigger_poly(polygon, static_cast<std::int16_t>(player));
    if (out.switches > 0) {
        post_switch_events(events_, world_, static_cast<std::int16_t>(player));
    }
    return out;
}

std::size_t GameRuntime::aim_switch_from_basis(std::size_t player,
                                               const CameraBasis& basis,
                                               double max_distance) {
    // `0x41e025` output 2 is `local_50`, the direction `0x43c51f` receives;
    // `camera_basis()` names the same row `forward`.
    if (!std::isfinite(basis.forward.x) || !std::isfinite(basis.forward.y) ||
        !std::isfinite(basis.forward.z)) {
        return 0;
    }
    return aim_switch(player, basis.forward, max_distance);
}

std::size_t GameRuntime::aim_switch_from_camera(std::size_t player, double roll,
                                                double pitch, double yaw,
                                                double max_distance) {
    if (!std::isfinite(roll) || !std::isfinite(pitch) || !std::isfinite(yaw)) {
        return 0;
    }
    return aim_switch_from_basis(player, camera_basis(roll, pitch, yaw), max_distance);
}

void GameRuntime::set_ai_projectile_template(Thing projectile) {
    ai_projectile_template_ = std::move(projectile);
}

void GameRuntime::clear_ai_projectile_template() {
    ai_projectile_template_.reset();
}

void GameRuntime::set_player_weapons(Weapons table, Arsenal arsenal) {
    player_weapons_ = std::move(table);
    player_arsenal_ = arsenal;
    player_charge_ = ChargeState{};
    pending_player_input_.reset();
    pending_player_origin_.reset();
    last_player_weapon_tick_ = PlayerWeaponTick{};
}

void GameRuntime::set_player_weapons(Weapons table) {
    Arsenal arsenal = table.initial();
    set_player_weapons(std::move(table), arsenal);
}

void GameRuntime::clear_player_weapons() {
    player_weapons_.reset();
    player_arsenal_ = Arsenal{};
    player_charge_ = ChargeState{};
    pending_player_input_.reset();
    pending_player_origin_.reset();
    last_player_weapon_tick_ = PlayerWeaponTick{};
}

void GameRuntime::set_player_projectile(Thing projectile, int owner_player,
                                        double level_gravity) {
    player_projectile_template_ = std::move(projectile);
    player_owner_ = owner_player;
    player_level_gravity_ = level_gravity;
}

void GameRuntime::clear_player_projectile() {
    player_projectile_template_.reset();
    player_owner_ = 0;
    player_level_gravity_ = 0.0;
}

void GameRuntime::set_player_weapon_input(PlayerWeaponInput input, Vec3d origin) {
    pending_player_input_ = input;
    pending_player_origin_ = origin;
}

PlayerWeaponTick GameRuntime::update_player_weapon(const PlayerWeaponInput& input,
                                                   Vec3d origin) {
    PlayerWeaponTick tick;
    if (!player_weapons_.has_value()) {
        // No tables: nothing can fire, but the call still reports why.
        tick.action.shot.error = FireError::no_weapon;
        last_player_weapon_tick_ = tick;
        return tick;
    }
    tick.action = gm::update_player_weapon(*player_weapons_, player_arsenal_,
                                           player_charge_, input, origin,
                                           collision_);
    if (tick.action.fired() && player_projectile_template_.has_value()) {
        const Weapon* weapon = player_weapons_->at(tick.action.slot);
        if (weapon != nullptr) {
            tick.spawned = spawn_player_shot(
                *weapon, tick.action.shot, *player_projectile_template_,
                player_owner_, player_level_gravity_);
        }
    }
    last_player_weapon_tick_ = tick;
    return tick;
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

void GameRuntime::ensure_motion_automata() {
    if (!config_.ai_apply_aim || !ai_attached_) {
        return;
    }
    if (things_.motion_enabled()) {
        return;
    }
    // The collision world of the runtime is the probe geometry of the
    // automata.  The pointer stays valid: `GameRuntime` is neither copyable
    // nor movable, and `build_world()` replaces the *contents* of
    // `collision_`, not the member itself.
    things_.set_motion_enabled(true, &collision_);
}

void GameRuntime::tick_once() {
    // 0. Opt-in aim bridge (see `GameRuntimeConfig::ai_apply_aim`): the
    //    movement automata must be enabled *before* phase 1, because
    //    `Things::tick` creates their state and `aim_motion_at` needs the
    //    body angles of a stepped state.  With the bridge off nothing here
    //    changes: the automata stay disabled and every phase is
    //    byte-for-byte as before.
    ensure_motion_automata();

    // 1. Things: age, lifetime, expire events (posted once per thing).
    things_.tick(1, &events_.queue());

    // 2. AI: `Ai::tick` mirrors thing vitals internally (a thing killed by
    //    damage dies here), then runs one fixed 1/91 s state step.
    if (ai_attached_) {
        ai_.tick(collision_, kAiTickSeconds);
    }

    // 2a. AI aim -> movement automata (opt-in).  The AI phase is the only
    //     producer of `aim_point`, so the bridge runs right after it: the
    //     decision of this tick reaches the automata, which consume it in the
    //     next tick (phase 1 steps them).
    //
    //     The order against the AI-shot bridge (2b) does not change any
    //     observable: the shot records (origin, direction, aim) are computed
    //     inside `Ai::tick` from `agent.aim_point`, so the shots of this tick
    //     are the same whether the aim bridge runs before or after them.
    if (config_.ai_apply_aim && ai_attached_) {
        stats_.aimed_things = ai_.apply_aim(things_);
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

    // 2c. Player weapon seam (opt-in): one `0x4453d0` tick from the input the
    //     caller installed for this step, then `spawn_player_shot()`.  It runs
    //     before the projectile step, so the new round is stepped (and its
    //     create event dispatched) by this same tick.  The input is consumed:
    //     a held trigger must be re-installed every tick by the input layer.
    if (config_.player_weapons_in_tick && pending_player_input_.has_value() &&
        pending_player_origin_.has_value()) {
        update_player_weapon(*pending_player_input_, *pending_player_origin_);
        pending_player_input_.reset();
        pending_player_origin_.reset();
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
    // Door open/close/crush events (`0x44f3f0`, `0x450590`) are GENERATE
    // records, not Trigger Switch ones: post them into the same staging queue
    // so `last_events_` carries them to the VM/game layer.
    world_.post_door_events(events_.queue());
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
    return apply_projectile_blasts_impl(&weapon, nullptr, &context, {},
                                        player_vitals, thing_vitals, only_new);
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts(
    const Weapon& weapon, std::span<const ProjectileBlastContext> contexts,
    std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
    bool only_new) {
    return apply_projectile_blasts_impl(&weapon, nullptr, nullptr, contexts,
                                        player_vitals, thing_vitals, only_new);
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts(
    const EventBlastRegistry& registry, const ProjectileBlastContext& context,
    std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
    bool only_new) {
    return apply_projectile_blasts_impl(nullptr, &registry, &context, {},
                                        player_vitals, thing_vitals, only_new);
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts(
    const EventBlastRegistry& registry,
    std::span<const ProjectileBlastContext> contexts,
    std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
    bool only_new) {
    return apply_projectile_blasts_impl(nullptr, &registry, nullptr, contexts,
                                        player_vitals, thing_vitals, only_new);
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts_impl(
    const Weapon* weapon, const EventBlastRegistry* registry,
    const ProjectileBlastContext* shared,
    std::span<const ProjectileBlastContext> per_record,
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
    // provide.  Both arrays are runtime scratch: kept between calls, so the
    // bridge allocates only when the thing count grows.
    const std::vector<Thing>& list = things_.things();
    const std::size_t thing_count = list.size();
    blast_thing_positions_.resize(thing_count);
    if (thing_count > blast_thing_alive_capacity_) {
        blast_thing_alive_ = std::make_unique<bool[]>(thing_count);
        blast_thing_alive_capacity_ = thing_count;
    }
    for (std::size_t i = 0; i < thing_count; ++i) {
        const Thing& thing = list[i];
        const bool absolute = thing.position.has_value();
        blast_thing_positions_[i] =
            absolute ? Vec3d{static_cast<double>((*thing.position)[0]),
                             static_cast<double>((*thing.position)[1]),
                             static_cast<double>((*thing.position)[2])}
                     : Vec3d{};
        blast_thing_alive_[i] = thing.alive && absolute;
    }

    ProjectileBlastTargets targets;
    targets.player_positions = player_eyes_;
    targets.player_vitals = player_vitals;
    targets.thing_positions = blast_thing_positions_;
    if (thing_count != 0) {
        // An empty `thing_alive` is the documented "no data" span; a
        // `(nullptr, 0)` span is avoided for the empty case.
        targets.thing_alive =
            std::span<const bool>(blast_thing_alive_.get(), thing_count);
    }
    targets.thing_vitals = thing_vitals;

    for (std::size_t i = 0; i < count; ++i) {
        const ProjectileTickRecord& record = last_projectile_results_[i];
        const ProjectileBlastContext* context =
            shared != nullptr ? shared
                              : (i < per_record.size() ? &per_record[i] : nullptr);
        // Without a context entry the proximity opt-in is unknown, so a
        // proximity record is not treated as a trigger (the default is off).
        if (!blast_trigger(record.status, record.already_spent,
                           context != nullptr && context->allow_proximity)) {
            continue;
        }
        if (blasted_results_[i] != 0) {
            if (only_new) {
                ++batch.already_applied;
                continue;
            }
        }
        if (context == nullptr) {
            // Per-record form with a short span: no owner is invented, the
            // record is reported instead of being blasted with wrong ids.
            ++batch.skipped_no_context;
            continue;
        }

        BlastDamageResult one;
        if (registry != nullptr) {
            // Event-driven lookup: the round's own collide/expire/destroy id,
            // exactly the record the original posts from 0x4752d0/0x4703a0.
            std::uint32_t event_id = 0;
            switch (record.status) {
            case ProjectileStatus::expired:
                event_id = record.expire_event;
                break;
            case ProjectileStatus::bounced:
                event_id = record.collide_event;
                break;
            case ProjectileStatus::destroyed:
                event_id = record.destroy_event;
                break;
            case ProjectileStatus::flying:
            case ProjectileStatus::proximity:
                break;
            }
            if (event_id == 0) {
                // A proximity result has no proven blast link (its event is
                // the +0x21c GENERATE id, not THING_TYPES collide/expire), so
                // it only reaches this path with a caller's opt-in and still
                // has no id to look up; nothing is applied.
                ++batch.skipped_no_event;
                blasted_results_[i] = 1;
                continue;
            }
            one = apply_event_blast(
                *registry, event_id, record.position, context->underwater,
                context->from_ground, context->deathmatch,
                context->source_player, context->source_thing,
                targets.player_positions, targets.player_vitals,
                targets.thing_positions, targets.thing_alive,
                targets.thing_vitals);
        } else {
            apply_projectile_blast(
                *weapon, projectile_result_of(record),
                record.status == ProjectileStatus::expired, *context, targets,
                &one);
        }

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

ProjectileBlastBatch GameRuntime::apply_projectile_blasts_to_players(
    const Weapon& weapon, const ProjectileBlastContext& context,
    std::span<ThingVitals> thing_vitals, bool only_new) {
    // Same marks and the same per-record loop: a caller may alternate the two
    // entry points and still blast each generation of records at most once.
    return apply_projectile_blasts_impl(&weapon, nullptr, &context, {},
                                        std::span<Vitals>(player_vitals_),
                                        thing_vitals, only_new);
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts_to_players(
    const Weapon& weapon, std::span<const ProjectileBlastContext> contexts,
    std::span<ThingVitals> thing_vitals, bool only_new) {
    return apply_projectile_blasts_impl(&weapon, nullptr, nullptr, contexts,
                                        std::span<Vitals>(player_vitals_),
                                        thing_vitals, only_new);
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts_to_players(
    const EventBlastRegistry& registry, const ProjectileBlastContext& context,
    std::span<ThingVitals> thing_vitals, bool only_new) {
    return apply_projectile_blasts_impl(nullptr, &registry, &context, {},
                                        std::span<Vitals>(player_vitals_),
                                        thing_vitals, only_new);
}

ProjectileBlastBatch GameRuntime::apply_projectile_blasts_to_players(
    const EventBlastRegistry& registry,
    std::span<const ProjectileBlastContext> contexts,
    std::span<ThingVitals> thing_vitals, bool only_new) {
    return apply_projectile_blasts_impl(nullptr, &registry, nullptr, contexts,
                                        std::span<Vitals>(player_vitals_),
                                        thing_vitals, only_new);
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

int GameRuntime::spawn_player_shot(const Weapon& weapon, const ShotResult& shot,
                                   const Thing& projectile_thing,
                                   int owner_player, double level_gravity) {
    if (!shot.ok || !shot.fired || !shot.projectile) {
        return -1;
    }
    Thing projectile = projectile_thing;
    if (projectile.id < 0) {
        projectile.id = take_ai_projectile_id();
    }
    const std::optional<ProjectileSpawn> spawn = projectile_spawn_from_shot(
        weapon, shot, &projectile, owner_player, projectile.id, level_gravity);
    if (!spawn) {
        return -1;
    }
    return projectiles_.spawn(*spawn, &events_.queue());
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
    player_vitals_.clear();
    // Level-side player weapon state is dropped with the level; the projectile
    // template and its owner/gravity are caller configuration and stay.
    player_weapons_.reset();
    player_arsenal_ = Arsenal{};
    player_charge_ = ChargeState{};
    pending_player_input_.reset();
    pending_player_origin_.reset();
    last_player_weapon_tick_ = PlayerWeaponTick{};
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
    stats_ = GameRuntimeStats{};
}

void GameRuntime::clear_events() {
    events_.queue().clear();
    world_.clear_events();
    ai_.clear_events();
    last_projectile_results_.clear();
    blasted_results_.clear();
    last_events_ = EventDispatchResult{};
    // The aim counter describes the last tick, whose results are dropped here.
    stats_ = GameRuntimeStats{};
}

void GameRuntime::clear_projectiles() {
    projectiles_ = Projectiles{};
    last_projectile_results_.clear();
    blasted_results_.clear();
}

} // namespace gm
