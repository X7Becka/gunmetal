// A moving panel really carries the player standing on it (M6.4.1, slice 5;
// docs/PLATFORM_CARRY.md).
//
// Slice 4 made the lift cabin a kinematic panel and pinned the negative: a
// standing player is **not** carried by a raising cabin (`dz = -39.306847`,
// `test_lift_collision`).  This test pins the opt-in carry rule of
// `SimPlayer`:
//
//   * with `set_platform_carry(true)` the player standing on a dynamic floor
//     triangle (the `level14` KELLIFT stand-in `sec=35 obj=1`, bound to the lift
//     exactly like `test_lift_collision`) is moved by the panel's own delta:
//     a raise of 260 over 30 ticks lifts the player by 260 and keeps
//     `on_platform() == true`; a lowering moves it back down;
//   * stepping off (strafe, no jump) clears `on_platform()`, after which the
//     player falls normally and `platform_delta()` stays zero;
//   * a panel driven into a static wall carries the player until the capsule
//     would overlap the wall, then the transfer is cancelled: the position
//     never gets inside the wall;
//   * with the rule off (the default) the numbers are the slice-4 ones
//     verbatim: `dz = -39.306847`, `on_platform() == false`, no platform delta;
//   * the rule changes nothing while the panel rests (settle position and
//     `platform_delta() == 0`), and `set_platform_carry(false)` clears the
//     remembered panel.
//
// Real data used (level14, read from the shipped files):
//   lift0 KELLIFT pos=(-128,5816,620) pos_delta=(0,0,260) speed=300
//   stops=[0,65535], height 0.  The bound platform is `sec=35 obj=1` (52
//   triangles, AABB -243..-13 x 5701..5931 x 722..760, clean +Z top at 760).
// The wall case adds one synthetic quad (object 1 of the same sector) and
// drives the *same* cabin panel horizontally (`pos_delta` edited to (400,0,0)):
// the panel itself may pass through static geometry (no object-vs-object
// collision, docs/MOVING_COLLIDERS.md §6), the carried capsule may not.
//
// Every number below is printed, so the report can quote the run verbatim.
#include "game/runtime.h"

#include "game/chassis_loadout.h"
#include "game/sim_player.h"
#include "player/controller.h"
#include "render/collide.h"
#include "res/level.h"
#include "res/sbtvm.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "world/doors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
constexpr std::uint32_t kPlateSector = 35;
constexpr std::uint32_t kPlateObject = 1;
constexpr double kPlateX = -128.0;
constexpr double kPlateY = 5816.0;
constexpr double kPlateTop = 760.0;
/// The panel travels `pos_delta` (`(0,0,260)` on the shipped lift); the tests
/// raise/lower it in this many ticks.
constexpr int kTravelTicks = 30;
constexpr int kSettleTicks = 30;

// --- fixtures ---------------------------------------------------------------

struct Fixture {
    gm::SbtLevel sbt;
    gm::SbwLevel sbw;
    gm::VmState vm;
    gm::ChassisSpec chassis;
};

bool load_fixture(Fixture& out, std::string& error) {
    const auto level = gm::load_level(kGame, "level14", error);
    if (!level || !level->entities || !level->world) {
        error = "level14: no .sbt/.sbw pair";
        return false;
    }
    out.sbt = *level->entities;
    out.sbw = *level->world;
    out.vm = gm::vm_state_from_level(out.sbt);
    const auto chassis = gm::chassis_from_level(kGame / "level14.sbt", &error);
    if (!chassis) {
        return false;
    }
    for (const gm::ChassisSpec& spec : *chassis) {
        if (spec.name.find("LIGHT") != std::string::npos) {
            out.chassis = spec;
            return true;
        }
    }
    error = "level14: no LIGHT chassis record";
    return false;
}

/// A level with exactly one object, keeping its real 96-byte header and
/// geometry (same helper as `test_lift_collision.cpp`), so
/// `CollisionWorld::build` produces the same triangles and the single object
/// is sector 0 / object 0.
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

void put_f64(std::uint8_t* p, double value) { std::memcpy(p, &value, sizeof(value)); }

