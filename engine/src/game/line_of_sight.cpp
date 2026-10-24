#include "game/line_of_sight.h"

#include <cmath>
#include <utility>

namespace gm {

namespace {

void set_error(std::string* error, const char* text) {
    if (error != nullptr) {
        *error = text;
    }
}

} // namespace

LosResult line_of_sight(const CollisionWorld& world, Vec3d eye,
                        const LosTarget& target, const CollideFilter& filter,
                        std::string* error) {
    LosResult result;
    result.id = target.id;
    if (error != nullptr) {
        error->clear();
    }

    // Validation first: a malformed request is a refusal, never a "visible".
    if (!finite(eye)) {
        set_error(error, "line_of_sight: eye is not finite");
        return result;
    }
    if (!finite(target.center)) {
        set_error(error, "line_of_sight: target center is not finite");
        return result;
    }
    if (!std::isfinite(target.radius) || target.radius < 0.0) {
        set_error(error, "line_of_sight: target radius is negative or not finite");
        return result;
    }
    if (world.triangle_count() == 0) {
        set_error(error, "line_of_sight: collision world has no static triangles");
        return result;
    }

    const Vec3d delta = target.center - eye;
    const double distance = length(delta);
    result.distance = distance;

    // A target at the eye, or one whose sphere reaches it, has no room for a
    // surface in between.
    const double limit = distance - target.radius;
    if (!(limit > 0.0)) {
        result.visible = true;
        return result;
    }

    // One central trace, stopped at the near side of the target's sphere.
    // `raycast` accepts `t <= limit`; the strict `t < limit` below turns the
    // documented "closer than distance - radius" rule into code, so geometry
    // exactly at the bounding sphere (the target's own near face) does not
    // block, and nothing behind the limit can be reported at all.
    const std::optional<RayHit> hit = world.raycast(eye, delta, limit, filter);
    if (!hit.has_value() || !(hit->t < limit)) {
        result.visible = true;
        return result;
    }

    result.visible = false;
    result.triangle = hit->triangle;
    result.triangle_index = hit->triangle_index;
    result.dynamic = hit->dynamic;
    result.blocker = kLosWorldBlocker;
    result.blocker_distance = hit->t;
    return result;
}

std::vector<LosResult> line_of_sight_all(const CollisionWorld& world, Vec3d eye,
                                         std::span<const LosTarget> targets,
                                         const CollideFilter& filter,
                                         std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    std::vector<LosResult> results;
    results.reserve(targets.size());
    for (const LosTarget& target : targets) {
        std::string one;
        results.push_back(line_of_sight(world, eye, target, filter, &one));
        if (error != nullptr && error->empty() && !one.empty()) {
            *error = std::move(one);
        }
    }
    return results;
}

} // namespace gm
