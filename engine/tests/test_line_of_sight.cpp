// Target-aware line of sight over `.sbw` level geometry (docs/LINE_OF_SIGHT.md,
// M6.2.5 slice 2).
//
// Synthetic part: one wall between eye and target proves the "strictly closer
// than distance - radius" rule, a box around the target proves that the
// target's own geometry is never cast against (the central ray stops at the
// near side of the target's bounding sphere), and a barrier moved inside that
// margin turns the same target blocked.  Batch results are compared field by
// field with single calls; NaN/inf/negative radius/empty world are refusals.
//
// Real part: level0/dm1 from the game install (skipped when absent).  The pairs
// below were found by scanning a 64-unit grid at the documented spawn eye
// heights and are printed with their distances, blocking `t` and triangle
// indices; the wall numbers match docs/COLLIDE.md §6.2 (+x wall at t = 56,
// -x at 72, +y at 336, -y at 80 for level0).  No target is tested against
// another target: a batch answer is bit-identical to the single call.
#include "game/line_of_sight.h"

#include "res/sbw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (eps))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s=%.17g != %s=%.17g\n", __FILE__, __LINE__,   \
                         #a, va, #b, vb);                                                    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

// --- synthetic .sbw construction (same shape as test_collide.cpp) ----------

struct TriSpec {
    gm::Vec3d a;
    gm::Vec3d b;
    gm::Vec3d c;
    std::uint32_t sector = 0;
};

void put_f64(std::uint8_t* p, double value) {
    std::memcpy(p, &value, sizeof(value));
}

void write_identity_header(std::array<std::uint8_t, 96>& header) {
    header.fill(0);
    for (int point = 0; point < 4; ++point) {
        for (int component = 0; component < 3; ++component) {
            const double value = (point == 0) ? 0.0 : (component == point - 1 ? 1.0 : 0.0);
            put_f64(header.data() + 8 * (point * 3 + component), value);
        }
    }
}

gm::SbwLevel make_level(const std::vector<TriSpec>& specs) {
    gm::SbwLevel level;
    std::uint32_t sector_count = 1;
    for (const TriSpec& spec : specs) {
        sector_count = std::max(sector_count, spec.sector + 1);
    }
    level.header.sectors.resize(sector_count);
    for (const TriSpec& spec : specs) {
        gm::SbwSectorRef& sector = level.header.sectors.at(spec.sector);
        gm::SbwObjectRef object;
        write_identity_header(object.header);
        sector.objects.push_back(object);

        gm::SbwGeometry geometry;
        geometry.n_verts = 3;
        geometry.vertices = {gm::Vec3f{static_cast<float>(spec.a.x), static_cast<float>(spec.a.y),
                                       static_cast<float>(spec.a.z)},
                             gm::Vec3f{static_cast<float>(spec.b.x), static_cast<float>(spec.b.y),
                                       static_cast<float>(spec.b.z)},
                             gm::Vec3f{static_cast<float>(spec.c.x), static_cast<float>(spec.c.y),
                                       static_cast<float>(spec.c.z)}};
        gm::SbwPoly poly;
        poly.indices = {0, 1, 2};
        geometry.polys.push_back(poly);
        geometry.n_polys = 1;
        level.geometry.push_back(geometry);
    }
    level.header.object_count = specs.size();
    return level;
}

void add_quad(std::vector<TriSpec>& out, gm::Vec3d p0, gm::Vec3d p1, gm::Vec3d p2,
              gm::Vec3d p3, std::uint32_t sector = 0) {
    out.push_back({p0, p1, p2, sector});
    out.push_back({p0, p2, p3, sector});
}

/// Wall in the YZ plane at `x`, spanning `-s..s` in y and `z0..z1`.
void add_wall_x(std::vector<TriSpec>& out, double x, double s, double z0, double z1,
                std::uint32_t sector = 0) {
    add_quad(out, {x, -s, z0}, {x, s, z0}, {x, s, z1}, {x, -s, z1}, sector);
}

