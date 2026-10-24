// Volume queries over the static triangles of a `CollisionWorld`
// (docs/COLLIDE_OVERLAP.md).
//
// What is under test is *exactness*, not speed.  `overlap_triangles` and
// `overlaps_static` must report the same index set as a full scan of
// `world.triangles()` with an independently written narrow phase, and
// `overlap_sphere` the same set as a full scan with an independent
// point-to-triangle distance.  The reference below shares no algorithm with the
// module: it decides "triangle touches box" from the three triangle vertices,
// the eight box corners and both edge sets (a segment test each), while the
// module uses a 13-axis separating-axis test.  Two independent derivations
// agreeing on real geometry is the evidence; a copy of the SAT in the test
// would only prove that the code is deterministic.
//
// Real levels: `level0.sbw` (the campaign hub, 3162 triangles) and `dm1.sbw`
// (the largest death match arena, 11781 triangles) from the game install or its
// mirror.  Boxes and spheres are deterministic (SplitMix64, the seed is printed
// below), so a failure can be replayed by re-running the binary.
//
// Also covered: whole-world, void, point-like and per-surface-filtered queries
// with their counts; the radius 0/1/100 spheres around a triangle centroid;
// the AABB prefilter being a superset of the exact answer; a suppressed
// `(sector, object)` pair dropping out of the answer; and refusals (min > max,
// NaN/inf, negative radius, empty world) with an error line.

#include "render/collide_overlap.h"

#include "res/sbw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kInstall = "/mnt/d/games/Gunmetal 3dfx";
constexpr std::uint64_t kSeed = 0x0dd'ba11'5eed'2026ULL;
constexpr int kParityBoxes = 240; ///< per level, >= the 200 the task sheet asks for
constexpr int kParitySpheres = 120;
constexpr int kSyntheticCases = 2000;

// --- setup -----------------------------------------------------------------

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

/// Game install first, then the warm mirror used by `regress.py --mirror`.
fs::path find_sbw(const char* name) {
    const fs::path installed = kInstall / name;
    if (fs::exists(installed)) {
        return installed;
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        const fs::path mirrored = fs::path(home) / ".cache/gunmetal-re/game" / name;
        if (fs::exists(mirrored)) {
            return mirrored;
        }
    }
    return {};
}

bool load_level(const char* name, gm::CollisionWorld& world) {
    const fs::path sbw = find_sbw(name);
    if (sbw.empty()) {
        std::printf("collide_overlap: %s not installed, real-level part skipped\n", name);
        return false;
    }
    std::string error;
    const std::vector<std::uint8_t> raw = read_file(sbw);
    CHECK(!raw.empty());
    const std::optional<gm::SbwLevel> level = gm::parse_sbw(raw, error);
    CHECK(level.has_value());
    if (!level) {
        std::fprintf(stderr, "collide_overlap: parse_sbw(%s): %s\n", name, error.c_str());
        return false;
    }
    error.clear();
    CHECK(world.build(*level, &error));
    CHECK(error.empty());
    const gm::Aabb& bounds = world.bounds();
    std::printf("collide_overlap: %s triangles=%zu sectors=%zu "
                "bounds=(%.1f %.1f %.1f)..(%.1f %.1f %.1f)\n",
                name, world.triangle_count(), world.sector_count(), bounds.lo.x, bounds.lo.y,
                bounds.lo.z, bounds.hi.x, bounds.hi.y, bounds.hi.z);
    return true;
}

// --- deterministic numbers --------------------------------------------------

/// SplitMix64: tiny, portable, and the seed is printed so any case can be
/// replayed.
struct Lcg {
    std::uint64_t state = 0;

    explicit Lcg(std::uint64_t seed) : state(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    /// Uniform on [0, 1).
    double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    double uniform(double lo, double hi) { return lo + (hi - lo) * unit(); }

    std::size_t index(std::size_t count) {
        return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(count));
    }
};

// --- independent reference narrow phase -------------------------------------
//
// "Triangle touches box" without any SAT: a contact exists iff one of
//   (a) a triangle vertex is inside the box,
//   (b) a box corner lies on the triangle (plane distance <= eps and inside the
//       projected triangle),
//   (c) a triangle edge crosses the box,
//   (d) a box edge crosses the triangle
// holds.  Completeness argument (docs/COLLIDE_OVERLAP.md §2): if the plane does
// not cut the box, a triangle vertex inside the box is the only way to touch
// (a); if it does, the plane/box section P is a convex polygon inside the
// triangle plane, and T n P != {} implies either a T vertex inside the box
// (a), a P vertex (= a box edge crossing the plane) inside T (d), or a T edge
// crossing the boundary of P (= entering the box, c).

constexpr double kRefEps = 1e-9;

