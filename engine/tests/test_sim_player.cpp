// Scripted player over `GameRuntime` (`M6.5.1`, slice 1; docs/SIM_PLAYER.md).
//
// The module is a caller of two already pinned pieces: the fixed-tick
// controller (`player/controller.h`, `gm_player_controller_tests`) and the
// `0x4453d0` weapon seam of the runtime (`game/trigger.h`,
// `gm_player_trigger_tests`).  This test therefore checks the *composition*,
// on real levels, not the physics or the weapon rules:
//
//   * `parse_drive()` accepts the documented script and rejects every bad
//     field with a text (no exception);
//   * on `dm1`, 900 ticks of `forward=1` from the campaign spawn of slot 0
//     move the player, keep every coordinate finite and end on the ground;
//   * two runs of the same plan are identical to 1e-12;
//   * running into the nearest wall the level geometry actually has (found
//     with `CollisionWorld::raycast`, not by eye) reports `hit_wall`;
//   * a `fire=300` segment spends ammo, counts shots and advances
//     `Arsenal::shot_index`;
//   * level1 is a smoke case (the install ships level1/level2 swapped by name,
//     and this test follows the name-based `--sim` path, so the numbers below
//     describe the file called level1.sbt).
//
// The load follows `engine/src/main.cpp::simulate()` and
// `res/level.cpp::load_level()`: `.sbt` + `.sbw` -> `parse_sbt` (inside
// `load_level`), `decode_sbt_script`, `vm_state_from_level`, `GameRuntime::build`,
// `extract_scene` for the spawn and `chassis_from_level` for the LIGHT chassis.
#include "game/sim_player.h"

#include "game/chassis_loadout.h"
#include "game/runtime.h"
#include "game/weapons.h"
#include "res/level.h"
#include "res/scene.h"
#include "res/sbt.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "res/sbtvm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
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
constexpr double kPi = 3.14159265358979323846;

/// One parsed level plus the runtime built from it, the campaign spawn of
/// slot 0 and the LIGHT chassis of that level.
struct Fixture {
    std::unique_ptr<gm::GameRuntime> runtime;
    gm::Vec3d feet{};
    double yaw = 0.0;
    gm::ChassisSpec chassis;
    std::size_t instructions = 0;
};

/// Same runtime configuration as `main.cpp::simulate()`: the AI -> movement
/// bridge is on (the runtime is built exactly like the CLI slice).  The test
/// itself never calls `GameRuntime::tick()`, so the AI cannot move the player.
gm::GameRuntimeConfig sim_config() {
    gm::GameRuntimeConfig config;
    config.ai_apply_aim = true;
    return config;
}

