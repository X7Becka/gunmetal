// Player -> projectile -> hit -> blast damage (`M6.5.1`, slice 2;
// docs/SIM_PROJECTILE.md).
//
// Slice 1 (`sim_player`) proved motion and the weapon seam, but `fire` with a
// projectile weapon stayed at "the trigger was accepted": without a template
// `PlayerWeaponTick::spawned` is -1 and nothing flies.  This test closes that
// hole on the existing APIs only:
//
//   * a real `dm1` runtime + the real `level0` weapon table (ROCKET, slot 7,
//     fire mode 1, impact/expire blasts 10000@80 + 12000@256);
//   * a THING-style template (`movement_type = 6`, `id = -1`, unique create
//     event), installed with `SimPlayer::set_projectile()`;
//   * the weakest damageable thing of the level is selected from `Things`
//     itself (its own `position`), and the firing line is found with the real
//     `GameRuntime::update_player_weapon()` direction -- the weapon spread
//     (`+0x14/+0x18`) moves the round, so a plain yaw ray would be a guess.
//     The search only calls the public seam on a fresh arsenal (restored
//     before the real run), so the first shot of the test has exactly the
//     probed direction;
//   * the player fires for 20 ticks: the round is visible in
//     `projectiles().list()` in flight, then expires on the wall and blasts;
//     the target loses every shield and all HP and becomes `alive == false`;
//   * a second run on a fresh runtime gives the same numbers (determinism);
//   * negatives: `fire` without a template leaves `spawned == -1` and
//     `projectiles_spawned() == 0`; a template that is not movement type 6 is
//     accepted by the runtime (documented limitation, no invented validation);
//     `clear_projectile()` returns the player to the slice-1 tick.
//
// Load path is the one of `main.cpp::simulate()` / `test_sim_player.cpp`
// (`load_level` -> `vm_state_from_level` -> `GameRuntime::build`,
// `chassis_from_level` for the LIGHT capsule).
#include "game/sim_player.h"

#include "game/chassis_loadout.h"
#include "game/projectile.h"
#include "game/runtime.h"
#include "game/weapons.h"
#include "res/level.h"
#include "res/sbt.h"
#include "res/sbw.h"
#include "res/sbtvm.h"
#include "res/vm_exec.h"
#include "world/things.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";
constexpr double kPi = 3.14159265358979323846;
/// Create event of the test template: only proves the spawn posts *something*
/// unique (`Things`/the runtime keep no consumer for it in this slice).
constexpr std::uint32_t kTestCreateEvent = 64201;
constexpr int kFireTicks = 20;
/// ROCKET's widest air blast is 12000@256 (`docs/WEAPONS_CPP.md` §3), so a
/// contact within this distance of a thing is guaranteed to damage it.
constexpr double kBlastReach = 200.0;

/// One built `dm1` runtime, the LIGHT chassis, and the real level0 weapon
/// table with a synthetic ROCKET loadout (the table-only load has an empty
/// mask and zero ammo, like `test_sim_player.cpp`).
struct Fixture {
    std::unique_ptr<gm::GameRuntime> runtime;
    gm::ChassisSpec chassis;
    std::optional<gm::Weapons> weapons;
    gm::Arsenal base_arsenal;
    const gm::Weapon* rocket = nullptr;
};