bool ref_point_in_box(gm::Vec3d p, const gm::Aabb& box) {
    return p.x >= box.lo.x && p.x <= box.hi.x && p.y >= box.lo.y && p.y <= box.hi.y &&
           p.z >= box.lo.z && p.z <= box.hi.z;
}

/// Clip `p..q` against the box (slab method on `[0, 1]`), inclusive.
bool ref_segment_box(gm::Vec3d p, gm::Vec3d q, const gm::Aabb& box) {
    double t0 = 0.0;
    double t1 = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double a = axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
        const double b = axis == 0 ? q.x : (axis == 1 ? q.y : q.z);
        const double lo = axis == 0 ? box.lo.x : (axis == 1 ? box.lo.y : box.lo.z);
        const double hi = axis == 0 ? box.hi.x : (axis == 1 ? box.hi.y : box.hi.z);
        const double d = b - a;
        if (std::fabs(d) < 1e-300) {
            if (a < lo || a > hi) {
                return false;
            }
            continue;
        }
        const double inv = 1.0 / d;
        double ta = (lo - a) * inv;
        double tb = (hi - a) * inv;
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) {
            return false;
        }
    }
    return true;
}

/// Point inside the triangle projected onto its plane (own barycentric solve,
/// 1e-12 slack on the edge signs).
bool ref_inside_projected(gm::Vec3d p, const gm::Triangle& triangle) {
    const gm::Vec3d v0 = triangle.b - triangle.a;
    const gm::Vec3d v1 = triangle.c - triangle.a;
    const gm::Vec3d v2 = p - triangle.a;
    const double d00 = gm::dot(v0, v0);
    const double d01 = gm::dot(v0, v1);
    const double d11 = gm::dot(v1, v1);
    const double d20 = gm::dot(v2, v0);
    const double d21 = gm::dot(v2, v1);
    const double denom = d00 * d11 - d01 * d01;
    if (!(denom > 0.0)) {
        return false;
    }
    const double v = (d11 * d20 - d01 * d21) / denom;
    const double w = (d00 * d21 - d01 * d20) / denom;
    const double u = 1.0 - v - w;
    constexpr double kBar = 1e-12;
    return u >= -kBar && v >= -kBar && w >= -kBar;
}

/// `p` lies on the triangle sheet: on its plane and inside the projection.
bool ref_point_on_triangle(gm::Vec3d p, const gm::Triangle& triangle) {
    const gm::Vec3d normal = gm::cross(triangle.b - triangle.a, triangle.c - triangle.a);
    const double len = gm::length(normal);
    if (!(len > 0.0)) {
        return false;
    }
    const double distance = std::fabs(gm::dot(normal, p - triangle.a)) / len;
    return distance <= kRefEps && ref_inside_projected(p, triangle);
}

std::array<gm::Vec3d, 8> box_corners(const gm::Aabb& box) {
    std::array<gm::Vec3d, 8> corners{};
    for (int i = 0; i < 8; ++i) {
        corners[static_cast<std::size_t>(i)] = {(i & 1) != 0 ? box.hi.x : box.lo.x,
                                               (i & 2) != 0 ? box.hi.y : box.lo.y,
                                               (i & 4) != 0 ? box.hi.z : box.lo.z};
    }
    return corners;
}

constexpr int kBoxEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                 {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

/// Segment/segment crossing in 2D, strict (endpoint touches are covered by the
/// inside tests of the caller).
bool ref_segments_cross_2d(double ax, double ay, double bx, double by, double cx, double cy,
                           double dx, double dy) {
    const auto side = [](double ox, double oy, double px, double py, double qx, double qy) {
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox);
    };
    const double s1 = side(ax, ay, bx, by, cx, cy);
    const double s2 = side(ax, ay, bx, by, dx, dy);
    const double s3 = side(cx, cy, dx, dy, ax, ay);
    const double s4 = side(cx, cy, dx, dy, bx, by);
    return ((s1 > 0.0) != (s2 > 0.0)) && ((s3 > 0.0) != (s4 > 0.0));
}

/// Coplanar case of `ref_segment_triangle`: project on the plane axis with the
/// largest normal component and test the segment against the triangle in 2D.
bool ref_segment_triangle_flat(gm::Vec3d p, gm::Vec3d q, const gm::Triangle& triangle,
                               gm::Vec3d unit) {
    int drop = 0;
    double largest = std::fabs(unit.x);
    if (std::fabs(unit.y) > largest) {
        drop = 1;
        largest = std::fabs(unit.y);
    }
    if (std::fabs(unit.z) > largest) {
        drop = 2;
    }
    const auto pick = [drop](gm::Vec3d v) {
        return drop == 0 ? std::array<double, 2>{v.y, v.z}
                         : (drop == 1 ? std::array<double, 2>{v.x, v.z}
                                      : std::array<double, 2>{v.x, v.y});
    };
    const std::array<double, 2> sp = pick(p);
    const std::array<double, 2> sq = pick(q);
    const std::array<double, 2> ta = pick(triangle.a);
    const std::array<double, 2> tb = pick(triangle.b);
    const std::array<double, 2> tc = pick(triangle.c);
    if (ref_inside_projected(p, triangle) || ref_inside_projected(q, triangle)) {
        return true;
    }
    return ref_segments_cross_2d(sp[0], sp[1], sq[0], sq[1], ta[0], ta[1], tb[0], tb[1]) ||
           ref_segments_cross_2d(sp[0], sp[1], sq[0], sq[1], tb[0], tb[1], tc[0], tc[1]) ||
           ref_segments_cross_2d(sp[0], sp[1], sq[0], sq[1], tc[0], tc[1], ta[0], ta[1]);
}

