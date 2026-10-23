// Swing doors as kinematic colliders (M6.4.1, slice 3;
// docs/MOVING_COLLIDERS.md, docs/DOOR_COLLISION.md).
//
// `0x44dfc0` lerps `center = closed + (open - closed) * phase/65535`, picks a
// rotation axis from `A1..A3` (record `+0x48`, priority z > x > y, unit vector
// -x / +y / -z) and calls `0x434887`, which writes the basis as `R(-angle)`
// about that axis and `origin = center - basis^T * size`.  Folding the rest
// pose (object origin `D1..D3`, basis identity) in gives
//
//     world(f) = center(f) + R(angle(f)) * (rest - closed)
//
// with `angle = A_i * phase` and `closed = D1..D3 + D7..D9` (the loader folds
// the size into both corners, `0x44e368..0x44e3f6`).  `MovingCollider` rotates
// every vertex and both normals by that matrix.
//
// Real data (read from the shipped `.sbt`/`.sbw`, never guessed):
//   * level0 `sec=48 obj=6`: A = (2.094395102, 0, 0) -> axis -x, 120 deg,
//     `open == closed` (pure swing), 12 polygons / 16 vertices;
//   * dm1 `sec=17 obj=55`: A = (0, 0, -1.570796327) -> axis -z, -90 deg,
//     `open == closed`, a 8 x 174 x 72 leaf hinged at (7034, 240, 182).
//
// The test pins:
//   * f = 0: the pose is the `rest` pose bit for bit (triangle fields, bounds,
//     and ray/sphere hits against a `CollisionWorld` built from the same
//     triangles);
//   * f = 0.5: every vertex is the rest vertex rotated by half the angle about
//     the pivot (checked against `door_transform_at` at the full phase and by
//     the angle between the radial vectors), three edge lengths are preserved,
//     and a plane that contained the pivot at rest still does (a plane keeps
//     its distance to the pivot; for the dm1 leaf two faces pass through it);
//   * f = 1: the ray and sphere that crossed the closed leaf miss the swung
//     one, and the vertices equal `door_transform_at(0xffff)`;
//   * the player: `gm::step` walking into the closed leaf stops at its face
//     (`wall_ticks > 0`), the same run walks through the open pose;
//   * the runtime publishes the swing pair (suppressed static, posed dynamic)
//     and updates it when the door phase changes;
//   * negatives: a zero swing angle, a non-unit axis and a NaN fraction are
//     refused without a crash and keep the last good pose.
//
// Every number below is printed, so the report can quote the run verbatim.
#include "game/runtime.h"

#include "player/controller.h"
#include "render/collide.h"
#include "res/level.h"
#include "res/sbt.h"
#include "res/sbtvm.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "world/doors.h"
#include "world/moving_collider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
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
constexpr double kPi = 3.14159265358979323846;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

struct LevelData {
    gm::SbtLevel sbt;
    gm::SbwLevel sbw;
    bool ok = false;
};

