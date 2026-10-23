#pragma once

// Collision primitives over `.sbw` level geometry: triangles in the engine
// world frame (`origin + basis * local`), a sector-AABB broad phase, ray and
// swept-sphere casts, a floor query and an AABB overlap query.
//
// Conventions (see docs/COLLIDE.md for the numbers and the proof):
//   * double precision throughout; results are in engine world units;
//   * every triangle carries two normals: `normal` from the vertex winding
//     (used by the renderer and by callers that care about the triangle soup)
//     and `data_normal`, the normal stored in the `.sbw` polygon.  In the
//     shipped levels the two disagree for most horizontal faces: the index
//     order is opposite to the stored normal, so the winding alone classifies
//     floors as ceilings and vice versa;
//   * surfaces are classified by `data_normal` when it is non-zero and by the
//     winding `normal` otherwise: floor when `n.z >= min_normal_z`, ceiling
//     when `n.z <= -min_normal_z`, wall otherwise (the 0.8 threshold used by
//     tools/re/spawn.py);
//   * collision is two-sided regardless of the winding: a ray or a sphere
//     reaches a surface from either side, so walls hold from the inside and
//     from the outside.  The reported normal always faces the incoming query
//     so a caller can slide along a surface without knowing its winding;
//   * `raycast` returns the nearest hit with `t >= 1e-9`, so a ray leaving a
//     surface it starts on does not hit that surface again;
//   * queries do not allocate: results are values/pointers into the world,
//     `overlap_aabb` writes into a caller-owned vector.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "render/render.h" // Vec3d

