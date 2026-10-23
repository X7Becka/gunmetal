// Lift cabins as kinematic colliders (M6.4.1, slice 4;
// docs/LIFT_COLLISION.md, docs/MOVING_COLLIDERS.md §2.2).
//
// The cabin is a **runtime** object built from `SOB_TABLE[Lift::sob]`
// (`0x44e800` -> `0x47c990` -> `0x4277cd` / `0x43b639`), not a `.sbw` object:
// no shipped lift has a `(sector, object)` binding.  The corpus probe behind
// that statement is in docs/LIFT_COLLISION.md §2 (45/45 lifts unbound, 0 exact
// origin matches, 0 reused model meshes).
//
// This test pins:
//   * the seam `MovingCollider::update(fraction, lift_offset)` on the real
//     level14 `sec=35 obj=1` platform (230 x 230 x 38 at the KELLIFT base): a pure
//     translation keeps every normal, shifts `bounds` by exactly
//     `pos_delta * fraction`, moves a floor ray by the same amount, and
//     refuses a NaN offset without disturbing the pose;
//   * the runtime: an unbound level reports every lift in `lifts_unbound`,
//     `tick()` stays healthy, and the dynamic set is untouched;
//   * a caller-supplied binding (`Lift::cabin_sector`/`cabin_object`) puts the
//     cabin into the dynamic set (the static pair is suppressed), the rest pose
//     is bit-for-bit the static triangles, a height change re-poses the panel
//     by exactly `pos_delta * height/65535`, and a second tick with the same
//     height publishes nothing;
//   * the player/floor behavior: `floor_at` follows the raised cabin, while a
//     standing player is **not** carried (no contact resolve) and falls.
//
// Real data used (level14, read from the shipped files):
//   lift0 KELLIFT pos=(-128,5816,620) pos_delta=(0,0,260) speed=300
//   stops=[0,65535], height 0.  The bound platform is `sec=35 obj=1`, origin
//   (-128,5816,741), AABB -243..-13 x 5701..5931 x 722..760, 18 polygons /
//   8+8+... vertices, with a clean +Z top face at z=760.
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
#include <optional>
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
constexpr int kLevel = 14;
constexpr std::uint32_t kPlateSector = 35;
constexpr std::uint32_t kPlateObject = 1;
constexpr std::size_t kLiftSlot = 0; // KELLIFT
constexpr double kPlateX = -128.0;
constexpr double kPlateY = 5816.0;
constexpr double kPlateTop = 760.0;

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
/// full level (same helper as test_swing_door.cpp).
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

bool triangle_identical(const gm::Triangle& a, const gm::Triangle& b) {
    return a.a.x == b.a.x && a.a.y == b.a.y && a.a.z == b.a.z && a.b.x == b.b.x &&
           a.b.y == b.b.y && a.b.z == b.b.z && a.c.x == b.c.x && a.c.y == b.c.y &&
           a.c.z == b.c.z && a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
           a.normal.z == b.normal.z && a.data_normal.x == b.data_normal.x &&
           a.data_normal.y == b.data_normal.y && a.data_normal.z == b.data_normal.z &&
           a.plane_offset == b.plane_offset && a.centroid.x == b.centroid.x &&
           a.centroid.y == b.centroid.y && a.centroid.z == b.centroid.z;
}

bool aabb_identical(const gm::Aabb& a, const gm::Aabb& b) {
    return a.lo.x == b.lo.x && a.lo.y == b.lo.y && a.lo.z == b.lo.z && a.hi.x == b.hi.x &&
           a.hi.y == b.hi.y && a.hi.z == b.hi.z;
}

/// Triangles of one `(sector, object)` pair (static or dynamic set).
std::vector<gm::Triangle> pair_triangles(const std::vector<gm::Triangle>& triangles,
                                         std::uint32_t sector, std::uint32_t object) {
    std::vector<gm::Triangle> out;
    for (const gm::Triangle& t : triangles) {
        if (t.sector == sector && t.object == object) {
            out.push_back(t);
        }
    }
    return out;
}

double max_normal_drift(const std::vector<gm::Triangle>& a,
                        const std::vector<gm::Triangle>& b) {
    double worst = 0.0;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        worst = std::max(worst, gm::length(a[i].normal - b[i].normal));
        worst = std::max(worst, gm::length(a[i].data_normal - b[i].data_normal));
    }
    return worst;
}

