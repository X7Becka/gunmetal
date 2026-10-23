// Kinematic colliders for door leaves and lift cabins
// (docs/MOVING_COLLIDERS.md).
//
// Real part: `level0.sbw` + `level0.sbt` and `dm1.sbw` + `dm1.sbt` from the
// shipped game.  The rest triangles are the door object's triangles as
// `CollisionWorld::build` produces them, the travel is `Door::open -
// Door::closed` (the exact target of the engine lerp `0x44dfc0`, docs/DOOR_FIELDS.md
// §1.2), and the queried rays cross the leaf's face.
//
// Pinned numbers were produced by an independent Python parse (`gm.sbw` +
// `gm.scene`, fan triangulation, Möller-Trumbore):
//   * level0 door (sector 4, object 1): travel (0, 0, 176), 12 triangles,
//     a +X ray from the bounds centre - 40 hits the closed leaf at t = 36,
//     and misses it fully open;
//   * dm1 door (sector 17, object 37): travel (0, 130, 0), 8 triangles,
//     the same ray hits at t = 37.5 and misses open.
//
// The synthetic part covers diagonal travel (level0 sector 23 object 1,
// travel (63, 0, 40)), floor/ceiling/filter selection, NaN/inf refusal,
// negative lift offsets and the empty-set negatives.  The swing block poses
// the real level0 `sec=48 obj=6` leaf: closed it is the rest pose, fully open
// every vertex moved, and a zero swing angle is refused.  The parity block
// checks `MovingCollider` against `CollisionWorld` on the same triangles (rays
// and spheres), so the duplicated sweep in `moving_collider.cpp` cannot drift.
// The detailed swing-door numbers live in `test_swing_door.cpp`.
#include "world/moving_collider.h"

#include "res/sbt.h"
#include "res/sbw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
constexpr double kEps = 1e-9;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

struct LoadedLevel {
    gm::SbtLevel sbt;
    gm::SbwLevel sbw;
    bool ok = false;
};

LoadedLevel load_real(const fs::path& sbt_path, const fs::path& sbw_path) {
    LoadedLevel out;
    std::string error;
    const auto sbt_bytes = read_file(sbt_path);
    const auto sbw_bytes = read_file(sbw_path);
    if (sbt_bytes.empty() || sbw_bytes.empty()) {
        return out;
    }
    const auto sbt = gm::parse_sbt(sbt_bytes, error);
    if (!sbt) {
        std::fprintf(stderr, "parse_sbt(%s): %s\n", sbt_path.c_str(), error.c_str());
        return out;
    }
    const auto sbw = gm::parse_sbw(sbw_bytes, error);
    if (!sbw) {
        std::fprintf(stderr, "parse_sbw(%s): %s\n", sbw_path.c_str(), error.c_str());
        return out;
    }
    out.sbt = *sbt;
    out.sbw = *sbw;
    out.ok = true;
    return out;
}

/// Triangles of one level object as the collision builder sees them.
bool panel_from_world(const gm::CollisionWorld& world, std::uint32_t sector,
                      std::uint32_t object, gm::Vec3d travel, gm::MovingPanel* out) {
    out->rest.clear();
    out->travel = travel;
    for (const gm::Triangle& triangle : world.triangles()) {
        if (triangle.sector == sector && triangle.object == object) {
            out->rest.push_back(triangle);
        }
    }
    return !out->rest.empty();
}

const gm::Door* find_door_slot(const gm::WorldState& state, int sector, int object) {
    for (const gm::Door& door : state.doors()) {
        if (door.sector == sector && door.object == object) {
            return &door;
        }
    }
    return nullptr;
}

/// A level with exactly one object, keeping its real 96-byte header and
/// geometry, so `CollisionWorld::build` produces the same triangles as in the
/// full level (the only difference is sector/object = 0).
gm::SbwLevel single_object_level(const gm::SbwLevel& level, std::uint32_t sector,
                                 std::uint32_t object) {
    gm::SbwLevel out;
    std::size_t geometry_index = 0;
    for (std::uint32_t s = 0; s < sector; ++s) {
        geometry_index += level.header.sectors[s].objects.size();
    }
    geometry_index += object;

    out.header.sectors.resize(1);
    out.header.sectors[0].objects.push_back(level.header.sectors[sector].objects[object]);
    out.header.object_count = 1;
    out.geometry.push_back(level.geometry[geometry_index]);
    return out;
}

void print_vec(const char* label, gm::Vec3d v) {
    std::printf("%s=(%.6f, %.6f, %.6f) ", label, v.x, v.y, v.z);
}

void count_surfaces(const std::vector<gm::Triangle>& triangles, std::size_t* floors,
                    std::size_t* walls, std::size_t* ceilings) {
    *floors = 0;
    *walls = 0;
    *ceilings = 0;
    for (const gm::Triangle& triangle : triangles) {
        switch (gm::surface_of(triangle)) {
        case gm::Surface::floor:
            ++*floors;
            break;
        case gm::Surface::wall:
            ++*walls;
            break;
        case gm::Surface::ceiling:
            ++*ceilings;
            break;
        }
    }
}