/// Axis-aligned box (12 triangles) around `centre` with half-size `half`.
void add_box(std::vector<TriSpec>& out, gm::Vec3d centre, double half,
             std::uint32_t sector = 0) {
    const gm::Vec3d lo = centre - gm::Vec3d{half, half, half};
    const gm::Vec3d hi = centre + gm::Vec3d{half, half, half};
    const gm::Vec3d p000{lo.x, lo.y, lo.z};
    const gm::Vec3d p100{hi.x, lo.y, lo.z};
    const gm::Vec3d p110{hi.x, hi.y, lo.z};
    const gm::Vec3d p010{lo.x, hi.y, lo.z};
    const gm::Vec3d p001{lo.x, lo.y, hi.z};
    const gm::Vec3d p101{hi.x, lo.y, hi.z};
    const gm::Vec3d p111{hi.x, hi.y, hi.z};
    const gm::Vec3d p011{lo.x, hi.y, hi.z};
    add_quad(out, p100, p110, p111, p101, sector); // +X
    add_quad(out, p000, p001, p011, p010, sector); // -X
    add_quad(out, p010, p011, p111, p110, sector); // +Y
    add_quad(out, p000, p100, p101, p001, sector); // -Y
    add_quad(out, p001, p101, p111, p011, sector); // +Z
    add_quad(out, p000, p010, p110, p100, sector); // -Z
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

/// Build a real level; prints "skipped" and returns false when absent.
bool load_level(const char* name, gm::CollisionWorld& world) {
    const fs::path sbw = kGame / name;
    if (!fs::exists(sbw)) {
        std::printf("line_of_sight: %s not installed, real-level part skipped\n", name);
        return false;
    }
    std::string error;
    const std::vector<std::uint8_t> raw = read_file(sbw);
    CHECK(!raw.empty());
    const auto level = gm::parse_sbw(raw, error);
    CHECK(level.has_value());
    if (!level) {
        std::fprintf(stderr, "line_of_sight: parse_sbw(%s): %s\n", name, error.c_str());
        return false;
    }
    error.clear();
    CHECK(world.build(*level, &error));
    CHECK(error.empty());
    std::printf("line_of_sight: %s triangles=%zu sectors=%zu\n", name, world.triangle_count(),
                world.sector_count());
    return true;
}

// --- synthetic -------------------------------------------------------------

void test_wall_and_radius_rule() {
    std::vector<TriSpec> specs;
    // A floor far below, and the blocking wall at x = 20.
    add_quad(specs, {-50, -50, -10}, {50, -50, -10}, {50, 50, -10}, {-50, 50, -10});
    add_wall_x(specs, 20.0, 50.0, -10.0, 100.0);
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_level(specs), &error));
    CHECK(error.empty());

    const gm::Vec3d eye{0, 0, 5};

    // Blocked: the wall at t = 20 is closer than 30 - 1.
    {
        const gm::LosResult r = gm::line_of_sight(world, eye, {11, {30, 0, 5}, 1.0}, {}, &error);
        CHECK(error.empty());
        CHECK(!r.visible);
        CHECK(r.id == 11u);
        CHECK_NEAR(r.distance, 30.0, 1e-12);
        CHECK(r.triangle != nullptr);
        CHECK_NEAR(r.blocker_distance, 20.0, 1e-12);
        CHECK_NEAR(r.blocker_distance, r.distance - 10.0, 0.0); // t < distance - radius
        CHECK(r.blocker == gm::kLosWorldBlocker);
        CHECK(!r.dynamic);
        CHECK(r.triangle_index < world.triangle_count());
        CHECK(r.triangle == &world.triangles()[r.triangle_index]);
        CHECK(gm::surface_of(*r.triangle) == gm::Surface::wall);
        std::printf("synthetic blocked: center=(30,0,5) distance=%.6f blocker_t=%.6f tri=%zu\n",
                    r.distance, r.blocker_distance, r.triangle_index);
    }

    // Visible: the same wall is beyond 10 - 1.
    {
        const gm::LosResult r = gm::line_of_sight(world, eye, {12, {10, 0, 5}, 1.0}, {}, &error);
        CHECK(error.empty());
        CHECK(r.visible);
        CHECK(r.triangle == nullptr);
        CHECK(r.blocker == gm::kLosNoBlocker);
        CHECK_NEAR(r.distance, 10.0, 1e-12); // exactly Euclidean
        CHECK_NEAR(r.blocker_distance, 0.0, 0.0);
        std::printf("synthetic visible: center=(10,0,5) distance=%.6f\n", r.distance);
    }

    // Target centred on the eye: nothing can be between.
    {
        const gm::LosResult r = gm::line_of_sight(world, eye, {13, eye, 1.0}, {}, &error);
        CHECK(r.visible);
        CHECK_NEAR(r.distance, 0.0, 0.0);
    }
    // A radius that reaches the eye leaves no room for a surface either.
    {
        const gm::LosResult r = gm::line_of_sight(world, eye, {14, {0, 0, 25}, 30.0}, {}, &error);
        CHECK(r.visible);
        CHECK_NEAR(r.distance, 20.0, 1e-12);
    }

    // The strict boundary: a wall exactly at distance - radius does not block.
    {
        const gm::LosResult r = gm::line_of_sight(world, eye, {15, {30, 0, 5}, 10.0}, {}, &error);
        CHECK(r.visible);
        std::printf("synthetic boundary: distance=%.6f limit=20.000000 wall_t=20.000000 visible=%d\n",
                    r.distance, static_cast<int>(r.visible));
    }

    // Filter changes the answer: without walls the horizontal ray sees nothing.
    {
        gm::CollideFilter floors;
        floors.walls = false;
        floors.ceilings = false;
        const gm::LosResult r =
            gm::line_of_sight(world, eye, {16, {30, 0, 5}, 1.0}, floors, &error);
        CHECK(r.visible);
        const gm::LosResult r2 =
            gm::line_of_sight(world, eye, {16, {30, 0, 5}, 1.0}, {}, &error);
        CHECK(!r2.visible);
        std::printf("synthetic filter: default visible=%d, floors-only visible=%d\n",
                    static_cast<int>(r2.visible), static_cast<int>(r.visible));
    }

    // One-sided clipping: the wall is wound towards +X, so a +X ray arrives on
    // its back side and `backfaces = false` drops it.
    {
        gm::CollideFilter front_only;
        front_only.backfaces = false;
        const gm::LosResult r =
            gm::line_of_sight(world, eye, {17, {30, 0, 5}, 1.0}, front_only, &error);
        CHECK(r.visible);
        CHECK(!gm::line_of_sight(world, eye, {17, {30, 0, 5}, 1.0}, {}, &error).visible);
    }
}