LevelData load_level_data(const std::string& name) {
    LevelData out;
    std::string error;
    const auto sbt_bytes = read_file(kGame / (name + ".sbt"));
    const auto sbw_bytes = read_file(kGame / (name + ".sbw"));
    if (sbt_bytes.empty() || sbw_bytes.empty()) {
        return out;
    }
    const auto sbt = gm::parse_sbt(sbt_bytes, error);
    if (!sbt) {
        std::fprintf(stderr, "parse_sbt(%s): %s\n", name.c_str(), error.c_str());
        return out;
    }
    const auto sbw = gm::parse_sbw(sbw_bytes, error);
    if (!sbw) {
        std::fprintf(stderr, "parse_sbw(%s): %s\n", name.c_str(), error.c_str());
        return out;
    }
    out.sbt = *sbt;
    out.sbw = *sbw;
    out.ok = true;
    return out;
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

const gm::Door* find_door(const gm::WorldState& state, int sector, int object) {
    for (const gm::Door& door : state.doors()) {
        if (door.sector == sector && door.object == object) {
            return &door;
        }
    }
    return nullptr;
}

bool triangle_identical(const gm::Triangle& a, const gm::Triangle& b) {
    return a.a.x == b.a.x && a.a.y == b.a.y && a.a.z == b.a.z && a.b.x == b.b.x &&
           a.b.y == b.b.y && a.b.z == b.b.z && a.c.x == b.c.x && a.c.y == b.c.y &&
           a.c.z == b.c.z && a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
           a.normal.z == b.normal.z && a.data_normal.x == b.data_normal.x &&
           a.data_normal.y == b.data_normal.y && a.data_normal.z == b.data_normal.z &&
           a.plane_offset == b.plane_offset && a.centroid.x == b.centroid.x &&
           a.centroid.y == b.centroid.y && a.centroid.z == b.centroid.z &&
           a.bounds.lo.x == b.bounds.lo.x && a.bounds.lo.y == b.bounds.lo.y &&
           a.bounds.lo.z == b.bounds.lo.z && a.bounds.hi.x == b.bounds.hi.x &&
           a.bounds.hi.y == b.bounds.hi.y && a.bounds.hi.z == b.bounds.hi.z;
}

bool aabb_identical(const gm::Aabb& a, const gm::Aabb& b) {
    return a.lo.x == b.lo.x && a.lo.y == b.lo.y && a.lo.z == b.lo.z && a.hi.x == b.hi.x &&
           a.hi.y == b.hi.y && a.hi.z == b.hi.z;
}

/// `0x434887`'s transform at an explicit phase, applied to a rest world point:
/// `world = origin + basis^T * local`, `local = rest - (closed - size)` because
/// the rest object origin is `closed - size` and its basis is the identity.
gm::Vec3d transform_point(const gm::DoorTransform& transform, const gm::Door& door,
                          gm::Vec3d rest) {
    const gm::Vec3d local{rest.x - (door.closed[0] - door.size[0]),
                          rest.y - (door.closed[1] - door.size[1]),
                          rest.z - (door.closed[2] - door.size[2])};
    double x = transform.origin[0];
    double y = transform.origin[1];
    double z = transform.origin[2];
    for (int k = 0; k < 3; ++k) {
        const double component = k == 0 ? local.x : (k == 1 ? local.y : local.z);
        x += transform.basis[static_cast<std::size_t>(k * 3 + 0)] * component;
        y += transform.basis[static_cast<std::size_t>(k * 3 + 1)] * component;
        z += transform.basis[static_cast<std::size_t>(k * 3 + 2)] * component;
    }
    return {x, y, z};
}

double edge_length(gm::Vec3d a, gm::Vec3d b) { return gm::length(b - a); }

/// Angle between the radial vectors of a vertex before and after the pose,
/// measured in the plane perpendicular to the rotation axis (the axial
/// component is invariant and would dilute the angle).
double radial_angle(gm::Vec3d before, gm::Vec3d after, gm::Vec3d pivot, gm::Vec3d axis) {
    gm::Vec3d u = before - pivot;
    gm::Vec3d v = after - pivot;
    u = u - axis * gm::dot(axis, u);
    v = v - axis * gm::dot(axis, v);
    const double lu = gm::length(u);
    const double lv = gm::length(v);
    if (!(lu > 1e-9) || !(lv > 1e-9)) {
        return -1.0;
    }
    const double c = std::clamp(gm::dot(u, v) / (lu * lv), -1.0, 1.0);
    return std::acos(c);
}

struct WalkResult {
    gm::Vec3d start{};
    gm::Vec3d end{};
    int wall_ticks = 0;
    bool ok = false;
};

/// `gm::step` for `ticks` ticks with `forward = 1` along `yaw` and no gravity,
/// so the run stays on the leaf's height band (same helper as
/// `test_door_collision.cpp`).
WalkResult walk(gm::CollisionWorld& world, const gm::Vec3d& feet, double yaw, int ticks) {
    WalkResult out;
    gm::ControllerConfig config;
    config.gravity = 0.0;
    gm::PlayerState state = gm::make_state(feet, yaw, config);
    gm::Input input;
    input.forward = 1.0;
    out.start = state.pos;
    out.ok = true;
    for (int i = 0; i < ticks; ++i) {
        const gm::StepResult step = gm::step(state, input, world, config);
        if (!step.ok) {
            std::fprintf(stderr, "step failed at tick %d: %s\n", i, step.error.c_str());
            out.ok = false;
            break;
        }
        state = step.state;
        if (step.hit_wall) {
            ++out.wall_ticks;
        }
    }
    out.end = state.pos;
    return out;
}

/// One real swing door exercised in isolation: the single-object world holds
/// exactly the leaf's triangles, so the player run has no other geometry.
struct SwingCase {
    std::string level;
    int sector = 0;
    int object = 0;
};

void run_swing_case(const LevelData& level, const SwingCase& swing_case, bool runtime_check) {
    std::string error;
    gm::WorldState state;
    CHECK(state.build(level.sbt, level.sbw, &error));
    if (!error.empty()) {
        std::fprintf(stderr, "world build: %s\n", error.c_str());
    }
    const gm::Door* found = find_door(state, swing_case.sector, swing_case.object);
    CHECK(found != nullptr);
    if (found == nullptr) {
        return;
    }
    const gm::Door door = *found;
    const gm::DoorAxis axis = gm::door_axis(door);
    const gm::DoorSwing spec = gm::door_swing(door);
    const gm::Vec3d pivot = spec.pivot;

    std::printf("swing_door: %s sec=%d obj=%d: A=(%.9f, %.9f, %.9f) axis=(%.0f, %.0f, %.0f) "
                "angle=%.9f rad (%.3f deg) pivot=(%.3f, %.3f, %.3f) travel=(%.3f, %.3f, "
                "%.3f) kind=%d step=%d\n",
                swing_case.level.c_str(), door.sector, door.object,
                door.axis[0] * static_cast<double>(gm::kDoorPhaseMax),
                door.axis[1] * static_cast<double>(gm::kDoorPhaseMax),
                door.axis[2] * static_cast<double>(gm::kDoorPhaseMax), spec.axis.x, spec.axis.y,
                spec.axis.z, spec.max_angle, spec.max_angle * 180.0 / kPi, pivot.x, pivot.y,
                pivot.z, gm::door_travel(door).x, gm::door_travel(door).y,
                gm::door_travel(door).z, door.kind, door.step);
    CHECK(axis == gm::DoorAxis::Rotating);
    CHECK(spec.max_angle != 0.0);
    CHECK_NEAR(gm::length(spec.axis), 1.0, 1e-12);

    gm::CollisionWorld world;
    CHECK(world.build(single_object_level(level.sbw, static_cast<std::uint32_t>(swing_case.sector),
                                         static_cast<std::uint32_t>(swing_case.object)),
                      &error));
    CHECK(error.empty());
    const std::vector<gm::Triangle>& rest = world.triangles();
    CHECK(!rest.empty());

    gm::MovingPanel panel;
    panel.travel = gm::door_travel(door);
    panel.rest = rest;
    gm::MovingCollider collider;
    collider.set_panels({panel}, axis, spec);
    CHECK(collider.ready());
    CHECK(collider.last_update_ok());
    CHECK(collider.triangle_count() == rest.size());
    std::printf("  rest: %zu triangles, bounds (%.3f, %.3f, %.3f)..(%.3f, %.3f, %.3f)\n",
                rest.size(), world.bounds().lo.x, world.bounds().lo.y, world.bounds().lo.z,
                world.bounds().hi.x, world.bounds().hi.y, world.bounds().hi.z);

    // --- f = 0: bit-for-bit the rest pose --------------------------------
    bool identical = true;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        identical = identical && triangle_identical(collider.triangles()[i], rest[i]);
    }
    CHECK(identical);
    CHECK(aabb_identical(collider.bounds(), world.bounds()));

    // The query pair that crosses the leaf in the closed pose: the centre of
    // the largest triangle along its normal.  The same ray/sphere is reused
    // for the open-miss check below.
    std::size_t widest = 0;
    double widest_area = -1.0;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const double area = gm::length(gm::cross(rest[i].b - rest[i].a, rest[i].c - rest[i].a));
        if (area > widest_area) {
            widest_area = area;
            widest = i;
        }
    }
    const gm::Vec3d target = rest[widest].centroid;
    const gm::Vec3d normal = rest[widest].normal;
    const gm::Vec3d ray_origin = target + normal * 30.0;
    const gm::Vec3d ray_direction = -normal;
    std::printf("  closed ray: triangle %zu centroid=(%.3f, %.3f, %.3f) normal=(%.3f, %.3f, "
                "%.3f)\n",
                widest, target.x, target.y, target.z, normal.x, normal.y, normal.z);

    const auto static_ray = world.raycast(ray_origin, ray_direction, 120.0);
    const auto static_sphere = world.sphere_cast(ray_origin, ray_direction, 5.0, 120.0);
    const auto closed_ray = collider.raycast(ray_origin, ray_direction, 120.0);
    const auto closed_sphere = collider.sphere_cast(ray_origin, ray_direction, 5.0, 120.0);
    CHECK(static_ray.has_value());
    CHECK(closed_ray.has_value());
    CHECK(static_sphere.has_value());
    CHECK(closed_sphere.has_value());
    if (static_ray && closed_ray) {
        std::printf("  closed ray: t=%.17g index=%u == static t=%.17g index=%u (bit-identical)\n",
                    closed_ray->t, closed_ray->triangle_index, static_ray->t,
                    static_ray->triangle_index);
        CHECK(closed_ray->t == static_ray->t);
        CHECK(closed_ray->point.x == static_ray->point.x);
        CHECK(closed_ray->point.y == static_ray->point.y);
        CHECK(closed_ray->point.z == static_ray->point.z);
        CHECK(closed_ray->normal.x == static_ray->normal.x);
        CHECK(closed_ray->normal.y == static_ray->normal.y);
        CHECK(closed_ray->normal.z == static_ray->normal.z);
        CHECK(closed_ray->triangle_index == static_ray->triangle_index);
    }
    if (static_sphere && closed_sphere) {
        std::printf("  closed sphere r=5: t=%.17g index=%u == static t=%.17g (bit-identical)\n",
                    closed_sphere->t, closed_sphere->triangle_index, static_sphere->t);
        CHECK(closed_sphere->t == static_sphere->t);
        CHECK(closed_sphere->point.x == static_sphere->point.x);
        CHECK(closed_sphere->point.y == static_sphere->point.y);
        CHECK(closed_sphere->point.z == static_sphere->point.z);
        CHECK(closed_sphere->triangle_index == static_sphere->triangle_index);
    }

    // --- f = 1: matches `door_transform_at(0xffff)` ----------------------
    collider.update(1.0);
    CHECK(collider.last_update_ok());
    const gm::DoorTransform full = gm::door_transform_at(door, gm::kDoorPhaseMax);
    CHECK(full.rotating());
    double worst_full = 0.0;
    double max_displacement = 0.0;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const gm::Triangle& t = collider.triangles()[i];
        for (int k = 0; k < 3; ++k) {
            const gm::Vec3d p = k == 0 ? rest[i].a : (k == 1 ? rest[i].b : rest[i].c);
            const gm::Vec3d got = k == 0 ? t.a : (k == 1 ? t.b : t.c);
            const gm::Vec3d want = transform_point(full, door, p);
            worst_full = std::max(worst_full, gm::length(got - want));
            max_displacement = std::max(max_displacement, gm::length(got - p));
        }
        CHECK_NEAR(gm::length(t.normal), 1.0, 1e-12);
        if (gm::length(rest[i].data_normal) > 0.0) {
            CHECK_NEAR(gm::length(t.data_normal), 1.0, 1e-12);
        }
        CHECK(t.plane_offset == gm::dot(t.normal, t.a));
    }
    std::printf("  open f=1: max |vertex - door_transform_at(0xffff)| = %.3g, max vertex "
                "displacement = %.6f\n",
                worst_full, max_displacement);
    CHECK(worst_full <= 1e-9);
    CHECK(max_displacement > 1e-6);

    const auto open_ray = collider.raycast(ray_origin, ray_direction, 120.0);
    const auto open_sphere = collider.sphere_cast(ray_origin, ray_direction, 5.0, 120.0);
    std::printf("  open f=1: ray=%s sphere=%s (the closed pose was hit at t=%.6f and "
                "t=%.6f)\n",
                open_ray.has_value() ? "HIT" : "miss", open_sphere.has_value() ? "HIT" : "miss",
                static_ray ? static_ray->t : -1.0, static_sphere ? static_sphere->t : -1.0);
    CHECK(!open_ray.has_value());
    CHECK(!open_sphere.has_value());

    // --- f = 0.5: half the angle, edges and planes -----------------------
    collider.update(0.5);
    CHECK(collider.last_update_ok());
    const gm::Vec3d center_half{
        pivot.x + panel.travel.x * 0.5, pivot.y + panel.travel.y * 0.5,
        pivot.z + panel.travel.z * 0.5};
    std::size_t edge_checks = 0;
    std::size_t plane_checks = 0;
    std::size_t plane_through_center = 0;
    double worst_edge = 0.0;
    double worst_plane = 0.0;
    double worst_plane_center = 0.0;
    double worst_half_angle = 0.0;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const gm::Triangle& r = rest[i];
        const gm::Triangle& t = collider.triangles()[i];
        // Edge lengths are invariant under a rigid rotation (3 edges of every
        // triangle that has a non-degenerate first edge).
        const double before = edge_length(r.a, r.b);
        if (before > 1e-6) {
            worst_edge = std::max(worst_edge, std::fabs(edge_length(t.a, t.b) - before));
            if (edge_checks < 3) {
                ++edge_checks;
            }
        }
        // The plane keeps its distance to the pivot; when that distance is 0
        // (a face through the hinge line) the plane still contains the centre.
        const double rest_distance = gm::dot(r.normal, pivot - r.a);
        const double open_distance = gm::dot(t.normal, center_half - t.a);
        worst_plane = std::max(worst_plane, std::fabs(open_distance - rest_distance));
        if (std::fabs(rest_distance) <= kEps) {
            ++plane_checks;
            worst_plane_center = std::max(worst_plane_center, std::fabs(open_distance));
            if (std::fabs(open_distance) <= kEps) {
                ++plane_through_center;
            }
        }
        // Vertices turn by half the full angle about the pivot.  Both cases
        // below are pure swing (`open == closed`), so the pivot does not slide.
        for (int k = 0; k < 3; ++k) {
            const gm::Vec3d p = k == 0 ? r.a : (k == 1 ? r.b : r.c);
            const gm::Vec3d got = k == 0 ? t.a : (k == 1 ? t.b : t.c);
            const double turned = radial_angle(p, got, pivot, spec.axis);
            if (turned > 0.0) {
                worst_half_angle =
                    std::max(worst_half_angle, std::fabs(turned - std::fabs(spec.max_angle) * 0.5));
            }
        }
    }
    std::printf("  half f=0.5: edges=%zu max |dL| = %.3g; planes=%zu max |distance drift| = "
                "%.3g; planes through the pivot=%zu max |offset| = %.3g; max |angle - "
                "half| = %.3g rad\n",
                edge_checks, worst_edge, plane_checks, worst_plane, plane_checks,
                worst_plane_center, worst_half_angle);
    CHECK(edge_checks == 3);
    CHECK(worst_edge <= 1e-9);
    CHECK(worst_plane <= 1e-9);
    // A plane that contained the pivot at rest still passes through it (the
    // dm1 leaf has two faces through the hinge line; the level0 panel has
    // none, which is why the check is conditional).
    if (plane_checks > 0) {
        CHECK(plane_through_center >= 1);
        CHECK(worst_plane_center <= 1e-9);
    }
    CHECK(worst_half_angle <= 1e-9);

    // --- the player: closed leaf holds, open leaf lets it through --------
    struct WalkSetup {
        gm::Vec3d feet;
        double yaw = 0.0;
        int ticks = 0;
    };
    // Pitch the walk along +X at the leaf's mid-height / mid-width; the dm1
    // leaf spans x 7034..7042, y 240..414 and the open pose swings to
    // y ~ 240..248, so +X at y = 330 crosses the closed leaf and clears the
    // open one.  For level0 the door is found from its own bounds.
    const gm::Aabb rest_bounds = world.bounds();
    WalkSetup setup;
    if (swing_case.level == "dm1") {
        setup.feet = {6960.0, 330.0, 146.0};
        setup.yaw = kPi / 2.0;
        setup.ticks = 150;
    } else {
        // The level0 panel is two angled blades (not a box): a horizontal +X
        // ray at y = -300, z ~ 0 crosses the first blade 68 units in, and the
        // open pose (120 deg about -x) swings both blades out of that line.
        setup.feet = {-500.0, -300.0, 0.0};
        setup.yaw = kPi / 2.0;
        setup.ticks = 200;
    }

    CHECK(world.set_dynamic(rest, {{0u, 0u}}, &error));
    const WalkResult closed_walk = walk(world, setup.feet, setup.yaw, setup.ticks);
    CHECK(closed_walk.ok);
    std::printf("  player closed: (%.3f, %.3f, %.3f) -> (%.6f, %.6f, %.6f) wall_ticks=%d "
                "leaf x/y bounds (%.3f..%.3f, %.3f..%.3f)\n",
                closed_walk.start.x, closed_walk.start.y, closed_walk.start.z,
                closed_walk.end.x, closed_walk.end.y, closed_walk.end.z,
                closed_walk.wall_ticks, rest_bounds.lo.x, rest_bounds.hi.x, rest_bounds.lo.y,
                rest_bounds.hi.y);
    CHECK(closed_walk.wall_ticks > 0);

    collider.update(1.0);
    CHECK(world.set_dynamic(collider.triangles(), {{0u, 0u}}, &error));
    const WalkResult open_walk = walk(world, setup.feet, setup.yaw, setup.ticks);
    CHECK(open_walk.ok);
    std::printf("  player open:   (%.3f, %.3f, %.3f) -> (%.6f, %.6f, %.6f) wall_ticks=%d\n",
                open_walk.start.x, open_walk.start.y, open_walk.start.z, open_walk.end.x,
                open_walk.end.y, open_walk.end.z, open_walk.wall_ticks);
    CHECK(open_walk.wall_ticks == 0);
    CHECK(gm::length(open_walk.end - closed_walk.end) > 10.0);

    // --- runtime: the swing pair is suppressed and posed ------------------
    if (runtime_check) {
        const auto loaded = gm::load_level(kGame, swing_case.level, error);
        CHECK(loaded.has_value());
        if (loaded && loaded->entities && loaded->world) {
            const gm::VmState vm = gm::vm_state_from_level(*loaded->entities);
            gm::GameRuntime runtime;
            CHECK(runtime.build(*loaded->entities, vm, *loaded->world, &error));
            if (!error.empty()) {
                std::fprintf(stderr, "runtime build: %s\n", error.c_str());
            }
            const gm::DoorCollisionStats& stats = runtime.door_collision();
            std::printf("  runtime: doors=%zu panels=%zu rotating=%zu skipped(stationary=%zu "
                        "duplicate=%zu unbound=%zu refused=%zu) dynamic=%zu suppressed=%zu\n",
                        stats.doors, stats.panels, stats.rotating, stats.skipped_static,
                        stats.skipped_duplicate, stats.skipped_unbound, stats.skipped_refused,
                        runtime.collision().dynamic_triangle_count(),
                        runtime.collision().suppressed_count());
            CHECK(stats.rotating > 0);
            CHECK(stats.skipped_rotating == 0);
            CHECK(runtime.collision().is_suppressed(
                static_cast<std::uint32_t>(swing_case.sector),
                static_cast<std::uint32_t>(swing_case.object)));

            // The shipped phase is 0: the published dynamic triangles of the
            // pair are the static rest triangles, bit for bit.
            std::size_t dynamic_pair = 0;
            bool same = true;
            for (const gm::Triangle& t : runtime.collision().dynamic_triangles()) {
                if (t.sector != static_cast<std::uint32_t>(swing_case.sector) ||
                    t.object != static_cast<std::uint32_t>(swing_case.object)) {
                    continue;
                }
                if (dynamic_pair < rest.size()) {
                    same = same && triangle_identical(t, rest[dynamic_pair]);
                }
                ++dynamic_pair;
            }
            std::printf("  runtime rest pose: %zu dynamic triangles of the pair, "
                        "bit-identical=%d\n",
                        dynamic_pair, same ? 1 : 0);
            CHECK(dynamic_pair == rest.size());
            CHECK(same);

            // Force the opening phase and tick: the pair is re-posed.
            int slot = -1;
            for (std::size_t i = 0; i < runtime.world().doors().size(); ++i) {
                if (runtime.world().doors()[i].sector == swing_case.sector &&
                    runtime.world().doors()[i].object == swing_case.object) {
                    slot = static_cast<int>(i);
                    break;
                }
            }
            CHECK(slot >= 0);
            if (slot >= 0) {
                const std::size_t rebuilds = runtime.door_collision().rebuilds;
                runtime.world().doors_mutable()[static_cast<std::size_t>(slot)].phase =
                    gm::kDoorPhaseMax;
                runtime.tick(1);
                std::printf("  runtime open phase: rebuilds %zu -> %zu, door f=%.6f\n",
                            rebuilds, runtime.door_collision().rebuilds,
                            runtime.world().doors()[static_cast<std::size_t>(slot)]
                                .motion_fraction());
                CHECK(runtime.door_collision().rebuilds == rebuilds + 1);
                CHECK_NEAR(runtime.world().doors()[static_cast<std::size_t>(slot)]
                               .motion_fraction(),
                           1.0, 0.0);
                bool differs = false;
                std::size_t posed = 0;
                for (const gm::Triangle& t : runtime.collision().dynamic_triangles()) {
                    if (t.sector != static_cast<std::uint32_t>(swing_case.sector) ||
                        t.object != static_cast<std::uint32_t>(swing_case.object)) {
                        continue;
                    }
                    if (posed < rest.size() && !triangle_identical(t, rest[posed])) {
                        differs = true;
                    }
                    ++posed;
                }
                CHECK(posed == rest.size());
                CHECK(differs);
                std::printf("  runtime open pose: %zu triangles, moved=%d\n", posed,
                            differs ? 1 : 0);
            }
        }
    }
}