bool load_fixture(const std::string& name, Fixture& out, std::string& error) {
    const auto level = gm::load_level(kGame, name, error);
    if (!level) {
        return false;
    }
    if (!level->entities || !level->world) {
        error = name + ": no .sbt/.sbw pair";
        return false;
    }

    // The compiled script must decode; `vm_state_from_level` and
    // `chassis_from_level` decode it again, this is the explicit check and
    // the instruction count printed by the test.
    std::string script_error;
    const auto program = gm::decode_sbt_script(level->entities->script_code, script_error);
    if (!program) {
        error = name + ": script: " + script_error;
        return false;
    }
    out.instructions = program->insns.size();

    const gm::VmState vm = gm::vm_state_from_level(*level->entities);
    out.runtime = std::make_unique<gm::GameRuntime>(sim_config());
    if (!out.runtime->build(*level->entities, vm, *level->world, &error)) {
        return false;
    }

    std::string scene_error;
    const auto scene = gm::extract_scene(*level->entities, scene_error, false);
    if (!scene) {
        error = name + ": scene: " + scene_error;
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

    std::string chassis_error;
    const auto chassis = gm::chassis_from_level(kGame / (name + ".sbt"), &chassis_error);
    if (!chassis) {
        error = name + ": chassis: " + chassis_error;
        return false;
    }
    const gm::ChassisSpec* light = nullptr;
    for (const gm::ChassisSpec& spec : *chassis) {
        if (spec.name.find("LIGHT") != std::string::npos) {
            light = &spec;
            break;
        }
    }
    if (light == nullptr) {
        error = name + ": no LIGHT chassis record";
        return false;
    }
    out.chassis = *light;
    return true;
}

/// The caller loop of `SimPlayer::tick`: `0..segment.ticks-1`.  `wall_hits`
/// counts the ticks whose step reported a wall contact.
bool drive(gm::SimPlayer& player, gm::GameRuntime& runtime,
           const gm::DriveSegment& segment, std::string& error, int* wall_hits = nullptr) {
    for (int tick = 0; tick < segment.ticks; ++tick) {
        if (!player.tick(runtime, segment, tick, &error)) {
            return false;
        }
        if (wall_hits != nullptr && player.last_step().hit_wall) {
            ++*wall_hits;
        }
    }
    return true;
}

bool finite_state(const gm::PlayerState& state) {
    return gm::finite(state.pos) && gm::finite(state.vel) && std::isfinite(state.yaw) &&
           std::isfinite(state.height) && std::isfinite(state.eye_height);
}

double horizontal_distance(const gm::Vec3d& a, const gm::Vec3d& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// --- parser ----------------------------------------------------------------

void test_parse_drive() {
    std::string error = "stale";
    const auto parsed =
        gm::parse_drive("forward=1:900;turn=0.5:300;fire:200;jump=5", &error);
    CHECK(parsed.has_value());
    CHECK(error.empty());
    if (parsed) {
        CHECK(parsed->size() == 4);
        if (parsed->size() == 4) {
            CHECK((*parsed)[0].ticks == 900);
            CHECK((*parsed)[0].forward == 1.0);
            CHECK((*parsed)[0].strafe == 0.0);
            CHECK((*parsed)[0].turn == 0.0);
            CHECK(!(*parsed)[0].fire);
            CHECK(!(*parsed)[0].jump);
            CHECK(!(*parsed)[0].crouch);

            CHECK((*parsed)[1].ticks == 300);
            CHECK((*parsed)[1].turn == 0.5);

            CHECK((*parsed)[2].ticks == 200);
            CHECK((*parsed)[2].fire);
            CHECK((*parsed)[2].forward == 0.0);

            CHECK((*parsed)[3].ticks == 5);
            CHECK((*parsed)[3].jump);
            std::printf("sim_player: parse_drive -> %zu segments (ticks %d/%d/%d/%d)\n",
                        parsed->size(), (*parsed)[0].ticks, (*parsed)[1].ticks,
                        (*parsed)[2].ticks, (*parsed)[3].ticks);
        }
    }

    // Spaces/tabs around fields, a negative axis value, explicit booleans and
    // the "do not fire" spelling a generator would emit.
    error.clear();
    const auto spaced = gm::parse_drive(" strafe = -0.25 : 7 ; crouch=1:3; fire=0:2\t", &error);
    CHECK(spaced.has_value());
    CHECK(error.empty());
    if (spaced && spaced->size() == 3) {
        CHECK_NEAR((*spaced)[0].strafe, -0.25, 0.0);
        CHECK((*spaced)[0].ticks == 7);
        CHECK((*spaced)[1].crouch);
        CHECK(!(*spaced)[2].fire);
        CHECK((*spaced)[2].ticks == 2);
    }

    // The upper tick bound is accepted, one above it is not.
    error.clear();
    CHECK(gm::parse_drive("forward=1:1000000", &error).has_value());
    error.clear();
    CHECK(!gm::parse_drive("forward=1:1000001", &error).has_value());
    CHECK(!error.empty());
}

// --- parser / init / tick negatives ---------------------------------------

void test_negatives(Fixture& fixture) {
    // Every rejection must produce nullopt and a text, never an exception.
    struct Case {
        const char* script;
        const char* needle; // substring expected in the error
    };
    const Case cases[] = {
        {"forward=abc:10", "bad number"},
        {"", "empty"},
        {"forward=1:0", "ticks"},
        {"forward=1:-5", "ticks"},
        {"forward=1:1000001", "ticks"},
        {"forward=1:2.5", "ticks"},
        {"forward=1", "needs a value"},   // no tick field, no value
        {"forward", "expected"},          // neither value nor ticks
        {"forward:10", "needs a value"},
        {"forward=:10", "bad number"},
        {"fire=abc:10", "bad boolean"},
        {"fire", "expected"},             // no ticks
        {"nope=1:10", "unknown key"},
        {"=1:10", "missing key"},
        {"forward=1:10;", "empty segment"},
        {";forward=1:10", "empty segment"},
        {"forward=nan:10", "bad number"},
        {"forward=inf:10", "bad number"},
        {"forward=-inf:10", "bad number"},
        {"forward=1e999:10", "bad number"},
        {"turn=,5:10", "bad number"},
        {"forward=1:10:20", "more than one ':'"},
    };
    int rejected = 0;
    for (const Case& item : cases) {
        std::string error;
        const auto parsed = gm::parse_drive(item.script, &error);
        bool ok = !parsed.has_value() && !error.empty();
        if (!ok) {
            std::fprintf(stderr, "FAIL parse_drive(\"%s\") accepted or silent\n", item.script);
            ++g_failures;
        }
        if (!error.empty() && error.find(item.needle) == std::string::npos) {
            std::fprintf(stderr, "FAIL parse_drive(\"%s\"): error \"%s\" has no \"%s\"\n",
                         item.script, error.c_str(), item.needle);
            ++g_failures;
            ok = false;
        }
        if (ok) {
            ++rejected;
        }
    }
    std::printf("sim_player: parse_drive negatives %d/%zu rejected with a text\n", rejected,
                sizeof(cases) / sizeof(cases[0]));
    CHECK(rejected == static_cast<int>(sizeof(cases) / sizeof(cases[0])));

    // init() guards: a non-finite spawn/yaw and a broken chassis eye/speed.
    std::string error;
    const double nan = std::nan("");
    gm::SimPlayer player;
    CHECK(!player.init({nan, 0.0, 0.0}, 0.0, fixture.chassis, &error));
    CHECK(!error.empty());
    CHECK(!player.initialized());
    error.clear();
    CHECK(!player.init(fixture.feet, nan, fixture.chassis, &error));
    CHECK(!error.empty());

    gm::ChassisSpec broken = fixture.chassis;
    broken.eye_height = 0.0;
    error.clear();
    CHECK(!player.init(fixture.feet, fixture.yaw, broken, &error));
    CHECK(!error.empty());
    broken = fixture.chassis;
    broken.eye_height = nan;
    error.clear();
    CHECK(!player.init(fixture.feet, fixture.yaw, broken, &error));
    CHECK(!error.empty());
    broken = fixture.chassis;
    broken.max_speed = -1.0;
    error.clear();
    CHECK(!player.init(fixture.feet, fixture.yaw, broken, &error));
    CHECK(!error.empty());
    broken = fixture.chassis;
    broken.max_speed = std::numeric_limits<double>::infinity();
    error.clear();
    CHECK(!player.init(fixture.feet, fixture.yaw, broken, &error));
    CHECK(!error.empty());

    // tick() before init() and a negative segment index.
    error.clear();
    CHECK(!player.tick(*fixture.runtime, gm::DriveSegment{}, 0, &error));
    CHECK(!error.empty());

    CHECK(player.init(fixture.feet, fixture.yaw, fixture.chassis, &error));
    error.clear();
    CHECK(!player.tick(*fixture.runtime, gm::DriveSegment{}, -1, &error));
    CHECK(!error.empty());

    // A non-finite value reaches tick() only when the caller builds the struct
    // directly (`parse_drive()` rejects it earlier).  The tick is refused and
    // neither the state nor the tick counter changes.
    const gm::PlayerState before = player.state();
    gm::DriveSegment bad;
    bad.forward = nan;
    error.clear();
    CHECK(!player.tick(*fixture.runtime, bad, 0, &error));
    CHECK(!error.empty());
    bad = gm::DriveSegment{};
    bad.turn = std::numeric_limits<double>::infinity();
    error.clear();
    CHECK(!player.tick(*fixture.runtime, bad, 0, &error));
    CHECK(!error.empty());
    CHECK(player.tick_index() == 0);
    CHECK(player.state().pos.x == before.pos.x && player.state().pos.y == before.pos.y &&
          player.state().pos.z == before.pos.z);
    CHECK(player.shots_fired() == 0);
}

// --- dm1: movement, determinism, walls ------------------------------------

void test_dm1_forward(Fixture& fixture) {
    std::string error;
    std::printf("sim_player: dm1 spawn (%.17g, %.17g, %.17g) yaw=%.17g script=%zu insns\n",
                fixture.feet.x, fixture.feet.y, fixture.feet.z, fixture.yaw,
                fixture.instructions);

    const auto plan = gm::parse_drive("forward=1:900", &error);
    CHECK(plan.has_value());
    if (!plan) {
        std::fprintf(stderr, "sim_player: %s\n", error.c_str());
        ++g_failures;
        return;
    }

    gm::SimPlayer player;
    CHECK(player.init(fixture.feet, fixture.yaw, fixture.chassis, &error));
    CHECK(player.initialized());
    CHECK(player.tick_index() == 0);
    CHECK_NEAR(player.config().eye_height, fixture.chassis.eye_height, 0.0);
    CHECK_NEAR(player.config().max_speed, fixture.chassis.max_speed * 91.0, 1e-12);
    CHECK_NEAR(player.config().retention, 0.9, 1e-12);
    CHECK_NEAR(player.state().pos.x, fixture.feet.x, 0.0);
    CHECK_NEAR(player.state().pos.y, fixture.feet.y, 0.0);
    CHECK_NEAR(player.state().pos.z, fixture.feet.z, 0.0);
    CHECK_NEAR(player.yaw(), fixture.yaw, 0.0);

    int wall_hits = 0;
    CHECK(drive(player, *fixture.runtime, (*plan)[0], error, &wall_hits));
    CHECK(player.tick_index() == 900);
    const gm::PlayerState& state = player.state();
    const double travelled = horizontal_distance(fixture.feet, state.pos);
    std::printf("sim_player: dm1 forward=1:900 -> (%.17g, %.17g, %.17g) travelled=%.17g "
                "on_ground=%d wall_ticks=%d shots=%d\n",
                state.pos.x, state.pos.y, state.pos.z, travelled,
                state.on_ground ? 1 : 0, wall_hits, player.shots_fired());

    CHECK(finite_state(state));
    CHECK(travelled > 100.0);
    // Landing, not flying: the feet sit on the contact skin.  `on_ground`
    // stays 0 because the shipped `dm1` floor polygons are wound downwards
    // (`RayHit::front_face == false`), so the controller's floor probe
    // classifies them as ceilings (docs/COLLIDE.md §6.2 and the level0 case of
    // `test_player_controller`).  The grounded case is pinned separately on a
    // front-wound `dm1` floor in `test_dm1_ground()`.
    CHECK(!state.on_ground);
    CHECK_NEAR(state.pos.z, player.config().contact_skin, 1e-4);
    CHECK(player.shots_fired() == 0);
    CHECK(!std::isnan(state.pos.x) && !std::isnan(state.pos.y) && !std::isnan(state.pos.z));
    CHECK_NEAR(player.yaw(), fixture.yaw, 0.0); // no turn in this segment

    // The camera point is the eye the runtime was given.
    const gm::Vec3d eye = gm::eye_position(state);
    CHECK_NEAR(eye.z, state.pos.z + fixture.chassis.eye_height, 1e-12);
}

/// The `on_ground == true` case on real `dm1` geometry: find a front-wound
/// floor triangle of the level (what `surface_of` calls a floor, i.e. what the
/// controller's ground probe accepts) and rest the capsule on it.  Nothing
/// synthetic: the triangle comes from `dm1.sbw`.
void test_dm1_ground(Fixture& fixture) {
    std::string error;
    const gm::CollisionWorld& world = fixture.runtime->collision();
    // Collect the level's own front-wound floors (n.z >= 0.9: the flattest
    // patches are ramps, so the capsule settles a little below the centroid),
    // nearest to the campaign spawn first, then rest the capsule on the first
    // one that keeps `on_ground` for ten idle ticks without drifting.
    std::vector<std::size_t> candidates;
    const std::vector<gm::Triangle>& triangles = world.triangles();
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        const gm::Triangle& triangle = triangles[index];
        if (triangle.normal.z < 0.9) {
            continue; // too steep: the capsule would slide off
        }
        if (gm::surface_of(triangle) != gm::Surface::floor) {
            continue; // wound downwards: the ground probe rejects it
        }
        const double extent_x = triangle.bounds.hi.x - triangle.bounds.lo.x;
        const double extent_y = triangle.bounds.hi.y - triangle.bounds.lo.y;
        if (extent_x < 8.0 || extent_y < 8.0) {
            continue; // narrower than the capsule: the feet would hang over
        }
        const auto floor_z = world.floor_at(triangle.centroid.x, triangle.centroid.y,
                                            triangle.centroid.z + 1.0, 5.0);
        if (!floor_z || std::fabs(*floor_z - triangle.centroid.z) > 1e-6) {
            continue; // something else is above this triangle
        }
        candidates.push_back(index);
    }
    std::sort(candidates.begin(), candidates.end(), [&](std::size_t l, std::size_t r) {
        return horizontal_distance(triangles[l].centroid, fixture.feet) <
               horizontal_distance(triangles[r].centroid, fixture.feet);
    });

    int resting = 0;
    gm::Vec3d resting_at{};
    double resting_z = 0.0;
    std::uint32_t resting_sector = 0;
    int tried = 0;
    for (const std::size_t index : candidates) {
        if (++tried > 200) {
            break;
        }
        const gm::Triangle& triangle = triangles[index];
        gm::SimPlayer player;
        if (!player.init(triangle.centroid, 0.0, fixture.chassis, &error)) {
            continue;
        }
        const gm::DriveSegment idle;
        bool ok = true;
        for (int i = 0; i < 10 && ok; ++i) {
            ok = player.tick(*fixture.runtime, idle, i, &error);
        }
        if (!ok) {
            continue;
        }
        const double drift = horizontal_distance(triangle.centroid, player.state().pos);
        if (player.state().on_ground && drift < 1.0) {
            ++resting;
            resting_at = triangle.centroid;
            resting_z = player.state().pos.z;
            resting_sector = triangle.sector;
            break;
        }
    }
    std::printf("sim_player: dm1 front-wound flat floors: %zu candidate(s), tried=%d, "
                "resting=%d at (%.6f, %.6f, %.6f) sector=%u feet_z=%.17g\n",
                candidates.size(), tried, resting, resting_at.x, resting_at.y, resting_at.z,
                resting_sector, resting_z);
    CHECK(!candidates.empty());
    CHECK(resting == 1);
    // The selected patch is one of the level's ramps (normal.z >= 0.9), so the
    // capsule settles a little below the triangle centroid on the slope; the
    // grounded invariant is the probe itself, not an exact z golden.
    CHECK(resting_z <= resting_at.z + 1e-3);
    CHECK(resting_at.z - resting_z < 3.0); // within one capsule radius
}

void test_dm1_determinism(Fixture& fixture) {
    std::string error;
    const auto plan = gm::parse_drive("forward=1:900", &error);
    CHECK(plan.has_value());
    if (!plan) {
        return;
    }

    gm::SimPlayer first;
    gm::SimPlayer second;
    CHECK(first.init(fixture.feet, fixture.yaw, fixture.chassis, &error));
    CHECK(second.init(fixture.feet, fixture.yaw, fixture.chassis, &error));
    CHECK(drive(first, *fixture.runtime, (*plan)[0], error));
    // Two independent players on the same runtime: the only runtime state they
    // share is the eye list, overwritten on every tick, so the second run
    // starts from the same player state.
    CHECK(drive(second, *fixture.runtime, (*plan)[0], error));

    const gm::PlayerState& a = first.state();
    const gm::PlayerState& b = second.state();
    std::printf("sim_player: determinism first=(%.17g, %.17g, %.17g, yaw %.17g)\n",
                a.pos.x, a.pos.y, a.pos.z, a.yaw);
    std::printf("             second=(%.17g, %.17g, %.17g, yaw %.17g)\n",
                b.pos.x, b.pos.y, b.pos.z, b.yaw);
    CHECK_NEAR(a.pos.x, b.pos.x, 1e-12);
    CHECK_NEAR(a.pos.y, b.pos.y, 1e-12);
    CHECK_NEAR(a.pos.z, b.pos.z, 1e-12);
    CHECK_NEAR(a.yaw, b.yaw, 1e-12);
    CHECK(first.tick_index() == second.tick_index());
}

/// The nearest horizontal geometry around the spawn, from the level's own
/// triangle soup: eight directions, sorted by ray distance.
struct WallProbe {
    double yaw = 0.0;
    double distance = 0.0;
};

std::vector<WallProbe> probe_walls(const gm::CollisionWorld& world, const gm::Vec3d& eye,
                                   double max_distance) {
    std::vector<WallProbe> probes;
    for (int k = 0; k < 8; ++k) {
        const double yaw = static_cast<double>(k) * kPi / 4.0;
        const gm::Vec3d direction{std::cos(yaw), std::sin(yaw), 0.0};
        const auto hit = world.raycast(eye, direction, max_distance);
        if (hit) {
            probes.push_back({yaw, hit->t});
        }
    }
    std::sort(probes.begin(), probes.end(),
              [](const WallProbe& l, const WallProbe& r) { return l.distance < r.distance; });
    return probes;
}

void test_dm1_wall(Fixture& fixture) {
    std::string error;
    const gm::Vec3d eye{fixture.feet.x, fixture.feet.y,
                        fixture.feet.z + fixture.chassis.eye_height};
    const std::vector<WallProbe> probes = probe_walls(fixture.runtime->collision(), eye, 2000.0);
    CHECK(!probes.empty());
    if (probes.empty()) {
        return;
    }
    std::printf("sim_player: dm1 wall probes (yaw deg, ray distance):");
    for (const WallProbe& probe : probes) {
        std::printf(" %.0f/%.3f", probe.yaw * 180.0 / kPi, probe.distance);
    }
    std::printf("\n");

    // Run at the nearest geometry first, then the next ones: the capsule may
    // slide along a slanted triangle instead of reporting a hit, and the test
    // records the actual numbers either way.
    int blocked = 0;
    double blocked_yaw = 0.0;
    double blocked_distance = 0.0;
    int blocked_ticks = 0;
    for (const WallProbe& probe : probes) {
        gm::SimPlayer player;
        if (!player.init(fixture.feet, probe.yaw, fixture.chassis, &error)) {
            std::fprintf(stderr, "sim_player: init: %s\n", error.c_str());
            ++g_failures;
            return;
        }
        gm::DriveSegment segment;
        segment.forward = 1.0;
        // Reach the probed triangle (at ~max_speed) plus the acceleration ramp.
        const double per_tick = player.config().max_speed * player.config().dt;
        segment.ticks = std::min(gm::kDriveMaxTicks,
                                 200 + static_cast<int>(std::ceil(probe.distance / per_tick)));
        int wall_hits = 0;
        if (!drive(player, *fixture.runtime, segment, error, &wall_hits)) {
            std::fprintf(stderr, "sim_player: wall drive: %s\n", error.c_str());
            ++g_failures;
            return;
        }
        const double moved = horizontal_distance(fixture.feet, player.state().pos);
        std::printf("           forward yaw=%7.2f deg for %4d ticks: wall_ticks=%3d "
                    "moved=%.3f (ray %.3f) on_ground=%d\n",
                    probe.yaw * 180.0 / kPi, segment.ticks, wall_hits, moved,
                    probe.distance, player.state().on_ground ? 1 : 0);
        if (wall_hits > 0) {
            if (blocked == 0) {
                blocked_yaw = probe.yaw;
                blocked_distance = probe.distance;
                blocked_ticks = wall_hits;
            }
            ++blocked;
        }
    }
    std::printf("sim_player: blocked directions=%d (first yaw=%.17g, ray=%.3f, "
                "wall_ticks=%d)\n",
                blocked, blocked_yaw, blocked_distance, blocked_ticks);
    CHECK(blocked > 0);

    // The same direction replayed: deterministic, and the player did not pass
    // through the probed geometry.
    if (blocked > 0) {
        gm::SimPlayer player;
        CHECK(player.init(fixture.feet, blocked_yaw, fixture.chassis, &error));
        gm::DriveSegment segment;
        segment.forward = 1.0;
        const double per_tick = player.config().max_speed * player.config().dt;
        segment.ticks = std::min(gm::kDriveMaxTicks,
                                 200 + static_cast<int>(std::ceil(blocked_distance / per_tick)));
        int wall_hits = 0;
        CHECK(drive(player, *fixture.runtime, segment, error, &wall_hits));
        CHECK(wall_hits == blocked_ticks);
        CHECK(wall_hits > 0);
        CHECK(finite_state(player.state()));
        CHECK(horizontal_distance(fixture.feet, player.state().pos) <=
              blocked_distance + 1.0);
    }
}

// --- dm1: fire -------------------------------------------------------------

void test_dm1_fire(Fixture& fixture) {
    std::string error;

    // The real weapon table of level0 (docs/SIM_PLAYER.md §4.3) with the
    // synthetic loadout below: the table-only `load` has an empty mask and
    // zero ammo (no save is read here), so slot 0 is made available with 50
    // rounds.
    const auto weapons = gm::Weapons::load(kGame / "level0.sbt", &error);
    CHECK(weapons.has_value());
    if (!weapons) {
        std::fprintf(stderr, "sim_player: weapons: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    const gm::Weapon* weapon = weapons->at(0);
    CHECK(weapon != nullptr);
    if (weapon == nullptr) {
        return;
    }
    gm::Arsenal arsenal = weapons->initial();
    arsenal.mask = 0x1u;
    arsenal.current = 0;
    arsenal.ammo[0] = 50;
    fixture.runtime->set_player_weapons(*weapons, arsenal);
    CHECK(fixture.runtime->has_player_weapons());

    gm::SimPlayer player;
    CHECK(player.init(fixture.feet, fixture.yaw, fixture.chassis, &error));
    // `ammo()` reads the runtime the player was last ticked with; before the
    // first tick there is no binding and the accessor is 0.
    CHECK(player.ammo(0) == 0u);
    CHECK(player.ammo(-1) == 0u);
    CHECK(player.ammo(gm::kWeaponSlots) == 0u);
    const std::uint32_t ammo_before = fixture.runtime->player_arsenal().ammo[0];
    CHECK(ammo_before == 50u);

    const auto plan = gm::parse_drive("fire:300", &error);
    CHECK(plan.has_value());
    if (!plan) {
        return;
    }
    CHECK(drive(player, *fixture.runtime, (*plan)[0], error));

    const gm::Arsenal& after = fixture.runtime->player_arsenal();
    std::printf("sim_player: fire:300 slot=0 weapon=\"%s\" mode=%d period=%d ticks -> "
                "shots=%d ammo %u->%u shot_index=%llu spawned=%d fired=%d\n",
                weapon->name.c_str(), weapon->fire_mode, weapon->fire_ticks,
                player.shots_fired(), ammo_before, after.ammo[0],
                static_cast<unsigned long long>(after.shot_index),
                player.last_weapon().spawned,
                player.last_weapon().action.fired() ? 1 : 0);

    CHECK(player.shots_fired() > 0);
    CHECK(after.ammo[0] < ammo_before);
    CHECK(after.shot_index > 0u);
    CHECK(player.ammo(0) == after.ammo[0]);
    // The aim direction is the yaw of the last tick: `fire:300` does not turn.
    CHECK_NEAR(player.yaw(), fixture.yaw, 0.0);
    CHECK(player.last_weapon().action.slot == 0);
    // A weapon tick does not move the player (no recoil in this slice).
    CHECK(finite_state(player.state()));
}

// --- level1 smoke ----------------------------------------------------------

void test_level1() {
    Fixture fixture;
    std::string error;
    if (!load_fixture("level1", fixture, error)) {
        std::fprintf(stderr, "sim_player: level1 fixture: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    const auto plan = gm::parse_drive("forward=1:200;turn=1:100", &error);
    CHECK(plan.has_value());
    if (!plan) {
        return;
    }
    gm::SimPlayer player;
    CHECK(player.init(fixture.feet, fixture.yaw, fixture.chassis, &error));
    CHECK(drive(player, *fixture.runtime, (*plan)[0], error));
    CHECK(drive(player, *fixture.runtime, (*plan)[1], error));
    const gm::PlayerState& state = player.state();
    std::printf("sim_player: level1 spawn (%.17g, %.17g, %.17g) -> (%.17g, %.17g, %.17g) "
                "travelled=%.3f yaw %.6f -> %.6f on_ground=%d\n",
                fixture.feet.x, fixture.feet.y, fixture.feet.z, state.pos.x, state.pos.y,
                state.pos.z, horizontal_distance(fixture.feet, state.pos), fixture.yaw,
                state.yaw, state.on_ground ? 1 : 0);
    CHECK(finite_state(state));
    CHECK(player.tick_index() == 300);
    // turn=1 rad/s for 100 ticks (100/91 s) is +1.0989 rad.
    CHECK_NEAR(state.yaw, fixture.yaw + 100.0 / 91.0, 1e-12);
    CHECK_NEAR(gm::eye_position(state).z, state.pos.z + fixture.chassis.eye_height, 1e-12);
}

} // namespace

int main() {
    test_parse_drive();

    const bool have_dm1 = fs::exists(kGame / "dm1.sbt") && fs::exists(kGame / "dm1.sbw");
    const bool have_level1 = fs::exists(kGame / "level1.sbt") && fs::exists(kGame / "level1.sbw");
    if (have_dm1) {
        // One runtime for the whole dm1 group: every test re-inits its own
        // SimPlayer, and the only runtime state they share is the eye list
        // (overwritten on every tick) -- except the fire test, which runs last
        // and installs the weapon tables.
        Fixture dm1;
        std::string error;
        if (!load_fixture("dm1", dm1, error)) {
            std::fprintf(stderr, "sim_player: dm1 fixture: %s\n", error.c_str());
            ++g_failures;
        } else {
            test_negatives(dm1);
            test_dm1_forward(dm1);
            test_dm1_determinism(dm1);
            test_dm1_wall(dm1);
            test_dm1_ground(dm1);
            test_dm1_fire(dm1);
        }
    } else {
        std::printf("sim_player: dm1 not installed, real-level tests skipped\n");
    }
    if (have_level1) {
        test_level1();
    } else {
        std::printf("sim_player: level1 not installed, smoke test skipped\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "sim_player: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sim_player: ok\n");
    return 0;
}