void test_target_does_not_block_itself() {
    // The target is a 2-unit box at (5,0,5) (near face at x = 4); a wall at
    // x = 30 sits behind it.  Eye at the origin: distance = 5, radius = 2,
    // so the trace stops at x = 3 and neither the box nor the wall is cast.
    std::vector<TriSpec> specs;
    add_box(specs, {5, 0, 5}, 1.0);
    add_wall_x(specs, 30.0, 50.0, -50.0, 100.0);
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_level(specs), &error));
    CHECK(error.empty());

    const gm::Vec3d eye{0, 0, 5};
    const gm::Vec3d centre{5, 0, 5};

    // The box really is in the world: an unbounded ray hits its near face.
    const auto unbounded = world.raycast(eye, centre - eye);
    CHECK(unbounded.has_value());
    if (unbounded) {
        CHECK_NEAR(unbounded->t, 4.0, 1e-12);
    }

    const gm::LosResult r = gm::line_of_sight(world, eye, {21, centre, 2.0}, {}, &error);
    CHECK(error.empty());
    CHECK(r.visible);
    CHECK(r.triangle == nullptr);
    CHECK_NEAR(r.distance, 5.0, 1e-12);
    std::printf("self-geometry: eye=(0,0,5) center=(5,0,5) radius=2 "
                "raycast_t=4.0 limit=3.0 visible=%d\n",
                static_cast<int>(r.visible));

    // Control: a barrier inside the margin (x = 2 < 3) blocks the same target.
    specs.push_back({{2, -50, -50}, {2, 50, -50}, {2, 50, 100}});
    specs.push_back({{2, -50, -50}, {2, 50, 100}, {2, -50, 100}});
    gm::CollisionWorld blocked_world;
    CHECK(blocked_world.build(make_level(specs), &error));
    const gm::LosResult blocked =
        gm::line_of_sight(blocked_world, eye, {21, centre, 2.0}, {}, &error);
    CHECK(!blocked.visible);
    CHECK_NEAR(blocked.blocker_distance, 2.0, 1e-12);
    CHECK(blocked.blocker_distance < blocked.distance - 2.0);
    std::printf("self-geometry control: barrier_t=%.6f limit=3.0 visible=%d\n",
                blocked.blocker_distance, static_cast<int>(blocked.visible));

    // And the wall behind the target is never reported even with radius 0:
    // at radius 0 the box itself is the first surface, not the wall.
    const gm::LosResult point = gm::line_of_sight(world, eye, {22, centre, 0.0}, {}, &error);
    CHECK(!point.visible);
    CHECK_NEAR(point.blocker_distance, 4.0, 1e-12);
    CHECK(point.triangle_index < 12); // the box, built first
    std::printf("self-geometry radius=0: blocker_t=%.6f tri=%zu (box triangles 0..11)\n",
                point.blocker_distance, point.triangle_index);
}

