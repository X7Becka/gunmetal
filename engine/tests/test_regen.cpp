// Shield / armor regeneration in the player tick (`M6.1.3`, docs/REGEN.md).
//
// What this pins:
//
//   * the data fact: `dm1`/`level0` carry exactly one
//     `DEFINE_REGENERATION_MARKER (19, 3000.0, 50.0)` (opcode 174, `0x45c375`,
//     `docs/PICKUP.md` §7).  The two doubles have no reader in 1.36, so the
//     shipped game does not regenerate; the rule of `game/regen.h` is a model;
//   * the pure tick arithmetic: the delay window, the per-tick rate, a
//     fractional carry, the fixed fill order (shields front/right/back/left,
//     then armor), no overfill and the invalid-rule refusals;
//   * the player seam on a real `dm1` runtime + the real `level0` ROCKET
//     (same fixture as `test_sim_damage.cpp`): damage restarts the delay, the
//     shields then grow by `rate` per tick, `damage_taken()` never drops, and
//     a new hit restarts the window again;
//   * default off = the old slice-3 behaviour: after the same 30-tick shot the
//     gauges stay frozen for 120 idle ticks, and a replay produces identical
//     numbers;
//   * negatives: negative/NaN/inf `rate_per_tick`, negative/oversized
//     `delay_ticks` and an empty (neither pool) rule are refused with a text.
//
// Load path is the one of `test_sim_damage.cpp` (`load_level` ->
// `vm_state_from_level` -> `GameRuntime::build`, `chassis_from_level`).
#include "game/regen.h"

#include "game/chassis_loadout.h"
#include "game/projectile.h"
#include "game/runtime.h"
#include "game/sim_player.h"
#include "game/weapons.h"
#include "res/level.h"
#include "res/sbt.h"
#include "res/sbtvm.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "world/things.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
constexpr std::uint32_t kTestCreateEvent = 64201;
/// Blast reach of the widest ROCKET air blast (12000@256), as in slice 3.
constexpr double kBlastReach = 200.0;

int gauge_sum(const gm::Vitals& vitals) {
    int sum = vitals.armor;
    for (const std::int32_t shield : vitals.shields) {
        sum += shield;
    }
    return sum;
}

int shield_sum(const gm::Vitals& vitals) {
    int sum = 0;
    for (const std::int32_t shield : vitals.shields) {
        sum += shield;
    }
    return sum;
}

bool gauges_within_max(const gm::Vitals& vitals) {
    if (vitals.armor < 0 || vitals.armor > vitals.armor_max) {
        return false;
    }
    for (const std::int32_t shield : vitals.shields) {
        if (shield < 0 || shield > vitals.shield_max) {
            return false;
        }
    }
    return true;
}

void print_gauges(const char* tag, const gm::Vitals& vitals) {
    std::printf("%s armor=%d/%d shields=%d/%d/%d/%d shield_max=%d\n", tag, vitals.armor,
                vitals.armor_max, vitals.shields[0], vitals.shields[1], vitals.shields[2],
                vitals.shields[3], vitals.shield_max);
}

// --- the data fact ---------------------------------------------------------

struct MarkerFact {
    int count = 0;
    std::int64_t sob = -1;
    double a = 0.0;
    double b = 0.0;
    std::string error;
};

/// Find every `DEFINE_REGENERATION_MARKER` of a decoded SCRIPT block (bodies
/// included; the shipped ones are top level, but the walk is generic).
void find_marker(const std::vector<gm::SbtInsn>& insns, MarkerFact& out) {
    for (const gm::SbtInsn& insn : insns) {
        if (insn.opcode == 174) {
            ++out.count;
            if (insn.args.size() == 3) {
                out.sob = static_cast<std::int64_t>(insn.args[0].u);
                out.a = insn.args[1].d;
                out.b = insn.args[2].d;
            }
        }
        find_marker(insn.body, out);
    }
}

