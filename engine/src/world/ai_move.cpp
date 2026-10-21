#include "world/ai_move.h"

#include <algorithm>
#include <cmath>

#include "world/ai.h"

namespace gm {
namespace {

bool finite_vec(const Vec3d& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// `travel` units along a unit `direction`, clamped by the swept-sphere query.
/// Shared by the seek and the retreat step so both honour the same collision
/// rule (`0x472120` / `0x43823c` in the original).
AiSeekResult swept_step(const Vec3d& from, const Vec3d& direction, double travel,
                        const CollisionWorld& world, const AiMoveParams& params) {
    AiSeekResult result;
    result.position = from;
    if (!(travel > 0.0)) {
        return result;
    }

    const Vec3d displacement = direction * travel;
    Vec3d desired = from + displacement;
    if (params.radius > 0.0) {
        const auto hit = world.sphere_cast(from, displacement, params.radius,
                                           length(displacement));
        if (hit.has_value()) {
            desired = hit->centre;
            result.blocked = true;
        }
    }

    result.position = desired;
    result.delta = desired - from;
    return result;
}

} // namespace

AiSeekResult ai_seek_step(const Vec3d& from, const Vec3d& target,
                          const CollisionWorld& world, const AiMoveParams& params,
                          double dt) {
    AiSeekResult result;
    result.position = from;
    if (!finite_vec(from) || !finite_vec(target) || !std::isfinite(dt) ||
        !(dt > 0.0)) {
        return result;
    }

    if (octagonal_distance(from, target) <= params.arrive_radius) {
        result.arrived = true;
        return result;
    }

    const Vec3d to_target = target - from;
    const double distance = length(to_target);
    if (!(distance > 1e-12)) {
        result.arrived = true;
        return result;
    }

    const double speed =
        std::max(0.0, params.speed_per_tick) * kAiTickHz * dt;
    const double travel = std::min(distance, speed);
    result = swept_step(from, to_target * (1.0 / distance), travel, world, params);
    result.arrived = octagonal_distance(result.position, target) <= params.arrive_radius;
    return result;
}

AiSeekResult ai_retreat_step(const Vec3d& from, const Vec3d& threat,
                             const CollisionWorld& world,
                             const AiMoveParams& params, double dt) {
    AiSeekResult result;
    result.position = from;
    if (!finite_vec(from) || !finite_vec(threat) || !std::isfinite(dt) ||
        !(dt > 0.0)) {
        return result;
    }

    // Planar away vector: the scaffold has no floor/gravity layer, so a
    // vertical offset must not push the body through the ground.
    Vec3d away = from - threat;
    away.z = 0.0;
    const double distance = length(away);
    if (!(distance > 1e-12)) {
        return result;
    }

    const double speed =
        std::max(0.0, params.speed_per_tick) * kAiTickHz * dt;
    return swept_step(from, away * (1.0 / distance), speed, world, params);
}

} // namespace gm
