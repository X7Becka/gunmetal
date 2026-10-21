#include "world/ai_move.h"

#include <algorithm>
#include <cmath>

#include "world/ai.h"

namespace gm {

AiSeekResult ai_seek_step(const Vec3d& from, const Vec3d& target,
                          const CollisionWorld& world, const AiMoveParams& params,
                          double dt) {
    AiSeekResult result;
    result.position = from;
    if (!std::isfinite(from.x) || !std::isfinite(from.y) ||
        !std::isfinite(from.z) || !std::isfinite(target.x) ||
        !std::isfinite(target.y) || !std::isfinite(target.z) ||
        !std::isfinite(dt) || !(dt > 0.0)) {
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
    if (!(travel > 0.0)) {
        return result;
    }

    Vec3d desired = from + to_target * (travel / distance);
    const Vec3d displacement = desired - from;
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
    result.arrived = octagonal_distance(desired, target) <= params.arrive_radius;
    return result;
}

} // namespace gm