MarkerFact marker_fact(const char* level_name) {
    MarkerFact out;
    std::string error;
    const auto level = gm::load_level(kGame, level_name, error);
    if (!level) {
        out.error = error;
        return out;
    }
    if (!level->entities) {
        out.error = "no SCRIPT/SBT record";
        return out;
    }
    const auto program = gm::decode_sbt_script(level->entities->script_code, error);
    if (!program) {
        out.error = error;
        return out;
    }
    find_marker(program->insns, out);
    return out;
}

void test_marker_data() {
    for (const char* name : {"dm1", "level0"}) {
        const MarkerFact fact = marker_fact(name);
        std::printf("regen: %s DEFINE_REGENERATION_MARKER count=%d args=(%lld, %.1f, %.1f)%s%s\n",
                    name, fact.count, static_cast<long long>(fact.sob), fact.a, fact.b,
                    fact.error.empty() ? "" : " error=", fact.error.c_str());
        CHECK_EQ(fact.count, 1);
        CHECK_EQ(fact.sob, 19);
        CHECK_EQ(fact.a, 3000.0);
        CHECK_EQ(fact.b, 50.0);
    }
}

// --- pure rule arithmetic --------------------------------------------------

void test_pure_delay_and_rate() {
    // front shield only damaged, rest full; delay 3 ticks, 2 hundredths/tick.
    gm::Vitals vitals{10000, 10000, 8000, {7000, 8000, 8000, 8000}, 1.0, 1.0, 0, 0, 0};
    const gm::Vitals start = vitals;
    gm::RegenState state;
    const gm::RegenRule rule{3, 2.0, true, false};

    CHECK(gm::regen_rule_error(rule) == nullptr);
    CHECK(gm::apply_regen(vitals, state, rule, true)); // damage tick
    CHECK_EQ(state.since_damage, 0);
    CHECK_EQ(state.carry, 0.0);
    CHECK(gm::apply_regen(vitals, state, rule, true));
    CHECK_EQ(vitals.shields[0], 7000);

    for (int tick = 1; tick <= 3; ++tick) {
        CHECK(gm::apply_regen(vitals, state, rule, false));
        CHECK_EQ(state.since_damage, tick);
        CHECK_EQ(vitals.shields[0], 7000); // still inside the delay
    }
    CHECK(gm::apply_regen(vitals, state, rule, false)); // delay + 1
    CHECK_EQ(state.since_damage, 4);
    CHECK_EQ(vitals.shields[0], 7002);
    for (int tick = 0; tick < 9; ++tick) {
        CHECK(gm::apply_regen(vitals, state, rule, false));
    }
    std::printf("regen: pure delay=3 rate=2.0: front shield %d -> %d after 3+10 ticks, "
                "since_damage=%d carry=%.1f\n",
                start.shields[0], vitals.shields[0], state.since_damage, state.carry);
    CHECK_EQ(vitals.shields[0], 7020); // 7000 + 2 * 10
    CHECK_EQ(state.carry, 0.0);
    CHECK(gauges_within_max(vitals));
}

void test_pure_fractional_carry() {
    gm::Vitals vitals{10000, 10000, 8000, {7990, 8000, 8000, 8000}, 1.0, 1.0, 0, 0, 0};
    gm::RegenState state;
    const gm::RegenRule rule{0, 0.5, true, false};

    // 10 ticks at 0.5 hundredths = exactly 5 hundredths; the odd ticks carry.
    CHECK(gm::apply_regen(vitals, state, rule, false));
    CHECK_EQ(vitals.shields[0], 7990);
    CHECK(gm::apply_regen(vitals, state, rule, false));
    CHECK_EQ(vitals.shields[0], 7991);
    for (int tick = 2; tick < 10; ++tick) {
        CHECK(gm::apply_regen(vitals, state, rule, false));
    }
    std::printf("regen: pure rate=0.5 over 10 ticks: front shield 7990 -> %d, carry=%.1f\n",
                vitals.shields[0], state.carry);
    CHECK_EQ(vitals.shields[0], 7995);
    CHECK_EQ(state.carry, 0.0);
}

