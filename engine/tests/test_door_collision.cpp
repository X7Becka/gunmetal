// Moving doors really hold the player (M6.4.1, slice 2: integration;
// docs/DOOR_COLLISION.md).
//
// Slice 1 built the kinematic panel (`world/moving_collider.h`); this test
// pins the integration:
//
//   * `CollisionWorld::set_dynamic` installs the current pose of a moving
//     object and suppresses the stale static triangles of its
//     `(sector, object)`; an empty dynamic set with an empty suppression list
//     is bit-identical to the static-only world, and the refusal rules
//     (NaN/inf vertex, out-of-range coordinate) leave the previous set alone;
//   * `GameRuntime::build_world` builds one panel per moving sliding door from
//     the level's own triangles and publishes them, so `runtime.collision()`
//     holds `level0`'s door `sec=4 obj=1` (12 triangles, travel +Z 176) in its
//     *current* pose: the closed leaf is hit by a ray/sphere and, once the
//     door is forced to `open_fraction == 1`, the same ray no longer meets it;
//   * the dynamic set is rebuilt only when a door phase changed (a resting
//     level rebuilds nothing per tick);
//   * `gm::step` walks a player against that leaf: closed, the capsule stops
//     before the leaf's face (`wall_ticks > 0`); open, the same run passes
//     the face -- on the real `dm1` door `sec=17 obj=37` through the runtime,
//     and on the real `level0` leaf triangles in a single-object world (the
//     shipped `level0` leaf sits inside a sealed frame box, see §6 of the doc);
//   * negatives: a swing door stays static, a door rebound to a missing object
//     is counted and does not break the tick.
//
// The load path follows `engine/src/main.cpp::simulate()` and
// `res/level.cpp::load_level()`: `.sbt` + `.sbw` -> `parse_sbt`,
// `decode_sbt_script`, `vm_state_from_level`, `GameRuntime::build`,
// `extract_scene` for the spawn and `chassis_from_level` for the LIGHT chassis.
#include "game/runtime.h"

#include "game/chassis_loadout.h"
#include "game/sim_player.h"
#include "player/controller.h"
#include "render/collide.h"
#include "res/level.h"
#include "res/sbt.h"
#include "res/sbtvm.h"
#include "res/sbw.h"
#include "res/scene.h"
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

/// One parsed level plus the runtime built from it, the campaign spawn of
/// slot 0 and the LIGHT chassis of that level (same shape as
/// `test_sim_player.cpp`).
struct Fixture {
    std::unique_ptr<gm::GameRuntime> runtime;
    gm::Vec3d feet{};
    double yaw = 0.0;
    gm::ChassisSpec chassis;
};