// --- batch, refusals, determinism -----------------------------------------

void test_batch_equivalence() {
    std::vector<TriSpec> specs;
    add_quad(specs, {-50, -50, -10}, {50, -50, -10}, {50, 50, -10}, {-50, 50, -10});
    add_wall_x(specs, 20.0, 50.0, -10.0, 100.0);
    add_box(specs, {5, 0, 5}, 1.0);
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_level(specs), &error));

    const gm::Vec3d eye{0, 0, 5};
    std::vector<gm::LosTarget> targets;
    for (int i = 0; i < 10; ++i) {
        targets.push_back({100u + static_cast<std::uint32_t>(i),
                           {5.0 * i, 0.0, 5.0}, (i % 3 == 0) ? 2.0 : 0.0});
    }
    const std::vector<gm::LosResult> batch =
        gm::line_of_sight_all(world, eye, targets, {}, &error);
    CHECK(error.empty());
    CHECK(batch.size() == targets.size());

    std::size_t visible = 0;
    std::size_t blocked = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        std::string one_error;
        const gm::LosResult single =
            gm::line_of_sight(world, eye, targets[i], {}, &one_error);
        CHECK(one_error.empty());
        CHECK(batch[i].id == targets[i].id);
        CHECK(batch[i].visible == single.visible);
        CHECK(batch[i].distance == single.distance);
        CHECK(batch[i].triangle == single.triangle);
        CHECK(batch[i].triangle_index == single.triangle_index);
        CHECK(batch[i].blocker == single.blocker);
        CHECK(batch[i].dynamic == single.dynamic);
        CHECK(batch[i].blocker_distance == single.blocker_distance);
        (batch[i].visible ? visible : blocked) += 1;
        std::printf("batch[%zu] id=%u center=(%.1f,0,5) distance=%.6f visible=%d "
                    "blocker_t=%.6f tri=%zu\n",
                    i, batch[i].id, targets[i].center.x, batch[i].distance,
                    static_cast<int>(batch[i].visible), batch[i].blocker_distance,
                    batch[i].triangle_index);
    }
    std::printf("batch: %zu targets, visible=%zu blocked=%zu, all equal to single calls\n",
                targets.size(), visible, blocked);
    CHECK(visible + blocked == targets.size());

    // Determinism: the same calls give bit-identical values.
    for (int repeat = 0; repeat < 3; ++repeat) {
        const std::vector<gm::LosResult> again =
            gm::line_of_sight_all(world, eye, targets, {}, &error);
        for (std::size_t i = 0; i < targets.size(); ++i) {
            CHECK(again[i].visible == batch[i].visible);
            CHECK(again[i].distance == batch[i].distance);
            CHECK(again[i].triangle == batch[i].triangle);
            CHECK(again[i].triangle_index == batch[i].triangle_index);
            CHECK(again[i].blocker_distance == batch[i].blocker_distance);
        }
    }
}

