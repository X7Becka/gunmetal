#include "render/collide_overlap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gm {
namespace {

/// World-unit slack of the inclusive sphere test.  Coordinates on the shipped
/// levels stay below 1e4, where a double's ulp is ~2e-12, so 1e-9 keeps a point
/// that lies exactly on a face inside a radius-0 sphere without widening the
/// query in any measurable way.
constexpr double kOverlapSphereEps = 1e-9;

double axis_of(Vec3d v, int axis) {
    switch (axis) {
    case 0:
        return v.x;
    case 1:
        return v.y;
    default:
        return v.z;
    }
}

Vec3d unit_axis(int axis) {
    switch (axis) {
    case 0:
        return {1.0, 0.0, 0.0};
    case 1:
        return {0.0, 1.0, 0.0};
    default:
        return {0.0, 0.0, 1.0};
    }
}

void set_error(const char* who, const char* what, std::string* error) {
    if (error == nullptr) {
        return;
    }
    error->assign(who);
    error->append(": ");
    error->append(what);
}

bool validate_world(const CollisionWorld& world, const char* who, std::string* error) {
    if (world.empty()) {
        set_error(who, "empty world: nothing to query", error);
        return false;
    }
    return true;
}

/// Refuse what `Aabb` cannot represent: a NaN/inf corner or `min > max`.
/// A degenerate box (`min == max` on an axis) is valid on purpose.
bool validate_box(const Aabb& box, const char* who, std::string* error) {
    if (!finite(box.lo) || !finite(box.hi)) {
        set_error(who, "box is not finite: min/max must be finite", error);
        return false;
    }
    if (!box.valid()) {
        set_error(who, "box is empty: min > max on some axis", error);
        return false;
    }
    return true;
}

/// Walk the static triangles and hand the ones that reach the narrow phase to
/// `narrow`.  `out == nullptr` means "stop at the first match".
///
/// Broad phase: a sector is tested once per run of its triangles (the array is
/// built sector-major, so a sector is one contiguous range), then the triangle
/// AABB.  Suppression is the world's own mask asked through the public
/// `is_suppressed()`, and it is only asked when the world has any pair, so a
/// plain world pays nothing for it.
template <typename Narrow>
std::size_t collect(const CollisionWorld& world, const Aabb& broad, const CollideFilter& filter,
                    std::vector<std::uint32_t>* out, Narrow&& narrow) {
    const std::vector<Triangle>& triangles = world.triangles();
    const bool check_suppressed = world.suppressed_count() != 0;
    std::uint32_t sector = std::numeric_limits<std::uint32_t>::max();
    bool sector_hit = false;
    std::size_t found = 0;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const Triangle& triangle = triangles[i];
        if (triangle.sector != sector) {
            sector = triangle.sector;
            sector_hit = overlaps(world.sector_bounds(sector), broad);
        }
        if (!sector_hit) {
            continue;
        }
        if (check_suppressed && world.is_suppressed(triangle.sector, triangle.object)) {
            continue; // stale rest pose of a moving object
        }
        if (!filter.allows(triangle)) {
            continue;
        }
        if (!overlaps(triangle.bounds, broad)) {
            continue;
        }
        if (!narrow(triangle)) {
            continue;
        }
        if (out == nullptr) {
            return 1;
        }
        out->push_back(static_cast<std::uint32_t>(i));
        ++found;
    }
    return found;
}

} // namespace

