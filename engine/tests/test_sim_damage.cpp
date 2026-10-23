// Player damage, death and respawn (`M6.5.1`, slice 3; docs/SIM_DAMAGE.md).
//
// Slices 1-2 proved motion, the weapon seam and the projectile blasts, but the
// player itself was untouchable: `player_vitals` was empty, so a point-blank
// rocket hurt only things.  This test closes the player side on the existing
// APIs:
//
//   * a real `dm1` runtime + the real `level0` weapon table (ROCKET, slot 7,
//     fire mode 1, 80-tick period, impact/expire blasts 10000@80 + 12000@256)
//     and the LIGHT starting gauges of `saved0` (armor 10000/10000, shields
//     8000 x4, docs/PLAYER_FIELDS.md §11);
//   * the firing line is the one `test_sim_projectile.cpp` already proves: a
//     damageable thing of the level, the player 30..90 units in front of it,
//     and the *real* `GameRuntime::update_player_weapon()` direction (the
//     ROCKET spread is not a plain yaw ray).  The round expires on the wall /
//     target and the weapon-keyed blasts catch the shooter inside their radii;
//   * firing on until `armor <= 0`: the gauge loss is the sum of the armor and
//     shield decreases, the death tick is the tick of the killing blast, and
//     every later `tick()` is a successful no-op;
//   * `respawn()` returns the feet/yaw of `init()`, restores the starting
//     gauges, clears `alive()` back to true and counts one death; the next
//     shots hurt again;
//   * `set_vitals()` with a small gauge set proves the custom starting gauges
//     reach the runtime and die in a single blast; a second run on a fresh
//     runtime reproduces every number (determinism);
//   * negatives: negative armor/shields (and armor above the maximum) are
//     refused with a text, `respawn()` before `init()` is refused.
//
// Load path is the one of `main.cpp::simulate()` / `test_sim_projectile.cpp`
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
constexpr std::uint32_t kTestCreateEvent = 64201;
/// Blast reach of the widest ROCKET air blast (12000@256): the first surface
/// of the firing line must be this close to the target, as in slice 2.
constexpr double kBlastReach = 200.0;
/// Hard stop for a fire series; ROCKET fires every 80 ticks, 20 rounds.
constexpr int kMaxFireTicks = 2000;

/// One built `dm1` runtime, the LIGHT chassis and the real level0 weapon table
/// with a synthetic ROCKET loadout (same fixture as `test_sim_projectile.cpp`).
struct Fixture {
    std::unique_ptr<gm::GameRuntime> runtime;
    gm::ChassisSpec chassis;
    std::optional<gm::Weapons> weapons;
    gm::Arsenal base_arsenal;
    const gm::Weapon* rocket = nullptr;
};

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

void reset_loadout(Fixture& fixture) {
    fixture.runtime->set_player_weapons(*fixture.weapons, fixture.base_arsenal);
}

gm::Thing make_projectile_template() {
    gm::Thing projectile;
    projectile.id = -1;
    projectile.type = 31; // TT31, the ROCKET round of the shipped data
    projectile.movement_type = 6;
    projectile.gravity = 0.0;
    projectile.damping = 0.8;
    projectile.collision_shape = 1;
    projectile.radius = 10.0;
    projectile.hits_left = 1;
    projectile.expire_ticks = 728;
    projectile.create_event = kTestCreateEvent;
    return projectile;
}

/// One "stand at `feet`, aim at `yaw`, the real spread direction reaches a
/// surface within blast range of the thing" setup (the finder of
/// `test_sim_projectile.cpp`, kept here so the two slices stay independent).
struct ShotScenario {
    std::size_t thing_index = 0;
    int thing_id = -1;
    std::string type_name;
    gm::Vec3d thing_position{};
    gm::Vec3d feet{};
    double yaw = 0.0;
    double stand_off = 0.0;
    double hit_distance = 0.0; ///< eye -> first surface (the blast centre)
};

