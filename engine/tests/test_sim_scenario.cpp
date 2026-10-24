// Golden deterministic scenarios of the scripted `--sim` slice (`M9.5`, slice 1;
// docs/SIM_SCENARIOS.md).
//
// The scenario tests of this file answer one question: *do the numbers of the
// documented CLI runs stay the same after a change in the game tick?*  They do
// not re-test the physics, the weapon rules or the damage model -- those are
// pinned by `sim_player`, `sim_projectile`, `sim_damage` and the module tests.
// Here the *composition* `main.cpp::simulate()` performs with `--sim-drive` is
// repeated step for step, on a real level, and every printed number is asserted:
//
//   S1  `forward=1:900`           position, velocity, travelled distance, ticks;
//   S2  `turn=0.5:300`            yaw (= 300 * 0.5 / 91 rad) and position;
//   S3a `fire:200` (hitscan)      shots, ammo spent, `shot_index`;
//   S3b `fire:200 --sim-projectile` shots, rounds spawned, ammo, no target;
//   S3c aimed round at the weakest damageable thing of the level: blasts, target
//       hp/alive, ammo spent;
//   S4  `--sim-vitals 500,0` with `--sim-respawn 20`: death tick, respawn tick,
//       armor after the respawn; the same plan without `--sim-respawn` as the
//       control (the death tick freezes the player);
//   S5  `--god`: the VM context flag `god_mode` is set exactly as `--god` does,
//       and the player damage/death numbers are *identical* to S4 -- `--god` is
//       a script condition (`GOD_MODE` op), not a gauge guard at this layer;
//       the default LIGHT gauges survive one blast (armor untouched, plates
//       down) so "one blast does not kill" has a number too.
//
// Every scenario runs twice on two freshly loaded runtimes and the second run
// must repeat the first (`1e-12` on the doubles, exact on the counters).  The
// load path is `main.cpp::simulate()`: `load_level` -> `vm_state_from_level`
// (VmContext of the CLI defaults) -> `GameRuntime::build` (`ai_apply_aim`),
// `extract_scene` for the campaign spawn of slot 0, `chassis_from_level` for the
// LIGHT capsule, `Weapons::load(<level>.sbt)` with the full pickup loadout.
//
// What is *not* reachable from `main.cpp` today is stated in
// docs/SIM_SCENARIOS.md §4: the CLI places the player at the campaign spawn
// only, so the aimed scenarios (S3c/S4/S5) place the capsule explicitly with
// `SimPlayer::init()`; the numbers are the same tick, only the start pose is the
// caller's.
#include "game/sim_player.h"

#include "game/chassis_loadout.h"
#include "game/projectile.h"
#include "game/runtime.h"
#include "game/weapons.h"
#include "res/level.h"
#include "res/sbt.h"
#include "res/sbtvm.h"
#include "res/scene.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "world/things.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
/// `main.cpp::simulate()` puts the eye point of the runtime at `feet.z + 10`
/// before the first drive tick; the tick itself then installs the controller's
/// own eye.  Kept here so the setup matches the CLI exactly.
constexpr double kCliEyeOffset = 10.0;
/// Blast reach of the widest ROCKET air blast (`12000@256`): the first surface
/// of a firing line must be this close to the target (docs/SIM_DAMAGE.md §6).
constexpr double kBlastReach = 200.0;

// --- fixture ---------------------------------------------------------------

/// One parsed level plus the runtime built exactly like `main.cpp::simulate()`,
/// the campaign spawn of slot 0, the LIGHT chassis and the level weapon table.
struct Fixture {
    std::string level;
    std::unique_ptr<gm::GameRuntime> runtime;
    gm::Vec3d spawn{};
    double spawn_yaw = 0.0;
    bool have_spawn = false;
    gm::ChassisSpec chassis;
    std::optional<gm::Weapons> weapons;
    int projectile_slot = -1;
};

gm::GameRuntimeConfig sim_config() {
    gm::GameRuntimeConfig config;
    // Opt-in AI -> movement bridge, the CLI setting (`--sim` must not look the
    // same at tick 1 and tick 900).
    config.ai_apply_aim = true;
    return config;
}

/// VmContext of the CLI defaults (`sim_mode 0`, `sim_players 1`,
/// `sim_difficulty 1`) with `--god` as the only variable.
gm::VmContext cli_context(bool god) {
    gm::VmContext context;
    context.game_mode = 0;
    context.player_count = 1;
    context.players = 1;
    context.difficulty = 1;
    context.god_mode = god;
    return context;
}