/// Check the invariants the task pins: unit normals, a recomputed plane, a
/// centroid/bounds that follow the vertices, and a translation by *delta*.
void check_pose(const gm::MovingCollider& collider, const std::vector<gm::Triangle>& rest,
                gm::Vec3d delta) {
    CHECK(collider.triangle_count() == rest.size());
    if (collider.triangle_count() != rest.size()) {
        return;
    }
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const gm::Triangle& t = collider.triangles()[i];
        const gm::Triangle& r = rest[i];

        CHECK_NEAR(gm::length(t.normal), 1.0, 1e-12);
        CHECK_NEAR(gm::length(t.data_normal), gm::length(r.data_normal), 1e-12);
        if (gm::length(t.data_normal) > 0.0) {
            CHECK_NEAR(gm::length(t.data_normal), 1.0, 1e-12);
        }
        // The plane offset must be exactly the offset of the stored normal.
        CHECK(t.plane_offset == gm::dot(t.normal, t.a));

        CHECK_NEAR(t.centroid.x, (t.a.x + t.b.x + t.c.x) / 3.0, 1e-12);
        CHECK_NEAR(t.centroid.y, (t.a.y + t.b.y + t.c.y) / 3.0, 1e-12);
        CHECK_NEAR(t.centroid.z, (t.a.z + t.b.z + t.c.z) / 3.0, 1e-12);

        CHECK_NEAR(t.bounds.lo.x, std::min({t.a.x, t.b.x, t.c.x}), 0.0);
        CHECK_NEAR(t.bounds.hi.x, std::max({t.a.x, t.b.x, t.c.x}), 0.0);
        CHECK_NEAR(t.bounds.lo.y, std::min({t.a.y, t.b.y, t.c.y}), 0.0);
        CHECK_NEAR(t.bounds.hi.y, std::max({t.a.y, t.b.y, t.c.y}), 0.0);
        CHECK_NEAR(t.bounds.lo.z, std::min({t.a.z, t.b.z, t.c.z}), 0.0);
        CHECK_NEAR(t.bounds.hi.z, std::max({t.a.z, t.b.z, t.c.z}), 0.0);

        // The triangle is the rest triangle translated by `delta`.
        CHECK_NEAR(t.a.x - r.a.x, delta.x, kEps);
        CHECK_NEAR(t.a.y - r.a.y, delta.y, kEps);
        CHECK_NEAR(t.a.z - r.a.z, delta.z, kEps);
        CHECK_NEAR(t.b.x - r.b.x, delta.x, kEps);
        CHECK_NEAR(t.b.y - r.b.y, delta.y, kEps);
        CHECK_NEAR(t.b.z - r.b.z, delta.z, kEps);
        CHECK_NEAR(t.c.x - r.c.x, delta.x, kEps);
        CHECK_NEAR(t.c.y - r.c.y, delta.y, kEps);
        CHECK_NEAR(t.c.z - r.c.z, delta.z, kEps);

        // Normals do not turn under a translation.
        CHECK_NEAR(t.normal.x, r.normal.x, 1e-12);
        CHECK_NEAR(t.normal.y, r.normal.y, 1e-12);
        CHECK_NEAR(t.normal.z, r.normal.z, 1e-12);
        CHECK_NEAR(t.data_normal.x, r.data_normal.x, 1e-12);
        CHECK_NEAR(t.data_normal.y, r.data_normal.y, 1e-12);
        CHECK_NEAR(t.data_normal.z, r.data_normal.z, 1e-12);
        // plane_offset' = dot(n, a) + dot(n, delta).
        CHECK_NEAR(t.plane_offset, r.plane_offset + gm::dot(r.normal, delta), 1e-9);
    }
}

// --- level0: a sliding door in detail --------------------------------------