/// Build with the default `GameRuntimeConfig`: no AI aim/motion bridge and no
/// player-weapon phase, so a tick only runs the phases the projectile slice
/// needs (things, AI state, projectiles, world, events).  `SimPlayer` owns the
/// runtime tick itself while a projectile template is installed.
bool load_fixture(Fixture& out, std::string& error) {
    const auto level = gm::load_level(kGame, "dm1", error);
    if (!level) {
        return false;
    }
    if (!level->entities || !level->world) {
        error = "dm1: no .sbt/.sbw pair";
        return false;
    }
    const gm::VmState vm = gm::vm_state_from_level(*level->entities);
    out.runtime = std::make_unique<gm::GameRuntime>();
    if (!out.runtime->build(*level->entities, vm, *level->world, &error)) {
        return false;
    }
    const auto chassis = gm::chassis_from_level(kGame / "dm1.sbt", &error);
    if (!chassis) {
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
        error = "dm1: no LIGHT chassis record";
        return false;
    }
    out.chassis = *light;

    auto weapons = gm::Weapons::load(kGame / "level0.sbt", &error);
    if (!weapons) {
        return false;
    }
    const gm::Weapon* rocket = nullptr;
    for (const gm::Weapon& weapon : weapons->table()) {
        if (weapon.name == "ROCKET") {
            rocket = &weapon;
            break;
        }
    }
    if (rocket == nullptr) {
        error = "level0: no ROCKET record";
        return false;
    }
    out.base_arsenal = weapons->initial();
    out.base_arsenal.mask = 1u << 7;
    out.base_arsenal.current = 7;
    out.base_arsenal.ammo[7] = 20;
    out.weapons = std::move(*weapons);
    out.rocket = rocket;
    return true;
}

/// Fresh loadout: cooldown 0, ammo 20, `shot_index` 0, so the next shot has the
/// same deterministic spread (`fire()` hashes `seed ^ slot ^ shot_index`).
void reset_loadout(Fixture& fixture) {
    fixture.runtime->set_player_weapons(*fixture.weapons, fixture.base_arsenal);
}

/// The THING-style projectile of `test_player_trigger.cpp`: id -1 (runtime id
/// pool 0x10000+ per round), movement type 6, one geometry hit (so the first
/// contact expires the round and its expire blasts run).
gm::Thing make_projectile_template() {
    gm::Thing projectile;
    projectile.id = -1;
    projectile.type = 31; // TT31, the ROCKET round of the shipped data
    projectile.movement_type = 6;
    projectile.gravity = 0.0; // no drop at the tested range
    projectile.damping = 0.8;
    projectile.collision_shape = 1;
    projectile.radius = 10.0;
    projectile.hits_left = 1;
    projectile.expire_ticks = 728;
    projectile.create_event = kTestCreateEvent;
    return projectile;
}

/// One "stand at `feet`, aim at `yaw`, the real spread direction reaches a
/// surface within blast range of the thing" setup.
struct ShotScenario {
    std::size_t thing_index = 0;
    int thing_id = -1;
    std::string type_name;
    gm::ThingVitals before{};
    gm::Vec3d thing_position{};
    gm::Vec3d feet{};
    double yaw = 0.0;
    gm::Vec3d shot_direction{};
    double spread_x = 0.0;
    double spread_y = 0.0;
    double hit_distance = 0.0;   ///< ray distance from the eye to the surface
    double blast_distance = 0.0; ///< surface point distance from the thing
};

/// Ask the real weapon seam where the first shot of this aim would go, then
/// check that the first surface it reaches is within `kBlastReach` of the
/// thing.  No projectile is spawned (`spawned == -1`): only `fire()` runs.
bool probe_line(Fixture& fixture, const gm::Vec3d& eye, double yaw,
                const gm::Vec3d& thing_position, gm::Vec3d& direction,
                double& spread_x, double& spread_y, double& hit_distance) {
    reset_loadout(fixture);
    fixture.runtime->clear_player_projectile(); // the probe must not spawn a round
    gm::PlayerWeaponInput input;
    input.trigger_held = true;
    input.trigger_edge = true; // first tick of a fire series
    input.slot = -1;
    input.aim_direction = {std::cos(yaw), std::sin(yaw), 0.0};
    const gm::PlayerWeaponTick tick = fixture.runtime->update_player_weapon(input, eye);
    if (!tick.action.fired() || !tick.action.shot.projectile) {
        return false;
    }
    direction = tick.action.shot.direction;
    spread_x = tick.action.shot.spread_x;
    spread_y = tick.action.shot.spread_y;
    const auto hit = fixture.runtime->collision().raycast(eye, direction, 500.0);
    if (!hit) {
        return false;
    }
    hit_distance = hit->t;
    const gm::Vec3d delta{hit->point.x - thing_position.x,
                          hit->point.y - thing_position.y,
                          hit->point.z - thing_position.z};
    const double distance = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                      delta.z * delta.z);
    return distance <= kBlastReach;
}