bool load_fixture(const std::string& name, Fixture& out, std::string& error) {
    const auto level = gm::load_level(kGame, name, error);
    if (!level || !level->entities || !level->world) {
        error = name + ": no .sbt/.sbw pair";
        return false;
    }
    const gm::VmState vm = gm::vm_state_from_level(*level->entities);
    out.runtime = std::make_unique<gm::GameRuntime>();
    if (!out.runtime->build(*level->entities, vm, *level->world, &error)) {
        return false;
    }
    const auto scene = gm::extract_scene(*level->entities, error, false);
    if (!scene) {
        error = name + ": scene: " + error;
        return false;
    }
    const gm::SceneSpawn* start = nullptr;
    for (const gm::SceneSpawn& spawn : scene->spawns) {
        if (!spawn.deathmatch && spawn.slot == 0) {
            start = &spawn;
            break;
        }
    }
    if (start == nullptr) {
        error = name + ": no campaign spawn for slot 0";
        return false;
    }
    out.feet = {static_cast<double>(start->x), static_cast<double>(start->y),
                static_cast<double>(start->z)};
    out.yaw = start->angles[2];

    const auto chassis = gm::chassis_from_level(kGame / (name + ".sbt"), &error);
    if (!chassis) {
        return false;
    }
    for (const gm::ChassisSpec& spec : *chassis) {
        if (spec.name.find("LIGHT") != std::string::npos) {
            out.chassis = spec;
            return true;
        }
    }
    error = name + ": no LIGHT chassis record";
    return false;
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

const gm::Door* find_door_slot(const gm::WorldState& state, int sector, int object) {
    for (const gm::Door& door : state.doors()) {
        if (door.sector == sector && door.object == object) {
            return &door;
        }
    }
    return nullptr;
}

int find_door_index(const gm::WorldState& state, int sector, int object) {
    const std::vector<gm::Door>& doors = state.doors();
    for (std::size_t i = 0; i < doors.size(); ++i) {
        if (doors[i].sector == sector && doors[i].object == object) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void print_vec(const char* label, gm::Vec3d v) {
    std::printf("%s=(%.6f, %.6f, %.6f) ", label, v.x, v.y, v.z);
}

/// Axis-aligned bounds of every triangle of one `(sector, object)` pair in
/// `triangles`, or an empty box when the pair is absent.
gm::Aabb pair_bounds(const std::vector<gm::Triangle>& triangles, std::uint32_t sector,
                     std::uint32_t object, std::size_t* count = nullptr) {
    gm::Aabb box;
    std::size_t n = 0;
    for (const gm::Triangle& triangle : triangles) {
        if (triangle.sector == sector && triangle.object == object) {
            box.expand(triangle.bounds);
            ++n;
        }
    }
    if (count != nullptr) {
        *count = n;
    }
    return box;
}

bool finite_state(const gm::PlayerState& state) {
    return gm::finite(state.pos) && gm::finite(state.vel) && std::isfinite(state.yaw) &&
           std::isfinite(state.height) && std::isfinite(state.eye_height);
}

// --- 1. CollisionWorld::set_dynamic guards and bit-exact parity -------------

void test_set_dynamic(const LevelData& level0) {
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(single_object_level(level0.sbw, 4, 1), &error));
    CHECK(error.empty());
    CHECK(world.triangle_count() == 12);
    CHECK(world.dynamic_triangle_count() == 0);
    CHECK(world.suppressed_count() == 0);

    const gm::Vec3d origin{620.0, 928.0, 0.0};
    const gm::Vec3d direction{1.0, 0.0, 0.0};
    const auto baseline = world.raycast(origin, direction);
    CHECK(baseline.has_value());
    if (!baseline) {
        return;
    }
    std::printf("door_collision: single-object world: %zu static triangles, baseline ray "
                "t=%.6f (static)\n",
                world.triangle_count(), baseline->t);
    CHECK_NEAR(baseline->t, 40.0, 1e-9);
    CHECK(!baseline->dynamic);

    // The current pose as the runtime publishes it: rest triangles + the
    // suppressed pair.  The hit must come from the dynamic set now.
    const std::vector<gm::Triangle> rest = world.triangles();
    CHECK(world.set_dynamic(rest, {{0u, 0u}}, &error));
    CHECK(error.empty());
    CHECK(world.dynamic_triangle_count() == 12);
    CHECK(world.suppressed_count() == 1);
    CHECK(world.triangle_count() == 12); // the static storage is untouched
    CHECK(world.stats().triangles == 12);
    CHECK(world.is_suppressed(0, 0));
    CHECK(!world.is_suppressed(0, 1));
    const auto dynamic_hit = world.raycast(origin, direction);
    CHECK(dynamic_hit.has_value());
    if (dynamic_hit) {
        CHECK(dynamic_hit->dynamic);
        CHECK_NEAR(dynamic_hit->t, 40.0, 1e-9);
        CHECK(dynamic_hit->triangle_index < world.dynamic_triangle_count());
        CHECK_NEAR(dynamic_hit->point.x, 660.0, 1e-9);
        std::printf("  dynamic rest pose: hit t=%.6f point.x=%.3f dynamic=%d index=%u\n",
                    dynamic_hit->t, dynamic_hit->point.x, dynamic_hit->dynamic ? 1 : 0,
                    dynamic_hit->triangle_index);
    }

    // Refusals: NaN/inf vertex, out-of-range coordinate.  The previous set and
    // the suppression list must survive, so the ray result is unchanged.
    std::vector<gm::Triangle> broken = rest;
    broken[3].b.x = std::numeric_limits<double>::quiet_NaN();
    error.clear();
    CHECK(!world.set_dynamic(broken, {{0u, 0u}}, &error));
    CHECK(!error.empty());
    std::printf("  NaN vertex: refused (\"%s\")\n", error.c_str());
    CHECK(world.dynamic_triangle_count() == 12);
    CHECK(world.suppressed_count() == 1);
    const auto after_nan = world.raycast(origin, direction);
    CHECK(after_nan.has_value());
    if (after_nan) {
        CHECK_NEAR(after_nan->t, 40.0, 0.0); // bit-identical: the old pose is intact
        CHECK(after_nan->dynamic);
    }

    broken = rest;
    broken[0].a.x = 1e12; // beyond kCollideMaxCoordinate
    error.clear();
    CHECK(!world.set_dynamic(broken, {}, &error));
    CHECK(!error.empty());
    std::printf("  out-of-range vertex: refused (\"%s\")\n", error.c_str());
    CHECK(world.dynamic_triangle_count() == 12);
    CHECK(world.suppressed_count() == 1);

    // An open leaf (the moving collider produces the pose, exactly the path the
    // runtime uses): the same ray misses.
    gm::MovingPanel panel;
    panel.rest = rest;
    panel.travel = {0.0, 0.0, 176.0};
    gm::MovingCollider collider;
    collider.set_panels({panel}, gm::DoorAxis::Z);
    CHECK(collider.ready());
    collider.update(1.0);
    CHECK(collider.last_update_ok());
    CHECK(world.set_dynamic(collider.triangles(), {{0u, 0u}}, &error));
    const auto open_hit = world.raycast(origin, direction);
    std::printf("  open leaf (+Z 176): ray %s\n", open_hit ? "HIT" : "miss");
    CHECK(!open_hit.has_value());
    CHECK(!world.sphere_cast(origin, direction, 1.0).has_value());

    // Empty dynamic set with an empty suppression list: the static result is
    // bit-identical to the world that never had a dynamic set.
    CHECK(world.set_dynamic({}, {}, &error));
    CHECK(world.dynamic_triangle_count() == 0);
    CHECK(world.suppressed_count() == 0);
    const auto restored = world.raycast(origin, direction);
    CHECK(restored.has_value());
    if (restored) {
        CHECK(!restored->dynamic);
        CHECK(restored->t == baseline->t);
        CHECK(restored->triangle_index == baseline->triangle_index);
        CHECK(restored->point.x == baseline->point.x);
        CHECK(restored->triangle_index == baseline->triangle_index);
        std::printf("  static-only again: t=%.17g index=%u == baseline (bit-identical)\n",
                    restored->t, restored->triangle_index);
    }

    // Suppression without a panel leaves a hole: documented, not a crash.
    CHECK(world.set_dynamic({}, {{0u, 0u}}, &error));
    CHECK(!world.raycast(origin, direction).has_value());
    std::printf("  suppression without a panel: hole (documented)\n");

    // A bare `Triangle{a, b, c}` gets its `bounds`/`normal`/`centroid`/
    // `plane_offset` filled in, so queries work on caller-built triangles too.
    gm::Triangle bare;
    bare.a = {660.0, 856.0, -144.0};
    bare.b = {660.0, 1000.0, -144.0};
    bare.c = {660.0, 1000.0, 136.0};
    CHECK(world.set_dynamic({bare}, {}, &error));
    CHECK(error.empty());
    const gm::Triangle& filled = world.dynamic_triangles()[0];
    CHECK(filled.bounds.valid());
    CHECK_NEAR(gm::length(filled.normal), 1.0, 1e-12);
    CHECK(filled.plane_offset == gm::dot(filled.normal, filled.a));
    CHECK_NEAR(filled.centroid.y, (856.0 + 1000.0 + 1000.0) / 3.0, 1e-9);
    CHECK(world.raycast({620.0, 928.0, 0.0}, {1.0, 0.0, 0.0}).has_value());
    world.clear_dynamic();
    CHECK(world.dynamic_triangle_count() == 0);
    CHECK(world.suppressed_count() == 0);
}

// --- 2. level0 through GameRuntime: dynamic pose, rebuild-on-change ---------

void test_runtime_level0(const Fixture& fixture) {
    gm::GameRuntime& runtime = *fixture.runtime;
    const gm::CollisionWorld& world = runtime.collision();
    const gm::DoorCollisionStats& stats = runtime.door_collision();

    std::printf("door_collision: level0 static=%zu dynamic=%zu suppressed=%zu "
                "doors=%zu panels=%zu rebuilds=%zu skipped(rotating=%zu static=%zu "
                "duplicate=%zu unbound=%zu refused=%zu)\n",
                world.triangle_count(), world.dynamic_triangle_count(),
                world.suppressed_count(), stats.doors, stats.panels, stats.rebuilds,
                stats.skipped_rotating, stats.skipped_static, stats.skipped_duplicate,
                stats.skipped_unbound, stats.skipped_refused);

    CHECK(world.triangle_count() > 0);
    CHECK(world.dynamic_triangle_count() > 0);
    CHECK(world.suppressed_count() > 0);
    CHECK(stats.doors > 0);
    CHECK(stats.panels == stats.doors);
    CHECK(stats.triangles == world.dynamic_triangle_count());
    CHECK(stats.static_triangles == world.triangle_count());
    // Swing doors are kinematic colliders too now (`test_swing_door`); nothing
    // is skipped for the rotating axis any more.
    CHECK(stats.skipped_rotating == 0);
    CHECK(stats.rebuilds == 1);        // the build happens once
    CHECK(world.is_suppressed(4, 1));

    // The static storage is not destroyed: `triangles()` still holds the rest
    // pose, `stats()` is unchanged, only queries skip the pair.
    std::size_t static_leaf = 0;
    std::size_t dynamic_leaf = 0;
    const gm::Aabb static_bounds = pair_bounds(world.triangles(), 4, 1, &static_leaf);
    const gm::Aabb dynamic_bounds = pair_bounds(world.dynamic_triangles(), 4, 1, &dynamic_leaf);
    std::printf("  door sec=4 obj=1: static=%zu dynamic=%zu dynamic bounds "
                "(%.3f, %.3f, %.3f)..(%.3f, %.3f, %.3f)\n",
                static_leaf, dynamic_leaf, dynamic_bounds.lo.x, dynamic_bounds.lo.y,
                dynamic_bounds.lo.z, dynamic_bounds.hi.x, dynamic_bounds.hi.y,
                dynamic_bounds.hi.z);
    CHECK(static_leaf == 12);
    CHECK(dynamic_leaf == 12);
    CHECK(world.stats().triangles == world.triangle_count());
    CHECK_NEAR(static_bounds.lo.z, -144.0, 1e-9);
    CHECK_NEAR(dynamic_bounds.lo.z, -144.0, 1e-9);
    CHECK_NEAR(dynamic_bounds.hi.z, 136.0, 1e-9);

    // The leaf is embedded in the closed frame box `sec=4 obj=0` (see §6 of the
    // doc), so the ray starts in the 4-unit slot in front of the face.
    const gm::Vec3d origin{658.0, 928.0, 0.0};
    const gm::Vec3d direction{1.0, 0.0, 0.0};
    const auto closed_ray = world.raycast(origin, direction, 40.0);
    CHECK(closed_ray.has_value());
    if (closed_ray) {
        std::printf("  closed ray: t=%.6f point=(%.3f, %.3f, %.3f) dynamic=%d "
                    "sec=%u obj=%u\n",
                    closed_ray->t, closed_ray->point.x, closed_ray->point.y,
                    closed_ray->point.z, closed_ray->dynamic ? 1 : 0, closed_ray->triangle->sector,
                    closed_ray->triangle->object);
        CHECK(closed_ray->dynamic);
        CHECK_NEAR(closed_ray->t, 2.0, 1e-9); // 658 -> leaf face at x = 660
        CHECK_NEAR(closed_ray->point.x, 660.0, 1e-9);
        CHECK(closed_ray->triangle != nullptr);
        if (closed_ray->triangle != nullptr) {
            CHECK(closed_ray->triangle->sector == 4);
            CHECK(closed_ray->triangle->object == 1);
        }
    }

    const auto closed_sphere = world.sphere_cast(origin, direction, 1.0, 40.0);
    CHECK(closed_sphere.has_value());
    if (closed_sphere) {
        std::printf("  closed sphere r=1: t=%.6f centre.x=%.3f dynamic=%d\n",
                    closed_sphere->t, closed_sphere->centre.x,
                    closed_sphere->dynamic ? 1 : 0);
        CHECK(closed_sphere->dynamic);
        CHECK_NEAR(closed_sphere->t, 1.0, 1e-9);
    }

    // Force the door fully open the way a level script would (the phase word of
    // the record) and tick: the runtime re-poses the panel and republishes.
    const int slot = find_door_index(runtime.world(), 4, 1);
    CHECK(slot >= 0);
    if (slot < 0) {
        return;
    }
    const std::size_t rebuilds_before = runtime.door_collision().rebuilds;
    gm::Door& door = fixture.runtime->world().doors_mutable()[static_cast<std::size_t>(slot)];
    door.phase = gm::kDoorPhaseMax;
    fixture.runtime->tick(1);
    const std::size_t rebuilds_after = runtime.door_collision().rebuilds;
    std::printf("  door phase -> %u (fraction %.6f): rebuilds %zu -> %zu\n",
                door.engine_phase(), door.motion_fraction(), rebuilds_before, rebuilds_after);
    CHECK_NEAR(door.motion_fraction(), 1.0, 0.0);
    CHECK(rebuilds_after == rebuilds_before + 1);

    std::size_t opened_leaf = 0;
    const gm::Aabb opened_bounds = pair_bounds(world.dynamic_triangles(), 4, 1, &opened_leaf);
    std::printf("  open dynamic bounds z=%.3f..%.3f (dz=%.3f), triangles=%zu\n",
                opened_bounds.lo.z, opened_bounds.hi.z, opened_bounds.lo.z + 144.0,
                opened_leaf);
    CHECK(opened_leaf == 12);
    CHECK_NEAR(opened_bounds.lo.z - dynamic_bounds.lo.z, 176.0, 1e-9);
    CHECK_NEAR(opened_bounds.hi.z - dynamic_bounds.hi.z, 176.0, 1e-9);

    // The doorway is clear now: nothing within 5 units, and the far ray hits
    // the static frame (x = 672), never the leaf.
    const auto clear = world.raycast(origin, direction, 5.0);
    CHECK(!clear.has_value());
    const auto open_ray = world.raycast(origin, direction, 40.0);
    CHECK(open_ray.has_value());
    if (open_ray) {
        std::printf("  open ray: t=%.6f point.x=%.3f dynamic=%d sec=%u obj=%u\n", open_ray->t,
                    open_ray->point.x, open_ray->dynamic ? 1 : 0, open_ray->triangle->sector,
                    open_ray->triangle->object);
        CHECK(!open_ray->dynamic);
        CHECK_NEAR(open_ray->point.x, 672.0, 1e-9);
        CHECK(open_ray->triangle->object == 0); // the frame, not the leaf
    }
    CHECK(!world.sphere_cast(origin, direction, 1.0, 5.0).has_value());

    // Resting doors: another tick changes no fraction, so nothing is rebuilt.
    fixture.runtime->tick(2);
    std::printf("  two resting ticks: rebuilds=%zu sync_ticks=%zu (no rebuild)\n",
                runtime.door_collision().rebuilds, runtime.door_collision().sync_ticks);
    CHECK(runtime.door_collision().rebuilds == rebuilds_after);
    CHECK(runtime.door_collision().sync_ticks > rebuilds_after);

    // A swing door stays static: its triangles are not suppressed and its
    // panel was refused (not silently translated).
    const gm::Door* swinging = nullptr;
    for (const gm::Door& candidate : runtime.world().doors()) {
        if (gm::door_axis(candidate) == gm::DoorAxis::Rotating && candidate.object >= 0) {
            swinging = &candidate;
            break;
        }
    }
    CHECK(swinging != nullptr);
    if (swinging != nullptr) {
        const bool suppressed =
            world.is_suppressed(static_cast<std::uint32_t>(swinging->sector),
                                static_cast<std::uint32_t>(swinging->object));
        std::printf("  rotating door sec=%d obj=%d: suppressed=%d (kinematic)\n",
                    swinging->sector, swinging->object, suppressed ? 1 : 0);
        // The swing leaf owns its (sector, object) pair now, so the static copy
        // is suppressed and the rebuilt pose is the only one queried
        // (`docs/DOOR_COLLISION.md` §2.1).
        CHECK(suppressed);
    }

    // A door rebound to an object that does not exist is counted, and neither
    // the rebuild nor the tick breaks.
    const int victim = find_door_index(runtime.world(), 23, 1);
    CHECK(victim >= 0);
    if (victim >= 0) {
        const std::size_t unbound_before = runtime.door_collision().skipped_unbound;
        const std::size_t dynamic_before = world.dynamic_triangle_count();
        gm::Door& missing =
            fixture.runtime->world().doors_mutable()[static_cast<std::size_t>(victim)];
        const int saved_sector = missing.sector;
        missing.sector = 9999;
        std::string error;
        CHECK(fixture.runtime->rebuild_door_colliders(&error));
        CHECK(error.empty());
        CHECK(runtime.door_collision().skipped_unbound == unbound_before + 1);
        CHECK(world.dynamic_triangle_count() < dynamic_before);
        std::printf("  unbound door (sec=9999): skipped_unbound=%zu, dynamic %zu -> %zu, "
                    "tick ",
                    runtime.door_collision().skipped_unbound, dynamic_before,
                    world.dynamic_triangle_count());
        fixture.runtime->tick(1);
        std::printf("ok\n");
        missing.sector = saved_sector;
        CHECK(fixture.runtime->rebuild_door_colliders(&error));
        CHECK(world.dynamic_triangle_count() == dynamic_before);
    }
}

// --- 3. the player is held by a closed leaf, open one lets it through ------

struct WalkResult {
    gm::Vec3d start{};
    gm::Vec3d end{};
    int wall_ticks = 0;
    bool ok = false;
};

/// `gm::step` for `ticks` ticks with `forward = 1` along `yaw`; gravity is off
/// so the run stays on the leaf's own height band (the caller places the feet
/// at the leaf face and no floor is needed in the single-object world).
WalkResult walk(gm::CollisionWorld& world, const gm::Vec3d& feet, double yaw, int ticks,
                double gravity) {
    WalkResult out;
    gm::ControllerConfig config;
    config.gravity = gravity;
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

void test_player_real_leaf(const LevelData& level0) {
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(single_object_level(level0.sbw, 4, 1), &error));

    // Closed: publish the rest pose as the dynamic set and carve the static copy
    // out, exactly what `GameRuntime` does for a door.
    const std::vector<gm::Triangle> rest = world.triangles();
    CHECK(world.set_dynamic(rest, {{0u, 0u}}, &error));

    const gm::Vec3d feet{620.0, 928.0, 0.0};
    const double yaw = kPi / 2.0; // forward = (sin yaw, cos yaw) = (+1, 0)
    const WalkResult closed = walk(world, feet, yaw, 60, 0.0);
    CHECK(closed.ok);
    std::printf("door_collision: level0 leaf (12 triangles, travel +Z 176), player "
                "gravity=0 from (%.3f, %.3f, %.3f):\n",
                closed.start.x, closed.start.y, closed.start.z);
    std::printf("  closed: pos=(%.6f, %.6f, %.6f) wall_ticks=%d (leaf faces x=660/668)\n",
                closed.end.x, closed.end.y, closed.end.z, closed.wall_ticks);
    CHECK(finite_state(gm::make_state(closed.end, yaw)));
    CHECK(closed.wall_ticks > 0);
    CHECK(closed.end.x < 660.0);   // did not reach the leaf's near face
    CHECK(closed.end.x > 640.0);   // ... and really walked towards it

    // Open: the moving collider poses the same triangles at f = 1.
    gm::MovingPanel panel;
    panel.rest = rest;
    panel.travel = {0.0, 0.0, 176.0};
    gm::MovingCollider collider;
    collider.set_panels({panel}, gm::DoorAxis::Z);
    collider.update(1.0);
    CHECK(collider.last_update_ok());
    CHECK(world.set_dynamic(collider.triangles(), {{0u, 0u}}, &error));

    const WalkResult open = walk(world, feet, yaw, 60, 0.0);
    CHECK(open.ok);
    std::printf("  open:   pos=(%.6f, %.6f, %.6f) wall_ticks=%d (passed both faces)\n",
                open.end.x, open.end.y, open.end.z, open.wall_ticks);
    CHECK(open.end.x > 668.0);
    CHECK(open.wall_ticks == 0);
    CHECK(open.end.x > closed.end.x + 20.0);
}

// --- 4. dm1 through GameRuntime: a real doorway the player can walk --------

void test_player_runtime_dm1(const Fixture& fixture) {
    gm::GameRuntime& runtime = *fixture.runtime;
    const gm::CollisionWorld& world = runtime.collision();
    const gm::DoorCollisionStats& stats = runtime.door_collision();
    std::printf("door_collision: dm1 static=%zu dynamic=%zu suppressed=%zu doors=%zu "
                "panels=%zu rebuilds=%zu\n",
                world.triangle_count(), world.dynamic_triangle_count(),
                world.suppressed_count(), stats.doors, stats.panels, stats.rebuilds);
    CHECK(world.dynamic_triangle_count() > 0);
    CHECK(stats.doors > 0);

    const int slot = find_door_index(runtime.world(), 17, 37);
    CHECK(slot >= 0);
    if (slot < 0) {
        return;
    }
    const gm::Door& door = runtime.world().doors()[static_cast<std::size_t>(slot)];
    const gm::Vec3d travel = gm::door_travel(door);
    std::printf("  dm1 door sec=17 obj=37: travel=(%.3f, %.3f, %.3f) axis=%s\n", travel.x,
                travel.y, travel.z, gm::door_axis_name(gm::door_axis(door)));
    CHECK(gm::door_axis(door) == gm::DoorAxis::Y);
    CHECK_NEAR(travel.y, 130.0, 1e-12);

    std::size_t leaf_triangles = 0;
    const gm::Aabb closed_bounds =
        pair_bounds(runtime.collision().dynamic_triangles(), 17, 37, &leaf_triangles);
    std::printf("  dynamic leaf: %zu triangles, y=%.3f..%.3f z=%.3f..%.3f\n", leaf_triangles,
                closed_bounds.lo.y, closed_bounds.hi.y, closed_bounds.lo.z,
                closed_bounds.hi.z);
    CHECK(leaf_triangles == 8);

    // The leaf covers the doorway `sec=17 obj=0` (floor at z = 132) from
    // z = 132 to 225; the feet start on that floor band and walk -X
    // (yaw = -pi/2 -> forward = (-1, 0)).
    const gm::Vec3d feet{8050.0, 1700.0, 132.0};
    const double yaw = -kPi / 2.0;
    const int ticks = 150;

    auto drive = [&](std::size_t ticks_to_run, WalkResult& result) {
        gm::SimPlayer player;
        std::string error;
        if (!player.init(feet, yaw, fixture.chassis, &error)) {
            std::fprintf(stderr, "dm1 player init: %s\n", error.c_str());
            return false;
        }
        result.start = player.state().pos;
        gm::DriveSegment segment;
        segment.forward = 1.0;
        segment.ticks = static_cast<int>(ticks_to_run);
        for (int tick = 0; tick < segment.ticks; ++tick) {
            if (!player.tick(runtime, segment, tick, &error)) {
                std::fprintf(stderr, "dm1 player tick: %s\n", error.c_str());
                return false;
            }
            if (player.last_step().hit_wall) {
                ++result.wall_ticks;
            }
        }
        result.end = player.state().pos;
        result.ok = true;
        return true;
    };

    // Head-on ray at the chest height: the closed dynamic leaf is the first
    // thing the level puts in the way.
    const gm::Vec3d eye{8050.0, 1700.0, 140.0};
    const gm::Vec3d forward{-1.0, 0.0, 0.0};
    const auto closed_ray = world.raycast(eye, forward, 200.0);
    CHECK(closed_ray.has_value());
    if (closed_ray) {
        std::printf("  closed ray: t=%.6f x=%.3f dynamic=%d sec=%u obj=%u\n", closed_ray->t,
                    closed_ray->point.x, closed_ray->dynamic ? 1 : 0,
                    closed_ray->triangle->sector, closed_ray->triangle->object);
        CHECK(closed_ray->dynamic);
        CHECK_NEAR(closed_ray->t, 43.0, 1e-9); // 8050 - 8007
        CHECK(closed_ray->triangle->sector == 17);
        CHECK(closed_ray->triangle->object == 37);
    }

    WalkResult closed;
    CHECK(drive(static_cast<std::size_t>(ticks), closed));
    std::printf("  closed: pos=(%.3f, %.3f, %.3f) -> (%.6f, %.6f, %.6f) wall_ticks=%d "
                "(leaf +X face x=8007)\n",
                closed.start.x, closed.start.y, closed.start.z, closed.end.x, closed.end.y,
                closed.end.z, closed.wall_ticks);
    CHECK(closed.ok);
    CHECK(finite_state(gm::make_state(closed.end, yaw)));
    CHECK(closed.wall_ticks > 0);
    // Stopped one capsule radius in front of the leaf's face.
    CHECK_NEAR(closed.end.x, 8010.0, 0.01);
    CHECK(closed.end.x > 8002.0); // still on the near side of the whole leaf
    CHECK(closed.end.x < 8050.0); // ... after walking up to it

    // Throw the same switch the level script would: the record's phase word.
    const std::size_t rebuilds_before = runtime.door_collision().rebuilds;
    runtime.world().doors_mutable()[static_cast<std::size_t>(slot)].phase = gm::kDoorPhaseMax;
    runtime.tick(1);
    CHECK(runtime.door_collision().rebuilds == rebuilds_before + 1);
    std::size_t opened_triangles = 0;
    const gm::Aabb open_bounds =
        pair_bounds(runtime.collision().dynamic_triangles(), 17, 37, &opened_triangles);
    std::printf("  door open: dynamic y=%.3f..%.3f (dy=%.3f), rebuilds %zu -> %zu\n",
                open_bounds.lo.y, open_bounds.hi.y, open_bounds.lo.y - closed_bounds.lo.y,
                rebuilds_before, runtime.door_collision().rebuilds);
    CHECK(opened_triangles == 8);
    CHECK_NEAR(open_bounds.lo.y - closed_bounds.lo.y, 130.0, 1e-9);

    // The same ray now runs through the open doorway and stops on the static
    // wall the leaf covered (`sec=17 obj=35`, x = 8002, docs/DOOR_COLLISION.md
    // §6): the *door* is no longer in the way.
    const auto open_ray = world.raycast(eye, forward, 200.0);
    CHECK(open_ray.has_value());
    if (open_ray) {
        std::printf("  open ray: t=%.6f x=%.3f dynamic=%d sec=%u obj=%u\n", open_ray->t,
                    open_ray->point.x, open_ray->dynamic ? 1 : 0, open_ray->triangle->sector,
                    open_ray->triangle->object);
        CHECK(!open_ray->dynamic);
        CHECK_NEAR(open_ray->point.x, 8002.0, 1e-9); // the frame wall behind the leaf
        CHECK(open_ray->triangle->object == 35);
        CHECK(open_ray->t > closed_ray->t);
    }

    WalkResult open;
    CHECK(drive(static_cast<std::size_t>(ticks), open));
    std::printf("  open:   pos=(%.3f, %.3f, %.3f) -> (%.6f, %.6f, %.6f) wall_ticks=%d "
                "(stopped by the static wall x=8002, leaf thickness crossed)\n",
                open.start.x, open.start.y, open.start.z, open.end.x, open.end.y, open.end.z,
                open.wall_ticks);
    CHECK(open.ok);
    // The capsule crossed the leaf's whole 5-unit thickness (8007..8002) and is
    // stopped by the static frame wall one radius behind it, not by the leaf.
    CHECK(open.end.x < 8006.0);
    CHECK(open.end.x < closed.end.x - 4.0);
}

} // namespace

int main() {
    const bool have_level0 = fs::exists(kGame / "level0.sbt") && fs::exists(kGame / "level0.sbw");
    const bool have_dm1 = fs::exists(kGame / "dm1.sbt") && fs::exists(kGame / "dm1.sbw");
    if (!have_level0) {
        std::printf("door_collision: level0 not installed, skipping\n");
        return 0;
    }

    const LevelData level0 = load_level_data("level0");
    CHECK(level0.ok);
    if (!level0.ok) {
        std::fprintf(stderr, "door_collision: level0 unreadable\n");
        return 1;
    }

    test_set_dynamic(level0);

    {
        Fixture fixture;
        std::string error;
        if (!load_fixture("level0", fixture, error)) {
            std::fprintf(stderr, "door_collision: level0 fixture: %s\n", error.c_str());
            ++g_failures;
        } else {
            test_runtime_level0(fixture);
        }
    }
    test_player_real_leaf(level0);

    if (have_dm1) {
        Fixture fixture;
        std::string error;
        if (!load_fixture("dm1", fixture, error)) {
            std::fprintf(stderr, "door_collision: dm1 fixture: %s\n", error.c_str());
            ++g_failures;
        } else {
            test_player_runtime_dm1(fixture);
        }
    } else {
        std::printf("door_collision: dm1 not installed, the runtime walk is skipped\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "door_collision: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("door_collision: ok\n");
    return 0;
}