bool load_fixture(const std::string& name, bool god, Fixture& out, std::string& error) {
    out.level = name;
    const auto level = gm::load_level(kGame, name, error);
    if (!level) {
        return false;
    }
    if (!level->entities || !level->world) {
        error = name + ": no .sbt/.sbw pair";
        return false;
    }
    const gm::VmState vm = gm::vm_state_from_level(*level->entities, cli_context(god));
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
    for (const gm::SceneSpawn& spawn : scene->spawns) {
        if (spawn.deathmatch || spawn.slot != 0) {
            continue;
        }
        out.spawn = {static_cast<double>(spawn.x), static_cast<double>(spawn.y),
                     static_cast<double>(spawn.z)};
        out.spawn_yaw = spawn.angles[2];
        out.have_spawn = true;
        break;
    }
    if (!out.have_spawn) {
        error = name + ": no campaign spawn for slot 0";
        return false;
    }

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

    auto weapons = gm::Weapons::load(kGame / (name + ".sbt"), &error);
    if (!weapons) {
        return false;
    }
    out.weapons = std::move(*weapons);
    return true;
}

/// The pickup loadout of `main.cpp::simulate()`: every table slot available
/// with its ammo-type maximum, slot 0 selected; with `want_projectile` the
/// preferred projectile slot is selected instead (ROCKET first, then the ranked
/// candidates -- the CLI choice).  Returns false when a projectile loadout was
/// asked for and the table has no non-hitscan weapon (the CLI error).
bool configure_weapons(Fixture& fixture, bool want_projectile) {
    gm::Arsenal arsenal = fixture.weapons->initial();
    arsenal.mask = 0;
    int projectile_slot = -1;
    for (int slot = 0; slot < static_cast<int>(fixture.weapons->size()); ++slot) {
        const gm::Weapon* weapon = fixture.weapons->at(slot);
        if (weapon == nullptr) {
            continue;
        }
        arsenal.mask |= 1u << slot;
        arsenal.ammo[slot] = fixture.weapons->ammo_max_for(weapon->ammo_type);
        if (weapon->name == "ROCKET") {
            projectile_slot = slot;
        } else if (!weapon->hitscan && projectile_slot >= 0 &&
                   fixture.weapons->at(projectile_slot)->name == "ROCKET") {
            // ROCKET stays the choice.
        } else if (!weapon->hitscan) {
            auto rank = [&fixture](int candidate) {
                if (candidate < 0) {
                    return 0;
                }
                const gm::Weapon* other = fixture.weapons->at(candidate);
                if (other == nullptr || other->hitscan) {
                    return 0;
                }
                const bool blasts = !other->impact_blasts.empty() ||
                                    !other->expire_blasts.empty();
                return 1 + (other->fire_mode == 1 ? 2 : 0) + (blasts ? 4 : 0);
            };
            const int rank_now = 1 + (weapon->fire_mode == 1 ? 2 : 0) +
                ((!weapon->impact_blasts.empty() || !weapon->expire_blasts.empty()) ? 4 : 0);
            if (rank_now > rank(projectile_slot)) {
                projectile_slot = slot;
            }
        }
    }
    fixture.projectile_slot = projectile_slot;
    if (want_projectile && projectile_slot < 0) {
        return false;
    }
    arsenal.current = want_projectile ? projectile_slot : 0;
    arsenal.shot_index = 0;
    fixture.runtime->set_player_weapons(*fixture.weapons, arsenal);
    return true;
}

/// ROCKET-only loadout of the aimed scenarios: the same fixture the damage
/// slice uses (slot 7, mask `1 << 7`, 20 rounds), so the probe and the real
/// shot share one spread stream for the first shot.
void set_rocket_loadout(Fixture& fixture) {
    gm::Arsenal arsenal = fixture.weapons->initial();
    arsenal.mask = 0;
    for (int slot = 0; slot < static_cast<int>(fixture.weapons->size()); ++slot) {
        if (fixture.weapons->at(slot) != nullptr) {
            arsenal.mask |= 1u << slot;
        }
    }
    arsenal.current = 7;
    arsenal.ammo[7] = 20;
    arsenal.shot_index = 0;
    fixture.runtime->set_player_weapons(*fixture.weapons, arsenal);
}

// --- synthetic round templates ---------------------------------------------