bool triangle_intersects_box(const Triangle& triangle, const Aabb& box) {
    // Work in the box frame.  The centre and the half extent are computed as
    // half-sums/differences: `lo + hi` can overflow for a box near the double
    // cap, `lo * 0.5 + hi * 0.5` cannot.
    const Vec3d centre{box.lo.x * 0.5 + box.hi.x * 0.5, box.lo.y * 0.5 + box.hi.y * 0.5,
                       box.lo.z * 0.5 + box.hi.z * 0.5};
    const Vec3d half{box.hi.x * 0.5 - box.lo.x * 0.5, box.hi.y * 0.5 - box.lo.y * 0.5,
                     box.hi.z * 0.5 - box.lo.z * 0.5};
    const Vec3d v0 = triangle.a - centre;
    const Vec3d v1 = triangle.b - centre;
    const Vec3d v2 = triangle.c - centre;

    // 1. The three box face normals: every projection on an axis lies outside
    //    [-half, half] -> a face plane separates the shapes.
    for (int axis = 0; axis < 3; ++axis) {
        const double p0 = axis_of(v0, axis);
        const double p1 = axis_of(v1, axis);
        const double p2 = axis_of(v2, axis);
        const double h = axis_of(half, axis);
        if (std::min(std::min(p0, p1), p2) > h || std::max(std::max(p0, p1), p2) < -h) {
            return false;
        }
    }

    // 2. The triangle plane.  An unnormalised normal is enough: the support
    //    radius of the box on `n` is |n.x|*hx + |n.y|*hy + |n.z|*hz.  A
    //    zero-area triangle gives n == 0, so the plane axis never separates --
    //    correct, the axes below still describe its edges.
    const Vec3d normal = cross(v1 - v0, v2 - v0);
    const double plane_distance = dot(normal, v0);
    const double plane_radius = std::fabs(normal.x) * half.x + std::fabs(normal.y) * half.y +
                                std::fabs(normal.z) * half.z;
    if (std::fabs(plane_distance) > plane_radius) {
        return false;
    }

    // 3. The nine cross products of a box axis and a triangle edge.  A zero
    //    axis means the edge is parallel to the box axis it is crossed with;
    //    that direction adds no information, so it is skipped.
    const Vec3d edges[3] = {v1 - v0, v2 - v1, v0 - v2};
    for (int edge = 0; edge < 3; ++edge) {
        for (int axis = 0; axis < 3; ++axis) {
            const Vec3d candidate = cross(unit_axis(axis), edges[edge]);
            if (candidate.x == 0.0 && candidate.y == 0.0 && candidate.z == 0.0) {
                continue;
            }
            const double p0 = dot(candidate, v0);
            const double p1 = dot(candidate, v1);
            const double p2 = dot(candidate, v2);
            const double radius = std::fabs(candidate.x) * half.x +
                                  std::fabs(candidate.y) * half.y +
                                  std::fabs(candidate.z) * half.z;
            if (std::min(std::min(p0, p1), p2) > radius ||
                std::max(std::max(p0, p1), p2) < -radius) {
                return false;
            }
        }
    }
    return true;
}

bool triangle_within_sphere(const Triangle& triangle, Vec3d centre, double radius) {
    const Vec3d closest = closest_point_on_triangle(centre, triangle);
    return length(centre - closest) <= radius + kOverlapSphereEps;
}

bool overlap_triangles(const CollisionWorld& world, const Aabb& box,
                       std::vector<std::uint32_t>& out, const CollideFilter& filter,
                       std::string* error) {
    out.clear();
    if (error != nullptr) {
        error->clear();
    }
    if (!validate_world(world, "overlap_triangles", error) ||
        !validate_box(box, "overlap_triangles", error)) {
        return false;
    }
    collect(world, box, filter, &out,
            [&box](const Triangle& triangle) { return triangle_intersects_box(triangle, box); });
    return true;
}

std::vector<std::uint32_t> overlap_triangles(const CollisionWorld& world, const Aabb& box,
                                             const CollideFilter& filter) {
    std::vector<std::uint32_t> out;
    overlap_triangles(world, box, out, filter);
    return out;
}

bool overlaps_static(const CollisionWorld& world, const Aabb& box, const CollideFilter& filter,
                     std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!validate_world(world, "overlaps_static", error) ||
        !validate_box(box, "overlaps_static", error)) {
        return false;
    }
    return collect(world, box, filter, nullptr,
                   [&box](const Triangle& triangle) {
                       return triangle_intersects_box(triangle, box);
                   }) != 0;
}

bool overlap_sphere(const CollisionWorld& world, Vec3d centre, double radius,
                    std::vector<std::uint32_t>& out, const CollideFilter& filter,
                    std::string* error) {
    out.clear();
    if (error != nullptr) {
        error->clear();
    }
    if (!validate_world(world, "overlap_sphere", error)) {
        return false;
    }
    if (!finite(centre)) {
        set_error("overlap_sphere", "sphere centre is not finite", error);
        return false;
    }
    if (!std::isfinite(radius)) {
        set_error("overlap_sphere", "sphere radius is not finite", error);
        return false;
    }
    if (radius < 0.0) {
        set_error("overlap_sphere", "sphere radius is negative", error);
        return false;
    }
    // Broad phase = the sphere AABB.  It may overflow to +-inf only for a
    // radius near the double cap; comparisons against inf stay well defined and
    // every static triangle is then a candidate, which is what such a sphere
    // means.  The narrow phase uses the centre and the radius, not this box.
    Aabb broad;
    broad.expand(centre);
    broad.grow(radius);
    collect(world, broad, filter, &out, [centre, radius](const Triangle& triangle) {
        return triangle_within_sphere(triangle, centre, radius);
    });
    return true;
}

std::vector<std::uint32_t> overlap_sphere(const CollisionWorld& world, Vec3d centre,
                                          double radius, const CollideFilter& filter) {
    std::vector<std::uint32_t> out;
    overlap_sphere(world, centre, radius, out, filter);
    return out;
}

} // namespace gm