/// Identity object header (the `.sbw` object transform), same as
/// `test_weapons.cpp`.
void write_identity_header(std::array<std::uint8_t, 96>& header) {
    header.fill(0);
    for (int point = 0; point < 4; ++point) {
        for (int component = 0; component < 3; ++component) {
            const double value =
                (point == 0) ? 0.0 : (component == point - 1 ? 1.0 : 0.0);
            put_f64(header.data() + 8 * (point * 3 + component), value);
        }
    }
}

/// The plate-only world plus one synthetic vertical wall quad at `x = wall_x`,
/// as object 1 of the same sector (sector 0 / object 1 after the build).
gm::SbwLevel plate_and_wall_level(const gm::SbwLevel& level, double wall_x) {
    gm::SbwLevel out = single_object_level(level, kPlateSector, kPlateObject);
    gm::SbwObjectRef wall;
    write_identity_header(wall.header);
    out.header.sectors[0].objects.push_back(wall);
    out.header.object_count = 2;

    gm::SbwGeometry geometry;
    geometry.n_verts = 4;
    geometry.vertices = {gm::Vec3f{static_cast<float>(wall_x), 5600.0f, 600.0f},
                         gm::Vec3f{static_cast<float>(wall_x), 5600.0f, 1200.0f},
                         gm::Vec3f{static_cast<float>(wall_x), 6000.0f, 1200.0f},
                         gm::Vec3f{static_cast<float>(wall_x), 6000.0f, 600.0f}};
    gm::SbwPoly poly;
    poly.indices = {0, 1, 2, 3};
    poly.normal = {-1.0f, 0.0f, 0.0f}; // authored normal, facing the player
    geometry.polys.push_back(poly);
    geometry.n_polys = 1;
    out.geometry.push_back(geometry);
    return out;
}

/// Build the runtime from the real level14 records and the given geometry, bind
/// the single lift to the single plate object (sector 0 / object 0) and publish
/// the panel at `height = 0`.  `pos_delta` is the travel the panel uses.
std::unique_ptr<gm::GameRuntime> make_runtime(const Fixture& fixture,
                                              const gm::SbwLevel& geometry,
                                              std::array<int, 3> pos_delta,
                                              std::string& error) {
    gm::GameRuntimeConfig config;
    config.build_ai = false; // the tick only has to re-pose the panel
    auto runtime = std::make_unique<gm::GameRuntime>(config);
    if (!runtime->build(fixture.sbt, fixture.vm, geometry, &error)) {
        return nullptr;
    }
    if (runtime->world().lifts().size() != 1) {
        error = "level14: expected exactly one lift";
        return nullptr;
    }
    gm::Lift& lift = runtime->world().lifts_mutable()[0];
    lift.cabin_sector = 0;
    lift.cabin_object = 0;
    lift.pos_delta = pos_delta;
    lift.height = 0;
    lift.moving = false;
    if (!runtime->rebuild_door_colliders(&error)) {
        return nullptr;
    }
    return runtime;
}

/// Set the cabin height and let the runtime re-pose/publish the panel.
void move_panel(gm::GameRuntime& runtime, int height) {
    gm::Lift& lift = runtime.world().lifts_mutable()[0];
    lift.height = height;
    lift.moving = false;
    runtime.tick(1);
}

double panel_top(const gm::GameRuntime& runtime) {
    gm::Aabb box;
    for (const gm::Triangle& triangle : runtime.collision().dynamic_triangles()) {
        box.expand(triangle.bounds);
    }
    return box.hi.z;
}

/// Idle input, one fixed tick of `SimPlayer`.
bool tick_idle(gm::SimPlayer& player, gm::GameRuntime& runtime, int index,
               std::string& error) {
    gm::DriveSegment idle;
    return player.tick(runtime, idle, index, &error);
}

// --- 1. the raising and lowering cabin really carries the player -----------