void test_pure_order_and_caps() {
    // Shields first, armor after they are full.
    gm::Vitals vitals{9990, 10000, 8000, {8000, 8000, 8000, 8000}, 1.0, 1.0, 0, 0, 0};
    gm::RegenState state;
    const gm::RegenRule rule{0, 5.0, true, true};
    for (int tick = 0; tick < 2; ++tick) {
        CHECK(gm::apply_regen(vitals, state, rule, false));
    }
    CHECK_EQ(vitals.armor, 10000);
    // A full set drops the carry and never overfills.
    for (int tick = 0; tick < 5; ++tick) {
        CHECK(gm::apply_regen(vitals, state, rule, false));
    }
    CHECK_EQ(vitals.armor, 10000);
    CHECK_EQ(state.carry, 0.0);
    CHECK(gauges_within_max(vitals));

    // One hundredth short in two pools: one tick fills both, the rest of the
    // 100-hundredth budget is dropped, not banked.
    gm::Vitals edge{9999, 10000, 8000, {7999, 8000, 8000, 8000}, 1.0, 1.0, 0, 0, 0};
    gm::RegenState edge_state;
    const gm::RegenRule big{0, 100.0, true, true};
    CHECK(gm::apply_regen(edge, edge_state, big, false));
    std::printf("regen: pure caps: armor 9999 -> %d, front shield 7999 -> %d, carry=%.1f\n",
                edge.armor, edge.shields[0], edge_state.carry);
    CHECK_EQ(edge.armor, 10000);
    CHECK_EQ(edge.shields[0], 8000);
    CHECK_EQ(edge_state.carry, 0.0);
    CHECK(gauges_within_max(edge));

    // shield_max = 0: the shield part is a no-op, the armor still fills.
    gm::Vitals no_shields{9999, 10000, 0, {0, 0, 0, 0}, 1.0, 1.0, 0, 0, 0};
    gm::RegenState no_shields_state;
    CHECK(gm::apply_regen(no_shields, no_shields_state, big, false));
    CHECK_EQ(no_shields.armor, 10000);
    for (const std::int32_t shield : no_shields.shields) {
        CHECK_EQ(shield, 0);
    }
}

void test_pure_negatives() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    struct Bad {
        const char* name;
        gm::RegenRule rule;
    };
    const Bad cases[] = {
        {"delay=-1", {-1, 2.0, true, true}},
        {"delay>max", {gm::kRegenMaxDelayTicks + 1, 2.0, true, true}},
        {"rate=-1", {0, -1.0, true, true}},
        {"rate=NaN", {0, nan, true, true}},
        {"rate=inf", {0, inf, true, true}},
        {"rate>max", {0, gm::kRegenMaxRatePerTick + 1.0, true, true}},
        {"no pools", {0, 2.0, false, false}},
    };
    for (const Bad& item : cases) {
        gm::Vitals vitals{9000, 10000, 8000, {7000, 8000, 8000, 8000}, 1.0, 1.0, 0, 0, 0};
        const gm::Vitals before = vitals;
        gm::RegenState state;
        state.since_damage = 7;
        state.carry = 0.25;
        const gm::RegenState state_before = state;
        const char* why = gm::regen_rule_error(item.rule);
        const bool applied = gm::apply_regen(vitals, state, item.rule, false);
        std::printf("regen: reject %s -> applied=%d error=\"%s\"\n", item.name,
                    applied ? 1 : 0, why != nullptr ? why : "");
        CHECK(why != nullptr);
        CHECK(why != nullptr && why[0] != '\0');
        CHECK(!applied);
        CHECK_EQ(vitals.armor, before.armor);
        CHECK_EQ(vitals.shields[0], before.shields[0]);
        CHECK_EQ(state.since_damage, state_before.since_damage);
        CHECK_EQ(state.carry, state_before.carry);
    }
}

// --- SimPlayer on the real dm1 + ROCKET fixture ----------------------------