/// The template `main.cpp --sim-projectile` installs (movement type 6, gravity
/// 1.0, expire 364, create event 901).
gm::Thing cli_projectile() {
    gm::Thing projectile;
    projectile.id = -1;
    projectile.type = 31; // TT31, the ROCKET round
    projectile.movement_type = 6;
    projectile.gravity = 1.0;
    projectile.damping = 0.8;
    projectile.collision_shape = 1;
    projectile.radius = 10.0;
    projectile.hits_left = 1;
    projectile.expire_ticks = 364;
    projectile.create_event = 901;
    return projectile;
}

/// The flat-flight template of `test_sim_projectile.cpp` / `test_sim_damage.cpp`
/// (gravity 0, expire 728, unique create event): the aimed round must reach the
/// target the firing line points at.
gm::Thing aimed_projectile() {
    gm::Thing projectile;
    projectile.id = -1;
    projectile.type = 31;
    projectile.movement_type = 6;
    projectile.gravity = 0.0;
    projectile.damping = 0.8;
    projectile.collision_shape = 1;
    projectile.radius = 10.0;
    projectile.hits_left = 1;
    projectile.expire_ticks = 728;
    projectile.create_event = 64201;
    return projectile;
}

/// `--sim-vitals ARMOR,SHIELD` as `main.cpp` builds it: armor sets its own
/// maximum when positive, the four shields get the same value, `shield_max`
/// falls back to the LIGHT maximum for a zero shield value.
gm::Vitals cli_vitals(int armor, int shield) {
    gm::Vitals vitals = gm::kLightPlayerVitals;
    vitals.armor = armor;
    vitals.armor_max = armor > 0 ? armor : gm::kLightPlayerVitals.armor_max;
    vitals.shields = {shield, shield, shield, shield};
    vitals.shield_max = shield > 0 ? shield : gm::kLightPlayerVitals.shield_max;
    return vitals;
}

// --- aimed firing line -----------------------------------------------------

/// Weakest damageable thing of the level with a firing line, and the pose of the
/// shooter (the finder of `test_sim_projectile.cpp`, kept independent here).
struct AimScenario {
    std::size_t thing_index = 0;
    int thing_id = -1;
    std::string type_name;
    gm::Vec3d thing_position{};
    gm::Vec3d feet{};
    double yaw = 0.0;
    double stand_off = 0.0;
    double hit_distance = 0.0;
};

/// Real weapon seam asks where the first shot goes; the first surface must be
/// within the blast reach of the thing.
bool probe_line(Fixture& fixture, const gm::Vec3d& eye, double yaw,
                const gm::Vec3d& thing_position, double& hit_distance) {
    set_rocket_loadout(fixture);
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
    const auto hit =
        fixture.runtime->collision().raycast(eye, tick.action.shot.direction, 500.0);
    if (!hit) {
        return false;
    }
    hit_distance = hit->t;
    const gm::Vec3d delta{hit->point.x - thing_position.x,
                          hit->point.y - thing_position.y,
                          hit->point.z - thing_position.z};
    return std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) <=
           kBlastReach;
}