void test_carry_up_and_down(const Fixture& fixture) {
    std::string error;
    auto runtime = make_runtime(fixture, single_object_level(fixture.sbw, kPlateSector,
                                                            kPlateObject),
                                {0, 0, 260}, error);
    CHECK(runtime != nullptr);
    if (!runtime) {
        std::fprintf(stderr, "platform_carry: runtime: %s\n", error.c_str());
        return;
    }
    CHECK(runtime->collision().dynamic_triangle_count() > 0);
    CHECK(runtime->collision().is_suppressed(0, 0));
    std::printf("platform_carry up/down: static=%zu dynamic=%zu suppressed=%zu lifts=%zu "
                "panel_top=%.6f\n",
                runtime->collision().triangle_count(),
                runtime->collision().dynamic_triangle_count(),
                runtime->collision().suppressed_count(),
                runtime->door_collision().lifts, panel_top(*runtime));

    gm::SimPlayer player;
    CHECK(player.init({kPlateX, kPlateY, kPlateTop + 10.0}, 0.0, fixture.chassis, &error));
    CHECK(!player.on_platform());
    CHECK(!player.platform_carry());
    player.set_platform_carry(true);
    CHECK(player.platform_carry());

    for (int i = 0; i < kSettleTicks; ++i) {
        CHECK(tick_idle(player, *runtime, i, error));
    }
    const double rest_z = player.state().pos.z;
    std::printf("  settle: feet z=%.17g on_ground=%d on_platform=%d delta=(%.6f, %.6f, "
                "%.6f)\n",
                rest_z, player.state().on_ground ? 1 : 0, player.on_platform() ? 1 : 0,
                player.platform_delta().x, player.platform_delta().y,
                player.platform_delta().z);
    CHECK(player.state().on_ground);
    CHECK(player.on_platform());
    CHECK_NEAR(rest_z, kPlateTop, 1e-5);
    // The panel rests: the rule must not move anything.
    CHECK_NEAR(player.platform_delta().z, 0.0, 0.0);

    // Raise the cabin to the top stop in kTravelTicks steps.
    double carried = 0.0;
    int on_platform_ticks = 0;
    int index = kSettleTicks;
    for (int step = 1; step <= kTravelTicks; ++step) {
        const int height = static_cast<int>((65535LL * step) / kTravelTicks);
        move_panel(*runtime, height);
        CHECK(tick_idle(player, *runtime, index++, error));
        carried += player.platform_delta().z;
        on_platform_ticks += player.on_platform() ? 1 : 0;
    }
    const double top_z = player.state().pos.z;
    const double top_panel = panel_top(*runtime);
    std::printf("  raise: height=%d panel_top=%.6f feet z %.6f -> %.6f dz=%.6f "
                "carried=%.6f on_platform_ticks=%d/%d on_ground=%d\n",
                runtime->world().lifts()[0].height, top_panel, rest_z, top_z,
                top_z - rest_z, carried, on_platform_ticks, kTravelTicks,
                player.state().on_ground ? 1 : 0);
    CHECK_NEAR(top_panel - kPlateTop, 260.0, 1e-9);
    CHECK_NEAR(top_z - rest_z, 260.0, 1e-3);
    CHECK_NEAR(carried, 260.0, 1e-3);
    CHECK(on_platform_ticks == kTravelTicks);
    CHECK(player.on_platform());
    CHECK(player.state().on_ground);
    CHECK_NEAR(top_z, top_panel, 1e-5);

    // Lower it back: the same rule, sign preserved.
    carried = 0.0;
    on_platform_ticks = 0;
    for (int step = 1; step <= kTravelTicks; ++step) {
        const int height = 65535 - static_cast<int>((65535LL * step) / kTravelTicks);
        move_panel(*runtime, height);
        CHECK(tick_idle(player, *runtime, index++, error));
        carried += player.platform_delta().z;
        on_platform_ticks += player.on_platform() ? 1 : 0;
    }
    const double back_z = player.state().pos.z;
    std::printf("  lower: panel_top=%.6f feet z %.6f -> %.6f dz=%.6f carried=%.6f "
                "on_platform_ticks=%d/%d on_ground=%d\n",
                panel_top(*runtime), top_z, back_z, back_z - top_z, carried,
                on_platform_ticks, kTravelTicks, player.state().on_ground ? 1 : 0);
    CHECK_NEAR(back_z - top_z, -260.0, 1e-3);
    CHECK_NEAR(carried, -260.0, 1e-3);
    CHECK(on_platform_ticks == kTravelTicks);
    CHECK(player.on_platform());
    CHECK_NEAR(back_z, rest_z, 1e-3);

    // Step off the panel: strafe right (+X at yaw 0) until the capsule leaves
    // the 230-unit plate; from then on the ordinary fall takes over.
    gm::DriveSegment walk;
    walk.strafe = 1.0;
    int left_off = -1;
    for (int i = 0; i < 240; ++i) {
        CHECK(player.tick(*runtime, walk, i, &error));
        if (left_off < 0 && !player.on_platform()) {
            left_off = i;
            std::printf("  step off: tick %d feet=(%.6f, %.6f, %.6f) on_platform=0\n", i,
                        player.state().pos.x, player.state().pos.y, player.state().pos.z);
        }
    }
    const double off_z = player.state().pos.z;
    std::printf("  step off end: feet=(%.6f, %.6f, %.6f) on_platform=%d delta_z=%.6f "
                "plate_x=-243..-13\n",
                player.state().pos.x, player.state().pos.y, off_z,
                player.on_platform() ? 1 : 0, off_z - rest_z);
    CHECK(left_off >= 0);
    CHECK(!player.on_platform());
    CHECK(player.state().pos.x > -13.0); // really past the plate
    CHECK(off_z < rest_z - 10.0);        // and falling, not carried
    const double fallen_from = player.state().pos.z;
    for (int i = 0; i < 30; ++i) {
        CHECK(tick_idle(player, *runtime, i, error));
    }
    std::printf("  after step off: 30 idle ticks z %.6f -> %.6f (dz=%.6f) on_platform=%d "
                "carried=%.6f\n",
                fallen_from, player.state().pos.z, player.state().pos.z - fallen_from,
                player.on_platform() ? 1 : 0, player.platform_delta().z);
    CHECK(player.state().pos.z < fallen_from);
    CHECK_NEAR(player.platform_delta().z, 0.0, 0.0);

    // Turning the rule off forgets the panel (and the delta).
    player.set_platform_carry(false);
    CHECK(!player.platform_carry());
    CHECK(!player.on_platform());
    CHECK_NEAR(player.platform_delta().z, 0.0, 0.0);
}