const gm::Lift* find_lift(const gm::WorldState& world) {
    if (kLiftSlot >= world.lifts().size()) {
        return nullptr;
    }
    return &world.lifts()[kLiftSlot];
}

/// Floor height under the plate's centre, straight down (the player's query).
/// Floor height under the platform's centre, straight down (the player's
/// query).  The optional sector filter isolates the bound cabin object from
/// the other floors of the level14 shaft (712/722/768/780/856/1200).
std::optional<double> floor_under_plate(
    const gm::CollisionWorld& collision,
    std::span<const std::uint32_t> sectors = {}) {
    // Start below the room's static ceiling (`level14` has a floor at 1276
    // over this shaft) but above both the rest and the raised cabin top.
    gm::CollideFilter filter;
    filter.sectors = sectors;
    return collision.floor_at(kPlateX, kPlateY, kPlateTop + 340.0,
                              gm::kCollideInfinity, filter);
}

std::span<const std::uint32_t> plate_sector_filter() {
    static constexpr std::uint32_t kSector = kPlateSector;
    return std::span<const std::uint32_t>(&kSector, 1);
}

// --- 1. the seam on the real plate -----------------------------------------

void test_seam(const LevelData& level) {
    std::string error;
    gm::WorldState state;
    CHECK(state.build(level.sbt, level.sbw, &error));
    const gm::Lift* lift = find_lift(state);
    CHECK(lift != nullptr);
    if (lift == nullptr) {
        return;
    }
    // The shipped data has no binding, and the probe numbers of the docs say so.
    CHECK(state.lifts().size() == 1);
    CHECK(!lift->cabin_bound());
    CHECK(lift->pos_delta[0] == 0 && lift->pos_delta[1] == 0);
    CHECK(lift->pos_delta[2] == 260);
    const double full = static_cast<double>(gm::kDoorPhaseMax) * gm::kDoorPhaseMaxToFraction;
    std::printf("lift_collision seam: level%d lift%zu sob=%d pos=(%d,%d,%d) "
                "pos_delta=(%d,%d,%d) speed=%d stops=%zu full_fraction=%.17g\n",
                kLevel, kLiftSlot, lift->sob, lift->x, lift->y, lift->z, lift->pos_delta[0],
                lift->pos_delta[1], lift->pos_delta[2], lift->speed, lift->stops.size(), full);

    gm::CollisionWorld world;
    CHECK(world.build(single_object_level(level.sbw, kPlateSector, kPlateObject), &error));
    CHECK(error.empty());
    const std::vector<gm::Triangle>& rest = world.triangles();
    CHECK(!rest.empty());
    std::printf("  plate sec=%u obj=%u: %zu triangles, bounds (%.3f, %.3f, %.3f).."
                "(%.3f, %.3f, %.3f)\n",
                kPlateSector, kPlateObject, rest.size(), world.bounds().lo.x,
                world.bounds().lo.y, world.bounds().lo.z, world.bounds().hi.x,
                world.bounds().hi.y, world.bounds().hi.z);

    gm::MovingPanel panel;
    panel.travel = {static_cast<double>(lift->pos_delta[0]),
                    static_cast<double>(lift->pos_delta[1]),
                    static_cast<double>(lift->pos_delta[2])};
    panel.rest = rest;
    gm::MovingCollider collider;
    collider.set_panels({panel}, gm::DoorAxis::Travel);
    CHECK(collider.ready());
    CHECK(collider.triangle_count() == rest.size());

    // f = 0: the pose is the rest pose bit for bit.
    bool identical = true;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        identical = identical && triangle_identical(collider.triangles()[i], rest[i]);
    }
    CHECK(identical);
    CHECK(aabb_identical(collider.bounds(), world.bounds()));

    // A downward ray from above the plate: t is the distance to its top face.
    const gm::Vec3d ray_origin{kPlateX, kPlateY, 2000.0};
    const gm::Vec3d ray_down{0.0, 0.0, -1.0};
    const auto rest_hit = collider.raycast(ray_origin, ray_down);
    CHECK(rest_hit.has_value());
    const double rest_t = rest_hit ? rest_hit->t : -1.0;
    std::printf("  f=0: ray from z=%.1f down -> t=%.6f point.z=%.6f\n", ray_origin.z, rest_t,
                rest_hit ? rest_hit->point.z : 0.0);

    // f = 1 (the cabin at `height = 0xffff`): translate by +260 exactly.
    collider.update(full);
    CHECK(collider.last_update_ok());
    const double delta_z = static_cast<double>(lift->pos_delta[2]) * full;
    const gm::Aabb posed = collider.bounds();
    std::printf("  f=1: delta_z=%.17g bounds.z %.3f..%.3f (rest %.3f..%.3f) drift_lo=%.3g "
                "drift_hi=%.3g\n",
                delta_z, posed.lo.z, posed.hi.z, world.bounds().lo.z, world.bounds().hi.z,
                posed.lo.z - (world.bounds().lo.z + delta_z),
                posed.hi.z - (world.bounds().hi.z + delta_z));
    CHECK(posed.lo.z == world.bounds().lo.z + delta_z);
    CHECK(posed.hi.z == world.bounds().hi.z + delta_z);
    CHECK(posed.lo.x == world.bounds().lo.x && posed.hi.x == world.bounds().hi.x);
    CHECK(posed.lo.y == world.bounds().lo.y && posed.hi.y == world.bounds().hi.y);
    const double normals = max_normal_drift(collider.triangles(), rest);
    std::printf("  f=1: max |normal drift| = %.3g\n", normals);
    CHECK(normals <= 1e-12);
    const auto raised_hit = collider.raycast(ray_origin, ray_down);
    CHECK(raised_hit.has_value());
    const double raised_t = raised_hit ? raised_hit->t : -1.0;
    std::printf("  f=1: ray t=%.6f (rest %.6f, delta %.6f) point.z=%.6f\n", raised_t, rest_t,
                rest_t - raised_t, raised_hit ? raised_hit->point.z : 0.0);
    CHECK_NEAR(rest_t - raised_t, delta_z, 1e-9);

    // The signed `lift_offset` seam: a cabin that goes down by 500 units.
    collider.update(0.0, -500.0);
    CHECK(collider.last_update_ok());
    std::printf("  lift_offset=-500: bounds.z %.3f..%.3f\n", collider.bounds().lo.z,
                collider.bounds().hi.z);
    CHECK(collider.bounds().lo.z == world.bounds().lo.z - 500.0);
    CHECK(collider.bounds().hi.z == world.bounds().hi.z - 500.0);
    const auto low_hit = collider.raycast(ray_origin, ray_down);
    CHECK(low_hit.has_value());
    CHECK_NEAR(rest_t - (low_hit ? low_hit->t : 0.0), -500.0, 1e-9);

    // A NaN offset is refused and keeps the last good pose.
    const gm::Aabb before = collider.bounds();
    collider.update(0.0, std::numeric_limits<double>::quiet_NaN());
    CHECK(!collider.last_update_ok());
    CHECK(!collider.error().empty());
    CHECK(aabb_identical(collider.bounds(), before));
    std::printf("  negative: NaN lift_offset refused (\"%s\"), pose kept\n",
                collider.error().c_str());
}