bool find_aim_scenario(Fixture& fixture, AimScenario& out) {
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
            continue; // damage-immune, apply_thing_damage is a no-op
        }
        candidates.push_back({index, vitals.max_hp});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& l, const Candidate& r) {
                  return l.max_hp != r.max_hp ? l.max_hp < r.max_hp
                                              : l.index < r.index;
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

// --- one run ---------------------------------------------------------------

struct RunOptions {
    std::string plan;
    std::optional<gm::Thing> projectile; ///< `--sim-projectile`
    std::optional<gm::Vitals> vitals;    ///< `--sim-vitals`
    int respawn_after = -1;              ///< `--sim-respawn`, negative = off
    bool use_ai_pose = false;            ///< take feet/yaw from `ai` (aimed cases)
    gm::Vec3d ai_feet{};
    double ai_yaw = 0.0;
    std::optional<std::size_t> target_index; ///< thing to read hp/alive from
};

struct Run {
    int drive_ticks = 0;
    int player_ticks = 0;
    int wall_ticks = 0;
    int respawns = 0;
    int first_respawn_tick = -1;
    gm::PlayerState state{};
    gm::Vitals vitals{};
    int shots = 0;
    int spawned = 0;
    int blast_hits = 0;
    int things_damaged = 0;
    int damage_taken = 0;
    int death_tick = -1;
    int deaths = 0;
    bool alive = true;
    int slot = -1;
    std::uint32_t ammo = 0;
    std::uint64_t shot_index = 0;
    std::string weapon;
    int weapon_mode = 0;
    int weapon_period = 0;
    bool weapon_hitscan = false;
    bool have_target = false;
    int target_hp_before = 0;
    int target_hp_after = 0;
    bool target_alive_after = true;
};

/// The `--sim-drive` loop of `main.cpp::simulate()`, step for step: player tick,
/// a runtime tick when the player is not in projectile mode (projectile mode
/// ticks the runtime itself), wall counter, `--sim-respawn N` delay counted in
/// drive ticks.
bool run_drive(Fixture& fixture, const RunOptions& options, Run& out, std::string& error) {
    const auto plan = gm::parse_drive(options.plan, &error);
    if (!plan) {
        return false;
    }
    if (!fixture.have_spawn) {
        error = fixture.level + ": no spawn";
        return false;
    }
    const gm::Vec3d feet = options.use_ai_pose ? options.ai_feet : fixture.spawn;
    const double yaw = options.use_ai_pose ? options.ai_yaw : fixture.spawn_yaw;
    if (!configure_weapons(fixture, options.projectile.has_value())) {
        error = fixture.level + ": no projectile weapon in the level table";
        return false;
    }
    if (options.target_index.has_value() &&
        *options.target_index >= fixture.runtime->things().things().size()) {
        error = fixture.level + ": target index out of range";
        return false;
    }

    gm::Vec3d eye = feet;
    eye.z += kCliEyeOffset;
    const std::span<const gm::Vec3d> eyes(&eye, 1);
    if (!fixture.runtime->set_player_eyes(eyes)) {
        error = fixture.level + ": player eye point rejected";
        return false;
    }

    gm::SimPlayer player;
    if (!player.init(feet, yaw, fixture.chassis, &error)) {
        return false;
    }
    if (options.vitals.has_value() && !player.set_vitals(*options.vitals, &error)) {
        return false;
    }
    if (options.projectile.has_value()) {
        player.set_projectile(*options.projectile, 0);
    }
    if (options.target_index.has_value()) {
        const gm::Thing& target =
            fixture.runtime->things().things().at(*options.target_index);
        out.have_target = true;
        out.target_hp_before = gm::thing_vitals(target).hp;
    }

    for (const gm::DriveSegment& segment : *plan) {
        for (int index = 0; index < segment.ticks; ++index) {
            ++out.drive_ticks;
            if (!player.tick(*fixture.runtime, segment, index, &error)) {
                return false;
            }
            if (!player.has_projectile()) {
                fixture.runtime->tick(1);
            }
            if (player.last_step().hit_wall) {
                ++out.wall_ticks;
            }
            if (!player.alive() && options.respawn_after >= 0 &&
                out.drive_ticks - player.death_tick() >= options.respawn_after) {
                if (!player.respawn(&error)) {
                    return false;
                }
                ++out.respawns;
                if (out.first_respawn_tick < 0) {
                    out.first_respawn_tick = out.drive_ticks;
                }
            }
        }
    }

    out.player_ticks = player.tick_index();
    out.state = player.state();
    out.vitals = player.vitals();
    out.shots = player.shots_fired();
    out.spawned = player.projectiles_spawned();
    out.blast_hits = player.blast_hits();
    out.things_damaged = player.things_damaged();
    out.damage_taken = player.damage_taken();
    out.death_tick = player.death_tick();
    out.deaths = player.deaths();
    out.alive = player.alive();
    out.slot = fixture.runtime->player_arsenal().current;
    out.ammo = player.ammo(out.slot);
    out.shot_index = fixture.runtime->player_arsenal().shot_index;
    if (const gm::Weapon* weapon = fixture.weapons->at(out.slot)) {
        out.weapon = weapon->name;
        out.weapon_mode = weapon->fire_mode;
        out.weapon_period = weapon->fire_ticks;
        out.weapon_hitscan = weapon->hitscan;
    }
    if (options.target_index.has_value()) {
        const gm::ThingVitals target = gm::thing_vitals(
            fixture.runtime->things().things().at(*options.target_index));
        out.target_hp_after = target.hp;
        out.target_alive_after = target.alive;
    }
    return true;
}

/// Two runs of the same options on two fresh runtimes must agree.
void check_determinism(const char* tag, const Run& a, const Run& b) {
    const int failures_before = g_failures;
    CHECK_EQ(a.drive_ticks, b.drive_ticks);
    CHECK_EQ(a.player_ticks, b.player_ticks);
    CHECK_EQ(a.wall_ticks, b.wall_ticks);
    CHECK_EQ(a.respawns, b.respawns);
    CHECK_EQ(a.first_respawn_tick, b.first_respawn_tick);
    CHECK_EQ(a.shots, b.shots);
    CHECK_EQ(a.spawned, b.spawned);
    CHECK_EQ(a.blast_hits, b.blast_hits);
    CHECK_EQ(a.things_damaged, b.things_damaged);
    CHECK_EQ(a.damage_taken, b.damage_taken);
    CHECK_EQ(a.death_tick, b.death_tick);
    CHECK_EQ(a.deaths, b.deaths);
    CHECK_EQ(a.alive, b.alive);
    CHECK_EQ(a.slot, b.slot);
    CHECK_EQ(a.ammo, b.ammo);
    CHECK_EQ(a.shot_index, b.shot_index);
    CHECK_EQ(a.target_hp_after, b.target_hp_after);
    CHECK_EQ(a.target_alive_after, b.target_alive_after);
    CHECK_NEAR(a.state.pos.x, b.state.pos.x, 1e-12);
    CHECK_NEAR(a.state.pos.y, b.state.pos.y, 1e-12);
    CHECK_NEAR(a.state.pos.z, b.state.pos.z, 1e-12);
    CHECK_NEAR(a.state.vel.x, b.state.vel.x, 1e-12);
    CHECK_NEAR(a.state.vel.y, b.state.vel.y, 1e-12);
    CHECK_NEAR(a.state.vel.z, b.state.vel.z, 1e-12);
    CHECK_NEAR(a.state.yaw, b.state.yaw, 1e-12);
    CHECK_EQ(a.state.on_ground, b.state.on_ground);
    CHECK_EQ(a.vitals.armor, b.vitals.armor);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK_EQ(a.vitals.shields[i], b.vitals.shields[i]);
    }
    std::printf("sim_scenario: determinism %s match=%d (ticks=%d pos=(%.6f, %.6f, "
                "%.6f) yaw=%.9f damage=%d death_tick=%d)\n",
                tag, g_failures == failures_before ? 1 : 0, a.drive_ticks, a.state.pos.x,
                a.state.pos.y, a.state.pos.z, a.state.yaw, a.damage_taken,
                a.death_tick);
}