/// Two-sided segment/triangle intersection.  The crossing is located from the
/// *plane distances* of the endpoints (stable for a segment parallel to the
/// plane, where a Möller–Trumbore determinant is pure rounding noise), and the
/// segment lying in the plane falls back to the 2D test.
bool ref_segment_triangle(gm::Vec3d p, gm::Vec3d q, const gm::Triangle& triangle) {
    const gm::Vec3d normal = gm::cross(triangle.b - triangle.a, triangle.c - triangle.a);
    const double len = gm::length(normal);
    if (!(len > 0.0)) {
        return false;
    }
    const gm::Vec3d unit = normal * (1.0 / len);
    const double d0 = gm::dot(unit, p - triangle.a);
    const double d1 = gm::dot(unit, q - triangle.a);
    if (d0 > kRefEps && d1 > kRefEps) {
        return false; // the whole segment is above the plane
    }
    if (d0 < -kRefEps && d1 < -kRefEps) {
        return false; // ... and below it
    }
    if (std::fabs(d0) > kRefEps && std::fabs(d1) > kRefEps) {
        // Proper crossing between the endpoints: the crossing point must be
        // inside the triangle.
        const double s = d0 / (d0 - d1);
        return ref_inside_projected(p + (q - p) * s, triangle);
    }
    return ref_segment_triangle_flat(p, q, triangle, unit);
}

bool ref_triangle_box(const gm::Triangle& triangle, const gm::Aabb& box) {
    if (!gm::overlaps(triangle.bounds, box)) {
        return false; // same cheap reject the production broad phase does
    }
    // (a) triangle vertex inside the box
    if (ref_point_in_box(triangle.a, box) || ref_point_in_box(triangle.b, box) ||
        ref_point_in_box(triangle.c, box)) {
        return true;
    }
    // (b) box corner on the triangle
    const std::array<gm::Vec3d, 8> corners = box_corners(box);
    for (const gm::Vec3d& corner : corners) {
        if (ref_point_on_triangle(corner, triangle)) {
            return true;
        }
    }
    // (c) triangle edge through the box
    if (ref_segment_box(triangle.a, triangle.b, box) ||
        ref_segment_box(triangle.b, triangle.c, box) ||
        ref_segment_box(triangle.c, triangle.a, box)) {
        return true;
    }
    // (d) box edge through the triangle
    for (const auto& edge : kBoxEdges) {
        if (ref_segment_triangle(corners[static_cast<std::size_t>(edge[0])],
                                 corners[static_cast<std::size_t>(edge[1])], triangle)) {
            return true;
        }
    }
    return false;
}

// --- independent reference for the sphere -----------------------------------

double ref_point_segment_distance(gm::Vec3d p, gm::Vec3d a, gm::Vec3d b) {
    const gm::Vec3d ab = b - a;
    const double denom = gm::dot(ab, ab);
    double t = 0.0;
    if (denom > 0.0) {
        t = gm::dot(p - a, ab) / denom;
        t = std::min(1.0, std::max(0.0, t));
    }
    return gm::length(p - (a + ab * t));
}

/// Distance from a point to the triangle *surface*, from the three edge
/// distances and the in-plane drop -- no reuse of `closest_point_on_triangle`.
double ref_point_triangle_distance(gm::Vec3d p, const gm::Triangle& triangle) {
    double best = std::min({ref_point_segment_distance(p, triangle.a, triangle.b),
                            ref_point_segment_distance(p, triangle.b, triangle.c),
                            ref_point_segment_distance(p, triangle.c, triangle.a)});
    const gm::Vec3d normal = gm::cross(triangle.b - triangle.a, triangle.c - triangle.a);
    const double len = gm::length(normal);
    if (len > 0.0) {
        const gm::Vec3d unit = normal * (1.0 / len);
        const double height = std::fabs(gm::dot(p - triangle.a, unit));
        const gm::Vec3d projected = p - unit * gm::dot(p - triangle.a, unit);
        if (ref_inside_projected(projected, triangle)) {
            best = std::min(best, height);
        }
    }
    return best;
}

// --- brute force ------------------------------------------------------------