// --- 2. the rule off: the slice-4 numbers verbatim -------------------------

void test_carry_off_parity(const Fixture& fixture) {
    std::string error;
    auto runtime = make_runtime(fixture, single_object_level(fixture.sbw, kPlateSector,
                                                            kPlateObject),
                                {0, 0, 260}, error);
    CHECK(runtime != nullptr);
    if (!runtime) {
        std::fprintf(stderr, "platform_carry parity: runtime: %s\n", error.c_str());
        return;
    }
    gm::SimPlayer player;
    CHECK(player.init({kPlateX, kPlateY, kPlateTop + 10.0}, 0.0, fixture.chassis, &error));
    // Deliberately never enabled: this is the default.
    CHECK(!player.platform_carry());
    for (int i = 0; i < kSettleTicks; ++i) {
        CHECK(tick_idle(player, *runtime, i, error));
    }
    const double rest_z = player.state().pos.z;
    CHECK(player.state().on_ground);
    CHECK(!player.on_platform());

    move_panel(*runtime, 65535);
    CHECK_NEAR(panel_top(*runtime) - kPlateTop, 260.0, 1e-9);
    for (int i = 0; i < kSettleTicks; ++i) {
        CHECK(tick_idle(player, *runtime, i, error));
    }
    const double after_z = player.state().pos.z;
    const double dz = after_z - rest_z;
    std::printf("platform_carry off: rest z=%.17g after cable raise z=%.17g dz=%.17g "
                "on_platform=%d carried=%.6f panel_top=%.6f\n",
                rest_z, after_z, dz, player.on_platform() ? 1 : 0,
                player.platform_delta().z, panel_top(*runtime));
    // The slice-4 golden of `test_lift_collision` (same 30 settling ticks, same
    // panel raise by 260): the player is not carried and falls through it.
    CHECK_NEAR(dz, -39.306847, 1e-6);
    CHECK(!player.on_platform());
    CHECK_NEAR(player.platform_delta().z, 0.0, 0.0);
}