struct Fixture {
    std::unique_ptr<gm::GameRuntime> runtime;
    gm::ChassisSpec chassis;
    std::optional<gm::Weapons> weapons;
    gm::Arsenal base_arsenal;
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
    out.base_arsenal = weapons->initial();
    out.base_arsenal.mask = 1u << 7;
    out.base_arsenal.current = 7;
    out.base_arsenal.ammo[7] = 20;
    out.weapons = std::move(*weapons);
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

struct ShotScenario {
    gm::Vec3d feet{};
    double yaw = 0.0;
    int thing_id = -1;
    int max_hp = 0;
    std::string type_name;
};

bool probe_line(Fixture& fixture, const gm::Vec3d& eye, double yaw,
                const gm::Vec3d& thing_position) {
    reset_loadout(fixture);
    fixture.runtime->clear_player_projectile();
    gm::PlayerWeaponInput input;
    input.trigger_held = true;
    input.trigger_edge = true;
    input.slot = -1;
    input.aim_direction = {std::cos(yaw), std::sin(yaw), 0.0};
    const gm::PlayerWeaponTick tick = fixture.runtime->update_player_weapon(input, eye);
    if (!tick.action.fired() || !tick.action.shot.projectile) {
        return false;
    }
    const auto hit = fixture.runtime->collision().raycast(eye, tick.action.shot.direction, 500.0);
    if (!hit) {
        return false;
    }
    const gm::Vec3d delta{hit->point.x - thing_position.x, hit->point.y - thing_position.y,
                          hit->point.z - thing_position.z};
    const double distance =
        std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    return distance <= kBlastReach;
}

/// The finder of `test_sim_damage.cpp`: weakest damageable thing with a firing
/// line, player 30..90 units in front of it.  With `strongest = true` the sort
/// is reversed, which the two-burst case needs: the target must survive the
/// first ROCKET, otherwise the second round flies past the player and there is
/// no second damage event to restart the delay.
bool find_shot_scenario(Fixture& fixture, ShotScenario& out, bool strongest) {
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
            continue;
        }
        candidates.push_back({index, vitals.max_hp});
    }
    std::sort(candidates.begin(), candidates.end(),
              [strongest](const Candidate& l, const Candidate& r) {
                  if (l.max_hp != r.max_hp) {
                      return strongest ? l.max_hp > r.max_hp : l.max_hp < r.max_hp;
                  }
                  return l.index < r.index;
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
                if (!probe_line(fixture, eye, yaw, thing_position)) {
                    continue;
                }
                out.feet = feet;
                out.yaw = yaw;
                out.thing_id = thing.id;
                out.max_hp = candidate.max_hp;
                out.type_name = thing.type_name;
                return true;
            }
        }
    }
    return false;
}

/// One scripted player on the fixture with the ROCKET template installed.
bool init_player(Fixture& fixture, const ShotScenario& scenario, gm::SimPlayer& player,
                 std::string& error) {
    reset_loadout(fixture);
    if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        return false;
    }
    player.set_projectile(make_projectile_template(), 0);
    return true;
}

struct RunSnapshot {
    gm::Vitals vitals{};
    int damage_taken = 0;
    int shots = 0;
    int spawned = 0;
    int blast_hits = 0;
    int ticks = 0;
    bool alive = true;
};

RunSnapshot snapshot(const gm::SimPlayer& player) {
    RunSnapshot out;
    out.vitals = player.vitals();
    out.damage_taken = player.damage_taken();
    out.shots = player.shots_fired();
    out.spawned = player.projectiles_spawned();
    out.blast_hits = player.blast_hits();
    out.ticks = player.tick_index();
    out.alive = player.alive();
    return out;
}

bool same_gauges(const gm::Vitals& a, const gm::Vitals& b) {
    if (a.armor != b.armor || a.armor_max != b.armor_max || a.shield_max != b.shield_max) {
        return false;
    }
    return a.shields == b.shields;
}

/// 220 ticks with the trigger on `[0,30)` and `[80,150)`, one damage log per
/// tick (the gauge sum, plus the ticks where it went down).
///
/// The second burst is longer than 30 ticks on purpose: the weapon cooldown of
/// ROCKET (80 ticks) only counts down while the trigger is held in this seam,
/// so after 30 trigger ticks the cooldown is still 50 and a 30-tick second
/// burst would not fire at all.
constexpr int kWindowTicks = 220;

struct WindowRun {
    std::vector<int> sums;
    std::vector<int> damage;
    std::vector<int> damage_ticks;
    int damage_taken_max = 0;
    int shots = 0;
    int spawned = 0;
    int blast_hits = 0;
    bool monotonic_damage = true;
    bool within_max = true;
    bool alive = true;
    gm::Vitals final{};
};