std::vector<std::uint32_t> brute_overlap(const gm::CollisionWorld& world,
                                         const gm::Aabb& box, const gm::CollideFilter& filter) {
    std::vector<std::uint32_t> out;
    const std::vector<gm::Triangle>& triangles = world.triangles();
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const gm::Triangle& triangle = triangles[i];
        if (world.suppressed_count() != 0 &&
            world.is_suppressed(triangle.sector, triangle.object)) {
            continue;
        }
        if (!filter.allows(triangle)) {
            continue;
        }
        if (!ref_triangle_box(triangle, box)) {
            continue;
        }
        out.push_back(static_cast<std::uint32_t>(i));
    }
    return out;
}

std::vector<std::uint32_t> brute_sphere(const gm::CollisionWorld& world, gm::Vec3d centre,
                                        double radius, const gm::CollideFilter& filter) {
    std::vector<std::uint32_t> out;
    const std::vector<gm::Triangle>& triangles = world.triangles();
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const gm::Triangle& triangle = triangles[i];
        if (world.suppressed_count() != 0 &&
            world.is_suppressed(triangle.sector, triangle.object)) {
            continue;
        }
        if (!filter.allows(triangle)) {
            continue;
        }
        if (ref_point_triangle_distance(centre, triangle) > radius + kRefEps) {
            continue;
        }
        out.push_back(static_cast<std::uint32_t>(i));
    }
    return out;
}

// --- reporting helpers ------------------------------------------------------

void report_mismatch(const char* what, const std::vector<std::uint32_t>& exact,
                     const std::vector<std::uint32_t>& reference) {
    std::fprintf(stderr, "collide_overlap: %s mismatch exact=%zu reference=%zu", what,
                 exact.size(), reference.size());
    const std::size_t n = std::min(exact.size(), reference.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (exact[i] != reference[i]) {
            std::fprintf(stderr, " first_diff@%zu exact=%u reference=%u", i, exact[i],
                         reference[i]);
            break;
        }
    }
    std::fputc('\n', stderr);
}

/// Run one box query, compare it with the reference and with `overlap_aabb`,
/// print the counts, return the exact answer.
std::vector<std::uint32_t> check_box(const gm::CollisionWorld& world, const char* label,
                                     const gm::Aabb& box, const gm::CollideFilter& filter) {
    std::string error;
    std::vector<std::uint32_t> exact;
    CHECK(gm::overlap_triangles(world, box, exact, filter, &error));
    CHECK(error.empty());

    const std::vector<std::uint32_t> reference = brute_overlap(world, box, filter);
    CHECK(exact == reference);
    if (exact != reference) {
        report_mismatch(label, exact, reference);
    }

    std::vector<std::uint32_t> broad;
    world.overlap_aabb(box, broad, filter);
    CHECK(std::includes(broad.begin(), broad.end(), exact.begin(), exact.end()));

    std::string flag_error;
    CHECK(gm::overlaps_static(world, box, filter, &flag_error) == !exact.empty());
    CHECK(flag_error.empty());

    // The convenience overload must return the same set.
    CHECK(gm::overlap_triangles(world, box, filter) == exact);

    std::printf("collide_overlap: %s tris=%zu aabb=%zu\n", label, exact.size(), broad.size());
    return exact;
}

std::vector<std::uint32_t> check_sphere(const gm::CollisionWorld& world, const char* label,
                                        gm::Vec3d centre, double radius,
                                        const gm::CollideFilter& filter) {
    std::string error;
    std::vector<std::uint32_t> exact;
    CHECK(gm::overlap_sphere(world, centre, radius, exact, filter, &error));
    CHECK(error.empty());

    const std::vector<std::uint32_t> reference = brute_sphere(world, centre, radius, filter);
    CHECK(exact == reference);
    if (exact != reference) {
        report_mismatch(label, exact, reference);
    }
    CHECK(gm::overlap_sphere(world, centre, radius, filter) == exact);
    std::printf("collide_overlap: %s r=%.3f tris=%zu\n", label, radius, exact.size());
    return exact;
}

// --- synthetic SAT cross-check ---------------------------------------------

