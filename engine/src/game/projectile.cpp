#include "game/projectile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "game/events.h"
#include "game/weapons.h"
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

} // namespace

ProjectileTickResult projectile_step(ProjectileState& state,
                                     const ProjectileTargets& targets,
                                     const CollisionWorld& world, int dt) {
    if (state.dead) {
        return make_result(state, ProjectileStatus::destroyed);
    }
    if (state.spent) {
        return make_result(state, ProjectileStatus::expired);
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

    // Proximity is checked at the current position, before any movement.
    const int proximity_target = find_proximity_target(state, targets);
    if (proximity_target >= 0) {
        const ProximityEntry& entry = state.spawn.proximity.front();
        state.last_target = proximity_target;
        state.cooldown += entry.cooldown;
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
    // the normal, drop the vertical speed, leave `hits_left`, `damping` and the
    // events untouched, and report a plain flight so the game layer does not
    // blast every tick.  Only floors qualify: gravity is what keeps the sphere
    // pressed against them (a wall or a ceiling is still an ordinary bounce).
    const double gravity_step = kProjectileGravityScale * gravity_scale *
                                static_cast<double>(dt) * static_cast<double>(dt);
    const double approach = -dot(displacement, contact_normal); // > 0 into surface
    if (contact_normal.z > 0.0 && approach > 0.0 &&
        approach <= gravity_step) {
        state.vertical_speed = 0.0;
        state.position = resolved;
        return make_result(state, ProjectileStatus::flying);
    }

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
    }
    return spawn;
}

int Projectiles::spawn(ProjectileSpawn spawn, EventQueue* events) {
    if (spawn.thing_id >= 0 && by_thing_id(spawn.thing_id) != nullptr) {
        return -1;
    }
    ProjectileState state;
    state.spawn = std::move(spawn);
    state.position = state.spawn.position;
    state.direction = safe_direction(state.spawn.direction);
    state.speed = finite_or(state.spawn.speed, 0.0);
    state.damping = finite_or(state.spawn.damping, 0.0);
    state.hits_left = state.spawn.hits_left;
    state.life_left = state.spawn.life_ticks;
    if (events != nullptr && state.spawn.create_event != 0) {
        events->post_generate(
            static_cast<std::int32_t>(state.spawn.create_event), 0,
            state.position.x, state.position.y, state.position.z,
            event_extra(state.spawn.thing_id));
    }
    projectiles_.push_back(std::move(state));
    return static_cast<int>(projectiles_.size() - 1);
}

void Projectiles::tick(int ticks, const ProjectileTargets& targets,
                       const CollisionWorld& world, EventQueue* events) {
    for (ProjectileState& projectile : projectiles_) {
        if (projectile.dead || projectile.spent) {
            continue;
        }
        const ProjectileTickResult result =
            projectile_step(projectile, targets, world, ticks);
        if (events != nullptr) {
            post_projectile_events(*events, result);
        }
    }
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