/// Ask the real weapon seam where the first shot of this aim would go, then
/// check that the first surface is within `kBlastReach` of the thing.
bool probe_line(Fixture& fixture, const gm::Vec3d& eye, double yaw,
                const gm::Vec3d& thing_position, double& hit_distance) {
    reset_loadout(fixture);
    fixture.runtime->clear_player_projectile(); // the probe must not spawn a round
    gm::PlayerWeaponInput input;
    input.trigger_held = true;
    input.trigger_edge = true;
    input.slot = -1;
    input.aim_direction = {std::cos(yaw), std::sin(yaw), 0.0};
    const gm::PlayerWeaponTick tick = fixture.runtime->update_player_weapon(input, eye);
    if (!tick.action.fired() || !tick.action.shot.projectile) {
        return false;
    }
    const auto hit = fixture.runtime->collision().raycast(
        eye, tick.action.shot.direction, 500.0);
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

/// Pick the weakest damageable thing with a firing line and stand 30..90 units
/// in front of it.  Everything comes from the level (thing `position`,
/// `CollisionWorld::raycast`, the real `fire()` direction).
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
            const double yaw = static_cast<double>(k) * 2.0 * kPi / 72.0;
            const gm::Vec3d aim{std::cos(yaw), std::sin(yaw), 0.0};
            for (const double stand_off : {30.0, 40.0, 60.0, 90.0}) {
                const gm::Vec3d feet{thing_position.x - aim.x * stand_off,
                                     thing_position.y - aim.y * stand_off,
                                     thing_position.z};
                const gm::Vec3d eye{feet.x, feet.y, feet.z + fixture.chassis.eye_height};
                double hit_distance = 0.0;
                if (!probe_line(fixture, eye, yaw, thing_position, hit_distance)) {
                    continue;
                }
                out.thing_index = candidate.index;
                out.thing_id = thing.id;
                out.type_name = thing.type_name.empty() ? "?" : thing.type_name;
                out.thing_position = thing_position;
                out.feet = feet;
                out.yaw = yaw;
                out.stand_off = stand_off;
                out.hit_distance = hit_distance;
                return true;
            }
        }
    }
    return false;
}

/// Outcome of one fire-until-death run (or until `max_ticks`).
struct DeathRun {
    int ticks = 0;
    int shots = 0;
    int spawned = 0;
    int blast_hits = 0;
    int damage_taken = 0;
    int death_tick = -1;
    int deaths = 0;
    bool alive = true;
    int armor = 0;
    int shields[4] = {0, 0, 0, 0};
    gm::PlayerState dead_state{}; ///< state after the killing tick
};

bool fire_until_dead(Fixture& fixture, const ShotScenario& scenario, int max_ticks,
                     DeathRun& out, std::string& error) {
    reset_loadout(fixture);
    gm::SimPlayer player;
    if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        return false;
    }
    player.set_projectile(make_projectile_template(), 0);

    gm::DriveSegment segment;
    segment.ticks = max_ticks;
    segment.fire = true;
    for (int tick = 0; tick < segment.ticks; ++tick) {
        if (!player.tick(*fixture.runtime, segment, tick, &error)) {
            return false;
        }
        ++out.ticks;
        if (!player.alive()) {
            out.death_tick = player.death_tick();
            out.dead_state = player.state();
            break;
        }
    }

    out.shots = player.shots_fired();
    out.spawned = player.projectiles_spawned();
    out.blast_hits = player.blast_hits();
    out.damage_taken = player.damage_taken();
    out.deaths = player.deaths();
    out.alive = player.alive();
    out.armor = player.vitals().armor;
    for (std::size_t i = 0; i < 4; ++i) {
        out.shields[i] = player.vitals().shields[i];
    }
    return true;
}

void print_gauges(const char* tag, const gm::Vitals& vitals) {
    std::printf("%s armor=%d/%d shields=%d/%d/%d/%d shield_max=%d\n", tag, vitals.armor,
                vitals.armor_max, vitals.shields[0], vitals.shields[1], vitals.shields[2],
                vitals.shields[3], vitals.shield_max);
}