void print_run(const char* tag, const Run& run) {
    std::printf("sim_scenario: %s ticks=%d player_ticks=%d pos=(%.6f, %.6f, %.6f) "
                "vel=(%.6f, %.6f, %.6f) yaw=%.9f on_ground=%d walls=%d\n",
                tag, run.drive_ticks, run.player_ticks, run.state.pos.x, run.state.pos.y,
                run.state.pos.z, run.state.vel.x, run.state.vel.y, run.state.vel.z,
                run.state.yaw, run.state.on_ground ? 1 : 0, run.wall_ticks);
    std::printf("sim_scenario: %s weapon=\"%s\" slot=%d mode=%d period=%d hitscan=%d "
                "shots=%d spawned=%d ammo=%u shot_index=%llu\n",
                tag, run.weapon.c_str(), run.slot, run.weapon_mode, run.weapon_period,
                run.weapon_hitscan ? 1 : 0, run.shots, run.spawned, run.ammo,
                static_cast<unsigned long long>(run.shot_index));
    std::printf("sim_scenario: %s armor=%d/%d shields=%d/%d/%d/%d alive=%d "
                "damage_taken=%d death_tick=%d deaths=%d respawns=%d "
                "first_respawn_tick=%d blasts=%d things_damaged=%d\n",
                tag, run.vitals.armor, run.vitals.armor_max, run.vitals.shields[0],
                run.vitals.shields[1], run.vitals.shields[2], run.vitals.shields[3],
                run.alive ? 1 : 0, run.damage_taken, run.death_tick, run.deaths,
                run.respawns, run.first_respawn_tick, run.blast_hits, run.things_damaged);
    if (run.have_target) {
        std::printf("sim_scenario: %s target_hp=%d->%d target_alive=%d\n", tag,
                    run.target_hp_before, run.target_hp_after,
                    run.target_alive_after ? 1 : 0);
    }
}

