#include "game/projectile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "game/events.h"
#include "game/weapons.h"
#include "world/ai.h"
#include "world/things.h"

namespace gm {
namespace {

// The projectile bridge converts Weapon units/second with this constant; keep
// it in lockstep with the two other 91 Hz clocks.
static_assert(kProjectileTickHz == kWeaponTickHz);
static_assert(kProjectileTickHz == static_cast<double>(kThingTickHz));

double finite_or(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

Vec3d safe_direction(const Vec3d& value) {
    const double len = length(value);
    if (!(len > 1e-12) || !std::isfinite(len)) {
        return {0.0, 0.0, 1.0};
    }
    return value * (1.0 / len);
}

std::uint32_t event_extra(int thing_id) {
    // 0x4703a0 / 0x470290: the low 16 bits are the runtime index + 8.
    return static_cast<std::uint32_t>(thing_id - 0xfff8);
}

std::uint32_t proximity_extra(int thing_id, int target) {
    const std::uint32_t projectile = static_cast<std::uint32_t>(
        std::max(0, thing_id) + 8);
    return (static_cast<std::uint32_t>(std::max(0, target)) << 16) | projectile;
}

double collision_radius(const ProjectileSpawn& spawn) {
    if (spawn.shape == 2) {
        return std::max({std::abs(spawn.box[0]), std::abs(spawn.box[1]),
                         std::abs(spawn.box[2])});
    }
    return std::max(0.0, spawn.radius);
}

int root_owner(const ProjectileTargets& targets, int thing, int guard = 64) {
    if (thing < 0 ||
        static_cast<std::size_t>(thing) >= targets.thing_owner.size()) {
        return -1;
    }
    int current = thing;
    for (int step = 0; step < guard; ++step) {
        if (current < 0 ||
            static_cast<std::size_t>(current) >= targets.thing_parent.size()) {
            break;
        }
        const int parent = targets.thing_parent[static_cast<std::size_t>(current)];
        if (parent < 0 || parent == current) {
            break;
        }
        current = parent;
    }
    if (static_cast<std::size_t>(current) >= targets.thing_owner.size()) {
        return -1;
    }
    return targets.thing_owner[static_cast<std::size_t>(current)];
}

int find_proximity_target(const ProjectileState& state,
                          const ProjectileTargets& targets) {
    if (state.spawn.proximity.empty()) {
        return -1;
    }
    const ProximityEntry& entry = state.spawn.proximity.front();
    if (entry.event == 0) {
        return -1;
    }

    int target = -1;
    for (std::size_t i = 0; i < targets.player_positions.size() && i < 8; ++i) {
        if (state.spawn.owner_player == static_cast<int>(i)) {
            continue;
        }
        const bool kind_ok = i >= targets.player_kind.size() ||
                             (targets.player_kind[i] != 0 &&
                              targets.player_kind[i] != 7);
        const bool object_ok = i >= targets.player_has_object.size() ||
                               targets.player_has_object[i] != 0;
        if (!kind_ok || !object_ok) {
            continue;
        }
        const double radius = i < targets.player_radii.size()
                                  ? targets.player_radii[i]
                                  : 0.0;
        if (length(targets.player_positions[i] - state.position) <
            static_cast<double>(entry.high) + radius) {
            target = static_cast<int>(i); // the last match wins, as in 0x4752d0
        }
    }
    if (target >= 0) {
        return target;
    }

    const std::size_t count = std::min(targets.thing_positions.size(),
                                       targets.thing_alive.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (targets.thing_alive[i] == 0) {
            continue;
        }
        if (i < targets.thing_flags2.size() &&
            (targets.thing_flags2[i] & 0x20000u) != 0) {
            continue;
        }
        if (i < targets.thing_has_object.size() &&
            targets.thing_has_object[i] == 0) {
            continue;
        }
        if (i < targets.thing_movement_type.size()) {
            const int move = targets.thing_movement_type[i];
            if (move == 0 || move == 6 || move == 7 || move == 8) {
                continue;
            }
        }
        if (state.spawn.owner_thing >= 0 &&
            root_owner(targets, static_cast<int>(i)) ==
                state.spawn.owner_thing) {
            continue;
        }
        const double radius = i < targets.thing_radii.size()
                                  ? targets.thing_radii[i]
                                  : 0.0;
        if (length(targets.thing_positions[i] - state.position) <
            static_cast<double>(entry.high) + radius) {
            target = static_cast<int>(i);
        }
    }
    return target < 0 ? -1 : 8 + target; // fixed player slots 0..7, things 8..
}

ProjectileTickResult make_result(const ProjectileState& state,
                                 ProjectileStatus status) {
    ProjectileTickResult result;
    result.status = status;
    result.position = state.position;
    result.direction = state.direction;
    result.speed = state.speed;
    result.vertical_speed = state.vertical_speed;
    result.hits_left = state.hits_left;
    result.life_left = state.life_left;
    return result;
}

void add_event(ProjectileTickResult& result, std::uint32_t id,
               const Vec3d& position, std::uint32_t extra) {
    if (id != 0) {
        result.events.push_back(ProjectileEvent{id, position, extra});
    }
}

ProjectileTickResult expire(ProjectileState& state) {
    state.life_left = 0;
    state.spent = true;
    ProjectileTickResult result = make_result(state, ProjectileStatus::expired);
    add_event(result, state.spawn.expire_event, state.position,
              event_extra(state.spawn.thing_id));
    return result;
}

/// Resolve the caller-supplied RGM target.  A live target index wins over the
/// point; an index that is not present in `targets` falls back to the point.
bool homing_target(const ProjectileState& state, const ProjectileTargets& targets,
                   Vec3d& out) {
    if (state.target_index >= 0) {
        if (state.target_index < 8) {
            const auto slot = static_cast<std::size_t>(state.target_index);
            if (slot < targets.player_positions.size() &&
                finite(targets.player_positions[slot])) {
                out = targets.player_positions[slot];
                return true;
            }
        } else {
            const auto slot = static_cast<std::size_t>(state.target_index - 8);
            if (slot < targets.thing_positions.size() &&
                finite(targets.thing_positions[slot])) {
                out = targets.thing_positions[slot];
                return true;
            }
        }
    }
    if (state.has_target_point && finite(state.target_point)) {
        out = state.target_point;
        return true;
    }
    return false;
}

/// Rotate `from` toward `to` by at most `max_angle` radians (Rodrigues).
/// The shortest arc is used; an antiparallel pair gets a deterministic
/// perpendicular axis so the result never depends on floating-point noise.
Vec3d turn_toward(Vec3d from, Vec3d to, double max_angle) {
    from = safe_direction(from);
    to = safe_direction(to);
    const double cos_angle = std::clamp(dot(from, to), -1.0, 1.0);
    const double angle = std::acos(cos_angle);
    if (angle <= 1e-12 || max_angle >= angle) {
        return to;
    }
    if (!(max_angle > 0.0)) {
        return from;
    }
    Vec3d axis = cross(from, to);
    if (length(axis) < 1e-12) {
        const Vec3d reference =
            std::abs(from.z) < 0.9 ? Vec3d{0.0, 0.0, 1.0}
                                   : Vec3d{0.0, 1.0, 0.0};
        axis = cross(from, reference);
    }
    axis = safe_direction(axis);
    const double c = std::cos(max_angle);
    const double s = std::sin(max_angle);
    return safe_direction(from * c + cross(axis, from) * s +
                          axis * (dot(axis, from) * (1.0 - c)));
}

/// RGM final (`0x475930`): destroy, never expire, and post +0x1f8 once.
ProjectileTickResult homing_destroy(ProjectileState& state, const Vec3d& position) {
    state.position = position;
    state.spent = true;
    state.dead = true;
    ProjectileTickResult result = make_result(state, ProjectileStatus::destroyed);
    add_event(result, state.spawn.destroy_event, state.position,
              event_extra(state.spawn.thing_id));
    return result;
}

/// One homing tick.  Unlike 0x4752d0 this branch has no gravity, damping,
/// hits counter or proximity table: it turns toward the target (instantly when
/// `homing_turn <= 0`, otherwise by at most `homing_turn * dt` radians), steps
/// `speed * dt` along the new heading and destroys on world contact or when
/// the target is reached.  Without a target it keeps flying straight.
ProjectileTickResult homing_step(ProjectileState& state,
                                 const ProjectileTargets& targets,
                                 const CollisionWorld& world, int dt) {
    const double ticks = static_cast<double>(dt);
    Vec3d target{};
    const bool has_target = homing_target(state, targets, target);
    double distance = 0.0;
    if (has_target) {
        const Vec3d to_target = target - state.position;
        distance = length(to_target);
        const Vec3d desired = safe_direction(to_target);
        state.direction = state.homing_turn > 0.0
                              ? turn_toward(state.direction, desired,
                                            state.homing_turn * ticks)
                              : desired;

        const double travel = std::max(0.0, state.speed) * ticks;
        const bool aligned = distance <= 1e-9 ||
                             (travel > 0.0 &&
                              dot(state.direction, desired) > 0.0);
        if (aligned && (distance <= 1e-9 || distance <= travel)) {
            return homing_destroy(state, target);
        }
    }

    const Vec3d displacement = state.direction * (state.speed * ticks);
    const double travel = length(displacement);
    const double radius = collision_radius(state.spawn);
    if (state.destroy_on_contact && travel > 1e-12 && radius > 0.0 &&
        std::isfinite(travel)) {
        const std::optional<SphereHit> hit =
            world.sphere_cast(state.position, displacement, radius, travel);
        if (hit.has_value()) {
            // 0x475930 advances to the contact point and calls the destroy
            // record (`0x470290`); it does not run the 0x4703a0 expire path.
            return homing_destroy(state, hit->centre);
        }
    }
    state.position = state.position + displacement;
    return make_result(state, ProjectileStatus::flying);
}

} // namespace

ProjectileTickResult projectile_step(ProjectileState& state,
                                     const ProjectileTargets& targets,
                                     const CollisionWorld& world, int dt) {
    if (state.dead) {
        return make_result(state, ProjectileStatus::destroyed);
    }
    if (state.spent) {
        ProjectileTickResult result = make_result(state, ProjectileStatus::expired);
        result.already_spent = true;
        return result;
    }
    if (dt <= 0) {
        return make_result(state, ProjectileStatus::flying);
    }

    // 0x470c70 checks the lifetime before the movement branch.
    if (state.life_left > 0) {
        state.life_left -= dt;
        if (state.life_left < 1) {
            return expire(state);
        }
    }
    if (state.timeout_left > 0) {
        state.timeout_left -= dt;
        if (state.timeout_left < 1) {
            state.timeout_left = 0;
            state.spent = true;
            ProjectileTickResult result =
                make_result(state, ProjectileStatus::expired);
            add_event(result, state.spawn.timeout_event, state.position,
                      event_extra(state.spawn.thing_id));
            return result;
        }
    }

    // RGM (`movement_type == 8`) is a separate movement branch in 0x470c70:
    // it does not consult the proximity table and never expires on a hit.
    if (state.homing) {
        return homing_step(state, targets, world, dt);
    }

    // Proximity is checked at the current position, before any movement.
    const int proximity_target = find_proximity_target(state, targets);
    if (proximity_target >= 0) {
        const ProximityEntry& entry = state.spawn.proximity.front();
        state.last_target = proximity_target;
        state.cooldown += entry.cooldown;
        state.rest_left = state.spawn.rest_ticks;
        ProjectileTickResult result =
            make_result(state, ProjectileStatus::proximity);
        add_event(result, entry.event, state.position,
                  proximity_extra(state.spawn.thing_id, proximity_target));
        return result;
    }

    const Vec3d old_position = state.position;
    const double old_speed = state.speed;
    const double old_vertical = state.vertical_speed;
    const double gravity_scale = state.spawn.gravity *
                                 state.spawn.level_gravity;

    // Semi-implicit Euler: the new vertical speed is used by this step.
    state.vertical_speed += kProjectileGravityScale * gravity_scale * dt;
    Vec3d desired = state.position +
                    state.direction * (state.speed * static_cast<double>(dt));
    desired.z -= state.vertical_speed * static_cast<double>(dt);

    const Vec3d displacement = desired - state.position;
    const double travel = length(displacement);
    std::optional<SphereHit> hit;
    const double radius = collision_radius(state.spawn);
    if (travel > 1e-12 && radius > 0.0 && std::isfinite(travel)) {
        hit = world.sphere_cast(state.position, displacement, radius, travel);
    }

    if (!hit.has_value()) {
        state.position = desired;
        state.rest_left = state.spawn.rest_ticks;
        return make_result(state, ProjectileStatus::flying);
    }

    const Vec3d contact_normal = safe_direction(hit->normal);
    // Leave the contact point by a tiny skin along the surface normal.  The
    // generic sphere_cast reports a touching sphere at t=0 even when it moves
    // away; without the skin a bounced projectile would stick to the surface.
    constexpr double kContactSkin = 1e-4;
    const Vec3d resolved = hit->centre + contact_normal * kContactSkin;

    // A contact that only creeps into the surface cannot become a bounce: the
    // reflected speed would be smaller than one step of gravity, so the very
    // next tick would touch the surface again.  The original still treats that
    // as a bounce, which halves `damping`, spends one hit and (because of the
    // collide gate below) posts nothing -- a projectile "resting" on the floor
    // would silently burn its hits and decay to speed 0.  The contact skin
    // removed the t = 0 contact of a touching sphere; this branch covers the
    // remaining repeated contact.  It is not a hit: push the centre out along
    // the normal, drop the vertical speed, leave `hits_left` and the events
    // untouched, and report a plain flight so the game layer does not blast
    // every tick.
    //
    // Only floors qualify (`kProjectileRestNormalZ`): gravity is what keeps
    // the sphere pressed against them, while a wall or a near-vertical ramp is
    // still an ordinary bounce/slide (QA-3-06: such contacts used to be
    // swallowed by the rest rule whenever `normal.z > 0`).
    const double gravity_step = kProjectileGravityScale * gravity_scale *
                                static_cast<double>(dt) * static_cast<double>(dt);
    const double approach = -dot(displacement, contact_normal); // > 0 into surface
    if (contact_normal.z >= kProjectileRestNormalZ && approach > 0.0 &&
        approach <= gravity_step) {
        state.vertical_speed = 0.0;
        // Keep the surface-parallel remainder of the step.  The sweep already
        // advanced the centre up to the contact point (`hit->t`), so without
        // this a grazing shot would be frozen at the contact point instead of
        // sliding along the floor.
        const double done = travel > 1e-12 ? hit->t / travel : 1.0;
        const Vec3d remaining =
            displacement * std::max(0.0, 1.0 - done);
        const Vec3d tangent =
            remaining - contact_normal * dot(remaining, contact_normal);
        const double slide = length(tangent);
        state.position = resolved;
        if (slide > 1e-9) {
            // Ground friction (clean-room): while the sphere really moves
            // along the floor, one rest tick costs `damping` of its speed, so
            // a nearly horizontal impact cannot slide for ever.  A purely
            // vertical rest (tangent == 0) keeps speed, damping and hits
            // exactly as before.
            state.speed *= std::clamp(state.damping, 0.0, 1.0);
            // The primary cast stopped at the floor, so a wall further along
            // the slide is not in its result; sweep the slide separately
            // instead of teleporting through it.
            std::optional<SphereHit> block;
            if (radius > 0.0 && std::isfinite(slide)) {
                block = world.sphere_cast(resolved, tangent, radius, slide);
            }
            if (block.has_value()) {
                state.position = block->centre +
                                 safe_direction(block->normal) * kContactSkin;
            } else {
                state.position = resolved + tangent;
            }
        }
        if (state.spawn.rest_ticks > 0) {
            // Lazy start: a manual `ProjectileState` need not pre-load
            // `rest_left`; `Projectiles::spawn` does it for the owner path.
            if (state.rest_left <= 0) {
                state.rest_left = state.spawn.rest_ticks;
            }
            state.rest_left -= dt;
            if (state.rest_left < 1) {
                // Clean-room rest timeout: the ordinary expire path (expire
                // event, `life_left = 0`, `spent`) -- not destroy, so the
                // record still has to be removed by the game layer.
                return expire(state);
            }
        }
        return make_result(state, ProjectileStatus::flying);
    }

    // Any real contact restarts the rest window: `rest_ticks` counts
    // consecutive rest ticks only.
    state.rest_left = state.spawn.rest_ticks;

    if (state.hits_left > 0) {
        --state.hits_left;
        state.position = resolved;
        if (state.hits_left == 0) {
            return expire(state);
        }
    }

    // Reflect the movement vector: dir' = inc - 2*(inc·n)*n.
    const Vec3d incoming = desired - old_position;
    const Vec3d normal = contact_normal;
    Vec3d reflected = incoming;
    const double projection = dot(incoming, normal);
    reflected = incoming - normal * (2.0 * projection);
    state.direction = safe_direction(reflected);
    state.vertical_speed = 0.0;

    ProjectileTickResult result = make_result(state, ProjectileStatus::bounced);
    // The original posts the old position, before `pos = resolved`.
    if (static_cast<double>(dt) <= std::abs(old_vertical) + old_speed) {
        add_event(result, state.spawn.collide_event, old_position,
                  event_extra(state.spawn.thing_id));
    }

    if (std::abs(old_speed * state.direction.x) <= 0.04 &&
        std::abs(old_speed * state.direction.y) <= 0.04) {
        state.damping *= 0.5;
    }
    state.speed = old_speed * state.damping;
    state.position = resolved;
    result.position = state.position;
    result.direction = state.direction;
    result.speed = state.speed;
    result.vertical_speed = state.vertical_speed;
    result.hits_left = state.hits_left;
    result.life_left = state.life_left;
    return result;
}

std::size_t post_projectile_events(EventQueue& queue,
                                   const ProjectileTickResult& result) {
    std::size_t posted = 0;
    for (const ProjectileEvent& event : result.events) {
        if (event.id == 0) {
            continue;
        }
        if (queue.post_generate(static_cast<std::int32_t>(event.id), 0,
                                event.position.x, event.position.y,
                                event.position.z, event.extra)) {
            ++posted;
        }
    }
    return posted;
}

ProjectileTickResult projectile_result_of(const ProjectileTickRecord& record) {
    ProjectileTickResult result;
    result.status = record.status;
    result.position = record.position;
    result.direction = record.direction;
    result.speed = record.speed;
    result.vertical_speed = record.vertical_speed;
    result.hits_left = record.hits_left;
    result.life_left = record.life_left;
    result.already_spent = record.already_spent;
    result.events = record.events;
    return result;
}

std::size_t apply_projectile_blast(const Weapon& weapon,
                                   const ProjectileTickResult& result,
                                   bool expired,
                                   const ProjectileBlastContext& context,
                                   const ProjectileBlastTargets& targets,
                                   BlastDamageResult* out) {
    bool trigger = false;
    switch (result.status) {
    case ProjectileStatus::bounced:
        trigger = true;
        break;
    case ProjectileStatus::expired:
        // A state that was already spent replays `expired` on every step; its
        // expiry blast belongs to the first (non-replayed) result only.
        trigger = !result.already_spent;
        break;
    case ProjectileStatus::proximity:
        trigger = context.allow_proximity;
        break;
    case ProjectileStatus::flying:
    case ProjectileStatus::destroyed:
        trigger = false;
        break;
    }
    if (!trigger) {
        if (out != nullptr) {
            *out = BlastDamageResult{};
        }
        return 0;
    }

    BlastDamageResult applied = apply_weapon_blast(
        weapon, expired, result.position, context.underwater,
        context.from_ground, context.deathmatch, context.source_player,
        context.source_thing, targets.player_positions, targets.player_vitals,
        targets.thing_positions, targets.thing_alive, targets.thing_vitals,
        context.power_up);
    const std::size_t hits = applied.hits.size();
    if (out != nullptr) {
        *out = std::move(applied);
    }
    return hits;
}

ProjectileState projectile_spawn_from_thing(const Thing& thing,
                                            const Vec3d& position,
                                            const Vec3d& direction,
                                            double speed_per_tick,
                                            int owner_player,
                                            int owner_thing,
                                            double level_gravity) {
    ProjectileState state;
    state.spawn.thing_id = thing.id;
    state.spawn.owner_player = owner_player;
    state.spawn.owner_thing = owner_thing;
    state.spawn.position = position;
    state.spawn.direction = safe_direction(direction);
    state.spawn.speed = speed_per_tick;
    state.spawn.gravity = thing.gravity;
    state.spawn.damping = thing.damping;
    state.spawn.level_gravity = level_gravity;
    state.spawn.hits_left = thing.hits_left;
    state.spawn.life_ticks = thing.expire_ticks;
    state.spawn.shape = thing.collision_shape;
    state.spawn.radius = thing.radius;
    state.spawn.box = thing.box;
    state.spawn.proximity = thing.proximity;
    state.spawn.collide_event = thing.collide_event;
    state.spawn.expire_event = thing.expire_event;
    state.spawn.destroy_event = thing.destroy_event;
    state.spawn.timeout_event = thing.timeout_event;
    state.spawn.create_event = thing.create_event;
    state.position = position;
    state.direction = safe_direction(direction);
    state.speed = speed_per_tick;
    state.damping = thing.damping;
    state.hits_left = thing.hits_left;
    state.life_left = thing.expire_ticks;
    // 0x475930 is selected by movement_type 8.  The target itself is still
    // the caller's job; until it is supplied the round flies straight.
    state.spawn.homing = thing.movement_type == 8;
    state.homing = state.spawn.homing;
    state.spawn.destroy_on_contact = true;
    state.destroy_on_contact = true;
    return state;
}

std::optional<ProjectileSpawn> projectile_spawn_from_shot(
    const Weapon& weapon, const ShotResult& shot,
    const Thing* projectile_thing, int owner_player, int owner_thing,
    double level_gravity) {
    if (!shot.ok || !shot.projectile) {
        return std::nullopt;
    }
    // ShotResult.speed is units/second; the runtime steps in units/tick.
    double speed_per_second = shot.projectile_speed;
    if (!std::isfinite(speed_per_second) || !(speed_per_second > 0.0)) {
        speed_per_second = weapon.projectile_speed;
    }
    if (!std::isfinite(speed_per_second) || !(speed_per_second > 0.0)) {
        return std::nullopt;
    }

    ProjectileSpawn spawn;
    spawn.owner_player = owner_player;
    spawn.owner_thing = owner_thing;
    spawn.position = shot.origin;
    spawn.direction = safe_direction(shot.direction);
    spawn.speed = speed_per_second / kProjectileTickHz;
    spawn.level_gravity = finite_or(level_gravity, 0.0);
    // Safe defaults: a pure flying point with no events and no lifetime.
    spawn.shape = 1;
    spawn.radius = 0.0;
    spawn.box = {0.0, 0.0, 0.0};
    spawn.hits_left = 0;
    spawn.life_ticks = 0;

    if (projectile_thing != nullptr) {
        const Thing& thing = *projectile_thing;
        spawn.thing_id = thing.id;
        spawn.gravity = finite_or(thing.gravity, 0.0);
        spawn.damping = finite_or(thing.damping, 0.0);
        spawn.shape = thing.collision_shape == 2 ? 2 : 1;
        spawn.radius = std::max(0.0, finite_or(thing.radius, 0.0));
        spawn.box = {std::max(0.0, finite_or(thing.box[0], 0.0)),
                     std::max(0.0, finite_or(thing.box[1], 0.0)),
                     std::max(0.0, finite_or(thing.box[2], 0.0))};
        spawn.proximity = thing.proximity;
        spawn.hits_left = std::max(0, thing.hits_left);
        spawn.life_ticks = std::max(0, thing.expire_ticks);
        spawn.collide_event = thing.collide_event;
        spawn.expire_event = thing.expire_event;
        spawn.destroy_event = thing.destroy_event;
        spawn.timeout_event = thing.timeout_event;
        spawn.create_event = thing.create_event;
        // RGM is launched through ADD_THING type 32; its movement_type is the
        // selector for the separate 0x475930 step.  The target (and an
        // optional turn rate) still comes from the calling layer.
        spawn.homing = thing.movement_type == 8;
    }
    return spawn;
}

bool ai_shot_projectile_spawn(const AiWeaponShot& shot,
                              const Thing* projectile_thing, int owner_player,
                              ProjectileSpawn& out, double level_gravity) {
    ShotResult result;
    result.ok = true;
    result.projectile = true;
    result.origin = shot.origin;
    result.direction = shot.direction;
    result.projectile_speed = shot.speed;

    Weapon weapon;
    // The weapon table has no speed field: prefer the shot (units/second),
    // otherwise take the proven projectile type speed (+0x10 units/tick).
    if (std::isfinite(shot.speed) && shot.speed > 0.0) {
        weapon.projectile_speed = shot.speed;
    } else if (projectile_thing != nullptr &&
               std::isfinite(projectile_thing->speed) &&
               projectile_thing->speed > 0.0) {
        weapon.projectile_speed = projectile_thing->speed * kProjectileTickHz;
    }

    const std::optional<ProjectileSpawn> spawn = projectile_spawn_from_shot(
        weapon, result, projectile_thing, owner_player, shot.thing_id,
        level_gravity);
    if (!spawn.has_value()) {
        return false;
    }
    out = *spawn;
    // Homing targets reuse the same data the AI already resolved.  A thing
    // target has no stable index here, but its aim point does.
    if (shot.target >= 0 && shot.target < kAiMaxPlayers) {
        out.target_index = shot.target;
    }
    if (finite(shot.aim)) {
        out.target_point = shot.aim;
        out.has_target_point = true;
    }
    return true;
}

std::size_t spawn_ai_shots(Ai& ai, Projectiles& projectiles,
                           EventQueue* events, const Thing* projectile_thing,
                           int owner_player, double level_gravity) {
    std::size_t spawned = 0;
    for (const AiWeaponShot& shot : ai.take_weapon_shots()) {
        ProjectileSpawn spawn;
        if (!ai_shot_projectile_spawn(shot, projectile_thing, owner_player,
                                      spawn, level_gravity)) {
            continue;
        }
        if (projectiles.spawn(std::move(spawn), events) >= 0) {
            ++spawned;
        }
    }
    return spawned;
}

int Projectiles::spawn(ProjectileSpawn spawn, EventQueue* events) {
    std::size_t reuse = projectiles_.size();
    if (spawn.thing_id >= 0) {
        for (std::size_t i = 0; i < projectiles_.size(); ++i) {
            if (projectiles_[i].spawn.thing_id != spawn.thing_id) {
                continue;
            }
            if (!projectiles_[i].dead) {
                return -1;
            }
            reuse = i;
            break;
        }
    }
    ProjectileState state;
    state.spawn = std::move(spawn);
    state.spawn.rest_ticks = std::max(0, state.spawn.rest_ticks);
    state.position = state.spawn.position;
    state.direction = safe_direction(state.spawn.direction);
    state.speed = finite_or(state.spawn.speed, 0.0);
    state.damping = finite_or(state.spawn.damping, 0.0);
    state.hits_left = state.spawn.hits_left;
    state.life_left = state.spawn.life_ticks;
    state.rest_left = state.spawn.rest_ticks;
    state.homing = state.spawn.homing;
    state.target_index = state.spawn.target_index;
    state.target_point = state.spawn.target_point;
    state.has_target_point = state.spawn.has_target_point;
    state.destroy_on_contact = state.spawn.destroy_on_contact;
    state.homing_turn = state.spawn.homing_turn;
    if (events != nullptr && state.spawn.create_event != 0) {
        events->post_generate(
            static_cast<std::int32_t>(state.spawn.create_event), 0,
            state.position.x, state.position.y, state.position.z,
            event_extra(state.spawn.thing_id));
    }
    if (reuse < projectiles_.size()) {
        projectiles_[reuse] = std::move(state);
        return static_cast<int>(reuse);
    }
    projectiles_.push_back(std::move(state));
    return static_cast<int>(projectiles_.size() - 1);
}

std::vector<ProjectileTickRecord> Projectiles::tick_results(
    int ticks, const ProjectileTargets& targets, const CollisionWorld& world,
    EventQueue* events) {
    std::vector<ProjectileTickRecord> records;
    records.reserve(projectiles_.size());
    for (std::size_t i = 0; i < projectiles_.size(); ++i) {
        ProjectileState& projectile = projectiles_[i];
        const bool was_dead = projectile.dead;
        // Every stored record is stepped, including spent/dead ones: the step
        // is a pure replay there (`expired`/`destroyed`, no events), which
        // keeps one record per slot and makes a repeated call idempotent for
        // the queue.  Event posting stays inside this single loop, so a record
        // can never be posted twice by the owner path.
        ProjectileTickResult result =
            projectile_step(projectile, targets, world, ticks);

        ProjectileTickRecord record;
        record.thing_id = projectile.spawn.thing_id;
        record.index = static_cast<int>(i);
        record.status = result.status;
        record.position = result.position;
        record.direction = result.direction;
        record.speed = result.speed;
        record.vertical_speed = result.vertical_speed;
        record.hits_left = result.hits_left;
        record.life_left = result.life_left;
        // A dead record was already removed (and its destroy event posted by
        // `remove()`), so its `destroyed` status is a replay too.
        record.already_spent = result.already_spent || was_dead;
        record.spent = projectile.spent;
        record.dead = projectile.dead;
        record.collide_event = projectile.spawn.collide_event;
        record.expire_event = projectile.spawn.expire_event;
        record.destroy_event = projectile.spawn.destroy_event;
        if (events != nullptr) {
            record.posted_events = post_projectile_events(*events, result);
        }
        record.events = std::move(result.events);
        records.push_back(std::move(record));
    }
    return records;
}

void Projectiles::tick(int ticks, const ProjectileTargets& targets,
                       const CollisionWorld& world, EventQueue* events) {
    // One implementation, two views: `tick()` keeps the old signature for the
    // callers that only need the side effects.
    (void)tick_results(ticks, targets, world, events);
}

ProjectileState* Projectiles::by_thing_id(int thing_id) {
    for (ProjectileState& projectile : projectiles_) {
        if (projectile.spawn.thing_id == thing_id) {
            return &projectile;
        }
    }
    return nullptr;
}

const ProjectileState* Projectiles::by_thing_id(int thing_id) const {
    for (const ProjectileState& projectile : projectiles_) {
        if (projectile.spawn.thing_id == thing_id) {
            return &projectile;
        }
    }
    return nullptr;
}

bool Projectiles::remove(int thing_id, EventQueue* events) {
    ProjectileState* projectile = by_thing_id(thing_id);
    if (projectile == nullptr || projectile->dead) {
        return false;
    }
    if (events != nullptr && projectile->spawn.destroy_event != 0) {
        events->post_generate(
            static_cast<std::int32_t>(projectile->spawn.destroy_event), 0,
            projectile->position.x, projectile->position.y,
            projectile->position.z,
            event_extra(projectile->spawn.thing_id));
    }
    projectile->dead = true;
    projectile->spent = true;
    return true;
}

} // namespace gm