/// The main scenario: default LIGHT gauges, fire point-blank until the armor
/// reaches zero, then the no-op tick, `respawn()` and damage after respawn.
void test_default_vitals_death(Fixture& fixture, const ShotScenario& scenario) {
    std::string error;
    gm::SimPlayer probe;
    if (!probe.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        std::fprintf(stderr, "sim_damage: init: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    std::printf("sim_damage: start gauges (LIGHT, saved0): ");
    print_gauges("", probe.vitals());
    CHECK_EQ(probe.vitals().armor, 10000);
    CHECK_EQ(probe.vitals().armor_max, 10000);
    CHECK_EQ(probe.vitals().shield_max, 8000);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK_EQ(probe.vitals().shields[i], 8000);
    }
    CHECK(probe.alive());
    CHECK_EQ(probe.deaths(), 0);
    CHECK_EQ(probe.damage_taken(), 0);

    std::printf("sim_damage: target [%zu] id=%d type=%s pos=(%.2f, %.2f, %.2f) "
                "feet=(%.2f, %.2f, %.2f) yaw=%.6f rad (%.1f deg) stand_off=%.0f "
                "blast_from_eye=%.2f\n",
                scenario.thing_index, scenario.thing_id, scenario.type_name.c_str(),
                scenario.thing_position.x, scenario.thing_position.y,
                scenario.thing_position.z, scenario.feet.x, scenario.feet.y,
                scenario.feet.z, scenario.yaw, scenario.yaw * 180.0 / kPi,
                scenario.stand_off, scenario.hit_distance);

    // One shot first, on its own fresh runtime: self-damage must exist while
    // the player is still alive.
    {
        Fixture one_shot;
        if (!load_fixture(one_shot, error)) {
            std::fprintf(stderr, "sim_damage: first-shot fixture: %s\n", error.c_str());
            ++g_failures;
            return;
        }
        reset_loadout(one_shot);
        gm::SimPlayer player;
        if (!player.init(scenario.feet, scenario.yaw, one_shot.chassis, &error)) {
            std::fprintf(stderr, "sim_damage: first shot init: %s\n", error.c_str());
            ++g_failures;
            return;
        }
        player.set_projectile(make_projectile_template(), 0);
        gm::DriveSegment segment;
        segment.ticks = 30; // one ROCKET (period 80), enough ticks to hit
        segment.fire = true;
        for (int tick = 0; tick < segment.ticks; ++tick) {
            if (!player.tick(*one_shot.runtime, segment, tick, &error)) {
                std::fprintf(stderr, "sim_damage: first shot tick: %s\n", error.c_str());
                ++g_failures;
                return;
            }
        }
        std::printf("sim_damage: first shot after %d ticks: shots=%d spawned=%d "
                    "blast_hits=%d damage_taken=%d alive=%d ",
                    player.tick_index(), player.shots_fired(),
                    player.projectiles_spawned(), player.blast_hits(),
                    player.damage_taken(), player.alive() ? 1 : 0);
        print_gauges("gauges:", player.vitals());
        CHECK_EQ(player.shots_fired(), 1);
        CHECK(player.blast_hits() > 0);
        CHECK(player.damage_taken() > 0);
        CHECK(player.alive());
    }

    DeathRun run;
    if (!fire_until_dead(fixture, scenario, kMaxFireTicks, run, error)) {
        std::fprintf(stderr, "sim_damage: death run: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    std::printf("sim_damage: fired %d ticks: shots=%d spawned=%d blast_hits=%d "
                "damage_taken=%d alive=%d deaths=%d death_tick=%d armor=%d "
                "shields=%d/%d/%d/%d\n",
                run.ticks, run.shots, run.spawned, run.blast_hits, run.damage_taken,
                run.alive ? 1 : 0, run.deaths, run.death_tick, run.armor, run.shields[0],
                run.shields[1], run.shields[2], run.shields[3]);
    CHECK(run.shots > 1);              // a series of point-blank rockets
    CHECK(run.damage_taken > 0);
    CHECK(!run.alive);                 // died inside the series
    CHECK_EQ(run.armor, 0);            // 0x4536e0 writes armor = 0
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK_EQ(run.shields[i], 0);   // every plate was overrun before the hull
    }
    CHECK(run.death_tick > 0);
    CHECK_EQ(run.death_tick, run.ticks); // the killing tick is death_tick
    CHECK(run.ticks <= kMaxFireTicks);

    // Post-death ticks are a successful no-op on a fresh player driven to death.
    {
        reset_loadout(fixture);
        gm::SimPlayer player;
        if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
            ++g_failures;
            return;
        }
        player.set_projectile(make_projectile_template(), 0);
        gm::DriveSegment segment;
        segment.ticks = kMaxFireTicks;
        segment.fire = true;
        int tick = 0;
        while (player.alive() && tick < segment.ticks) {
            if (!player.tick(*fixture.runtime, segment, tick, &error)) {
                std::fprintf(stderr, "sim_damage: death replay: %s\n", error.c_str());
                ++g_failures;
                return;
            }
            ++tick;
        }
        CHECK(!player.alive());
        const gm::PlayerState before = player.state();
        const gm::Vitals gauges = player.vitals();
        const int ticks_before = player.tick_index();
        const int damage_before = player.damage_taken();
        // A dead player: no motion, no shot, no runtime tick, no gauge change.
        for (int extra = 0; extra < 5; ++extra) {
            if (!player.tick(*fixture.runtime, segment, extra, &error)) {
                std::fprintf(stderr, "sim_damage: dead no-op tick: %s\n", error.c_str());
                ++g_failures;
                return;
            }
        }
        std::printf("sim_damage: after death 5 ticks: pos=(%.6f, %.6f, %.6f) -> "
                    "(%.6f, %.6f, %.6f) tick %d -> %d damage %d -> %d alive=%d\n",
                    before.pos.x, before.pos.y, before.pos.z, player.state().pos.x,
                    player.state().pos.y, player.state().pos.z, ticks_before,
                    player.tick_index(), damage_before, player.damage_taken(),
                    player.alive() ? 1 : 0);
        CHECK_NEAR(player.state().pos.x, before.pos.x, 0.0);
        CHECK_NEAR(player.state().pos.y, before.pos.y, 0.0);
        CHECK_NEAR(player.state().pos.z, before.pos.z, 0.0);
        CHECK_EQ(player.tick_index(), ticks_before);
        CHECK_EQ(player.damage_taken(), damage_before);
        CHECK_EQ(player.vitals().armor, gauges.armor);
        for (std::size_t i = 0; i < 4; ++i) {
            CHECK_EQ(player.vitals().shields[i], gauges.shields[i]);
        }
        CHECK_EQ(player.death_tick(), ticks_before);
        CHECK(!player.alive());

        // respawn(): starting pose, starting gauges, one death, alive again.
        if (!player.respawn(&error)) {
            std::fprintf(stderr, "sim_damage: respawn: %s\n", error.c_str());
            ++g_failures;
            return;
        }
        std::printf("sim_damage: respawn -> pos=(%.6f, %.6f, %.6f) yaw=%.17g "
                    "alive=%d deaths=%d damage_taken=%d death_tick=%d ",
                    player.state().pos.x, player.state().pos.y, player.state().pos.z,
                    player.state().yaw, player.alive() ? 1 : 0, player.deaths(),
                    player.damage_taken(), player.death_tick());
        print_gauges("gauges:", player.vitals());
        CHECK(player.alive());
        CHECK_EQ(player.deaths(), 1);
        CHECK_NEAR(player.state().pos.x, scenario.feet.x, 0.0);
        CHECK_NEAR(player.state().pos.y, scenario.feet.y, 0.0);
        CHECK_NEAR(player.state().pos.z, scenario.feet.z, 0.0);
        CHECK_NEAR(player.state().yaw, scenario.yaw, 0.0);
        CHECK_EQ(player.vitals().armor, 10000);
        CHECK_EQ(player.vitals().shields[0], 8000);
        CHECK_EQ(player.vitals().shields[1], 8000);
        CHECK_EQ(player.vitals().shields[2], 8000);
        CHECK_EQ(player.vitals().shields[3], 8000);
        CHECK_EQ(player.death_tick(), ticks_before); // kept as the last death

        // Damage after respawn goes through the same path: the restored gauges
        // are re-installed on the runtime by the first tick of the new life.
        const int damage_after_respawn = player.damage_taken();
        const int shots_after_respawn = player.shots_fired();
        // 250 ticks: the runtime weapon cooldown (ROCKET 80) has to expire and
        // a new round has to fly, so at least two fresh shots land.
        for (int t = 0; t < 250; ++t) {
            if (!player.tick(*fixture.runtime, segment, t, &error)) {
                std::fprintf(stderr, "sim_damage: post-respawn tick: %s\n", error.c_str());
                ++g_failures;
                return;
            }
        }
        std::printf("sim_damage: 250 ticks after respawn: damage %d -> %d shots %d -> %d "
                    "alive=%d armor=%d shields=%d/%d/%d/%d\n",
                    damage_after_respawn, player.damage_taken(), shots_after_respawn,
                    player.shots_fired(), player.alive() ? 1 : 0, player.vitals().armor,
                    player.vitals().shields[0], player.vitals().shields[1],
                    player.vitals().shields[2], player.vitals().shields[3]);
        CHECK(player.damage_taken() > damage_after_respawn);
        CHECK(player.shots_fired() > shots_after_respawn);
    }

    // Determinism: the same scenario on a fresh runtime gives the same numbers.
    Fixture replay;
    if (!load_fixture(replay, error)) {
        std::fprintf(stderr, "sim_damage: replay fixture: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    DeathRun second;
    if (!fire_until_dead(replay, scenario, kMaxFireTicks, second, error)) {
        std::fprintf(stderr, "sim_damage: replay: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    std::printf("sim_damage: replay ticks=%d shots=%d blast_hits=%d damage=%d "
                "death_tick=%d armor=%d (first ticks=%d shots=%d blast_hits=%d "
                "damage=%d death_tick=%d)\n",
                second.ticks, second.shots, second.blast_hits, second.damage_taken,
                second.death_tick, second.armor, run.ticks, run.shots, run.blast_hits,
                run.damage_taken, run.death_tick);
    CHECK_EQ(second.ticks, run.ticks);
    CHECK_EQ(second.shots, run.shots);
    CHECK_EQ(second.blast_hits, run.blast_hits);
    CHECK_EQ(second.damage_taken, run.damage_taken);
    CHECK_EQ(second.death_tick, run.death_tick);
    CHECK_EQ(second.armor, run.armor);
}

/// `set_vitals()` with a small gauge set: the custom gauges reach the runtime
/// (one point-blank blast kills) and the death tick is the actual tick.
void test_custom_vitals(Fixture& fixture, const ShotScenario& scenario) {
    std::string error;
    reset_loadout(fixture);
    gm::SimPlayer player;
    if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        ++g_failures;
        return;
    }
    gm::Vitals small;
    small.armor = 500;
    small.armor_max = 1000;
    small.shield_max = 0;
    small.shields = {0, 0, 0, 0};
    if (!player.set_vitals(small, &error)) {
        std::fprintf(stderr, "sim_damage: set_vitals(small): %s\n", error.c_str());
        ++g_failures;
        return;
    }
    player.set_projectile(make_projectile_template(), 0);

    gm::DriveSegment segment;
    segment.ticks = 40;
    segment.fire = true;
    int tick = 0;
    while (player.alive() && tick < segment.ticks) {
        if (!player.tick(*fixture.runtime, segment, tick, &error)) {
            std::fprintf(stderr, "sim_damage: custom vitals tick: %s\n", error.c_str());
            ++g_failures;
            return;
        }
        ++tick;
    }
    std::printf("sim_damage: set_vitals armor=500/1000 shields=0 -> ticks=%d "
                "death_tick=%d damage_taken=%d armor=%d alive=%d shots=%d\n",
                tick, player.death_tick(), player.damage_taken(), player.vitals().armor,
                player.alive() ? 1 : 0, player.shots_fired());
    CHECK(player.shots_fired() > 0);
    CHECK(!player.alive());
    CHECK_EQ(player.vitals().armor, 0);
    CHECK_EQ(player.death_tick(), tick);
    CHECK(player.damage_taken() >= 500);
}

/// Negatives: invalid gauges and `respawn()` before `init()` are refused with a
/// text and leave the player untouched.
void test_negatives(const gm::ChassisSpec& chassis) {
    gm::SimPlayer fresh;
    std::string error;
    const bool respawned = fresh.respawn(&error);
    std::printf("sim_damage: respawn before init -> ok=%d error=\"%s\"\n",
                respawned ? 1 : 0, error.c_str());
    CHECK(!respawned);
    CHECK(!error.empty());

    std::string init_error;
    gm::SimPlayer player;
    const gm::Vec3d feet{0.0, 0.0, 0.0};
    if (!player.init(feet, 0.0, chassis, &init_error)) {
        std::printf("sim_damage: negatives: init failed (%s)\n", init_error.c_str());
        ++g_failures;
        return;
    }
    const gm::Vitals before = player.vitals();

    struct Negative {
        const char* name;
        gm::Vitals vitals;
    };
    gm::Vitals negative_armor = before;
    negative_armor.armor = -1;
    gm::Vitals negative_shield = before;
    negative_shield.shields[2] = -5;
    gm::Vitals armor_over_max = before;
    armor_over_max.armor = before.armor_max + 1;
    gm::Vitals negative_shield_max = before;
    negative_shield_max.shield_max = -1;
    const Negative cases[] = {
        {"armor=-1", negative_armor},
        {"shields[2]=-5", negative_shield},
        {"armor=armor_max+1", armor_over_max},
        {"shield_max=-1", negative_shield_max},
    };
    for (const Negative& item : cases) {
        error.clear();
        const bool accepted = player.set_vitals(item.vitals, &error);
        std::printf("sim_damage: set_vitals %s -> ok=%d error=\"%s\"\n", item.name,
                    accepted ? 1 : 0, error.c_str());
        CHECK(!accepted);
        CHECK(!error.empty());
        CHECK_EQ(player.vitals().armor, before.armor);
        CHECK_EQ(player.vitals().shields[0], before.shields[0]);
        CHECK_EQ(player.vitals().armor_max, before.armor_max);
    }
}

} // namespace

int main() {
    if (!fs::exists(kGame / "dm1.sbt") || !fs::exists(kGame / "dm1.sbw") ||
        !fs::exists(kGame / "level0.sbt")) {
        std::printf("sim_damage: dm1/level0 not installed, skipped\n");
        return 0;
    }

    Fixture fixture;
    std::string error;
    if (!load_fixture(fixture, error)) {
        std::fprintf(stderr, "sim_damage: dm1 fixture: %s\n", error.c_str());
        return 1;
    }
    ShotScenario scenario;
    if (!find_shot_scenario(fixture, scenario)) {
        std::fprintf(stderr, "sim_damage: no damageable thing with a firing line\n");
        return 1;
    }

    test_default_vitals_death(fixture, scenario);

    // A second fresh runtime for the custom-gauge case, so the first run's
    // rounds/things cannot leak into it.
    Fixture custom;
    if (!load_fixture(custom, error)) {
        std::fprintf(stderr, "sim_damage: custom fixture: %s\n", error.c_str());
        return 1;
    }
    test_custom_vitals(custom, scenario);
    test_negatives(fixture.chassis);

    if (g_failures != 0) {
        std::fprintf(stderr, "sim_damage: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sim_damage: ok\n");
    return 0;
}