void test_refusals() {
    std::vector<TriSpec> specs;
    add_quad(specs, {-50, -50, -10}, {50, -50, -10}, {50, 50, -10}, {-50, 50, -10});
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_level(specs), &error));
    const gm::Vec3d eye{0, 0, 5};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    auto expect_refusal = [&](const char* what, gm::Vec3d origin, gm::LosTarget target) {
        std::string why;
        const gm::LosResult r = gm::line_of_sight(world, origin, target, {}, &why);
        CHECK(!r.visible);
        CHECK(r.triangle == nullptr);
        CHECK(!why.empty());
        std::printf("refusal %-18s: %s\n", what, why.c_str());
        return why;
    };

    expect_refusal("nan eye", {nan, 0, 5}, {1, {10, 0, 5}, 1.0});
    expect_refusal("inf eye", {0, inf, 5}, {1, {10, 0, 5}, 1.0});
    expect_refusal("nan center", eye, {1, {nan, 0, 5}, 1.0});
    expect_refusal("inf center", eye, {1, {inf, 0, 5}, 1.0});
    expect_refusal("negative radius", eye, {1, {10, 0, 5}, -1.0});
    expect_refusal("nan radius", eye, {1, {10, 0, 5}, nan});
    expect_refusal("inf radius", eye, {1, {10, 0, 5}, inf});

    // Empty world: refused, not "visible" (a failed build must not look clear).
    {
        gm::CollisionWorld empty;
        std::string why;
        const gm::LosResult r = gm::line_of_sight(empty, eye, {1, {10, 0, 5}, 1.0}, {}, &why);
        CHECK(!r.visible);
        CHECK(why.find("no static triangles") != std::string::npos);
        std::printf("refusal empty world     : %s\n", why.c_str());
    }

    // Batch: one bad target does not abort the others; the first message is
    // reported and the good results still equal the single calls.
    {
        const std::vector<gm::LosTarget> targets = {
            {31, {10, 0, 5}, 1.0},  // visible
            {32, {10, 0, 5}, -1.0}, // refused
            {33, {30, 0, 5}, 1.0},  // behind a wall? (none in this world: visible)
        };
        std::string why;
        const std::vector<gm::LosResult> batch =
            gm::line_of_sight_all(world, eye, targets, {}, &why);
        CHECK(batch.size() == 3);
        CHECK(batch[0].visible);
        CHECK(!batch[1].visible);
        CHECK(batch[2].visible); // the synthetic world here has only a floor
        CHECK(!why.empty());
        CHECK(why.find("radius") != std::string::npos);
        std::printf("batch refusal: error='%s'\n", why.c_str());
    }
}

// --- real levels -----------------------------------------------------------