void test_level0(bool have_level0) {
    if (!have_level0) {
        std::printf("moving_collider: level0 not installed, real part skipped\n");
        return;
    }
    const LoadedLevel level = load_real(kGame / "level0.sbt", kGame / "level0.sbw");
    CHECK(level.ok);
    if (!level.ok) {
        return;
    }
    std::string error;
    gm::CollisionWorld world;
    CHECK(world.build(level.sbw, &error));
    CHECK(error.empty());

    gm::WorldState state;
    CHECK(state.build(level.sbt, level.sbw, &error));
    CHECK(error.empty());

    // level0 door #0: 6 polygons over 8 vertices, authored travel +Z 176.
    const gm::Door* door = find_door_slot(state, 4, 1);
    CHECK(door != nullptr);
    if (door == nullptr) {
        return;
    }
    const gm::Vec3d travel = gm::door_travel(*door);
    const gm::DoorAxis axis = gm::door_axis(*door);
    std::printf("level0 door sec=4 obj=1: closed=(%.3f, %.3f, %.3f) ", door->closed[0],
                door->closed[1], door->closed[2]);
    print_vec("open", {door->open[0], door->open[1], door->open[2]});
    print_vec("travel", travel);
    std::printf("axis=%s\n", gm::door_axis_name(axis));

    CHECK(axis == gm::DoorAxis::Z);
    CHECK_NEAR(travel.x, 0.0, 0.0);
    CHECK_NEAR(travel.y, 0.0, 0.0);
    CHECK_NEAR(travel.z, 176.0, 1e-12);

    gm::MovingPanel panel;
    CHECK(panel_from_world(world, 4, 1, travel, &panel));
    std::printf("  panel triangles: %zu\n", panel.rest.size());
    CHECK(panel.rest.size() == 12);

    gm::MovingCollider collider;
    collider.set_panels({panel}, axis);
    CHECK(collider.ready());
    CHECK(collider.last_update_ok());
    CHECK(collider.panel_count() == 1);
    CHECK(collider.triangle_count() == 12);
    CHECK(collider.panel_of(11) == 0);

    const gm::Aabb rest_bounds = collider.bounds();
    std::printf("  rest bounds: (%.3f, %.3f, %.3f)..(%.3f, %.3f, %.3f)\n", rest_bounds.lo.x,
                rest_bounds.lo.y, rest_bounds.lo.z, rest_bounds.hi.x, rest_bounds.hi.y,
                rest_bounds.hi.z);
    // Object frame: a thin slab at x = 660..668, y = 856..1000, z = -144..136.
    CHECK_NEAR(rest_bounds.lo.x, 660.0, kEps);
    CHECK_NEAR(rest_bounds.hi.x, 668.0, kEps);
    CHECK_NEAR(rest_bounds.lo.y, 856.0, kEps);
    CHECK_NEAR(rest_bounds.hi.y, 1000.0, kEps);
    CHECK_NEAR(rest_bounds.lo.z, -144.0, kEps);
    CHECK_NEAR(rest_bounds.hi.z, 136.0, kEps);

    std::size_t floors = 0;
    std::size_t walls = 0;
    std::size_t ceilings = 0;
    count_surfaces(collider.triangles(), &floors, &walls, &ceilings);
    std::printf("  surfaces: floors=%zu walls=%zu ceilings=%zu\n", floors, walls, ceilings);
    CHECK(floors + walls + ceilings == 12);
    CHECK(walls >= 4); // the two big faces are walls

    // A ray straight through the leaf at its bounds centre.
    const gm::Vec3d centre = rest_bounds.centre();
    const gm::Vec3d origin = centre - gm::Vec3d{40.0, 0.0, 0.0};
    const gm::Vec3d direction{1.0, 0.0, 0.0};

    const auto closed_hit = collider.raycast(origin, direction);
    CHECK(closed_hit.has_value());
    if (closed_hit) {
        std::printf("  closed ray: hit t=%.6f point=(%.3f, %.3f, %.3f) normal=(%.3f, %.3f, "
                    "%.3f) t_index=%u\n",
                    closed_hit->t, closed_hit->point.x, closed_hit->point.y,
                    closed_hit->point.z, closed_hit->normal.x, closed_hit->normal.y,
                    closed_hit->normal.z, closed_hit->triangle_index);
        CHECK_NEAR(closed_hit->t, 36.0, 1e-9);
        CHECK_NEAR(closed_hit->point.x, 660.0, 1e-9);
        CHECK_NEAR(closed_hit->point.y, 928.0, 1e-9);
        CHECK_NEAR(closed_hit->point.z, -4.0, 1e-9);
        CHECK_NEAR(closed_hit->normal.x, -1.0, 1e-12); // against the +X ray
        CHECK_NEAR(closed_hit->normal.y, 0.0, 1e-12);
        CHECK_NEAR(closed_hit->normal.z, 0.0, 1e-12);
        CHECK(gm::surface_of(*closed_hit->triangle) == gm::Surface::wall);
        CHECK(collider.panel_of(closed_hit->triangle_index) == 0);
    }

    const auto closed_sphere = collider.sphere_cast(origin, direction, 8.0);
    CHECK(closed_sphere.has_value());
    if (closed_sphere) {
        std::printf("  closed sphere r=8: t=%.6f centre=(%.3f, %.3f, %.3f) normal=(%.3f, "
                    "%.3f, %.3f)\n",
                    closed_sphere->t, closed_sphere->centre.x, closed_sphere->centre.y,
                    closed_sphere->centre.z, closed_sphere->normal.x,
                    closed_sphere->normal.y, closed_sphere->normal.z);
        CHECK_NEAR(closed_sphere->t, 28.0, 1e-9);
        CHECK_NEAR(closed_sphere->centre.x, 652.0, 1e-9);
        CHECK_NEAR(closed_sphere->normal.x, -1.0, 1e-12);
    }

    // Filters select the leaf by `surface_of`: the ray meets a wall.
    {
        gm::CollideFilter walls_only;
        walls_only.floors = false;
        walls_only.ceilings = false;
        CHECK(collider.raycast(origin, direction, gm::kCollideInfinity, walls_only).has_value());

        gm::CollideFilter no_walls;
        no_walls.walls = false;
        CHECK(!collider.raycast(origin, direction, gm::kCollideInfinity, no_walls).has_value());

        gm::CollideFilter floors_only;
        floors_only.walls = false;
        floors_only.ceilings = false;
        CHECK(!collider.raycast(origin, direction, gm::kCollideInfinity, floors_only).has_value());

        const std::uint32_t here[1] = {4};
        gm::CollideFilter same_sector;
        same_sector.sectors = here;
        CHECK(collider.raycast(origin, direction, gm::kCollideInfinity, same_sector).has_value());
        const std::uint32_t elsewhere[1] = {5};
        gm::CollideFilter other_sector;
        other_sector.sectors = elsewhere;
        CHECK(!collider.raycast(origin, direction, gm::kCollideInfinity, other_sector)
                   .has_value());
    }

    // Fully open: the leaf has moved 176 units up the Z axis, the same ray and
    // the same sphere pass, and every dependent field follows the vertices.
    collider.update(1.0);
    CHECK(collider.last_update_ok());
    CHECK_NEAR(collider.open_fraction(), 1.0, 0.0);
    const gm::Aabb open_bounds = collider.bounds();
    std::printf("  open bounds: (%.3f, %.3f, %.3f)..(%.3f, %.3f, %.3f) dz=%.6f\n",
                open_bounds.lo.x, open_bounds.lo.y, open_bounds.lo.z, open_bounds.hi.x,
                open_bounds.hi.y, open_bounds.hi.z, open_bounds.lo.z - rest_bounds.lo.z);
    CHECK_NEAR(open_bounds.lo.z - rest_bounds.lo.z, 176.0, kEps);
    CHECK_NEAR(open_bounds.hi.z - rest_bounds.hi.z, 176.0, kEps);
    CHECK_NEAR(open_bounds.lo.x, rest_bounds.lo.x, 0.0);
    CHECK_NEAR(open_bounds.lo.y, rest_bounds.lo.y, 0.0);
    check_pose(collider, panel.rest, {0.0, 0.0, 176.0});

    CHECK(!collider.raycast(origin, direction).has_value());
    CHECK(!collider.sphere_cast(origin, direction, 8.0).has_value());
    std::printf("  open ray/sphere: miss (%s)\n",
                (!collider.raycast(origin, direction) &&
                 !collider.sphere_cast(origin, direction, 8.0))
                    ? "confirmed"
                    : "UNEXPECTED HIT");

    // Half-open: the leaf has moved 88 units up, the line at z = -4 is still
    // covered by the slab (its z range is -56..224), so the face is still hit
    // at the same x.  The pose is continuous, not snapped between the ends.
    collider.update(0.5);
    const auto half_hit = collider.raycast(origin, direction);
    std::printf("  half open: hit=%s", half_hit.has_value() ? "yes" : "no");
    if (half_hit) {
        std::printf(" t=%.6f z_min=%.3f", half_hit->t, collider.bounds().lo.z);
    }
    std::printf("\n");
    CHECK(half_hit.has_value());
    if (half_hit) {
        CHECK_NEAR(half_hit->t, 36.0, 1e-9);
    }
    CHECK_NEAR(collider.bounds().lo.z - rest_bounds.lo.z, 88.0, kEps);
    check_pose(collider, panel.rest, {0.0, 0.0, 88.0});

    // Fractions outside [0, 1] are clamped like the engine phase.
    collider.update(2.0);
    CHECK_NEAR(collider.open_fraction(), 1.0, 0.0);
    CHECK(!collider.raycast(origin, direction).has_value());

    // --- negative lift offset: the cabin can go below its rest height.
    collider.update(0.0, -37.0);
    CHECK(collider.last_update_ok());
    const gm::Aabb low = collider.bounds();
    std::printf("  lift -37: bounds z %.3f..%.3f (delta %.3f), centroid z %.3f\n", low.lo.z,
                low.hi.z, low.lo.z - rest_bounds.lo.z, collider.triangles()[0].centroid.z);
    CHECK_NEAR(low.lo.z - rest_bounds.lo.z, -37.0, kEps);
    CHECK_NEAR(low.hi.z - rest_bounds.hi.z, -37.0, kEps);
    check_pose(collider, panel.rest, {0.0, 0.0, -37.0});

    // --- refused updates: the previous pose survives.
    const std::size_t before = collider.triangle_count();
    const gm::Aabb before_bounds = collider.bounds();
    collider.update(std::numeric_limits<double>::quiet_NaN());
    CHECK(!collider.last_update_ok());
    CHECK(!collider.error().empty());
    CHECK(collider.triangle_count() == before);
    CHECK_NEAR(collider.bounds().lo.z, before_bounds.lo.z, 0.0);
    std::printf("  NaN open_fraction: refused (%s), pose kept\n", collider.error().c_str());

    collider.update(0.0, std::numeric_limits<double>::infinity());
    CHECK(!collider.last_update_ok());
    CHECK_NEAR(collider.bounds().lo.z, before_bounds.lo.z, 0.0);
    std::printf("  inf lift_offset: refused (%s)\n", collider.error().c_str());

    // A finite call recovers.
    collider.update(0.0);
    CHECK(collider.last_update_ok());
    CHECK(collider.raycast(origin, direction).has_value());

    // --- empty set: not ready, no hits.
    gm::MovingCollider empty;
    empty.set_panels({}, gm::DoorAxis::Z);
    CHECK(!empty.ready());
    CHECK(!empty.last_update_ok());
    CHECK(empty.triangle_count() == 0);
    CHECK(!empty.raycast({0, 0, 0}, {1, 0, 0}).has_value());
    CHECK(!empty.sphere_cast({0, 0, 0}, {1, 0, 0}, 4.0).has_value());
    empty.update(0.5);
    CHECK(!empty.last_update_ok());
    std::printf("  empty panels: ready=false raycast=miss error=\"%s\"\n",
                empty.error().c_str());

    // --- swing doors are modelled since slice 3: the panel turns about the
    // folded closed corner (`door_swing`), the closed pose is the rest pose.
    const gm::Door* rotating = nullptr;
    for (const gm::Door& candidate : state.doors()) {
        if (gm::door_axis(candidate) == gm::DoorAxis::Rotating) {
            rotating = &candidate;
            break;
        }
    }
    CHECK(rotating != nullptr);
    if (rotating != nullptr) {
        gm::MovingPanel swing;
        CHECK(panel_from_world(world, static_cast<std::uint32_t>(rotating->sector),
                               static_cast<std::uint32_t>(rotating->object),
                               gm::door_travel(*rotating), &swing));
        const gm::DoorSwing spec = gm::door_swing(*rotating);
        gm::MovingCollider swing_collider;
        swing_collider.set_panels({swing}, gm::door_axis(*rotating), spec);
        CHECK(swing_collider.ready());
        CHECK(swing_collider.last_update_ok());
        CHECK(swing_collider.axis() == gm::DoorAxis::Rotating);
        CHECK(swing_collider.swing().max_angle == spec.max_angle);
        // Closed: bit-identical to the rest pose (identity transform).
        CHECK(swing_collider.triangle_count() == swing.rest.size());
        bool same = swing_collider.triangle_count() == swing.rest.size();
        for (std::size_t i = 0; same && i < swing.rest.size(); ++i) {
            const gm::Triangle& a = swing_collider.triangles()[i];
            const gm::Triangle& b = swing.rest[i];
            same = a.a.x == b.a.x && a.a.y == b.a.y && a.a.z == b.a.z &&
                   a.b.x == b.b.x && a.b.y == b.b.y && a.b.z == b.b.z &&
                   a.c.x == b.c.x && a.c.y == b.c.y && a.c.z == b.c.z &&
                   a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
                   a.normal.z == b.normal.z && a.data_normal.x == b.data_normal.x &&
                   a.data_normal.y == b.data_normal.y && a.data_normal.z == b.data_normal.z;
        }
        CHECK(same);
        const gm::Aabb closed_swing = swing_collider.bounds();
        swing_collider.update(1.0);
        CHECK(swing_collider.last_update_ok());
        const gm::Aabb open_swing = swing_collider.bounds();
        bool moved = false;
        for (std::size_t i = 0; i < swing.rest.size(); ++i) {
            moved = moved || swing_collider.triangles()[i].a.x != swing.rest[i].a.x ||
                    swing_collider.triangles()[i].a.y != swing.rest[i].a.y ||
                    swing_collider.triangles()[i].a.z != swing.rest[i].a.z;
        }
        std::printf("  rotating door (sec=%d obj=%d): angle=%.6f rad axis=(%.0f, %.0f, %.0f) "
                    "tris=%zu closed bounds z=%.3f..%.3f open z=%.3f..%.3f moved=%d\n",
                    rotating->sector, rotating->object, spec.max_angle, spec.axis.x,
                    spec.axis.y, spec.axis.z, swing.rest.size(), closed_swing.lo.z,
                    closed_swing.hi.z, open_swing.lo.z, open_swing.hi.z, moved ? 1 : 0);
        CHECK(moved);
        // A zero swing angle is refused instead of posing a still leaf.
        gm::DoorSwing zero = spec;
        zero.max_angle = 0.0;
        gm::MovingCollider no_turn;
        no_turn.set_panels({swing}, gm::DoorAxis::Rotating, zero);
        CHECK(!no_turn.ready());
        CHECK(!no_turn.last_update_ok());
        std::printf("  rotating door with zero angle: refused (%s)\n",
                    no_turn.error().c_str());
    }

    // --- diagonal travel: 105 shipped sliding doors move on more than one
    // axis.  level0 sector 23 object 1 is (63, 0, 40).
    const gm::Door* diagonal = find_door_slot(state, 23, 1);
    CHECK(diagonal != nullptr);
    if (diagonal != nullptr) {
        const gm::Vec3d diagonal_travel = gm::door_travel(*diagonal);
        CHECK(gm::door_axis(*diagonal) == gm::DoorAxis::Travel);
        CHECK_NEAR(diagonal_travel.x, 63.0, 1e-12);
        CHECK_NEAR(diagonal_travel.y, 0.0, 0.0);
        CHECK_NEAR(diagonal_travel.z, 40.0, 1e-12);

        gm::MovingPanel diagonal_panel;
        CHECK(panel_from_world(world, 23, 1, diagonal_travel, &diagonal_panel));
        gm::MovingCollider diagonal_collider;
        diagonal_collider.set_panels({diagonal_panel}, gm::door_axis(*diagonal));
        CHECK(diagonal_collider.ready());
        const gm::Aabb closed_diag = diagonal_collider.bounds();
        diagonal_collider.update(1.0);
        const gm::Aabb open_diag = diagonal_collider.bounds();
        std::printf("  diagonal door sec=23 obj=1: travel=(%.3f, %.3f, %.3f) bounds delta=(%.3f, "
                    "%.3f, %.3f) tris=%zu\n",
                    diagonal_travel.x, diagonal_travel.y, diagonal_travel.z,
                    open_diag.lo.x - closed_diag.lo.x, open_diag.lo.y - closed_diag.lo.y,
                    open_diag.lo.z - closed_diag.lo.z, diagonal_panel.rest.size());
        CHECK_NEAR(open_diag.lo.x - closed_diag.lo.x, 63.0, kEps);
        CHECK_NEAR(open_diag.lo.y - closed_diag.lo.y, 0.0, 0.0);
        CHECK_NEAR(open_diag.lo.z - closed_diag.lo.z, 40.0, kEps);
        check_pose(diagonal_collider, diagonal_panel.rest, {63.0, 0.0, 40.0});
    }
}

