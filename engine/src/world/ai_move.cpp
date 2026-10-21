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

AiPathStep path_advance(const AiPathState& state,
                        std::span<const Vec3d> waypoints, AiPathMode mode) {
    AiPathStep step;
    step.state = state;

    const std::size_t clamped =
        std::min(waypoints.size(), static_cast<std::size_t>(kAiPathMaxPoints));
    const int count = static_cast<int>(clamped);

    // The original posts `0x4703a0` once and the thing is gone; keep the
    // terminal flag so repeated calls stay dead rather than restarting.
    if (state.expired) {
        step.index =
            count > 0 ? std::clamp(state.index, 0, count - 1) : -1;
        if (step.index >= 0) {
            step.point = waypoints[static_cast<std::size_t>(step.index)];
        }
        // This call did not trigger a new expiry; the terminal flag lives in
        // step.state.expired.  Returning true here would make a future caller
        // post `0x4703a0` on every tick.
        step.expired = false;
        return step;
    }

    // `path_count < 1`: only pathexpire dies (0x475f10, empty-list branch).
    if (count < 1) {
        step.state.index = -1;
        step.expired = (mode == AiPathMode::expire);
        step.state.expired = step.expired;
        return step;
    }

    if (state.waiting) {
        if (state.index >= 0 && state.index < count) {
            step.index = state.index;
            step.point = waypoints[static_cast<std::size_t>(state.index)];
            step.waiting = true;
            return step;
        }
        // Pending flag without a valid index (corrupt state): drop it and
        // select from the start below.
        step.state.waiting = false;
    }

    // Broken index guard: the original rejects i < 0 or 0x1f < i and reselects
    // from the start instead of reading outside the location list.
    int index = state.index;
    if (index < -1 || index > count - 1) {
        index = -1;
    }

    if (mode == AiPathMode::path && index == count - 1) {
        // Standing on the last point of a `path`: no new target, no event.
        step.index = index;
        step.point = waypoints[static_cast<std::size_t>(index)];
        step.arrived = true;
        step.state.index = index;
        step.state.waiting = false;
        return step;
    }
    if (mode == AiPathMode::expire && index == count - 1) {
        // The last point was reached; the original posts 0x4703a0 here.
        step.index = index;
        step.point = waypoints[static_cast<std::size_t>(index)];
        step.expired = true;
        step.state.index = index;
        step.state.waiting = false;
        step.state.expired = true;
        return step;
    }

    int next = index + 1;
    if (next >= count) {
        next = 0; // `loop`; `path`/`expire` were handled above
    }

    step.index = next;
    step.point = waypoints[static_cast<std::size_t>(next)];
    step.selected = true;
    step.waiting = true;
    step.state.index = next;
    step.state.waiting = true;
    step.state.expired = false;
    return step;
}

} // namespace gm