void test_level0() {
    gm::CollisionWorld world;
    if (!load_level("level0.sbw", world)) {
        return;
    }
    const gm::Vec3d eye{24, -208, 58}; // spawn eye, docs/COLLIDE.md §6.2
    std::string error;

    // Open space: a 64-unit scan found the pair; nothing between eye and
    // target at z = 58.
    {
        const gm::Vec3d center{-8, 104, 58};
        const gm::LosResult r = gm::line_of_sight(world, eye, {41, center, 10.0}, {}, &error);
        CHECK(error.empty());
        CHECK(r.visible);
        CHECK_NEAR(r.distance, std::sqrt(32.0 * 32.0 + 312.0 * 312.0), 1e-9);
        CHECK_NEAR(r.distance, 313.636733, 1e-6);
        CHECK(r.triangle == nullptr);
        std::printf("level0 visible: center=(-8,104,58) radius=10 distance=%.9f visible=%d\n",
                    r.distance, static_cast<int>(r.visible));
    }

    // Four walls of the spawn room, target 100 units behind each: the blocker
    // is the wall at the t documented for a straight ray from the same eye.
    struct WallPair {
        gm::Vec3d center;
        double distance;
        double wall_t;
        std::size_t triangle;
    };
    const WallPair pairs[] = {
        {{180, -208, 58}, 156.0, 56.0, 3091},  // +X wall x = 80
        {{-148, -208, 58}, 172.0, 72.0, 2355}, // -X wall x = -48
        {{24, 228, 58}, 436.0, 336.0, 747},    // +Y wall y = 128
        {{24, -388, 58}, 180.0, 80.0, 17},     // -Y wall y = -288
    };
    for (const WallPair& pair : pairs) {
        const gm::LosResult r =
            gm::line_of_sight(world, eye, {42, pair.center, 10.0}, {}, &error);
        CHECK(error.empty());
        CHECK(!r.visible);
        CHECK(r.triangle != nullptr);
        CHECK(r.blocker == gm::kLosWorldBlocker);
        CHECK_NEAR(r.distance, pair.distance, 1e-9);
        CHECK_NEAR(r.blocker_distance, pair.wall_t, 1e-9);
        CHECK(r.blocker_distance < r.distance - 10.0);
        CHECK(r.triangle_index == pair.triangle);
        CHECK(r.triangle == &world.triangles()[r.triangle_index]);
        std::printf("level0 blocked: center=(%.0f,%.0f,%.0f) distance=%.6f blocker_t=%.6f "
                    "tri=%zu limit=%.6f\n",
                    pair.center.x, pair.center.y, pair.center.z, r.distance,
                    r.blocker_distance, r.triangle_index, r.distance - 10.0);
    }

    // A target standing one unit in front of the +X wall with a 20-unit
    // radius is visible: the unbounded ray reaches the wall at t = 56, but the
    // LOS trace stops at 55 - 20 = 35, so neither the wall nor anything inside
    // the target's sphere can be a blocker.
    {
        const gm::Vec3d center{79, -208, 58};
        const auto wall = world.raycast(eye, center - eye);
        CHECK(wall.has_value());
        const gm::LosResult r = gm::line_of_sight(world, eye, {43, center, 20.0}, {}, &error);
        CHECK(r.visible);
        CHECK_NEAR(r.distance, 55.0, 1e-9);
        std::printf("level0 near-wall target: center=(79,-208,58) radius=20 distance=%.6f "
                    "unbounded_ray_t=%.6f limit=%.6f visible=%d\n",
                    r.distance, wall ? wall->t : -1.0, r.distance - 20.0,
                    static_cast<int>(r.visible));
    }

    // Filter changes the answer: the +Y wall is gone with `floors` only, and
    // the horizontal ray then reaches the target.
    {
        const gm::Vec3d center{24, 228, 58};
        const gm::LosResult blocked =
            gm::line_of_sight(world, eye, {44, center, 10.0}, {}, &error);
        CHECK(!blocked.visible);
        gm::CollideFilter floors;
        floors.walls = false;
        floors.ceilings = false;
        const gm::LosResult open =
            gm::line_of_sight(world, eye, {44, center, 10.0}, floors, &error);
        CHECK(open.visible);
        CHECK_NEAR(blocked.distance, open.distance, 0.0);
        std::printf("level0 filter: center=(24,228,58) default visible=%d floors-only visible=%d "
                    "distance=%.6f\n",
                    static_cast<int>(blocked.visible), static_cast<int>(open.visible),
                    open.distance);
    }

    // Batch of 10 real targets: 6 open points (the spawn alcove) and 4 walls.
    {
        std::vector<gm::LosTarget> targets = {
            {51, {-8, 104, 58}, 10.0},
            {52, {56, 104, 58}, 10.0},
            {53, {-8, 40, 58}, 10.0},
            {54, {56, 40, 58}, 10.0},
            {55, {-8, -24, 58}, 10.0},
            {56, {56, -24, 58}, 10.0},
            {57, {180, -208, 58}, 10.0},
            {58, {-148, -208, 58}, 10.0},
            {59, {24, 228, 58}, 10.0},
            {60, {24, -388, 58}, 10.0},
        };
        std::string batch_error;
        const std::vector<gm::LosResult> batch =
            gm::line_of_sight_all(world, eye, targets, {}, &batch_error);
        CHECK(batch_error.empty());
        std::size_t visible = 0;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const gm::LosResult single =
                gm::line_of_sight(world, eye, targets[i], {}, &error);
            CHECK(batch[i].visible == single.visible);
            CHECK(batch[i].distance == single.distance);
            CHECK(batch[i].triangle_index == single.triangle_index);
            CHECK(batch[i].id == targets[i].id);
            visible += batch[i].visible ? 1 : 0;
        }
        std::printf("level0 batch: 10 targets visible=%zu blocked=%zu (single calls identical)\n",
                    visible, targets.size() - visible);
        CHECK(visible == 6);
    }

    // Determinism on the real world.
    {
        const gm::LosTarget target{61, {24, 228, 58}, 10.0};
        const gm::LosResult a = gm::line_of_sight(world, eye, target, {}, &error);
        for (int i = 0; i < 3; ++i) {
            const gm::LosResult b = gm::line_of_sight(world, eye, target, {}, &error);
            CHECK(a.visible == b.visible);
            CHECK(a.distance == b.distance);
            CHECK(a.triangle == b.triangle);
            CHECK(a.triangle_index == b.triangle_index);
            CHECK(a.blocker_distance == b.blocker_distance);
        }
    }
}