// --- dm1: a second sliding door -------------------------------------------

void test_dm1(bool have_dm1) {
    if (!have_dm1) {
        std::printf("moving_collider: dm1 not installed, real part skipped\n");
        return;
    }
    const LoadedLevel level = load_real(kGame / "dm1.sbt", kGame / "dm1.sbw");
    CHECK(level.ok);
    if (!level.ok) {
        return;
    }
    std::string error;
    gm::CollisionWorld world;
    CHECK(world.build(level.sbw, &error));
    CHECK(error.empty());

    gm::WorldState state;
    CHECK(state.build(level.sbt, level.sbw, &error));
    CHECK(error.empty());

    // dm1 sector 17 object 37: 4 polygons over 8 vertices, travel +Y 130.
    const gm::Door* door = find_door_slot(state, 17, 37);
    CHECK(door != nullptr);
    if (door == nullptr) {
        return;
    }
    const gm::Vec3d travel = gm::door_travel(*door);
    const gm::DoorAxis axis = gm::door_axis(*door);
    std::printf("dm1 door sec=17 obj=37: ");
    print_vec("travel", travel);
    std::printf("axis=%s\n", gm::door_axis_name(axis));
    CHECK(axis == gm::DoorAxis::Y);
    CHECK_NEAR(travel.x, 0.0, 0.0);
    CHECK_NEAR(travel.y, 130.0, 1e-12);
    CHECK_NEAR(travel.z, 0.0, 0.0);

    gm::MovingPanel panel;
    CHECK(panel_from_world(world, 17, 37, travel, &panel));
    CHECK(panel.rest.size() == 8);

    gm::MovingCollider collider;
    collider.set_panels({panel}, axis);
    CHECK(collider.ready());
    const gm::Aabb rest_bounds = collider.bounds();
    CHECK_NEAR(rest_bounds.lo.x, 8002.0, kEps);
    CHECK_NEAR(rest_bounds.hi.x, 8007.0, kEps);
    CHECK_NEAR(rest_bounds.lo.y, 1648.0, kEps);
    CHECK_NEAR(rest_bounds.hi.y, 1787.0, kEps);
    CHECK_NEAR(rest_bounds.lo.z, 132.0, kEps);
    CHECK_NEAR(rest_bounds.hi.z, 225.0, kEps);

    const gm::Vec3d centre = rest_bounds.centre();
    const gm::Vec3d origin = centre - gm::Vec3d{40.0, 0.0, 0.0};
    const gm::Vec3d direction{1.0, 0.0, 0.0};

    const auto closed_ray = collider.raycast(origin, direction);
    CHECK(closed_ray.has_value());
    if (closed_ray) {
        std::printf("  closed ray: t=%.6f point=(%.3f, %.3f, %.3f) normal=(%.3f, %.3f, %.3f)\n",
                    closed_ray->t, closed_ray->point.x, closed_ray->point.y, closed_ray->point.z,
                    closed_ray->normal.x, closed_ray->normal.y, closed_ray->normal.z);
        CHECK_NEAR(closed_ray->t, 37.5, 1e-9);
        CHECK_NEAR(closed_ray->normal.x, -1.0, 1e-12);
    }
    const auto closed_sphere = collider.sphere_cast(origin, direction, 5.0);
    CHECK(closed_sphere.has_value());
    if (closed_sphere) {
        std::printf("  closed sphere r=5: t=%.6f normal=(%.3f, %.3f, %.3f)\n", closed_sphere->t,
                    closed_sphere->normal.x, closed_sphere->normal.y, closed_sphere->normal.z);
        CHECK_NEAR(closed_sphere->t, 32.5, 1e-9);
    }

    collider.update(1.0);
    CHECK(!collider.raycast(origin, direction).has_value());
    CHECK(!collider.sphere_cast(origin, direction, 5.0).has_value());
    const gm::Aabb open_bounds = collider.bounds();
    std::printf("  open: dy=%.3f, ray/sphere miss\n", open_bounds.lo.y - rest_bounds.lo.y);
    CHECK_NEAR(open_bounds.lo.y - rest_bounds.lo.y, 130.0, kEps);
    check_pose(collider, panel.rest, {0.0, 130.0, 0.0});
}

