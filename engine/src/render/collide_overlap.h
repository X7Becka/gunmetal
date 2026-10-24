#pragma once

// Volume queries against the *static* triangles of a `CollisionWorld`:
// "which triangles does this box / sphere touch?".
//
// Why a separate module: `CollisionWorld` answers ray and sphere *casts* plus
// `overlap_aabb`, whose narrow phase is the triangle AABB, not the triangle
// itself (docs/COLLIDE.md §5).  A volume query needs the exact test, and the
// exact test is different geometry (box/triangle SAT, point-to-triangle
// distance) that does not belong in the cast code.  `CollisionWorld` is
// therefore untouched: this module reads `triangles()`, `sector_bounds()`,
// `is_suppressed()` and a `CollideFilter`, and writes only its own output.
//
// Model and accuracy (docs/COLLIDE_OVERLAP.md):
//   * double precision, world units, the engine world frame of `Triangle`;
//   * the box type is `gm::Aabb` from `render/collide.h` -- the one box every
//     other query already uses, with fields `lo`/`hi` (the task sheet calls it
//     `Aabb { min, max }`; `aabb_of(min, max)` builds one from inclusive
//     corners).  A box with `lo == hi` on an axis is a valid degenerate box;
//   * broad phase: sector AABB (triangles are stored sector-major, so the
//     sector test runs once per run of triangles), then the triangle AABB, then
//     the exact test.  `CollideGrid` is *not* reused: it indexes triangles for
//     ray walks only and exposes no box traversal, so reusing it would mean a
//     second traversal with no proven gain (docs/COLLIDE_OVERLAP.md §4);
//   * narrow phase, box: 13-axis separating-axis test (3 box axes, the
//     triangle plane, 9 edge/axis cross products), inclusive -- a triangle that
//     only touches a face, edge or corner counts as overlapping, matching
//     `Aabb::contains`/`overlaps()` where boundaries are inside;
//   * narrow phase, sphere: exact distance from the centre to the triangle
//     (`closest_point_on_triangle`, the primitive the casts already use) with a
//     `1e-9` world-unit slack, so a point exactly on a face is inside a
//     radius-0 sphere;
//   * only *static* triangles are reported.  The dynamic set
//     (`CollisionWorld::set_dynamic`) is a separate triangle list with its own
//     indices; a caller that needs moving doors asks `MovingCollider` directly
//     (docs/MOVING_COLLIDERS.md).  Static triangles of a suppressed
//     `(sector, object)` pair are skipped, exactly as in `raycast`, because the
//     world already declared them stale;
//   * no object-vs-object test, no penetration depth, no push-out and no
//     swept query: a volume query reports *what* is touched, not how to resolve
//     it; `sphere_cast` remains the sweep (docs/COLLIDE.md §4.4);
//   * no allocation except the caller-owned output vector.  A refused query
//     clears the output first, so no stale indices survive an error.

#include <cstdint>
#include <string>
#include <vector>

#include "render/collide.h" // Vec3d, Aabb, Triangle, CollideFilter

namespace gm {

/// Box from inclusive corners.  Nothing is validated here: the query functions
/// refuse a non-finite box and `min > max` with an error line.
inline Aabb aabb_of(Vec3d min, Vec3d max) {
    return Aabb{min, max};
}

/// Exact static triangle / box overlap: 13-axis SAT with inclusive boundaries
/// (touching counts).  Degenerate triangles are handled by skipping the axes
/// that collapse to zero, so a zero-area triangle is treated as its edges.
bool triangle_intersects_box(const Triangle& triangle, const Aabb& box);

/// True when the triangle is within `radius` of `centre`, measured as the
/// distance from the centre to the closest point of the triangle surface (the
/// triangle is a zero-thickness sheet, as everywhere else in this module).
/// Inclusive with a `1e-9` world-unit slack.  `radius` must be finite and
/// non-negative; the query functions validate that.
bool triangle_within_sphere(const Triangle& triangle, Vec3d centre, double radius);

/// Static triangles that intersect `box`, as indices into `world.triangles()`,
/// in ascending build order (sector-major, then polygon order).  `out` is
/// cleared first and reused otherwise; the query allocates nothing else.
///
/// Returns false (and clears `out`) with a one-line `error` for an empty world,
/// a non-finite box or `min > max` on any axis.  Returns true with an empty
/// `out` when nothing is touched.
bool overlap_triangles(const CollisionWorld& world, const Aabb& box,
                       std::vector<std::uint32_t>& out, const CollideFilter& filter = {},
                       std::string* error = nullptr);

/// `overlap_triangles` into a fresh vector; the same refusal semantics, an
/// error is simply reported as an empty result.  Use the overload above when
/// the reason matters or when the vector is reused.
std::vector<std::uint32_t> overlap_triangles(const CollisionWorld& world, const Aabb& box,
                                             const CollideFilter& filter = {});

/// True when at least one static triangle passes `filter` and intersects `box`.
/// Answers exactly like `!overlap_triangles(...).empty()` but stops at the first
/// hit and allocates nothing.  Returns false on a refused query as well; only
/// `error` distinguishes a refusal from an empty box.
bool overlaps_static(const CollisionWorld& world, const Aabb& box,
                     const CollideFilter& filter = {}, std::string* error = nullptr);

/// Static triangles within `radius` of `centre` (exact surface distance,
/// `1e-9` slack), as indices into `world.triangles()`, ascending.  A
/// radius-0 query is the set of triangles the point lies on.  The broad phase
/// is the AABB of the sphere, so a triangle whose AABB misses it is never
/// distance-tested.
///
/// Returns false (and clears `out`) with a one-line `error` for an empty world,
/// a non-finite `centre`/`radius` or a negative radius.
bool overlap_sphere(const CollisionWorld& world, Vec3d centre, double radius,
                    std::vector<std::uint32_t>& out, const CollideFilter& filter = {},
                    std::string* error = nullptr);

/// `overlap_sphere` into a fresh vector; see the box overload for the error
/// semantics.
std::vector<std::uint32_t> overlap_sphere(const CollisionWorld& world, Vec3d centre,
                                          double radius, const CollideFilter& filter = {});

} // namespace gm