/// Load a fresh runtime, run the plan, then repeat on a second fresh runtime.
bool run_pair(const RunOptions& options, const char* tag, Run& primary, bool god = false) {
    std::string error;
    Fixture first;
    if (!load_fixture("dm1", god, first, error)) {
        std::fprintf(stderr, "sim_scenario: %s fixture: %s\n", tag, error.c_str());
        ++g_failures;
        return false;
    }
    if (!run_drive(first, options, primary, error)) {
        std::fprintf(stderr, "sim_scenario: %s: %s\n", tag, error.c_str());
        ++g_failures;
        return false;
    }
    print_run(tag, primary);

    Fixture second;
    if (!load_fixture("dm1", god, second, error)) {
        std::fprintf(stderr, "sim_scenario: %s replay fixture: %s\n", tag, error.c_str());
        ++g_failures;
        return false;
    }
    Run replay;
    if (!run_drive(second, options, replay, error)) {
        std::fprintf(stderr, "sim_scenario: %s replay: %s\n", tag, error.c_str());
        ++g_failures;
        return false;
    }
    check_determinism(tag, primary, replay);
    return true;
}

// --- S1..S5 -----------------------------------------------------------------

void test_s1_forward() {
    Run run;
    RunOptions options;
    options.plan = "forward=1:900"; // the documented plan of docs/SIM_PLAYER.md §4.1
    if (!run_pair(options, "S1", run)) {
        return;
    }
    const double travelled = std::sqrt((run.state.pos.x - (-160.0)) *
                                          (run.state.pos.x - (-160.0)) +
                                      (run.state.pos.y - 176.0) *
                                          (run.state.pos.y - 176.0));
    std::printf("sim_scenario: S1 travelled=%.6f\n", travelled);
    CHECK_EQ(run.drive_ticks, 900);
    CHECK_EQ(run.player_ticks, 900);
    CHECK_EQ(run.wall_ticks, 0);
    CHECK_EQ(run.shots, 0);
    CHECK_NEAR(run.state.pos.x, -160.0, 1e-6);
    CHECK_NEAR(run.state.pos.y, 1684.75, 1e-6);
    // The capsule rests on the contact skin above the dm1 ground plane: the
    // feet z is `contact_skin`, 1e-6 (docs/SIM_PLAYER.md §4.1), not 0.
    CHECK_NEAR(run.state.pos.z, 1e-6, 1e-9);
    CHECK_NEAR(travelled, 1508.75, 1e-6);
    // 1.7 world units per tick * 91 Hz (LIGHT `op4`), no vertical speed.
    CHECK_NEAR(run.state.vel.x, 0.0, 1e-12);
    CHECK_NEAR(run.state.vel.y, 154.7, 1e-6);
    CHECK_NEAR(run.state.vel.z, 0.0, 1e-12);
    CHECK(run.state.on_ground);
    CHECK_NEAR(run.state.yaw, 0.0, 0.0);
}

void test_s2_turn() {
    Run run;
    RunOptions options;
    options.plan = "turn=0.5:300";
    if (!run_pair(options, "S2", run)) {
        return;
    }
    // turn is radians per second: 300 ticks * 0.5 / 91 Hz.
    const double expected_yaw = 300.0 * 0.5 / 91.0;
    std::printf("sim_scenario: S2 yaw=%.12f expected=%.12f\n", run.state.yaw, expected_yaw);
    CHECK_EQ(run.drive_ticks, 300);
    CHECK_NEAR(run.state.yaw, expected_yaw, 1e-12);
    CHECK_NEAR(run.state.pos.x, -160.0, 1e-6); // no forward axis: no travel
    CHECK_NEAR(run.state.pos.y, 176.0, 1e-6);
    CHECK_NEAR(run.state.pos.z, 1e-6, 1e-9);
    CHECK_NEAR(run.state.vel.x, 0.0, 1e-12);
    CHECK_NEAR(run.state.vel.y, 0.0, 1e-12);
    CHECK(run.state.on_ground);
}

void test_s3_hitscan() {
    Run run;
    RunOptions options;
    options.plan = "fire:200";
    if (!run_pair(options, "S3a", run)) {
        return;
    }
    CHECK_EQ(run.slot, 0);
    CHECK(run.weapon_hitscan);
    CHECK_EQ(run.drive_ticks, 200);
    CHECK_EQ(run.shots, 12);          // MACHINE GUN, period 17 ticks: 200/17 -> 12
    CHECK_EQ(run.shot_index, 12);
    CHECK_EQ(run.ammo, 5000u - 12u);   // ammo-type maximum 5000, one round per shot
    CHECK_EQ(run.spawned, 0);          // hitscan: no round leaves the barrel
    CHECK_EQ(run.blast_hits, 0);
    CHECK_EQ(run.damage_taken, 0);     // no target on this line, no self-damage
    CHECK(run.alive);
}