// --- synthetic: surface filters, and a platform that only lifts ------------

void test_synthetic() {
    // A floor at z = 0 and a wall at x = 10, both with a stored +Z/+X normal
    // so `surface_of` classifies them the way the shipped data does.
    gm::MovingPanel platform;
    platform.travel = {0.0, 0.0, 0.0};
    gm::Triangle floor;
    floor.a = {-20.0, -20.0, 0.0};
    floor.b = {20.0, -20.0, 0.0};
    floor.c = {20.0, 20.0, 0.0};
    floor.normal = gm::normalized(gm::cross(floor.b - floor.a, floor.c - floor.a));
    floor.data_normal = {0.0, 0.0, 1.0};
    floor.plane_offset = gm::dot(floor.normal, floor.a);
    floor.centroid = (floor.a + floor.b + floor.c) * (1.0 / 3.0);
    floor.bounds.expand(floor.a);
    floor.bounds.expand(floor.b);
    floor.bounds.expand(floor.c);
    floor.sector = 7;
    gm::Triangle wall;
    wall.a = {10.0, -20.0, 0.0};
    wall.b = {10.0, 20.0, 0.0};
    wall.c = {10.0, 20.0, 10.0};
    wall.normal = gm::normalized(gm::cross(wall.b - wall.a, wall.c - wall.a));
    wall.data_normal = {1.0, 0.0, 0.0};
    wall.plane_offset = gm::dot(wall.normal, wall.a);
    wall.centroid = (wall.a + wall.b + wall.c) * (1.0 / 3.0);
    wall.bounds.expand(wall.a);
    wall.bounds.expand(wall.b);
    wall.bounds.expand(wall.c);
    wall.sector = 8;
    platform.rest = {floor, wall};

    gm::MovingCollider collider;
    collider.set_panels({platform}, gm::DoorAxis::Travel);
    CHECK(collider.ready());
    CHECK(gm::surface_of(collider.triangles()[0]) == gm::Surface::floor);
    CHECK(gm::surface_of(collider.triangles()[1]) == gm::Surface::wall);

    const gm::Vec3d down{0.0, 0.0, -1.0};
    const gm::Vec3d up{0.0, 0.0, 1.0};
    gm::CollideFilter floors;
    floors.walls = false;
    floors.ceilings = false;
    gm::CollideFilter walls;
    walls.floors = false;
    walls.ceilings = false;

    CHECK(collider.raycast({0, 0, 5}, down).has_value());
    CHECK(collider.raycast({0, 0, 5}, down, gm::kCollideInfinity, floors).has_value());
    CHECK(!collider.raycast({0, 0, 5}, down, gm::kCollideInfinity, walls).has_value());
    CHECK(collider.raycast({0, 0, 5}, {1, 0, 0}).has_value());
    CHECK(!collider.raycast({0, 0, 5}, {1, 0, 0}, gm::kCollideInfinity, floors).has_value());
    CHECK(collider.raycast({0, 0, 5}, {1, 0, 0}, gm::kCollideInfinity, walls).has_value());
    // The sector filter still reaches into the triangle fields.
    const std::uint32_t sector7[1] = {7};
    const std::uint32_t sector8[1] = {8};
    gm::CollideFilter only7;
    only7.sectors = sector7;
    gm::CollideFilter only8;
    only8.sectors = sector8;
    CHECK(collider.raycast({0, 0, 5}, down, gm::kCollideInfinity, only7).has_value());
    CHECK(!collider.raycast({0, 0, 5}, {1, 0, 0}, gm::kCollideInfinity, only7).has_value());
    CHECK(collider.raycast({0, 0, 5}, {1, 0, 0}, gm::kCollideInfinity, only8).has_value());

    // A platform lifts the whole panel along +Z; negative goes down.
    const gm::Aabb rest = collider.bounds();
    collider.update(0.0, 120.0);
    CHECK_NEAR(collider.bounds().lo.z - rest.lo.z, 120.0, 0.0);
    CHECK_NEAR(collider.bounds().hi.z - rest.hi.z, 120.0, 0.0);
    CHECK_NEAR(collider.triangles()[0].centroid.z, floor.centroid.z + 120.0, 1e-12);
    const auto lifted = collider.raycast({0, 0, 130}, down);
    CHECK(lifted.has_value());
    if (lifted) {
        std::printf("synthetic platform: lift +120 -> floor ray t=%.6f (z=%.3f), rest z=%.3f\n",
                    lifted->t, lifted->point.z, floor.a.z);
        CHECK_NEAR(lifted->t, 10.0, 1e-9);
        CHECK_NEAR(lifted->point.z, 120.0, 1e-9);
    }
    collider.update(0.0, -60.0);
    CHECK_NEAR(collider.bounds().lo.z - rest.lo.z, -60.0, 0.0);
    std::printf("synthetic platform: lift -60 -> bounds z=%.3f\n", collider.bounds().lo.z);

    // Query guards are the `CollisionWorld` ones.
    CHECK(!collider.raycast({0, 0, 5}, {0, 0, 0}).has_value());
    CHECK(!collider.raycast({0, 0, 5}, {0, 0, -1}, 0.0).has_value());
    CHECK(!collider.raycast({std::numeric_limits<double>::quiet_NaN(), 0, 5}, down).has_value());
    CHECK(!collider.sphere_cast({0, 0, 5}, down, -1.0).has_value());
    CHECK(!collider.sphere_cast({0, 0, 5}, down, std::numeric_limits<double>::quiet_NaN())
               .has_value());
    CHECK(!collider.sphere_cast({0, 0, 5}, {0, 0, 0}, 4.0).has_value());

    // Non-finite rest geometry is refused at `set_panels` time.
    gm::MovingPanel broken = platform;
    broken.rest[0].a.x = std::numeric_limits<double>::quiet_NaN();
    gm::MovingCollider refused;
    refused.set_panels({broken}, gm::DoorAxis::Travel);
    CHECK(!refused.ready());
    CHECK(!refused.last_update_ok());
    std::printf("synthetic: NaN rest vertex refused (\"%s\")\n", refused.error().c_str());
}