void test_synthetic() {
    Lcg rng(kSeed ^ 0x5a7u);
    int same = 0;
    int hits = 0;
    for (int i = 0; i < kSyntheticCases; ++i) {
        const gm::Vec3d a{rng.uniform(0.0, 100.0), rng.uniform(0.0, 100.0),
                          rng.uniform(0.0, 100.0)};
        const gm::Vec3d b = a + gm::Vec3d{rng.uniform(-40.0, 40.0), rng.uniform(-40.0, 40.0),
                                          rng.uniform(-40.0, 40.0)};
        const gm::Vec3d c = a + gm::Vec3d{rng.uniform(-40.0, 40.0), rng.uniform(-40.0, 40.0),
                                          rng.uniform(-40.0, 40.0)};
        gm::Triangle triangle;
        triangle.a = a;
        triangle.b = b;
        triangle.c = c;
        triangle.bounds.expand(a);
        triangle.bounds.expand(b);
        triangle.bounds.expand(c);

        const gm::Vec3d centre{rng.uniform(0.0, 100.0), rng.uniform(0.0, 100.0),
                               rng.uniform(0.0, 100.0)};
        const gm::Vec3d half{rng.uniform(0.1, 30.0), rng.uniform(0.1, 30.0),
                             rng.uniform(0.1, 30.0)};
        const gm::Aabb box = gm::aabb_of(centre - half, centre + half);

        const bool sat = gm::triangle_intersects_box(triangle, box);
        const bool reference = ref_triangle_box(triangle, box);
        if (sat == reference) {
            ++same;
        } else {
            std::fprintf(stderr,
                         "collide_overlap: synthetic mismatch case %d sat=%d reference=%d "
                         "box=(%.3f %.3f %.3f)..(%.3f %.3f %.3f)\n",
                         i, sat ? 1 : 0, reference ? 1 : 0, box.lo.x, box.lo.y, box.lo.z,
                         box.hi.x, box.hi.y, box.hi.z);
        }
        hits += sat ? 1 : 0;
    }
    CHECK(same == kSyntheticCases);
    std::printf("collide_overlap: synthetic SAT parity cases=%d same=%d/%d hits=%d\n",
                kSyntheticCases, same, kSyntheticCases, hits);

    // Hand cases: the boundary is inclusive, so a triangle that only touches a
    // corner counts; moving it one unit off the box does not.
    gm::Triangle corner_triangle;
    corner_triangle.a = {0.0, 0.0, 0.0};
    corner_triangle.b = {10.0, 0.0, 0.0};
    corner_triangle.c = {0.0, 10.0, 0.0};
    corner_triangle.bounds.expand(corner_triangle.a);
    corner_triangle.bounds.expand(corner_triangle.b);
    corner_triangle.bounds.expand(corner_triangle.c);
    const gm::Aabb unit_box = gm::aabb_of({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0});
    CHECK(gm::triangle_intersects_box(corner_triangle, unit_box)); // touches (0,0,0)
    gm::Triangle lifted = corner_triangle;
    lifted.a.z = 5.0;
    lifted.b.z = 5.0;
    lifted.c.z = 5.0;
    CHECK(!gm::triangle_intersects_box(lifted, unit_box)); // 4 units above
    // A large triangle slicing the box: no vertex or corner involved.
    gm::Triangle slicing;
    slicing.a = {-20.0, -20.0, 0.0};
    slicing.b = {20.0, -20.0, 0.0};
    slicing.c = {0.0, 20.0, 0.0};
    slicing.bounds.expand(slicing.a);
    slicing.bounds.expand(slicing.b);
    slicing.bounds.expand(slicing.c);
    CHECK(gm::triangle_intersects_box(slicing, unit_box));
    std::printf("collide_overlap: synthetic boundary/slice cases ok\n");
}

// --- parity: module vs full scan -------------------------------------------