namespace gm {

// --- double-precision vector helpers (same shape as core/math.h) -----------

constexpr Vec3d operator+(Vec3d a, Vec3d b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

constexpr Vec3d operator-(Vec3d a, Vec3d b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

constexpr Vec3d operator-(Vec3d v) {
    return {-v.x, -v.y, -v.z};
}

constexpr Vec3d operator*(Vec3d v, double s) {
    return {v.x * s, v.y * s, v.z * s};
}

constexpr Vec3d operator*(double s, Vec3d v) {
    return v * s;
}

constexpr double dot(Vec3d a, Vec3d b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3d cross(Vec3d a, Vec3d b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline double length(Vec3d v) {
    return std::sqrt(dot(v, v));
}

inline Vec3d normalized(Vec3d v) {
    const double len = length(v);
    return len > 0.0 ? v * (1.0 / len) : Vec3d{};
}

inline bool finite(Vec3d v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// "Far enough" instead of infinity: keeps slab tests free of inf * 0.
inline constexpr double kCollideInfinity = 1e30;

/// Smallest accepted ray parameter: a ray starting exactly on a triangle does
/// not report that triangle.
inline constexpr double kCollideRayEps = 1e-9;

/// Largest accepted world coordinate; anything beyond is rejected by
/// `CollisionWorld::build` as corrupt data.
inline constexpr double kCollideMaxCoordinate = 1e9;

/// Axis-aligned box.  Empty when `lo > hi` on any axis (the default state),
/// `expand` grows it, `contains` treats boundaries as inside.
struct Aabb {
    Vec3d lo{std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::infinity()};
    Vec3d hi{-std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity()};

    bool valid() const { return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z; }

    void expand(Vec3d p) {
        lo.x = p.x < lo.x ? p.x : lo.x;
        lo.y = p.y < lo.y ? p.y : lo.y;
        lo.z = p.z < lo.z ? p.z : lo.z;
        hi.x = p.x > hi.x ? p.x : hi.x;
        hi.y = p.y > hi.y ? p.y : hi.y;
        hi.z = p.z > hi.z ? p.z : hi.z;
    }

    void expand(const Aabb& other) {
        if (!other.valid()) {
            return;
        }
        expand(other.lo);
        expand(other.hi);
    }

    void grow(double amount) {
        lo = lo - Vec3d{amount, amount, amount};
        hi = hi + Vec3d{amount, amount, amount};
    }

    bool contains(Vec3d p) const {
        return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y &&
               p.z >= lo.z && p.z <= hi.z;
    }

    Vec3d centre() const { return (lo + hi) * 0.5; }
    Vec3d size() const { return hi - lo; }
};

inline bool overlaps(const Aabb& a, const Aabb& b) {
    return a.lo.x <= b.hi.x && b.lo.x <= a.hi.x && a.lo.y <= b.hi.y &&
           b.lo.y <= a.hi.y && a.lo.z <= b.hi.z && b.lo.z <= a.hi.z;
}

/// One triangle of the level in the engine world frame.
struct Triangle {
    Vec3d a;
    Vec3d b;
    Vec3d c;
    /// Unit geometric normal from the CCW winding: cross(b-a, c-a).  Used by
    /// the renderer; the shipped `.sbw` winding is often opposite to the
    /// stored polygon normal (docs/COLLIDE.md §1.2).
    Vec3d normal;
    /// Unit normal stored in the source polygon (`SbwPoly::normal`), zero when
    /// the stored value is absent or degenerate.  This is the authoring
    /// normal and the one `surface_of` classifies by.
    Vec3d data_normal;
    Vec3d centroid;
    Aabb bounds;
    double plane_offset = 0.0; ///< dot(normal, a)
    std::uint32_t sector = 0;
    std::uint32_t object = 0;
    std::uint16_t texture = 0;  ///< .sbw texture index (sign carries the table)
    std::uint16_t material = 0; ///< 0..5, see docs/FORMATS.md §10.1
    std::uint16_t lightmap = 0;
    std::uint32_t source_poly = 0; ///< polygon index inside its object
};

enum class Surface : std::uint8_t { floor, wall, ceiling };

/// Normal used for surface classification and for the back-face test: the
/// stored `.sbw` polygon normal (`data_normal`) when it is non-zero, otherwise
/// the winding normal.  Never zero for a built triangle.
Vec3d surface_normal(const Triangle& triangle);

/// Classify by `surface_normal()` (`min_normal_z` = 0.8 by default).
Surface surface_of(const Triangle& triangle, double min_normal_z = 0.8);

inline const char* surface_name(Surface surface) {
    switch (surface) {
    case Surface::floor:
        return "floor";
    case Surface::ceiling:
        return "ceiling";
    case Surface::wall:
        break;
    }
    return "wall";
}

/// Query filter.  All surfaces are enabled by default; `sectors` (if non-empty)
/// restricts the broad phase to those sector indices.
struct CollideFilter {
    bool floors = true;
    bool ceilings = true;
    bool walls = true;
    /// Accept hits on the back side of a triangle.  The back side is defined
    /// by `surface_normal()` (the stored polygon normal when present), *not*
    /// by the winding, and the default `true` makes every query two-sided:
    /// winding direction never decides whether geometry collides.  Set to
    /// `false` only when a caller deliberately wants one-sided clipping
    /// against the authored normal.
    bool backfaces = true;
    double min_normal_z = 0.8;
    std::span<const std::uint32_t> sectors{};

    bool allows(const Triangle& triangle) const;
};

struct RayHit {
    double t = 0.0;     ///< distance from origin along the (normalised) direction
    Vec3d point;
    Vec3d normal;       ///< unit, pointing against the ray (towards its origin)
    const Triangle* triangle = nullptr; ///< valid while the world is alive
    std::uint32_t triangle_index = 0;
    std::uint32_t sector = 0;
    bool front_face = false; ///< the ray arrived on the front side of `surface_normal()`
};

struct SphereHit {
    double t = 0.0;     ///< distance travelled by the centre
    Vec3d point;        ///< contact point on the triangle
    Vec3d centre;       ///< sphere centre at the moment of contact
    Vec3d normal;       ///< unit, pushes the sphere out of the surface
    const Triangle* triangle = nullptr;
    std::uint32_t triangle_index = 0;
    std::uint32_t sector = 0;
};

/// Immutable triangle soup plus sector AABBs.  Build once per level; queries
/// take `const CollisionWorld&` and allocate nothing.
class CollisionWorld {
public:
    struct BuildStats {
        std::size_t sectors = 0;
        std::size_t objects = 0;
        std::size_t polygons = 0;
        std::size_t triangles = 0;
        std::size_t degenerate = 0;  ///< skipped: <3 indices or zero area
        std::size_t non_finite = 0;  ///< skipped: NaN/inf vertex or transform
        std::size_t out_of_range = 0; ///< skipped: |coordinate| > 1e9
        std::size_t bad_index = 0;   ///< skipped: vertex index >= n_verts
    };

    /// Triangulate `level` (fan per polygon, object transform applied).  A
    /// malformed level (non-finite transform/vertex, out-of-range coordinate or
    /// vertex index) clears the world, fills `stats()` and returns false with a
    /// one-line `error`.  Zero-area triangles are skipped without failing.
    bool build(const SbwLevel& level, std::string* error = nullptr);

    void clear();

    bool empty() const { return triangles_.empty(); }

    const std::vector<Triangle>& triangles() const { return triangles_; }
    std::size_t triangle_count() const { return triangles_.size(); }
    std::size_t sector_count() const { return sectors_.size(); }
    const Aabb& bounds() const { return bounds_; }
    const Aabb& sector_bounds(std::uint32_t sector) const;
    const BuildStats& stats() const { return stats_; }

    /// Nearest triangle along `direction` (need not be unit).  Returns
    /// `std::nullopt` for a miss, a zero/NaN/inf direction, a NaN origin or a
    /// non-positive/NaN `max_t`.
    std::optional<RayHit> raycast(const Vec3d& origin, const Vec3d& direction,
                                  double max_t = kCollideInfinity,
                                  const CollideFilter& filter = {}) const;

    /// Sweep a sphere of `radius` from `origin` along `direction`.  Contact is
    /// exact for the face, edge and vertex features of each triangle; an
    /// initial overlap is reported as `t = 0` with a push-out normal.
    std::optional<SphereHit> sphere_cast(const Vec3d& origin, const Vec3d& direction,
                                         double radius, double max_t = kCollideInfinity,
                                         const CollideFilter& filter = {}) const;

    /// Highest floor surface below `(x, y, z)` within `max_drop`, ignoring
    /// walls and ceilings (slanted surfaces with |n.z| < min_normal_z too).
    /// Equivalent to a straight-down raycast with the floor filter.
    std::optional<double> floor_at(double x, double y, double z,
                                   double max_drop = kCollideInfinity,
                                   const CollideFilter& filter = {}) const;

    /// Triangle indices whose AABB overlaps `box`.  `out` is cleared first;
    /// reuse one vector to avoid per-query allocation.  Results are in build
    /// order (sector-major, then polygon order).
    std::size_t overlap_aabb(const Aabb& box, std::vector<std::uint32_t>& out,
                             const CollideFilter& filter = {}) const;

private:
    struct Sector {
        Aabb bounds;
        std::uint32_t begin = 0;
        std::uint32_t count = 0;
    };

    std::vector<Triangle> triangles_;
    std::vector<Sector> sectors_;
    Aabb bounds_;
    BuildStats stats_;
};

/// Exposed for tests and for tools that already have three points: nearest
/// point on a triangle, point-in-triangle (barycentric), and ray/triangle
/// parameter.  All use the same epsilons as `raycast`.
Vec3d closest_point_on_triangle(Vec3d p, const Triangle& triangle);
bool point_in_triangle(Vec3d p, const Triangle& triangle);
std::optional<double> ray_triangle(const Vec3d& origin, const Vec3d& unit_direction,
                                   const Triangle& triangle, double max_t,
                                   double* u = nullptr, double* v = nullptr);

} // namespace gm