/// Pick the weakest damageable thing that has a horizontal firing line and
/// return the player point 30..90 units in front of it.  Everything comes from
/// the level: the thing `position` from `Things`, the line from `raycast` with
/// the real `fire()` direction.
bool find_shot_scenario(Fixture& fixture, ShotScenario& out) {
    struct Candidate {
        std::size_t index;
        int max_hp;
    };
    std::vector<Candidate> candidates;
    const std::vector<gm::Thing>& things = fixture.runtime->things().things();
    for (std::size_t index = 0; index < things.size(); ++index) {
        const gm::Thing& thing = things[index];
        if (!thing.position.has_value() || !thing.alive) {
            continue;
        }
        const gm::ThingVitals vitals = gm::thing_vitals(thing);
        if (vitals.max_hp <= 0) {
            continue; // damage-immune thing (`apply_thing_damage` no-op)
        }
        candidates.push_back({index, vitals.max_hp});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& l, const Candidate& r) {
                  return l.max_hp != r.max_hp ? l.max_hp < r.max_hp : l.index < r.index;
              });

    for (const Candidate& candidate : candidates) {
        const gm::Thing& thing = things[candidate.index];
        const gm::Vec3d thing_position{static_cast<double>((*thing.position)[0]),
                                       static_cast<double>((*thing.position)[1]),
                                       static_cast<double>((*thing.position)[2])};
        for (int k = 0; k < 72; ++k) {
            // Yaw convention of `docs/CAMERA.md` §3: (cos, sin) in XY.
            const double yaw = static_cast<double>(k) * 2.0 * kPi / 72.0;
            const gm::Vec3d aim{std::cos(yaw), std::sin(yaw), 0.0};
            for (const double stand_off : {30.0, 40.0, 60.0, 90.0}) {
                const gm::Vec3d feet{thing_position.x - aim.x * stand_off,
                                     thing_position.y - aim.y * stand_off,
                                     thing_position.z};
                const gm::Vec3d eye{feet.x, feet.y, feet.z + fixture.chassis.eye_height};
                gm::Vec3d direction{};
                double spread_x = 0.0;
                double spread_y = 0.0;
                double hit_distance = 0.0;
                if (!probe_line(fixture, eye, yaw, thing_position, direction,
                                spread_x, spread_y, hit_distance)) {
                    continue;
                }
                out.thing_index = candidate.index;
                out.thing_id = thing.id;
                out.type_name = thing.type_name.empty() ? "?" : thing.type_name;
                out.before = gm::thing_vitals(thing);
                out.thing_position = thing_position;
                out.feet = feet;
                out.yaw = yaw;
                out.shot_direction = direction;
                out.spread_x = spread_x;
                out.spread_y = spread_y;
                out.hit_distance = hit_distance;
                const gm::Vec3d hit_point = eye + direction * hit_distance;
                const gm::Vec3d delta{hit_point.x - thing_position.x,
                                      hit_point.y - thing_position.y,
                                      hit_point.z - thing_position.z};
                out.blast_distance = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                               delta.z * delta.z);
                return true;
            }
        }
    }
    return false;
}

struct ShotResult {
    int shots_fired = 0;
    int spawned = 0;
    int blast_hits = 0;
    int things_damaged = 0;
    bool saw_round_in_flight = false;
    bool saw_expiry = false;
    bool round_spent = false;
    int round_thing_id = -1;
    int live_rounds_max = 0;
    gm::ThingVitals after{};
    bool target_alive = false;
};