// --- parity with CollisionWorld on the same triangles ----------------------

std::uint64_t g_rng = 0x243f6a8885a308d3ull;

double next_unit() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<double>(g_rng >> 11) * (1.0 / 9007199254740992.0);
}

double next_range(double lo, double hi) {
    return lo + (hi - lo) * next_unit();
}

void test_parity_with_collision_world(bool have_level0) {
    if (!have_level0) {
        std::printf("moving_collider: parity skipped (no level0)\n");
        return;
    }
    const LoadedLevel level = load_real(kGame / "level0.sbt", kGame / "level0.sbw");
    CHECK(level.ok);
    if (!level.ok) {
        return;
    }
    std::string error;
    gm::CollisionWorld reference;
    CHECK(reference.build(single_object_level(level.sbw, 4, 1), &error));
    CHECK(error.empty());

    gm::MovingPanel panel;
    panel.travel = {0.0, 0.0, 176.0};
    panel.rest = reference.triangles();
    CHECK(panel.rest.size() == reference.triangle_count());

    gm::MovingCollider collider;
    collider.set_panels({panel}, gm::DoorAxis::Z);
    CHECK(collider.ready());

    const gm::Aabb box = reference.bounds();
    const gm::Vec3d centre = box.centre();
    std::size_t ray_cases = 0;
    std::size_t ray_hits = 0;
    std::size_t sphere_cases = 0;
    std::size_t sphere_hits = 0;
    double worst_ray_t = 0.0;
    double worst_ray_normal = 0.0;
    double worst_sphere_t = 0.0;
    double worst_sphere_normal = 0.0;

    for (int i = 0; i < 128; ++i) {
        const gm::Vec3d origin = centre + gm::Vec3d{next_range(-260.0, 260.0),
                                                    next_range(-320.0, 320.0),
                                                    next_range(-320.0, 320.0)};
        const gm::Vec3d target = centre + gm::Vec3d{next_range(-60.0, 60.0),
                                                    next_range(-80.0, 80.0),
                                                    next_range(-140.0, 140.0)};
        const gm::Vec3d direction = target - origin;
        if (gm::length(direction) <= 1e-9) {
            continue;
        }
        const double max_t = next_range(1.0, 800.0);
        const auto want = reference.raycast(origin, direction, max_t);
        const auto got = collider.raycast(origin, direction, max_t);
        ++ray_cases;
        CHECK(want.has_value() == got.has_value());
        if (want.has_value() != got.has_value()) {
            continue;
        }
        if (want) {
            ++ray_hits;
            worst_ray_t = std::max(worst_ray_t, std::fabs(want->t - got->t));
            worst_ray_normal =
                std::max(worst_ray_normal, gm::length(want->normal - got->normal));
            CHECK_NEAR(want->t, got->t, 1e-12);
            CHECK_NEAR(want->normal.x, got->normal.x, 1e-12);
            CHECK_NEAR(want->normal.y, got->normal.y, 1e-12);
            CHECK_NEAR(want->normal.z, got->normal.z, 1e-12);
            CHECK_NEAR(want->point.x, got->point.x, 1e-12);
            CHECK_NEAR(want->point.y, got->point.y, 1e-12);
            CHECK_NEAR(want->point.z, got->point.z, 1e-12);
            CHECK(want->front_face == got->front_face);
        }

        const double radius = (i % 3 == 0) ? 2.0 : ((i % 3 == 1) ? 7.0 : 20.0);
        const auto want_sphere = reference.sphere_cast(origin, direction, radius, max_t);
        const auto got_sphere = collider.sphere_cast(origin, direction, radius, max_t);
        ++sphere_cases;
        CHECK(want_sphere.has_value() == got_sphere.has_value());
        if (want_sphere.has_value() != got_sphere.has_value()) {
            continue;
        }
        if (want_sphere) {
            ++sphere_hits;
            worst_sphere_t = std::max(worst_sphere_t, std::fabs(want_sphere->t - got_sphere->t));
            worst_sphere_normal = std::max(
                worst_sphere_normal, gm::length(want_sphere->normal - got_sphere->normal));
            CHECK_NEAR(want_sphere->t, got_sphere->t, 1e-12);
            CHECK_NEAR(want_sphere->normal.x, got_sphere->normal.x, 1e-12);
            CHECK_NEAR(want_sphere->normal.y, got_sphere->normal.y, 1e-12);
            CHECK_NEAR(want_sphere->normal.z, got_sphere->normal.z, 1e-12);
            CHECK_NEAR(want_sphere->point.x, got_sphere->point.x, 1e-12);
            CHECK_NEAR(want_sphere->point.y, got_sphere->point.y, 1e-12);
            CHECK_NEAR(want_sphere->point.z, got_sphere->point.z, 1e-12);
        }
    }
    std::printf("parity with CollisionWorld (level0 sec=4 obj=1, %zu triangles): "
                "rays %zu cases / %zu hits (max |dt|=%.3g, max |dn|=%.3g); "
                "spheres %zu cases / %zu hits (max |dt|=%.3g, max |dn|=%.3g)\n",
                panel.rest.size(), ray_cases, ray_hits, worst_ray_t, worst_ray_normal,
                sphere_cases, sphere_hits, worst_sphere_t, worst_sphere_normal);
    CHECK(ray_hits > 10);
    CHECK(sphere_hits > 10);
}

} // namespace

int main() {
    const bool have_level0 = fs::exists(kGame / "level0.sbw") && fs::exists(kGame / "level0.sbt");
    const bool have_dm1 = fs::exists(kGame / "dm1.sbw") && fs::exists(kGame / "dm1.sbt");

    test_synthetic();
    test_level0(have_level0);
    test_dm1(have_dm1);
    test_parity_with_collision_world(have_level0);

    if (g_failures != 0) {
        std::fprintf(stderr, "moving_collider: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("moving_collider: ok\n");
    return 0;
}