// --- negatives -------------------------------------------------------------

void test_negatives() {
    gm::MovingPanel panel;
    gm::Triangle triangle;
    triangle.a = {0.0, 0.0, 0.0};
    triangle.b = {10.0, 0.0, 0.0};
    triangle.c = {0.0, 10.0, 0.0};
    triangle.normal = {0.0, 0.0, 1.0};
    triangle.data_normal = {0.0, 0.0, 1.0};
    triangle.plane_offset = 0.0;
    triangle.centroid = {10.0 / 3.0, 10.0 / 3.0, 0.0};
    triangle.bounds.expand(triangle.a);
    triangle.bounds.expand(triangle.b);
    triangle.bounds.expand(triangle.c);
    panel.rest = {triangle};

    gm::DoorSwing swing;
    swing.pivot = {0.0, 0.0, 0.0};
    swing.axis = {0.0, 0.0, -1.0};
    swing.max_angle = kPi / 2.0;

    gm::MovingCollider good;
    good.set_panels({panel}, gm::DoorAxis::Rotating, swing);
    CHECK(good.ready());
    good.update(0.25);
    CHECK(good.last_update_ok());
    const std::size_t before_count = good.triangle_count();
    const gm::Aabb before_bounds = good.bounds();

    // NaN fraction: refused, previous pose kept.
    good.update(std::numeric_limits<double>::quiet_NaN());
    CHECK(!good.last_update_ok());
    CHECK(!good.error().empty());
    CHECK(good.triangle_count() == before_count);
    CHECK(aabb_identical(good.bounds(), before_bounds));
    std::printf("swing_door negative: NaN fraction refused (\"%s\"), pose kept\n",
                good.error().c_str());
    good.update(0.25);
    CHECK(good.last_update_ok());

    // Zero angle: refused instead of a still "rotating" leaf.
    gm::DoorSwing zero = swing;
    zero.max_angle = 0.0;
    gm::MovingCollider no_angle;
    no_angle.set_panels({panel}, gm::DoorAxis::Rotating, zero);
    CHECK(!no_angle.ready());
    CHECK(!no_angle.last_update_ok());
    CHECK(no_angle.error().find("zero") != std::string::npos);
    std::printf("swing_door negative: zero angle refused (\"%s\")\n",
                no_angle.error().c_str());

    // Non-unit axis and non-finite pivot are refused too.
    gm::DoorSwing bad_axis = swing;
    bad_axis.axis = {0.0, 0.0, -2.0};
    gm::MovingCollider no_unit;
    no_unit.set_panels({panel}, gm::DoorAxis::Rotating, bad_axis);
    CHECK(!no_unit.ready());
    std::printf("swing_door negative: non-unit axis refused (\"%s\")\n",
                no_unit.error().c_str());

    gm::DoorSwing bad_pivot = swing;
    bad_pivot.pivot = {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    gm::MovingCollider no_pivot;
    no_pivot.set_panels({panel}, gm::DoorAxis::Rotating, bad_pivot);
    CHECK(!no_pivot.ready());
    std::printf("swing_door negative: NaN pivot refused (\"%s\")\n",
                no_pivot.error().c_str());

    // An update on a refused collider stays refused and never crashes.
    no_angle.update(0.5);
    CHECK(!no_angle.last_update_ok());
    CHECK(no_angle.triangle_count() == 0);
    CHECK(!no_angle.raycast({0, 0, 5}, {0, 0, -1}).has_value());
}

} // namespace

int main() {
    const bool have_level0 = fs::exists(kGame / "level0.sbw") && fs::exists(kGame / "level0.sbt");
    const bool have_dm1 = fs::exists(kGame / "dm1.sbw") && fs::exists(kGame / "dm1.sbt");

    test_negatives();

    const LevelData level0 = have_level0 ? load_level_data("level0") : LevelData{};
    if (level0.ok) {
        run_swing_case(level0, {"level0", 48, 6}, true);
    } else {
        std::printf("swing_door: level0 not installed, level0 part skipped\n");
    }

    const LevelData dm1 = have_dm1 ? load_level_data("dm1") : LevelData{};
    if (dm1.ok) {
        run_swing_case(dm1, {"dm1", 17, 55}, true);
    } else {
        std::printf("swing_door: dm1 not installed, dm1 part skipped\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "swing_door: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("swing_door: ok\n");
    return 0;
}