/// Fire `kFireTicks` ticks at `scenario` and record what actually happened.
bool run_scenario(Fixture& fixture, const ShotScenario& scenario, ShotResult& out,
                  std::string& error) {
    gm::GameRuntime& runtime = *fixture.runtime;
    reset_loadout(fixture); // first shot: same spread the search probed
    gm::SimPlayer player;
    if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        return false;
    }
    player.set_projectile(make_projectile_template(), 0);
    CHECK(player.has_projectile());

    gm::DriveSegment segment;
    segment.ticks = kFireTicks;
    segment.fire = true;
    for (int tick = 0; tick < segment.ticks; ++tick) {
        if (!player.tick(runtime, segment, tick, &error)) {
            return false;
        }
        const std::vector<gm::ProjectileState>& rounds = runtime.projectiles().list();
        out.live_rounds_max =
            std::max(out.live_rounds_max, static_cast<int>(rounds.size()));
        for (const gm::ProjectileState& round : rounds) {
            if (!round.spent && !round.dead) {
                out.saw_round_in_flight = true;
            }
            if (round.spent) {
                out.saw_expiry = true;
            }
        }
        if (tick == 0 && player.last_weapon().spawned >= 0) {
            const auto index = static_cast<std::size_t>(player.last_weapon().spawned);
            if (index < rounds.size()) {
                out.round_thing_id = rounds[index].spawn.thing_id;
            }
        }
        // The expired record is not removed (`Projectiles` keeps the record);
        // `spent` is what "the round is gone" means here.
        const gm::ProjectileState* state =
            runtime.projectiles().by_thing_id(out.round_thing_id);
        out.round_spent = state != nullptr && state->spent;
    }

    out.shots_fired = player.shots_fired();
    out.spawned = player.projectiles_spawned();
    out.blast_hits = player.blast_hits();
    out.things_damaged = player.things_damaged();
    out.after = gm::thing_vitals(runtime.things().things()[scenario.thing_index]);
    out.target_alive = runtime.things().things()[scenario.thing_index].alive;

    std::printf("sim_projectile: rocket slot=%d mode=%d ticks=%d blasts=%zu+%zu; "
                "target [%zu] id=%d type=%s pos=(%.2f, %.2f, %.2f) "
                "hp=%d/%d alive=%d\n",
                fixture.rocket->slot, fixture.rocket->fire_mode,
                fixture.rocket->fire_ticks, fixture.rocket->impact_blasts.size(),
                fixture.rocket->expire_blasts.size(), scenario.thing_index,
                scenario.thing_id, scenario.type_name.c_str(),
                scenario.thing_position.x, scenario.thing_position.y,
                scenario.thing_position.z, scenario.before.hp,
                scenario.before.max_hp, scenario.before.alive ? 1 : 0);
    std::printf("sim_projectile: feet=(%.2f, %.2f, %.2f) yaw=%.6f rad (%.1f deg) "
                "shot_dir=(%.4f, %.4f, %.4f) spread=(%.4f, %.4f) hit=%.2f "
                "blast_from_target=%.2f; fire=%d ticks\n",
                scenario.feet.x, scenario.feet.y, scenario.feet.z, scenario.yaw,
                scenario.yaw * 180.0 / kPi, scenario.shot_direction.x,
                scenario.shot_direction.y, scenario.shot_direction.z,
                scenario.spread_x, scenario.spread_y, scenario.hit_distance,
                scenario.blast_distance, kFireTicks);
    std::printf("sim_projectile: shots=%d projectiles_spawned=%d in_flight=%d "
                "live_rounds_max=%d expiry=%d round_spent=%d thing_id=%d "
                "blast_hits=%d things_damaged=%d\n",
                out.shots_fired, out.spawned, out.saw_round_in_flight ? 1 : 0,
                out.live_rounds_max, out.saw_expiry ? 1 : 0, out.round_spent ? 1 : 0,
                out.round_thing_id, out.blast_hits, out.things_damaged);
    std::printf("sim_projectile: target after hp=%d/%d shields=%d/%d/%d/%d alive=%d\n",
                out.after.hp, out.after.max_hp, out.after.shields[0],
                out.after.shields[1], out.after.shields[2], out.after.shields[3],
                out.after.alive ? 1 : 0);
    return true;
}