void test_s3_projectile() {
    Run spawn_run;
    RunOptions options;
    options.plan = "fire:200";
    options.projectile = cli_projectile();
    if (!run_pair(options, "S3b", spawn_run)) {
        return;
    }
    CHECK_EQ(spawn_run.slot, 7); // ROCKET, the CLI projectile choice
    CHECK_EQ(spawn_run.drive_ticks, 200);
    CHECK_EQ(spawn_run.shots, 3);   // ROCKET period 80 ticks
    CHECK_EQ(spawn_run.spawned, 3); // every shot really spawned a round
    CHECK_EQ(spawn_run.shot_index, 3);
    CHECK_EQ(spawn_run.ammo, 47u);  // ammo-type maximum 50
    CHECK_EQ(spawn_run.blast_hits, 0); // the rounds of the spawn line hit nothing
    CHECK_EQ(spawn_run.things_damaged, 0);
    CHECK_EQ(spawn_run.damage_taken, 0);
    CHECK(spawn_run.alive);
}

void test_s3_target(const AimScenario& scenario) {
    Run run;
    RunOptions options;
    options.plan = "fire:20"; // one ROCKET (period 80), enough ticks to hit
    options.projectile = aimed_projectile();
    options.use_ai_pose = true;
    options.ai_feet = scenario.feet;
    options.ai_yaw = scenario.yaw;
    options.target_index = scenario.thing_index;
    std::printf("sim_scenario: S3c target [%zu] id=%d type=%s pos=(%.2f, %.2f, %.2f) "
                "feet=(%.2f, %.2f, %.2f) yaw=%.6f rad stand_off=%.0f "
                "blast_from_eye=%.2f\n",
                scenario.thing_index, scenario.thing_id, scenario.type_name.c_str(),
                scenario.thing_position.x, scenario.thing_position.y,
                scenario.thing_position.z, scenario.feet.x, scenario.feet.y,
                scenario.feet.z, scenario.yaw, scenario.stand_off,
                scenario.hit_distance);
    if (!run_pair(options, "S3c", run)) {
        return;
    }
    CHECK_EQ(run.slot, 7);
    CHECK_EQ(run.drive_ticks, 20);
    CHECK_EQ(run.shots, 1);
    CHECK_EQ(run.spawned, 1);
    CHECK_EQ(run.ammo, 49u);
    CHECK_EQ(run.shot_index, 1);
    CHECK_EQ(run.blast_hits, 2);      // both ROCKET air branches reach the target
    CHECK_EQ(run.things_damaged, 1);
    CHECK_EQ(run.target_hp_after, 0);
    CHECK(!run.target_alive_after);
    // Self-damage of the same blast with the dm1 ROCKET (the blast is ~166
    // units from the player: 12000*(256-166)/256 = 4218, >>2 = 1054 per plate).
    CHECK_EQ(run.damage_taken, 4216);
    CHECK(run.alive);
    CHECK_EQ(run.vitals.armor, 10000); // plates absorb it first
    CHECK_EQ(run.vitals.shields[0], 6946);
}

void test_s4_death_respawn(const AimScenario& scenario) {
    Run death;
    RunOptions options;
    options.plan = "fire:40";
    options.projectile = aimed_projectile();
    options.vitals = cli_vitals(500, 0);
    options.use_ai_pose = true;
    options.ai_feet = scenario.feet;
    options.ai_yaw = scenario.yaw;
    options.target_index = scenario.thing_index;
    if (!run_pair(options, "S4a", death)) {
        return;
    }
    CHECK_EQ(death.shots, 1);
    CHECK_EQ(death.damage_taken, 500);
    CHECK_EQ(death.death_tick, 9);
    CHECK_EQ(death.player_ticks, 9); // a dead tick is a no-op, the counter stops
    CHECK_EQ(death.drive_ticks, 40);
    CHECK_EQ(death.deaths, 0);
    CHECK(!death.alive);
    CHECK_EQ(death.vitals.armor, 0);

    Run respawned;
    options.plan = "fire:40";
    options.respawn_after = 20; // --sim-respawn 20
    if (!run_pair(options, "S4b", respawned)) {
        return;
    }
    std::printf("sim_scenario: S4b respawn: death_tick=%d first_respawn_tick=%d "
                "armor=%d/%d deaths=%d alive=%d\n",
                respawned.death_tick, respawned.first_respawn_tick,
                respawned.vitals.armor, respawned.vitals.armor_max, respawned.deaths,
                respawned.alive ? 1 : 0);
    CHECK_EQ(respawned.death_tick, 9);
    CHECK_EQ(respawned.first_respawn_tick, 29); // 9 + 20 drive ticks
    CHECK_EQ(respawned.deaths, 1);
    CHECK(respawned.alive);
    CHECK_EQ(respawned.vitals.armor, 500);      // starting gauges are back
    CHECK_EQ(respawned.vitals.armor_max, 500);
    CHECK_EQ(respawned.damage_taken, 500);      // total over the life of the player
    // 9 ticks of life before the death, then 11 more drive ticks after the
    // respawn at drive tick 29; the dead ticks did not advance the counter.
    CHECK_EQ(respawned.player_ticks, 20);
}