bool run_two_windows(Fixture& fixture, const ShotScenario& scenario,
                     const gm::RegenRule* rule, WindowRun& out, std::string& error) {
    gm::SimPlayer player;
    if (!init_player(fixture, scenario, player, error)) {
        return false;
    }
    if (rule != nullptr && !player.set_regen(*rule, &error)) {
        return false;
    }
    gm::DriveSegment segment;
    segment.ticks = 1; // unused by tick(); the loop drives one tick at a time
    int previous_sum = gauge_sum(player.vitals());
    int previous_damage = player.damage_taken();
    for (int tick = 0; tick < kWindowTicks; ++tick) {
        segment.fire = tick < 30 || (tick >= 80 && tick < 150);
        if (!player.tick(*fixture.runtime, segment, tick, &error)) {
            return false;
        }
        const int sum = gauge_sum(player.vitals());
        out.sums.push_back(sum);
        out.damage.push_back(player.damage_taken());
        if (sum < previous_sum) {
            out.damage_ticks.push_back(tick);
        }
        previous_sum = sum;
        if (player.damage_taken() < previous_damage) {
            out.monotonic_damage = false;
        }
        previous_damage = player.damage_taken();
        out.damage_taken_max = std::max(out.damage_taken_max, player.damage_taken());
        if (!gauges_within_max(player.vitals())) {
            out.within_max = false;
        }
    }
    out.alive = player.alive();
    out.final = player.vitals();
    out.shots = player.shots_fired();
    out.spawned = player.projectiles_spawned();
    out.blast_hits = player.blast_hits();
    return true;
}