// --- 2. runtime: unbound corpus, then a caller-supplied binding -------------

void test_runtime(const LevelData& level) {
    std::string error;
    const auto loaded = gm::load_level(kGame, "level14", error);
    CHECK(loaded.has_value());
    if (!loaded || !loaded->entities || !loaded->world) {
        std::fprintf(stderr, "load_level(%d): %s\n", kLevel, error.c_str());
        return;
    }
    const gm::VmState vm = gm::vm_state_from_level(*loaded->entities);
    gm::GameRuntime runtime;
    CHECK(runtime.build(*loaded->entities, vm, *loaded->world, &error));
    if (!error.empty()) {
        std::fprintf(stderr, "runtime build: %s\n", error.c_str());
    }
    const gm::WorldState& world = runtime.world();
    CHECK(world.lifts().size() == 1);
    for (const gm::Lift& lift : world.lifts()) {
        CHECK(!lift.cabin_bound());
    }
    const gm::DoorCollisionStats& stats = runtime.door_collision();
    std::printf("runtime unbound: lifts=%zu unbound=%zu bound=%zu dynamic=%zu suppressed=%zu\n",
                world.lifts().size(), stats.lifts_unbound, stats.lifts,
                runtime.collision().dynamic_triangle_count(),
                runtime.collision().suppressed_count());
    CHECK(stats.lifts == 0);
    CHECK(stats.lifts_unbound == world.lifts().size());

    // A tick with no binding must not break: the sync runs, nothing changes.
    const std::size_t dynamic_before = runtime.collision().dynamic_triangle_count();
    const std::size_t rebuilds_before = stats.rebuilds;
    const std::size_t sync_before = stats.sync_ticks;
    runtime.tick(1);
    std::printf("runtime unbound tick: sync %zu -> %zu, rebuilds %zu -> %zu, dynamic %zu -> "
                "%zu\n",
                sync_before, stats.sync_ticks, rebuilds_before, stats.rebuilds, dynamic_before,
                runtime.collision().dynamic_triangle_count());
    CHECK(stats.sync_ticks == sync_before + 1);
    CHECK(stats.rebuilds == rebuilds_before);
    CHECK(runtime.collision().dynamic_triangle_count() == dynamic_before);

    // Bind the real platform to the lift.  It already ships at the bottom
    // stop (height 0), so the first published pose is the `.sbw` pose.
    gm::Lift& lift = runtime.world().lifts_mutable()[kLiftSlot];
    CHECK(lift.height == 0);
    lift.cabin_sector = static_cast<int>(kPlateSector);
    lift.cabin_object = static_cast<int>(kPlateObject);
    CHECK(runtime.rebuild_door_colliders(&error));
    CHECK(error.empty());
    std::printf("runtime bound: lifts=%zu panels=%zu unbound=%zu dynamic=%zu suppressed=%zu "
                "applied_height=%d\n",
                stats.lifts, stats.lift_panels, stats.lifts_unbound,
                runtime.collision().dynamic_triangle_count(),
                runtime.collision().suppressed_count(),
                runtime.world().lifts()[kLiftSlot].height);
    CHECK(stats.lifts == 1);
    CHECK(stats.lift_panels == 1);
    CHECK(stats.lifts_unbound == world.lifts().size() - 1);
    CHECK(runtime.collision().is_suppressed(kPlateSector, kPlateObject));

    // The published cabin panel is the static pair bit for bit at height 0.
    const std::vector<gm::Triangle> static_pair =
        pair_triangles(runtime.collision().triangles(), kPlateSector, kPlateObject);
    const std::vector<gm::Triangle> rest_pair =
        pair_triangles(runtime.collision().dynamic_triangles(), kPlateSector, kPlateObject);
    CHECK(!static_pair.empty());
    CHECK(static_pair.size() == rest_pair.size());
    bool same = static_pair.size() == rest_pair.size();
    for (std::size_t i = 0; i < std::min(static_pair.size(), rest_pair.size()); ++i) {
        same = same && triangle_identical(static_pair[i], rest_pair[i]);
    }
    std::printf("runtime rest: static pair %zu triangles, dynamic pair %zu, bit-identical=%d\n",
                static_pair.size(), rest_pair.size(), same ? 1 : 0);
    CHECK(same);

    // The floor query of a standing player, before the move.
    const auto floor_rest = floor_under_plate(runtime.collision(), plate_sector_filter());
    CHECK(floor_rest.has_value());
    const double floor_rest_z = floor_rest.value_or(0.0);
    std::printf("runtime floor at rest: z=%.6f\n", floor_rest_z);

    // Raise the cabin to the top stop: one tick, one re-pose, one publish.
    const std::size_t rebuilds = stats.rebuilds;
    lift.height = 65535;
    runtime.tick(1);
    std::printf("runtime raised: rebuilds %zu -> %zu, height=%d fraction=%.17g\n", rebuilds,
                stats.rebuilds, runtime.world().lifts()[kLiftSlot].height,
                gm::lift_fraction(runtime.world().lifts()[kLiftSlot]));
    CHECK(stats.rebuilds == rebuilds + 1);
    const std::vector<gm::Triangle> raised_pair =
        pair_triangles(runtime.collision().dynamic_triangles(), kPlateSector, kPlateObject);
    CHECK(raised_pair.size() == rest_pair.size());
    const double fraction = gm::lift_fraction(runtime.world().lifts()[kLiftSlot]);
    const double expected_z = 260.0 * fraction;
    double worst = 0.0;
    for (std::size_t i = 0; i < std::min(raised_pair.size(), rest_pair.size()); ++i) {
        for (int k = 0; k < 3; ++k) {
            const gm::Vec3d was = k == 0 ? rest_pair[i].a : (k == 1 ? rest_pair[i].b : rest_pair[i].c);
            const gm::Vec3d now = k == 0 ? raised_pair[i].a : (k == 1 ? raised_pair[i].b : raised_pair[i].c);
            worst = std::max(worst, std::fabs((now.z - was.z) - expected_z));
            worst = std::max(worst, std::fabs(now.x - was.x));
            worst = std::max(worst, std::fabs(now.y - was.y));
        }
    }
    const double normals = max_normal_drift(raised_pair, rest_pair);
    std::printf("runtime raised: expected dz=%.17g max vertex drift=%.3g max normal drift=%.3g\n",
                expected_z, worst, normals);
    CHECK(worst <= 1e-12);
    CHECK(normals <= 1e-12);

    const auto floor_raised = floor_under_plate(runtime.collision(), plate_sector_filter());
    CHECK(floor_raised.has_value());
    const double floor_raised_z = floor_raised.value_or(0.0);
    std::printf("runtime floor raised: z=%.6f (rest %.6f, dz=%.6f)\n", floor_raised_z,
                floor_rest_z, floor_raised_z - floor_rest_z);
    CHECK_NEAR(floor_raised_z - floor_rest_z, expected_z, 1e-9);

    // A second tick with the same height publishes nothing.
    const std::size_t again = stats.rebuilds;
    runtime.tick(1);
    std::printf("runtime second tick: rebuilds %zu -> %zu (height unchanged)\n", again,
                stats.rebuilds);
    CHECK(stats.rebuilds == again);

    // --- the player: stands on the cabin, is not carried by it -----------
    // A small isolated world (just the plate) keeps the numbers about the
    // cabin: the rest pose is the dynamic floor, the raised one is above the
    // player.
    gm::CollisionWorld isolated;
    CHECK(isolated.build(single_object_level(level.sbw, kPlateSector, kPlateObject), &error));
    gm::MovingPanel panel;
    panel.travel = {0.0, 0.0, 260.0};
    panel.rest = pair_triangles(isolated.triangles(), 0, 0);
    gm::MovingCollider cabin;
    cabin.set_panels({panel}, gm::DoorAxis::Travel);
    CHECK(cabin.ready());
    CHECK(isolated.set_dynamic(cabin.triangles(), {{0u, 0u}}, &error));

    gm::ControllerConfig config;
    gm::PlayerState player = gm::make_state({kPlateX, kPlateY, kPlateTop + 10.0}, 0.0, config);
    for (int i = 0; i < 30; ++i) {
        const gm::StepResult step = gm::step(player, gm::Input{}, isolated, config);
        CHECK(step.ok);
        player = step.state;
    }
    const double standing = player.pos.z;
    std::printf("player on rest cabin: (%.6f, %.6f, %.6f) floor=%.6f\n", player.pos.x,
                player.pos.y, player.pos.z, floor_rest_z);
    CHECK_NEAR(standing, floor_rest_z, 1e-6);

    cabin.update(1.0);
    CHECK(isolated.set_dynamic(cabin.triangles(), {{0u, 0u}}, &error));
    const auto raised_floor = isolated.floor_at(kPlateX, kPlateY, kPlateTop + 1000.0);
    CHECK(raised_floor.has_value());
    std::printf("player floor after raise: z=%.6f (cabin top %.6f)\n",
                raised_floor.value_or(0.0), floor_raised_z);
    for (int i = 0; i < 30; ++i) {
        const gm::StepResult step = gm::step(player, gm::Input{}, isolated, config);
        CHECK(step.ok);
        player = step.state;
    }
    std::printf("player after raise: (%.6f, %.6f, %.6f) dz=%.6f (not carried; falls)\n",
                player.pos.x, player.pos.y, player.pos.z, player.pos.z - standing);
    CHECK(player.pos.z < standing);
}

} // namespace

int main() {
    const bool installed = fs::exists(kGame / "level14.sbw") && fs::exists(kGame / "level14.sbt");
    if (!installed) {
        std::printf("lift_collision: game not installed, skipped\n");
        return 0;
    }
    const LevelData level = load_level_data("level14");
    if (!level.ok) {
        std::printf("lift_collision: level14 not readable, skipped\n");
        return 0;
    }

    test_seam(level);
    test_runtime(level);

    if (g_failures != 0) {
        std::fprintf(stderr, "lift_collision: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("lift_collision: ok\n");
    return 0;
}