void test_s5_god(const AimScenario& scenario) {
    // `--god` sets `VmContext::god_mode`, which only feeds the VM condition
    // `GOD_MODE` (res/vm_exec.cpp) and the per-thing sub-scripts.  The player
    // gauge bridge (SimPlayer -> GameRuntime::set_player_vitals ->
    // apply_projectile_blasts_to_players) never reads it, so the same blast must
    // kill the same player on the same tick with `god=1`.
    Run god;
    RunOptions options;
    options.plan = "fire:40";
    options.projectile = aimed_projectile();
    options.vitals = cli_vitals(500, 0);
    options.use_ai_pose = true;
    options.ai_feet = scenario.feet;
    options.ai_yaw = scenario.yaw;
    options.target_index = scenario.thing_index;
    if (!run_pair(options, "S5a", god, /*god=*/true)) {
        return;
    }
    CHECK_EQ(god.shots, 1);
    CHECK_EQ(god.damage_taken, 500);
    CHECK_EQ(god.death_tick, 9);
    CHECK(!god.alive);
    CHECK_EQ(god.vitals.armor, 0);
    CHECK_EQ(god.blast_hits, 2);
    CHECK_EQ(god.things_damaged, 1);
    CHECK_EQ(god.target_hp_after, 0);

    // "One blast does not kill" with the default LIGHT gauges: the plates take
    // the hit, the armor is untouched.  This is the gauge rule, not `--god`.
    Run light;
    RunOptions light_options;
    light_options.plan = "fire:30";
    light_options.projectile = aimed_projectile();
    light_options.use_ai_pose = true;
    light_options.ai_feet = scenario.feet;
    light_options.ai_yaw = scenario.yaw;
    light_options.target_index = scenario.thing_index;
    if (!run_pair(light_options, "S5b", light)) {
        return;
    }
    CHECK_EQ(light.shots, 1);
    CHECK_EQ(light.blast_hits, 2);
    CHECK_EQ(light.damage_taken, 4216);
    CHECK(light.alive);
    CHECK_EQ(light.vitals.armor, 10000);
    CHECK_EQ(light.vitals.shields[0], 6946);
    CHECK_EQ(light.vitals.shields[1], 6946);
    CHECK_EQ(light.vitals.shields[2], 6946);
    CHECK_EQ(light.vitals.shields[3], 6946);
}

} // namespace

int main() {
    if (!fs::exists(kGame / "dm1.sbt") || !fs::exists(kGame / "dm1.sbw")) {
        std::printf("sim_scenario: dm1 not installed, skipped\n");
        return 0;
    }

    // The aimed scenarios need one scout runtime to pick the firing line; the
    // runs themselves always use fresh runtimes.
    AimScenario scenario;
    {
        std::string error;
        Fixture scout;
        if (!load_fixture("dm1", false, scout, error)) {
            std::fprintf(stderr, "sim_scenario: scout fixture: %s\n", error.c_str());
            return 1;
        }
        if (!find_aim_scenario(scout, scenario)) {
            std::fprintf(stderr, "sim_scenario: no damageable thing with a firing line\n");
            return 1;
        }
    }

    test_s1_forward();
    test_s2_turn();
    test_s3_hitscan();
    test_s3_projectile();
    test_s3_target(scenario);
    test_s4_death_respawn(scenario);
    test_s5_god(scenario);

    if (g_failures != 0) {
        std::fprintf(stderr, "sim_scenario: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sim_scenario: ok\n");
    return 0;
}