// --- 3. a panel driven into a wall stops the transfer ----------------------

void test_wall_block(const Fixture& fixture) {
    std::string error;
    constexpr double kWallX = 160.0;
    auto runtime = make_runtime(fixture, plate_and_wall_level(fixture.sbw, kWallX),
                                {400, 0, 0}, error);
    CHECK(runtime != nullptr);
    if (!runtime) {
        std::fprintf(stderr, "platform_carry wall: runtime: %s\n", error.c_str());
        return;
    }
    std::printf("wall: static=%zu dynamic=%zu suppressed=%zu wall_x=%.1f\n",
                runtime->collision().triangle_count(),
                runtime->collision().dynamic_triangle_count(),
                runtime->collision().suppressed_count(), kWallX);
    CHECK(runtime->collision().dynamic_triangle_count() > 0);

    gm::SimPlayer player;
    CHECK(player.init({kPlateX, kPlateY, kPlateTop + 10.0}, 0.0, fixture.chassis, &error));
    player.set_platform_carry(true);
    for (int i = 0; i < kSettleTicks; ++i) {
        CHECK(tick_idle(player, *runtime, i, error));
    }
    const double rest_z = player.state().pos.z;
    const double rest_x = player.state().pos.x;
    std::printf("  settle: feet=(%.6f, %.6f, %.6f) on_platform=%d\n", rest_x,
                player.state().pos.y, rest_z, player.on_platform() ? 1 : 0);
    CHECK(player.on_platform());

    // Slide the panel (pos_delta = (400,0,0)) into the wall at x = 160.
    double max_x = rest_x;
    double moved = 0.0;
    int blocked_ticks = 0;
    int index = kSettleTicks;
    for (int step = 1; step <= kTravelTicks; ++step) {
        const int height = static_cast<int>((65535LL * step) / kTravelTicks);
        move_panel(*runtime, height);
        CHECK(tick_idle(player, *runtime, index++, error));
        const double dx = player.platform_delta().x;
        moved += dx;
        if (dx == 0.0 && step > 1) {
            ++blocked_ticks;
        }
        max_x = std::max(max_x, player.state().pos.x);
    }
    const double end_x = player.state().pos.x;
    const double end_z = player.state().pos.z;
    std::printf("  wall: feet x %.6f -> %.6f (moved=%.6f, max_x=%.6f) blocked_ticks=%d "
                "capsule_limit=%.6f on_platform=%d z=%.6f\n",
                rest_x, end_x, moved, max_x, blocked_ticks, kWallX - 3.0,
                player.on_platform() ? 1 : 0, end_z);
    // The player really moved until the capsule reached the wall ...
    CHECK(moved > 200.0);
    CHECK(blocked_ticks > 0);
    // ... and never got inside it: the cancelled transfer keeps the last safe
    // position (distance to the wall >= radius).
    CHECK(max_x <= kWallX - 3.0 + 1e-6);
    CHECK(end_x <= kWallX - 3.0 + 1e-6);
    // The blocked tick transferred nothing at all (the whole delta is dropped).
    CHECK_NEAR(player.platform_delta().x, 0.0, 0.0);
}

} // namespace

int main() {
    const bool installed = fs::exists(kGame / "level14.sbw") && fs::exists(kGame / "level14.sbt");
    if (!installed) {
        std::printf("platform_carry: game not installed, skipped\n");
        return 0;
    }
    Fixture fixture;
    std::string error;
    if (!load_fixture(fixture, error)) {
        std::printf("platform_carry: level14 not readable (%s), skipped\n", error.c_str());
        return 0;
    }

    test_carry_up_and_down(fixture);
    test_carry_off_parity(fixture);
    test_wall_block(fixture);

    if (g_failures != 0) {
        std::fprintf(stderr, "platform_carry: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("platform_carry: ok\n");
    return 0;
}