/// The same plan on a fresh runtime: the module is deterministic and the
/// numbers do not depend on the first run.
void test_dm1_replay(const ShotScenario& scenario, const ShotResult& first) {
    Fixture replay;
    std::string error;
    if (!load_fixture(replay, error)) {
        std::fprintf(stderr, "sim_projectile: replay fixture: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    ShotResult second;
    if (!run_scenario(replay, scenario, second, error)) {
        std::fprintf(stderr, "sim_projectile: replay: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    std::printf("sim_projectile: replay spawned=%d blast_hits=%d things_damaged=%d "
                "hp=%d alive=%d (first spawned=%d blast_hits=%d things_damaged=%d "
                "hp=%d alive=%d)\n",
                second.spawned, second.blast_hits, second.things_damaged, second.after.hp,
                second.after.alive ? 1 : 0, first.spawned, first.blast_hits,
                first.things_damaged, first.after.hp, first.after.alive ? 1 : 0);
    CHECK_EQ(second.spawned, first.spawned);
    CHECK_EQ(second.blast_hits, first.blast_hits);
    CHECK_EQ(second.things_damaged, first.things_damaged);
    CHECK_EQ(second.after.hp, first.after.hp);
    CHECK_EQ(second.after.alive, first.after.alive);
    CHECK_EQ(second.round_thing_id, first.round_thing_id);
}

// --- negatives -------------------------------------------------------------

/// `fire` without a template: the weapon fires, but the runtime has nothing to
/// spawn, so `spawned == -1` and no counter moves.  This is the slice-1
/// behaviour the old `sim_player` test pins; here it is checked next to the
/// positive case.
void test_no_template(Fixture& fixture, const ShotScenario& scenario) {
    std::string error;
    reset_loadout(fixture);
    // The positive case installed a template on this runtime; a player that
    // never called `set_projectile()` sees a runtime without one.
    fixture.runtime->clear_player_projectile();
    gm::SimPlayer player;
    if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        std::fprintf(stderr, "sim_projectile: no-template init: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    const std::size_t rounds_before = fixture.runtime->projectiles().list().size();
    gm::DriveSegment segment;
    segment.ticks = 5;
    segment.fire = true;
    for (int tick = 0; tick < segment.ticks; ++tick) {
        if (!player.tick(*fixture.runtime, segment, tick, &error)) {
            std::fprintf(stderr, "sim_projectile: no-template tick: %s\n", error.c_str());
            ++g_failures;
            return;
        }
    }
    std::printf("sim_projectile: no template -> shots=%d spawned=%d "
                "projectiles_spawned=%d blast_hits=%d things_damaged=%d rounds %zu->%zu\n",
                player.shots_fired(), player.last_weapon().spawned,
                player.projectiles_spawned(), player.blast_hits(),
                player.things_damaged(), rounds_before,
                fixture.runtime->projectiles().list().size());
    CHECK(player.shots_fired() > 0);
    CHECK_EQ(player.last_weapon().spawned, -1);
    CHECK_EQ(player.projectiles_spawned(), 0);
    CHECK_EQ(player.blast_hits(), 0);
    CHECK_EQ(player.things_damaged(), 0);
    CHECK_EQ(fixture.runtime->projectiles().list().size(), rounds_before);
}

/// The runtime has no movement-type guard: `projectile_spawn_from_shot()` takes
/// any `Thing` and only movement type 8 selects the homing step.  The test
/// records that fact instead of inventing a SimPlayer-side rejection.
void test_non_movement_type(Fixture& fixture, const ShotScenario& scenario) {
    std::string error;
    reset_loadout(fixture);
    gm::Thing odd = make_projectile_template();
    odd.movement_type = 0; // not a projectile type, still accepted
    gm::SimPlayer player;
    if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        ++g_failures;
        return;
    }
    player.set_projectile(odd, 0);
    gm::DriveSegment segment;
    segment.ticks = 2;
    segment.fire = true;
    int spawned_any = -1;
    for (int tick = 0; tick < segment.ticks; ++tick) {
        if (!player.tick(*fixture.runtime, segment, tick, &error)) {
            std::fprintf(stderr, "sim_projectile: mt=0 tick: %s\n", error.c_str());
            ++g_failures;
            return;
        }
        if (player.last_weapon().spawned >= 0) {
            spawned_any = player.last_weapon().spawned;
        }
    }
    std::printf("sim_projectile: movement_type=0 template -> spawned=%d (last tick "
                "%d) projectiles_spawned=%d rounds=%zu (accepted by the runtime: no "
                "movement_type guard)\n",
                spawned_any, player.last_weapon().spawned,
                player.projectiles_spawned(),
                fixture.runtime->projectiles().list().size());
    CHECK(player.has_projectile());
    CHECK(spawned_any >= 0);
    CHECK_EQ(player.projectiles_spawned(), 1);

    // `clear_projectile()` returns the player to the slice-1 tick: no round.
    player.clear_projectile();
    CHECK(!player.has_projectile());
    reset_loadout(fixture);
    const int spawned_before = player.projectiles_spawned();
    for (int tick = 0; tick < 2; ++tick) {
        if (!player.tick(*fixture.runtime, segment, tick, &error)) {
            ++g_failures;
            return;
        }
    }
    std::printf("sim_projectile: clear_projectile -> has=%d spawned=%d "
                "projectiles_spawned=%d\n",
                player.has_projectile() ? 1 : 0, player.last_weapon().spawned,
                player.projectiles_spawned());
    CHECK_EQ(player.last_weapon().spawned, -1);
    CHECK_EQ(player.projectiles_spawned(), spawned_before);
}

} // namespace

int main() {
    if (!fs::exists(kGame / "dm1.sbt") || !fs::exists(kGame / "dm1.sbw") ||
        !fs::exists(kGame / "level0.sbt")) {
        std::printf("sim_projectile: dm1/level0 not installed, skipped\n");
        return 0;
    }

    Fixture fixture;
    std::string error;
    if (!load_fixture(fixture, error)) {
        std::fprintf(stderr, "sim_projectile: dm1 fixture: %s\n", error.c_str());
        return 1;
    }
    ShotScenario scenario;
    if (!find_shot_scenario(fixture, scenario)) {
        std::fprintf(stderr, "sim_projectile: no damageable thing with a firing line\n");
        return 1;
    }

    ShotResult result;
    if (!run_scenario(fixture, scenario, result, error)) {
        std::fprintf(stderr, "sim_projectile: %s\n", error.c_str());
        return 1;
    }
    CHECK(result.shots_fired > 0);
    CHECK(result.spawned > 0);         // a real round, not spawned == -1
    CHECK(result.saw_round_in_flight); // visible in projectiles().list()
    CHECK(result.round_thing_id >= 0);
    CHECK(result.saw_expiry);          // it hit and expired
    CHECK(result.round_spent);
    CHECK(result.blast_hits > 0);
    CHECK(result.things_damaged > 0);
    // Damage reached the thing: the weakest shipped thing with a firing line is
    // an OILDRUM (1000 HP and 1000 per shield), so the ROCKET blasts zero both
    // and kill it (`alive == false`).
    CHECK(result.after.hp < scenario.before.hp || !result.after.alive);
    CHECK(!result.after.alive);
    CHECK(!result.target_alive);

    test_dm1_replay(scenario, result);
    test_no_template(fixture, scenario);
    test_non_movement_type(fixture, scenario);

    if (g_failures != 0) {
        std::fprintf(stderr, "sim_projectile: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sim_projectile: ok\n");
    return 0;
}