void test_dm1() {
    gm::CollisionWorld world;
    if (!load_level("dm1.sbw", world)) {
        return;
    }
    // `--sim dm1` starts the player at (-160,176,64); the eye is +10
    // (docs/SIM.md §«Управление игроком»).
    const gm::Vec3d eye{-160, 176, 74};
    std::string error;

    // Open space found by the same scan: a long hall towards +X/+Y.
    {
        const gm::Vec3d center{2304, 624, 74};
        const gm::LosResult r = gm::line_of_sight(world, eye, {71, center, 10.0}, {}, &error);
        CHECK(error.empty());
        CHECK(r.visible);
        const double expected =
            std::sqrt(2464.0 * 2464.0 + 448.0 * 448.0);
        CHECK_NEAR(r.distance, expected, 1e-9);
        CHECK_NEAR(r.distance, 2504.396135, 1e-6);
        CHECK(r.triangle == nullptr);
        std::printf("dm1 visible: center=(2304,624,74) radius=10 distance=%.9f visible=%d\n",
                    r.distance, static_cast<int>(r.visible));
    }

    // Walls behind the target, derived from the real ray: the blocker t is the
    // wall distance and the target is 100 units further.
    {
        const gm::Vec3d dirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        std::size_t blocked_count = 0;
        for (const gm::Vec3d& dir : dirs) {
            const auto wall = world.raycast(eye, dir);
            CHECK(wall.has_value());
            if (!wall) {
                continue;
            }
            const gm::Vec3d center = eye + gm::normalized(dir) * (wall->t + 100.0);
            const gm::LosResult r =
                gm::line_of_sight(world, eye, {72, center, 10.0}, {}, &error);
            CHECK(error.empty());
            CHECK(!r.visible);
            CHECK_NEAR(r.blocker_distance, wall->t, 1e-9);
            CHECK_NEAR(r.distance, wall->t + 100.0, 1e-9);
            CHECK(r.blocker_distance < r.distance - 10.0);
            CHECK(r.triangle_index < world.triangle_count());
            CHECK(r.triangle == &world.triangles()[r.triangle_index]);
            ++blocked_count;
            std::printf("dm1 blocked: dir=(%.0f,%.0f,%.0f) center=(%.3f,%.3f,%.3f) "
                        "distance=%.6f blocker_t=%.6f tri=%zu\n",
                        dir.x, dir.y, dir.z, center.x, center.y, center.z, r.distance,
                        r.blocker_distance, r.triangle_index);
        }
        CHECK(blocked_count == 4);

        // Exact -Y pair from the scan: wall at t = 399, target 100 beyond.
        const gm::LosResult r =
            gm::line_of_sight(world, eye, {73, {-160, -323, 74}, 10.0}, {}, &error);
        CHECK(!r.visible);
        CHECK_NEAR(r.distance, 499.0, 1e-9);
        CHECK_NEAR(r.blocker_distance, 399.0, 1e-9);
        CHECK(r.triangle_index == 11524);
        std::printf("dm1 exact blocked: center=(-160,-323,74) distance=499.000000 "
                    "blocker_t=399.000000 tri=%zu\n",
                    r.triangle_index);
    }

    // Near-wall target: 1 unit in front of the -Y wall, radius 20.  The
    // unbounded ray hits the wall at t = 399; the LOS trace stops at 278.
    {
        const gm::Vec3d center{-160, -122, 74};
        const auto wall = world.raycast(eye, center - eye);
        CHECK(wall.has_value());
        const gm::LosResult r = gm::line_of_sight(world, eye, {74, center, 20.0}, {}, &error);
        CHECK(r.visible);
        CHECK_NEAR(r.distance, 298.0, 1e-9);
        std::printf("dm1 near-wall target: center=(-160,-122,74) radius=20 distance=%.6f "
                    "unbounded_ray_t=%.6f limit=%.6f visible=%d\n",
                    r.distance, wall ? wall->t : -1.0, r.distance - 20.0,
                    static_cast<int>(r.visible));
    }

    // Filter: only floors, the -X wall no longer blocks.
    {
        const gm::LosResult blocked =
            gm::line_of_sight(world, eye, {75, {-660, 176, 74}, 10.0}, {}, &error);
        CHECK(!blocked.visible);
        gm::CollideFilter floors;
        floors.walls = false;
        floors.ceilings = false;
        const gm::LosResult open =
            gm::line_of_sight(world, eye, {75, {-660, 176, 74}, 10.0}, floors, &error);
        CHECK(open.visible);
        std::printf("dm1 filter: center=(-660,176,74) default visible=%d floors-only visible=%d\n",
                    static_cast<int>(blocked.visible), static_cast<int>(open.visible));
    }
}

} // namespace

int main() {
    test_wall_and_radius_rule();
    test_target_does_not_block_itself();
    test_batch_equivalence();
    test_refusals();
    test_level0();
    test_dm1();
    if (g_failures != 0) {
        std::fprintf(stderr, "line_of_sight: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("line_of_sight: ok\n");
    return 0;
}