void test_sim_regen(Fixture& fixture, const ShotScenario& scenario) {
    constexpr int kDelay = 10;
    constexpr int kRate = 2;
    const gm::RegenRule rule{kDelay, static_cast<double>(kRate), true, false};
    WindowRun run;
    std::string error;
    if (!run_two_windows(fixture, scenario, &rule, run, error)) {
        std::fprintf(stderr, "regen: sim run: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    std::printf("regen: sim %d ticks (fire 0..29, 80..149) damage_ticks=%zu "
                "damage_taken=%d shots=%d spawned=%d blasts=%d alive=%d\n",
                kWindowTicks, run.damage_ticks.size(), run.damage_taken_max, run.shots,
                run.spawned, run.blast_hits, run.alive ? 1 : 0);
    CHECK(run.alive);
    CHECK(run.within_max);
    CHECK(run.monotonic_damage);
    CHECK(!run.damage_ticks.empty());

    // Two bursts = two damage events (a hit can land a few ticks after the
    // shot).  Find one per burst and check the delay window of each.
    int first_damage = -1;
    int second_damage = -1;
    for (const int tick : run.damage_ticks) {
        if (tick < 30 && first_damage < 0) {
            first_damage = tick;
        }
        if (tick >= 80) {
            second_damage = tick;
        }
    }
    std::printf("regen: sim first_damage=%d second_damage=%d (delay=%d rate=%d)\n",
                first_damage, second_damage, kDelay, kRate);
    CHECK(first_damage >= 0);
    CHECK(second_damage > first_damage);
    if (first_damage < 0 || second_damage <= first_damage) {
        return;
    }

    const auto growth = [&run](int tick) {
        return run.sums[static_cast<std::size_t>(tick)] -
               run.sums[static_cast<std::size_t>(tick - 1)];
    };
    // Nothing during the delay, then exactly `rate` per tick.
    for (int tick = first_damage + 1; tick <= first_damage + kDelay; ++tick) {
        if (tick <= second_damage) {
            CHECK_EQ(growth(tick), 0);
        }
    }
    const int first_regen = first_damage + kDelay + 1;
    std::printf("regen: sim first regen tick=%d growth=%d (expected %d), "
                "shields %d -> %d\n",
                first_regen, growth(first_regen), kRate,
                run.sums[static_cast<std::size_t>(first_regen - 1)],
                run.sums[static_cast<std::size_t>(first_regen)]);
    CHECK_EQ(growth(first_regen), kRate);
    CHECK_EQ(growth(first_regen + 1), kRate);
    CHECK_EQ(growth(first_regen + 2), kRate);
    // ... up to the tick before the second burst resets the window.
    for (int tick = first_regen + 3; tick < second_damage; ++tick) {
        CHECK_EQ(growth(tick), kRate);
    }
    // `damage_taken()` is gauge *loss*: the regen window must not change it.
    const int damage_after_first = run.damage[static_cast<std::size_t>(first_damage)];
    for (int tick = first_damage; tick < second_damage; ++tick) {
        CHECK_EQ(run.damage[static_cast<std::size_t>(tick)], damage_after_first);
    }
    // The second hit restarts the delay: nothing for `kDelay` ticks, then rate.
    for (int tick = second_damage + 1; tick <= second_damage + kDelay; ++tick) {
        CHECK_EQ(growth(tick), 0);
    }
    const int second_regen = second_damage + kDelay + 1;
    CHECK(second_regen < kWindowTicks);
    std::printf("regen: sim second regen tick=%d growth=%d (expected %d), "
                "shields %d -> %d\n",
                second_regen, growth(second_regen), kRate,
                run.sums[static_cast<std::size_t>(second_regen - 1)],
                run.sums[static_cast<std::size_t>(second_regen)]);
    CHECK_EQ(growth(second_regen), kRate);
    if (second_regen + 1 < kWindowTicks) {
        CHECK_EQ(growth(second_regen + 1), kRate);
    }
    const int damage_after_second = run.damage[static_cast<std::size_t>(second_damage)];
    CHECK(damage_after_second > damage_after_first);
    for (int tick = second_damage; tick < kWindowTicks; ++tick) {
        CHECK_EQ(run.damage[static_cast<std::size_t>(tick)], damage_after_second);
    }
}

/// `set_vitals()` near the caps + a fast rule: the tick never overfills and
/// the (zero) damage counter is untouched.
void test_sim_caps(Fixture& fixture, const ShotScenario& scenario) {
    gm::SimPlayer player;
    std::string error;
    if (!init_player(fixture, scenario, player, error)) {
        ++g_failures;
        return;
    }
    gm::Vitals near;
    near.armor = 9990;
    near.armor_max = 10000;
    near.shield_max = 8000;
    near.shields = {7999, 8000, 8000, 8000};
    if (!player.set_vitals(near, &error)) {
        std::fprintf(stderr, "regen: set_vitals(near): %s\n", error.c_str());
        ++g_failures;
        return;
    }
    const gm::RegenRule rule{0, 100.0, true, true};
    if (!player.set_regen(rule, &error)) {
        ++g_failures;
        return;
    }
    gm::DriveSegment idle;
    idle.ticks = 1;
    for (int tick = 0; tick < 10; ++tick) {
        if (!player.tick(*fixture.runtime, idle, tick, &error)) {
            std::fprintf(stderr, "regen: caps tick: %s\n", error.c_str());
            ++g_failures;
            return;
        }
    }
    print_gauges("regen: caps after 10 idle ticks:", player.vitals());
    CHECK_EQ(player.vitals().armor, 10000);
    CHECK_EQ(player.vitals().shield_max, 8000);
    for (const std::int32_t shield : player.vitals().shields) {
        CHECK_EQ(shield, 8000);
    }
    CHECK_EQ(player.damage_taken(), 0);
    CHECK(gauges_within_max(player.vitals()));
}

/// Default off: the 30-tick first shot of `test_sim_damage.cpp` followed by
/// 120 idle ticks must keep the gauges frozen, and a replay must match.
void test_default_parity(Fixture& fixture, const ShotScenario& scenario) {
    std::string error;
    reset_loadout(fixture);
    gm::SimPlayer player;
    if (!player.init(scenario.feet, scenario.yaw, fixture.chassis, &error)) {
        ++g_failures;
        return;
    }
    CHECK(!player.has_regen()); // no rule unless set_regen() is called
    player.set_projectile(make_projectile_template(), 0);
    gm::DriveSegment fire;
    fire.ticks = 30;
    fire.fire = true;
    for (int tick = 0; tick < fire.ticks; ++tick) {
        if (!player.tick(*fixture.runtime, fire, tick, &error)) {
            std::fprintf(stderr, "regen: parity fire: %s\n", error.c_str());
            ++g_failures;
            return;
        }
    }
    const RunSnapshot after_fire = snapshot(player);
    std::printf("regen: parity 30 ticks: shots=%d spawned=%d blasts=%d damage=%d ",
                after_fire.shots, after_fire.spawned, after_fire.blast_hits,
                after_fire.damage_taken);
    print_gauges("gauges:", after_fire.vitals);
    CHECK(after_fire.shots > 0);
    CHECK(after_fire.damage_taken > 0);
    CHECK(after_fire.alive);
    // The exact numbers of `test_sim_damage.cpp`'s "first shot after 30 ticks"
    // line on this fixture (`dm1` + level0 ROCKET, OILDRUM id 139): the default
    // path must stay byte-for-byte the slice-3 one.
    CHECK_EQ(after_fire.shots, 1);
    CHECK_EQ(after_fire.spawned, 1);
    CHECK_EQ(after_fire.blast_hits, 2);
    CHECK_EQ(after_fire.damage_taken, 5060);
    for (const std::int32_t shield : after_fire.vitals.shields) {
        CHECK_EQ(shield, 6735);
    }
    CHECK_EQ(after_fire.vitals.armor, 10000);

    gm::DriveSegment idle;
    idle.ticks = 120;
    for (int tick = 0; tick < idle.ticks; ++tick) {
        if (!player.tick(*fixture.runtime, idle, tick, &error)) {
            std::fprintf(stderr, "regen: parity idle: %s\n", error.c_str());
            ++g_failures;
            return;
        }
    }
    const RunSnapshot after_idle = snapshot(player);
    std::printf("regen: parity after 120 idle ticks: damage %d -> %d ",
                after_fire.damage_taken, after_idle.damage_taken);
    print_gauges("gauges:", after_idle.vitals);
    // No regeneration without a rule and no new damage without a shot: the
    // numbers of slice 3 are frozen, exactly as `test_sim_damage` sees them.
    CHECK(same_gauges(after_fire.vitals, after_idle.vitals));
    CHECK_EQ(after_idle.damage_taken, after_fire.damage_taken);

    // Replay the 30-tick shot on a fresh runtime: identical numbers.
    Fixture replay;
    if (!load_fixture(replay, error)) {
        ++g_failures;
        return;
    }
    reset_loadout(replay);
    gm::SimPlayer second;
    if (!second.init(scenario.feet, scenario.yaw, replay.chassis, &error)) {
        ++g_failures;
        return;
    }
    second.set_projectile(make_projectile_template(), 0);
    for (int tick = 0; tick < fire.ticks; ++tick) {
        if (!second.tick(*replay.runtime, fire, tick, &error)) {
            ++g_failures;
            return;
        }
    }
    const RunSnapshot replayed = snapshot(second);
    std::printf("regen: parity replay: shots=%d spawned=%d blasts=%d damage=%d\n",
                replayed.shots, replayed.spawned, replayed.blast_hits,
                replayed.damage_taken);
    CHECK_EQ(replayed.shots, after_fire.shots);
    CHECK_EQ(replayed.spawned, after_fire.spawned);
    CHECK_EQ(replayed.blast_hits, after_fire.blast_hits);
    CHECK_EQ(replayed.damage_taken, after_fire.damage_taken);
    CHECK(same_gauges(replayed.vitals, after_fire.vitals));
}

void test_sim_negatives(const gm::ChassisSpec& chassis) {
    gm::SimPlayer player;
    std::string error;
    const gm::Vec3d feet{0.0, 0.0, 0.0};
    if (!player.init(feet, 0.0, chassis, &error)) {
        ++g_failures;
        return;
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    struct Bad {
        const char* name;
        gm::RegenRule rule;
    };
    const Bad cases[] = {
        {"delay=-1", {-1, 2.0, true, true}},
        {"rate=-0.5", {10, -0.5, true, true}},
        {"rate=NaN", {10, nan, true, true}},
        {"no pools", {10, 2.0, false, false}},
    };
    for (const Bad& item : cases) {
        error.clear();
        const bool accepted = player.set_regen(item.rule, &error);
        std::printf("regen: set_regen %s -> ok=%d error=\"%s\"\n", item.name,
                    accepted ? 1 : 0, error.c_str());
        CHECK(!accepted);
        CHECK(!error.empty());
        CHECK(!player.has_regen());
    }

    const gm::RegenRule good{5, 1.5, true, true};
    CHECK(player.set_regen(good, &error));
    CHECK(player.has_regen());
    CHECK(player.regen_rule() != nullptr);
    CHECK_EQ(player.regen_rule()->delay_ticks, 5);
    CHECK_EQ(player.regen_rule()->rate_per_tick, 1.5);
    // A rejected replacement leaves the valid rule in place.
    CHECK(!player.set_regen({5, nan, true, true}, &error));
    CHECK(player.has_regen());
    CHECK_EQ(player.regen_rule()->rate_per_tick, 1.5);
    player.clear_regen();
    CHECK(!player.has_regen());
    CHECK_EQ(player.regen_state().since_damage, 0);
    CHECK_EQ(player.regen_state().carry, 0.0);
}

} // namespace

int main() {
    if (!fs::exists(kGame / "dm1.sbt") || !fs::exists(kGame / "dm1.sbw") ||
        !fs::exists(kGame / "level0.sbt")) {
        std::printf("regen: dm1/level0 not installed, skipped\n");
        return 0;
    }

    test_marker_data();
    test_pure_delay_and_rate();
    test_pure_fractional_carry();
    test_pure_order_and_caps();
    test_pure_negatives();

    Fixture fixture;
    std::string error;
    if (!load_fixture(fixture, error)) {
        std::fprintf(stderr, "regen: dm1 fixture: %s\n", error.c_str());
        return 1;
    }
    ShotScenario weak;
    ShotScenario strong;
    if (!find_shot_scenario(fixture, weak, false)) {
        std::fprintf(stderr, "regen: no damageable thing with a firing line\n");
        return 1;
    }
    if (!find_shot_scenario(fixture, strong, true)) {
        std::fprintf(stderr, "regen: no strong damageable thing with a firing line\n");
        return 1;
    }
    std::printf("regen: weak scenario thing id=%d type=%s max_hp=%d feet=(%.2f, %.2f, "
                "%.2f) yaw=%.6f rad (%.1f deg)\n",
                weak.thing_id, weak.type_name.c_str(), weak.max_hp, weak.feet.x,
                weak.feet.y, weak.feet.z, weak.yaw, weak.yaw * 180.0 / kPi);
    std::printf("regen: strong scenario thing id=%d type=%s max_hp=%d feet=(%.2f, %.2f, "
                "%.2f) yaw=%.6f rad (%.1f deg)\n",
                strong.thing_id, strong.type_name.c_str(), strong.max_hp, strong.feet.x,
                strong.feet.y, strong.feet.z, strong.yaw, strong.yaw * 180.0 / kPi);

    // Fresh runtimes per case: rounds/things of one run must not leak.
    Fixture regen_fixture;
    if (!load_fixture(regen_fixture, error)) {
        std::fprintf(stderr, "regen: regen fixture: %s\n", error.c_str());
        return 1;
    }
    test_sim_regen(regen_fixture, strong);

    Fixture caps_fixture;
    if (!load_fixture(caps_fixture, error)) {
        std::fprintf(stderr, "regen: caps fixture: %s\n", error.c_str());
        return 1;
    }
    test_sim_caps(caps_fixture, weak);

    Fixture parity_fixture;
    if (!load_fixture(parity_fixture, error)) {
        std::fprintf(stderr, "regen: parity fixture: %s\n", error.c_str());
        return 1;
    }
    test_default_parity(parity_fixture, weak);
    test_sim_negatives(fixture.chassis);

    if (g_failures != 0) {
        std::fprintf(stderr, "regen: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("regen: ok\n");
    return 0;
}