void test_parity(const gm::CollisionWorld& world, const char* level, int boxes) {
    Lcg rng(kSeed ^ 0x9a71'17u);
    const gm::Vec3d lo = world.bounds().lo;
    const gm::Vec3d hi = world.bounds().hi;
    const gm::CollideFilter filter{};
    int same = 0;
    int nonempty = 0;
    int narrowed = 0;
    std::size_t exact_total = 0;
    std::size_t broad_total = 0;
    for (int i = 0; i < boxes; ++i) {
        gm::Aabb box;
        if (i % 2 == 0) {
            // A box grown around a real triangle's bounds: never empty.
            const gm::Triangle& triangle =
                world.triangles()[rng.index(world.triangle_count())];
            box = triangle.bounds;
            box.grow(0.5 + rng.uniform(0.0, 12.0));
        } else {
            const gm::Vec3d centre{rng.uniform(lo.x, hi.x), rng.uniform(lo.y, hi.y),
                                   rng.uniform(lo.z, hi.z)};
            const gm::Vec3d half{rng.uniform(0.25, 24.0), rng.uniform(0.25, 24.0),
                                 rng.uniform(0.25, 24.0)};
            box = gm::aabb_of(centre - half, centre + half);
        }
        const std::vector<std::uint32_t> exact = gm::overlap_triangles(world, box, filter);
        const std::vector<std::uint32_t> reference = brute_overlap(world, box, filter);
        if (exact == reference) {
            ++same;
        } else {
            std::fprintf(stderr,
                         "collide_overlap: %s box %d mismatch box=(%.2f %.2f %.2f).."
                         "(%.2f %.2f %.2f)\n",
                         level, i, box.lo.x, box.lo.y, box.lo.z, box.hi.x, box.hi.y, box.hi.z);
            report_mismatch("parity", exact, reference);
        }
        if (!exact.empty()) {
            ++nonempty;
        }
        std::vector<std::uint32_t> broad;
        world.overlap_aabb(box, broad, filter);
        CHECK(std::includes(broad.begin(), broad.end(), exact.begin(), exact.end()));
        if (exact.size() < broad.size()) {
            ++narrowed;
        }
        exact_total += exact.size();
        broad_total += broad.size();
    }
    CHECK(same == boxes);
    CHECK(nonempty > 0);
    CHECK(narrowed > 0); // the narrow phase must actually reject AABB candidates
    std::printf("collide_overlap: %s parity boxes=%d same=%d/%d nonempty=%d aabb_narrowed=%d "
                "exact_tris=%zu aabb_tris=%zu\n",
                level, boxes, same, boxes, nonempty, narrowed, exact_total, broad_total);
}

void test_sphere_parity(const gm::CollisionWorld& world, const char* level, int spheres) {
    Lcg rng(kSeed ^ 0x5f1e'2eu);
    const gm::Vec3d lo = world.bounds().lo;
    const gm::Vec3d hi = world.bounds().hi;
    const gm::CollideFilter filter{};
    int same = 0;
    int nonempty = 0;
    std::size_t total = 0;
    for (int i = 0; i < spheres; ++i) {
        gm::Vec3d centre{};
        double radius = 0.0;
        if (i % 2 == 0) {
            // A centroid is on the geometry, so the sphere is never empty.
            centre = world.triangles()[rng.index(world.triangle_count())].centroid;
            radius = rng.uniform(0.0, 60.0);
        } else {
            centre = {rng.uniform(lo.x, hi.x), rng.uniform(lo.y, hi.y), rng.uniform(lo.z, hi.z)};
            radius = rng.uniform(0.0, 120.0);
        }
        const std::vector<std::uint32_t> exact =
            gm::overlap_sphere(world, centre, radius, filter);
        const std::vector<std::uint32_t> reference = brute_sphere(world, centre, radius, filter);
        if (exact == reference) {
            ++same;
        } else {
            std::fprintf(stderr,
                         "collide_overlap: %s sphere %d mismatch centre=(%.2f %.2f %.2f) r=%.2f\n",
                         level, i, centre.x, centre.y, centre.z, radius);
            report_mismatch("sphere parity", exact, reference);
        }
        if (!exact.empty()) {
            ++nonempty;
        }
        total += exact.size();
    }
    CHECK(same == spheres);
    CHECK(nonempty > 0);
    std::printf("collide_overlap: %s sphere_parity spheres=%d same=%d/%d nonempty=%d tris=%zu\n",
                level, spheres, same, spheres, nonempty, total);
}

// --- known queries ----------------------------------------------------------

void test_known_queries(const gm::CollisionWorld& world, const char* level) {
    const gm::Aabb bounds = world.bounds();

    // (1) A box around the level origin (the `.sbw` world origin, 0,0,0).
    const gm::Aabb origin_box = gm::aabb_of({-16.0, -16.0, -16.0}, {16.0, 16.0, 16.0});
    const std::vector<std::uint32_t> origin_hits =
        check_box(world, "origin_box(-16..16)^3", origin_box, {});
    CHECK(!origin_hits.empty());
    if (origin_hits.empty()) {
        return; // the rest of the function picks its anchor triangle from here
    }

    // (2) A box around the campaign spawn eye point (docs/COLLIDE.md §6.2).
    //     The eye point sits in an open room, 42 units above the alcove floor
    //     and 56 away from the +X wall, so a +-8 box touches nothing: 0 is the
    //     expected number, and it is checked against the full scan too.
    const gm::Vec3d spawn{24.0, -208.0, 58.0};
    const gm::Aabb spawn_box = gm::aabb_of(spawn - gm::Vec3d{8.0, 8.0, 8.0},
                                           spawn + gm::Vec3d{8.0, 8.0, 8.0});
    const std::vector<std::uint32_t> spawn_hits =
        check_box(world, "spawn_box(+-8, open room)", spawn_box, {});
    CHECK(spawn_hits.empty());

    // (3) A box in the void, well outside the world AABB.
    const gm::Vec3d void_lo = bounds.hi + gm::Vec3d{1000.0, 1000.0, 1000.0};
    const gm::Aabb void_box = gm::aabb_of(void_lo, void_lo + gm::Vec3d{16.0, 16.0, 16.0});
    const std::vector<std::uint32_t> void_hits = check_box(world, "void_box", void_box, {});
    CHECK(void_hits.empty());

    // (4) The whole world AABB: every static triangle, inclusive boundaries.
    const std::vector<std::uint32_t> all = check_box(world, "world_box", bounds, {});
    CHECK(all.size() == world.triangle_count());

    // (5) Per-surface filters on the same box, cross-checked against the
    //     brute-force scan and `overlap_aabb`.  level0's documented counts are
    //     558 floors / 2082 walls / 522 ceilings (docs/COLLIDE.md §6.2).
    gm::CollideFilter floors;
    floors.walls = false;
    floors.ceilings = false;
    gm::CollideFilter walls;
    walls.floors = false;
    walls.ceilings = false;
    gm::CollideFilter ceilings;
    ceilings.floors = false;
    ceilings.walls = false;
    const std::size_t floor_count = check_box(world, "world_box floors", bounds, floors).size();
    const std::size_t wall_count = check_box(world, "world_box walls", bounds, walls).size();
    const std::size_t ceiling_count =
        check_box(world, "world_box ceilings", bounds, ceilings).size();
    CHECK(floor_count + wall_count + ceiling_count == world.triangle_count());
    if (std::string(level) == "level0") {
        CHECK(floor_count == 558);
        CHECK(wall_count == 2082);
        CHECK(ceiling_count == 522);
    }

    // (6) A sector list narrows the answer and still matches the full scan.
    const gm::Triangle& first = world.triangles()[origin_hits.front()];
    const std::uint32_t only_sector[1] = {first.sector};
    gm::CollideFilter sector_filter;
    sector_filter.sectors = std::span<const std::uint32_t>(only_sector, 1);
    const std::vector<std::uint32_t> sector_hits =
        check_box(world, "world_box one_sector", bounds, sector_filter);
    CHECK(!sector_hits.empty());
    for (const std::uint32_t index : sector_hits) {
        CHECK(world.triangles()[index].sector == first.sector);
    }

    // (7) A tiny box around a triangle centroid (a point-like query): the
    //     triangle itself must be reported.
    const gm::Vec3d centre = first.centroid;
    const gm::Aabb point_box = gm::aabb_of(centre - gm::Vec3d{1e-3, 1e-3, 1e-3},
                                           centre + gm::Vec3d{1e-3, 1e-3, 1e-3});
    const std::vector<std::uint32_t> point_hits =
        check_box(world, "point_box(+-1e-3 at centroid)", point_box, {});
    CHECK(!point_hits.empty());
    CHECK(std::binary_search(point_hits.begin(), point_hits.end(), origin_hits.front()));

    // (8) Spheres around the same centroid: radius 0/1/100, monotone sets.
    const std::vector<std::uint32_t> r0 = check_sphere(world, "sphere centroid", centre, 0.0, {});
    const std::vector<std::uint32_t> r1 = check_sphere(world, "sphere centroid", centre, 1.0, {});
    const std::vector<std::uint32_t> r100 =
        check_sphere(world, "sphere centroid", centre, 100.0, {});
    CHECK(!r0.empty());
    CHECK(std::binary_search(r0.begin(), r0.end(), origin_hits.front()));
    CHECK(std::includes(r1.begin(), r1.end(), r0.begin(), r0.end()));
    CHECK(std::includes(r100.begin(), r100.end(), r1.begin(), r1.end()));

    // (9) A sphere in the void, and one that covers the whole world.
    const std::vector<std::uint32_t> void_sphere =
        check_sphere(world, "sphere void", void_lo + gm::Vec3d{8.0, 8.0, 8.0}, 10.0, {});
    CHECK(void_sphere.empty());
    const double diagonal = gm::length(bounds.size());
    const std::vector<std::uint32_t> all_sphere =
        check_sphere(world, "sphere world", bounds.centre(), diagonal, {});
    CHECK(all_sphere.size() == world.triangle_count());
}

// --- suppression ------------------------------------------------------------

void test_suppression(gm::CollisionWorld& world, const char* level) {
    const std::uint32_t sector = world.triangles()[0].sector;
    const std::uint32_t object = world.triangles()[0].object;
    const std::size_t before =
        gm::overlap_triangles(world, world.bounds(), {}).size();

    std::string error;
    CHECK(world.set_dynamic({}, {{sector, object}}, &error));
    CHECK(error.empty());
    const std::vector<std::uint32_t> exact =
        gm::overlap_triangles(world, world.bounds(), {});
    const std::vector<std::uint32_t> reference =
        brute_overlap(world, world.bounds(), {});
    CHECK(exact == reference);
    CHECK(exact.size() < before);
    CHECK(before - exact.size() > 0);
    std::printf("collide_overlap: %s suppression (sector=%u object=%u) tris=%zu -> %zu\n", level,
                sector, object, before, exact.size());

    world.clear_dynamic();
    CHECK(gm::overlap_triangles(world, world.bounds(), {}).size() == before);
}

// --- refusals ---------------------------------------------------------------

void test_refusals(const gm::CollisionWorld& world) {
    const gm::Aabb unit_box = gm::aabb_of({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // min > max
    std::vector<std::uint32_t> out{7, 7};
    std::string error;
    const gm::Aabb inverted = gm::aabb_of({10.0, 0.0, 0.0}, {0.0, 10.0, 10.0});
    CHECK(!gm::overlap_triangles(world, inverted, out, {}, &error));
    CHECK(!error.empty());
    CHECK(out.empty());
    std::printf("collide_overlap: refusal min>max: %s\n", error.c_str());
    CHECK(!gm::overlaps_static(world, inverted, {}, &error));
    CHECK(!error.empty());

    // NaN / inf corners
    const gm::Aabb nan_box = gm::aabb_of({nan, 0.0, 0.0}, {1.0, 1.0, 1.0});
    CHECK(!gm::overlap_triangles(world, nan_box, out, {}, &error));
    CHECK(!error.empty());
    std::printf("collide_overlap: refusal NaN box: %s\n", error.c_str());
    const gm::Aabb inf_box = gm::aabb_of({-inf, -1.0, -1.0}, {1.0, 1.0, 1.0});
    CHECK(!gm::overlap_triangles(world, inf_box, out, {}, &error));
    CHECK(!error.empty());
    std::printf("collide_overlap: refusal inf box: %s\n", error.c_str());

    // Sphere: negative, NaN, inf radius and a non-finite centre.
    CHECK(!gm::overlap_sphere(world, world.bounds().centre(), -1.0, out, {}, &error));
    CHECK(!error.empty());
    CHECK(out.empty());
    std::printf("collide_overlap: refusal radius<0: %s\n", error.c_str());
    CHECK(!gm::overlap_sphere(world, {nan, 0.0, 0.0}, 1.0, out, {}, &error));
    CHECK(!error.empty());
    std::printf("collide_overlap: refusal NaN centre: %s\n", error.c_str());
    CHECK(!gm::overlap_sphere(world, world.bounds().centre(), inf, out, {}, &error));
    CHECK(!error.empty());
    std::printf("collide_overlap: refusal inf radius: %s\n", error.c_str());
    CHECK(!gm::overlap_sphere(world, world.bounds().centre(), nan, out, {}, &error));
    CHECK(!error.empty());

    // A valid query after a refusal still works and the out vector is reused.
    CHECK(gm::overlap_triangles(world, unit_box, out, {}, &error));
    CHECK(error.empty());

    // Empty world: all three queries refuse instead of answering 0 silently.
    gm::CollisionWorld empty;
    CHECK(!gm::overlap_triangles(empty, unit_box, out, {}, &error));
    CHECK(!error.empty());
    CHECK(out.empty());
    std::printf("collide_overlap: refusal empty world: %s\n", error.c_str());
    CHECK(!gm::overlaps_static(empty, unit_box, {}, &error));
    CHECK(!error.empty());
    CHECK(!gm::overlap_sphere(empty, {0.0, 0.0, 0.0}, 1.0, out, {}, &error));
    CHECK(!error.empty());
    std::printf("collide_overlap: empty world out vec kept empty, all three refused\n");
}

// --- determinism ------------------------------------------------------------

void test_determinism(const gm::CollisionWorld& world) {
    const gm::Aabb box = world.bounds();
    const std::vector<std::uint32_t> a = gm::overlap_triangles(world, box, {});
    const std::vector<std::uint32_t> b = gm::overlap_triangles(world, box, {});
    CHECK(a == b);
    CHECK(!a.empty());
    const gm::Vec3d centre = world.triangles()[0].centroid;
    const std::vector<std::uint32_t> s1 = gm::overlap_sphere(world, centre, 100.0, {});
    const std::vector<std::uint32_t> s2 = gm::overlap_sphere(world, centre, 100.0, {});
    CHECK(s1 == s2);
    CHECK(!s1.empty());
    std::printf("collide_overlap: determinism box_tris=%zu sphere_tris=%zu (repeated, equal)\n",
                a.size(), s1.size());
}

} // namespace

int main() {
    std::printf("collide_overlap: seed=0x%016llx parity_boxes=%d parity_spheres=%d "
                "synthetic=%d\n",
                static_cast<unsigned long long>(kSeed), kParityBoxes, kParitySpheres,
                kSyntheticCases);
    test_synthetic();

    for (const char* name : {"level0.sbw", "dm1.sbw"}) {
        gm::CollisionWorld world;
        if (!load_level(name, world)) {
            continue;
        }
        const std::string level = name;
        const std::string stem = level.substr(0, level.find('.'));
        test_parity(world, stem.c_str(), kParityBoxes);
        test_sphere_parity(world, stem.c_str(), kParitySpheres);
        test_known_queries(world, stem.c_str());
        if (stem == "level0") {
            test_suppression(world, stem.c_str());
            test_refusals(world);
            test_determinism(world);
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "collide_overlap: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("collide_overlap: ok\n");
    return 0;
}
