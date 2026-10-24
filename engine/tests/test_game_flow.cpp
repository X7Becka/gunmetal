// Player death / respawn / "game over" flow (`M9.5`, slice 1;
// docs/GAME_FLOW.md).
//
// No game files: this test pins the pure module `game/game_flow.h` against the
// facts of `docs/GAME_OVER.md` and prints every number it checks.
//
//   * the exact tick arithmetic of §2.1: a death on tick 0 is `dying` for 136
//     ticks, `respawn_due()` is true on tick 137, the respawn runs for 136
//     ticks and the player is `alive` after tick 273;
//   * `respawn_due()` is a one-tick pulse: over 400 ticks the counter sees it
//     exactly once, on tick 137;
//   * the modelled life budget (MODEL: the game has no lives, §2.2): `lives`
//     2 spends one life per death, the second death ends in `restart_prompt`
//     (menu `RESTART GAME`, §0.2) with no respawn due, and `restart()` returns
//     to `alive` with the lives restored;
//   * `prompt_restart()` is the Activate-while-dead path (`0x443af9`): legal in
//     `dying`/`dead` only, it stops the flow and drops the timers;
//   * `return_to_hq()` (menu item `0x4428f0(1)`, §4.1/§4.2) resets the flow and
//     the timers from any state and restores the lives;
//   * negatives: negative `death_ticks`/`respawn_ticks`, `lives < -1` are
//     refused with a text (the previous config and flow stay intact), the
//     clamping `configure()` turns them into 0/-1 with a note, a repeated
//     `on_death()` while `dying` does not reset the timer, `on_respawn()` and
//     `prompt_restart()` outside their states are refused, and `tick()` in
//     `restart_prompt` changes nothing but the tick counter;
//   * boundaries: 0-tick death and 0-tick respawn windows;
//   * determinism: the same scripted call sequence produces the same trace.
#include "game/game_flow.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

using gm::FlowConfig;
using gm::GameFlow;
using gm::PlayerFlow;

std::string name_of(PlayerFlow state) {
    return std::string(GameFlow::state_name(state));
}

/// One printed snapshot of the flow.
void print_step(const char* what, int tick, const GameFlow& flow) {
    std::printf("game_flow: %-18s tick=%3d state=%-14s left=%3d due=%d deaths=%d "
                "respawns=%d lives=%d\n",
                what, tick, name_of(flow.state()).c_str(), flow.ticks_left(),
                flow.respawn_due() ? 1 : 0, flow.deaths(), flow.respawns(), flow.lives());
}

// --- constants and enum ----------------------------------------------------
void test_constants() {
    std::printf("game_flow: constants: death_ticks=%d respawn_ticks=%d enum "
                "alive=%d dying=%d dead=%d respawning=%d restart_prompt=%d\n",
                gm::kFlowDeathTicks, gm::kFlowRespawnTicks,
                static_cast<int>(PlayerFlow::alive), static_cast<int>(PlayerFlow::dying),
                static_cast<int>(PlayerFlow::dead),
                static_cast<int>(PlayerFlow::respawning),
                static_cast<int>(PlayerFlow::restart_prompt));
    CHECK_EQ(gm::kFlowDeathTicks, 136);
    CHECK_EQ(gm::kFlowRespawnTicks, 136);
    CHECK_EQ(static_cast<int>(PlayerFlow::alive), 0);
    CHECK_EQ(static_cast<int>(PlayerFlow::dying), 1);
    CHECK_EQ(static_cast<int>(PlayerFlow::dead), 2);
    CHECK_EQ(static_cast<int>(PlayerFlow::respawning), 3);
    CHECK_EQ(static_cast<int>(PlayerFlow::restart_prompt), 4);
    CHECK_EQ(name_of(PlayerFlow::alive), "alive");
    CHECK_EQ(name_of(PlayerFlow::dying), "dying");
    CHECK_EQ(name_of(PlayerFlow::dead), "dead");
    CHECK_EQ(name_of(PlayerFlow::respawning), "respawning");
    CHECK_EQ(name_of(PlayerFlow::restart_prompt), "restart_prompt");
    CHECK_EQ(name_of(static_cast<PlayerFlow>(9)), "unknown");

    const FlowConfig defaults;
    CHECK_EQ(defaults.death_ticks, gm::kFlowDeathTicks);
    CHECK_EQ(defaults.respawn_ticks, gm::kFlowRespawnTicks);
    CHECK(defaults.campaign);
    CHECK_EQ(defaults.lives, -1);
}

// --- the exact timeline of one death ---------------------------------------
void test_timeline() {
    GameFlow flow;
    flow.configure(FlowConfig{});
    CHECK_EQ(flow.state(), PlayerFlow::alive);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());

    flow.on_death();
    print_step("death", 0, flow);
    CHECK_EQ(flow.state(), PlayerFlow::dying);
    CHECK_EQ(flow.ticks_left(), 136);
    CHECK(!flow.respawn_due());
    CHECK_EQ(flow.deaths(), 1);
    CHECK_EQ(flow.lives(), -1);

    flow.tick();
    print_step("after tick", 1, flow);
    CHECK_EQ(flow.state(), PlayerFlow::dying);
    CHECK_EQ(flow.ticks_left(), 135);

    for (int t = 2; t <= 136; ++t) {
        flow.tick();
    }
    print_step("after tick", 136, flow);
    CHECK_EQ(flow.state(), PlayerFlow::dying);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());

    flow.tick();
    print_step("after tick", 137, flow);
    CHECK_EQ(flow.state(), PlayerFlow::dead);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(flow.respawn_due());

    flow.on_respawn();
    print_step("on_respawn", 137, flow);
    CHECK_EQ(flow.state(), PlayerFlow::respawning);
    CHECK_EQ(flow.ticks_left(), 136);
    CHECK(!flow.respawn_due());
    CHECK_EQ(flow.respawns(), 1);

    for (int t = 138; t <= 272; ++t) {
        flow.tick();
    }
    print_step("after tick", 272, flow);
    CHECK_EQ(flow.state(), PlayerFlow::respawning);
    CHECK_EQ(flow.ticks_left(), 1);

    flow.tick();
    print_step("after tick", 273, flow);
    CHECK_EQ(flow.state(), PlayerFlow::alive);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());
    CHECK_EQ(flow.ticks_elapsed(), 273u);
}

// --- respawn_due() is exactly one tick --------------------------------------
void test_due_pulse() {
    GameFlow flow;
    flow.configure(FlowConfig{});
    flow.on_death();
    int due_count = 0;
    int first_due = -1;
    int alive_tick = -1;
    for (int t = 1; t <= 400; ++t) {
        flow.tick();
        if (flow.respawn_due()) {
            ++due_count;
            first_due = t;
            flow.on_respawn();
        }
        if (flow.state() == PlayerFlow::alive && alive_tick < 0) {
            alive_tick = t;
        }
    }
    std::printf("game_flow: due pulse: count=%d first=%d alive_tick=%d over 400 ticks\n",
                due_count, first_due, alive_tick);
    CHECK_EQ(due_count, 1);
    CHECK_EQ(first_due, 137);
    CHECK_EQ(alive_tick, 273);
    CHECK_EQ(flow.state(), PlayerFlow::alive);
    CHECK_EQ(flow.respawns(), 1);

    // A second death after the respawn repeats the same arithmetic.
    flow.on_death();
    int second_due = 0;
    int second_alive = -1;
    for (int t = 1; t <= 300; ++t) {
        flow.tick();
        if (flow.respawn_due()) {
            ++second_due;
            flow.on_respawn();
        }
        if (flow.state() == PlayerFlow::alive && second_alive < 0) {
            second_alive = t;
        }
    }
    std::printf("game_flow: due pulse: second death count=%d alive_tick=+%d deaths=%d "
                "respawns=%d\n",
                second_due, second_alive, flow.deaths(), flow.respawns());
    CHECK_EQ(second_due, 1);
    CHECK_EQ(second_alive, 273);
    CHECK_EQ(flow.deaths(), 2);
    CHECK_EQ(flow.respawns(), 2);
}

// --- lives and the restart prompt (MODEL) ----------------------------------
void test_lives() {
    FlowConfig config;
    config.lives = 2;
    GameFlow flow;
    flow.configure(config);
    CHECK_EQ(flow.lives(), 2);

    flow.on_death();
    std::printf("game_flow: lives: first death lives=%d deaths=%d\n", flow.lives(),
                flow.deaths());
    CHECK_EQ(flow.lives(), 1);
    int due_count = 0;
    for (int t = 1; t <= 300; ++t) {
        flow.tick();
        if (flow.respawn_due()) {
            ++due_count;
            flow.on_respawn();
        }
    }
    CHECK_EQ(due_count, 1);
    CHECK_EQ(flow.state(), PlayerFlow::alive);
    CHECK_EQ(flow.lives(), 1);

    flow.on_death();
    CHECK_EQ(flow.lives(), 0);
    int due_second = 0;
    int prompt_tick = -1;
    for (int t = 1; t <= 200; ++t) {
        flow.tick();
        if (flow.respawn_due()) {
            ++due_second;
        }
        if (flow.state() == PlayerFlow::restart_prompt && prompt_tick < 0) {
            prompt_tick = t;
        }
    }
    std::printf("game_flow: lives: second death due=%d restart_prompt_tick=%d "
                "state=%s left=%d lives=%d\n",
                due_second, prompt_tick, name_of(flow.state()).c_str(), flow.ticks_left(),
                flow.lives());
    CHECK_EQ(due_second, 0);
    CHECK_EQ(prompt_tick, 137);
    CHECK_EQ(flow.state(), PlayerFlow::restart_prompt);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());
    CHECK_EQ(flow.lives(), 0);
    CHECK_EQ(flow.respawns(), 1);

    flow.restart();
    std::printf("game_flow: lives: restart -> state=%s lives=%d restarts=%d left=%d due=%d\n",
                name_of(flow.state()).c_str(), flow.lives(), flow.restarts(),
                flow.ticks_left(), flow.respawn_due() ? 1 : 0);
    CHECK_EQ(flow.state(), PlayerFlow::alive);
    CHECK_EQ(flow.lives(), 2);
    CHECK_EQ(flow.restarts(), 1);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());

    // `lives = 0` from the start: the very first death ends at the prompt.
    FlowConfig zero;
    zero.lives = 0;
    GameFlow no_lives;
    no_lives.configure(zero);
    no_lives.on_death();
    for (int t = 1; t <= 137; ++t) {
        no_lives.tick();
    }
    std::printf("game_flow: lives=0: first death state=%s lives=%d due=%d\n",
                name_of(no_lives.state()).c_str(), no_lives.lives(),
                no_lives.respawn_due() ? 1 : 0);
    CHECK_EQ(no_lives.state(), PlayerFlow::restart_prompt);
    CHECK_EQ(no_lives.lives(), 0);
    CHECK(!no_lives.respawn_due());

    // MODEL: `campaign = false` ignores the life budget entirely.
    FlowConfig dm;
    dm.campaign = false;
    dm.lives = 1;
    GameFlow dm_flow;
    dm_flow.configure(dm);
    dm_flow.on_death();
    CHECK_EQ(dm_flow.lives(), 1);
    for (int t = 1; t <= 300; ++t) {
        dm_flow.tick();
        if (dm_flow.respawn_due()) {
            dm_flow.on_respawn();
        }
    }
    std::printf("game_flow: campaign=0 lives=1: after death state=%s lives=%d\n",
                name_of(dm_flow.state()).c_str(), dm_flow.lives());
    CHECK_EQ(dm_flow.state(), PlayerFlow::alive);
    CHECK_EQ(dm_flow.lives(), 1);
}

// --- prompt_restart(): the Activate-while-dead path ------------------------
void test_prompt_restart() {
    GameFlow flow;
    flow.configure(FlowConfig{});
    CHECK(!flow.prompt_restart());
    std::printf("game_flow: prompt_restart(alive)=0 error=\"%s\"\n",
                flow.last_error().c_str());
    CHECK(!flow.last_error().empty());
    CHECK_EQ(flow.state(), PlayerFlow::alive);

    flow.on_death();
    for (int t = 1; t <= 10; ++t) {
        flow.tick();
    }
    CHECK_EQ(flow.ticks_left(), 126);
    CHECK(flow.prompt_restart());
    std::printf("game_flow: prompt_restart(dying left=126)=1 state=%s left=%d due=%d\n",
                name_of(flow.state()).c_str(), flow.ticks_left(),
                flow.respawn_due() ? 1 : 0);
    CHECK_EQ(flow.state(), PlayerFlow::restart_prompt);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());
    CHECK(flow.last_error().empty());

    const std::uint64_t before = flow.ticks_elapsed();
    for (int t = 0; t < 50; ++t) {
        flow.tick();
    }
    std::printf("game_flow: prompt_restart: 50 ticks -> state=%s ticks_elapsed=%llu\n",
                name_of(flow.state()).c_str(),
                static_cast<unsigned long long>(flow.ticks_elapsed()));
    CHECK_EQ(flow.state(), PlayerFlow::restart_prompt);
    CHECK_EQ(flow.ticks_elapsed(), before + 50);
    CHECK(!flow.respawn_due());
    CHECK(!flow.prompt_restart());
    CHECK(!flow.last_error().empty());

    flow.restart();
    CHECK_EQ(flow.state(), PlayerFlow::alive);

    // From `dead` (between the due tick and `on_respawn()`) the prompt is legal.
    flow.on_death();
    for (int t = 1; t <= 137; ++t) {
        flow.tick();
    }
    CHECK_EQ(flow.state(), PlayerFlow::dead);
    CHECK(flow.respawn_due());
    CHECK(flow.prompt_restart());
    std::printf("game_flow: prompt_restart(dead)=1 state=%s due=%d\n",
                name_of(flow.state()).c_str(), flow.respawn_due() ? 1 : 0);
    CHECK_EQ(flow.state(), PlayerFlow::restart_prompt);
    CHECK(!flow.respawn_due());

    // From `respawning` it is refused (the player is already coming back).
    flow.restart();
    flow.on_death();
    for (int t = 1; t <= 137; ++t) {
        flow.tick();
    }
    flow.on_respawn();
    CHECK_EQ(flow.state(), PlayerFlow::respawning);
    CHECK(!flow.prompt_restart());
    std::printf("game_flow: prompt_restart(respawning)=0 error=\"%s\"\n",
                flow.last_error().c_str());
    CHECK(!flow.last_error().empty());
    CHECK_EQ(flow.state(), PlayerFlow::respawning);
}

// --- return_to_hq() -------------------------------------------------------
void test_return_to_hq() {
    FlowConfig config;
    config.lives = 1;
    GameFlow flow;
    flow.configure(config);
    flow.on_death();
    CHECK_EQ(flow.lives(), 0);
    for (int t = 1; t <= 137; ++t) {
        flow.tick();
    }
    CHECK_EQ(flow.state(), PlayerFlow::restart_prompt);
    flow.return_to_hq();
    std::printf("game_flow: return_to_hq(prompt) -> state=%s lives=%d left=%d due=%d "
                "hq_returns=%d restarts=%d\n",
                name_of(flow.state()).c_str(), flow.lives(), flow.ticks_left(),
                flow.respawn_due() ? 1 : 0, flow.hq_returns(), flow.restarts());
    CHECK_EQ(flow.state(), PlayerFlow::alive);
    CHECK_EQ(flow.lives(), 1);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());
    CHECK_EQ(flow.hq_returns(), 1);
    CHECK_EQ(flow.restarts(), 0);

    // From the middle of a respawn: the timers are dropped too.
    flow.configure(FlowConfig{}); // unlimited lives: the respawn really starts
    flow.on_death();
    for (int t = 1; t <= 137; ++t) {
        flow.tick();
    }
    flow.on_respawn();
    flow.tick();
    CHECK_EQ(flow.state(), PlayerFlow::respawning);
    flow.return_to_hq();
    std::printf("game_flow: return_to_hq(respawning) -> state=%s left=%d due=%d "
                "lives=%d hq_returns=%d\n",
                name_of(flow.state()).c_str(), flow.ticks_left(),
                flow.respawn_due() ? 1 : 0, flow.lives(), flow.hq_returns());
    CHECK_EQ(flow.state(), PlayerFlow::alive);
    CHECK_EQ(flow.ticks_left(), 0);
    CHECK(!flow.respawn_due());
    CHECK_EQ(flow.lives(), -1);
    CHECK_EQ(flow.hq_returns(), 1);
}

// --- negatives and clamps --------------------------------------------------
void test_negatives() {
    FlowConfig good;
    good.death_ticks = 10;
    good.respawn_ticks = 5;
    good.lives = 3;
    GameFlow flow;
    std::string error;
    CHECK(flow.configure(good, &error));
    CHECK(error.empty());
    CHECK_EQ(flow.config().death_ticks, 10);
    CHECK_EQ(flow.lives(), 3);
    flow.on_death();
    flow.tick();
    flow.tick();
    CHECK_EQ(flow.ticks_left(), 8);
    CHECK_EQ(flow.deaths(), 1);

    FlowConfig bad = good;
    bad.death_ticks = -1;
    error.clear();
    CHECK(!flow.configure(bad, &error));
    std::printf("game_flow: configure(death_ticks=-1)=0 error=\"%s\"\n", error.c_str());
    CHECK(!error.empty());
    CHECK_EQ(flow.config().death_ticks, 10);
    CHECK_EQ(flow.config().respawn_ticks, 5);
    CHECK_EQ(flow.state(), PlayerFlow::dying);
    CHECK_EQ(flow.ticks_left(), 8);
    CHECK_EQ(flow.ticks_elapsed(), 2u);
    CHECK_EQ(flow.deaths(), 1);

    bad = good;
    bad.respawn_ticks = -1;
    error.clear();
    CHECK(!flow.configure(bad, &error));
    std::printf("game_flow: configure(respawn_ticks=-1)=0 error=\"%s\"\n", error.c_str());
    CHECK(!error.empty());
    CHECK_EQ(flow.config().respawn_ticks, 5);

    bad = good;
    bad.lives = -2;
    error.clear();
    CHECK(!flow.configure(bad, &error));
    std::printf("game_flow: configure(lives=-2)=0 error=\"%s\"\n", error.c_str());
    CHECK(!error.empty());
    CHECK_EQ(flow.lives(), 2); // the death above spent one of the three
    CHECK_EQ(flow.config().lives, 3);

    // The clamping form accepts them and records a note.
    FlowConfig clamp;
    clamp.death_ticks = -5;
    clamp.respawn_ticks = -1;
    clamp.lives = -3;
    GameFlow clamped;
    clamped.configure(clamp);
    std::printf("game_flow: configure(clamp) death_ticks=%d respawn_ticks=%d lives=%d "
                "error=\"%s\"\n",
                clamped.config().death_ticks, clamped.config().respawn_ticks, clamped.lives(),
                clamped.last_error().c_str());
    CHECK_EQ(clamped.config().death_ticks, 0);
    CHECK_EQ(clamped.config().respawn_ticks, 0);
    CHECK_EQ(clamped.lives(), -1);
    CHECK(clamped.last_error().find("clamped") != std::string::npos);

    // A repeated on_death() while dying is a no-op and keeps the timer.
    GameFlow repeat;
    repeat.configure(FlowConfig{});
    repeat.on_death();
    for (int t = 1; t <= 50; ++t) {
        repeat.tick();
    }
    const int left_before = repeat.ticks_left();
    repeat.on_death();
    std::printf("game_flow: on_death twice: left %d -> %d deaths=%d error=\"%s\"\n",
                left_before, repeat.ticks_left(), repeat.deaths(),
                repeat.last_error().c_str());
    CHECK_EQ(repeat.ticks_left(), left_before);
    CHECK_EQ(repeat.deaths(), 1);
    CHECK(!repeat.last_error().empty());
    CHECK_EQ(repeat.state(), PlayerFlow::dying);

    // on_death() in `dead`/`restart_prompt` and on_respawn() outside `dead`.
    for (int t = 0; t < left_before + 1; ++t) {
        repeat.tick();
    }
    CHECK_EQ(repeat.state(), PlayerFlow::dead);
    repeat.on_death();
    std::printf("game_flow: on_death(dead) error=\"%s\"\n", repeat.last_error().c_str());
    CHECK(!repeat.last_error().empty());
    CHECK_EQ(repeat.state(), PlayerFlow::dead);
    CHECK_EQ(repeat.deaths(), 1);

    GameFlow fresh;
    fresh.configure(FlowConfig{});
    fresh.on_respawn();
    std::printf("game_flow: on_respawn(alive) error=\"%s\"\n", fresh.last_error().c_str());
    CHECK(!fresh.last_error().empty());
    CHECK_EQ(fresh.state(), PlayerFlow::alive);
    CHECK_EQ(fresh.respawns(), 0);

    FlowConfig none;
    none.lives = 1;
    GameFlow prompt;
    prompt.configure(none);
    prompt.on_death();
    for (int t = 1; t <= 137; ++t) {
        prompt.tick();
    }
    CHECK_EQ(prompt.state(), PlayerFlow::restart_prompt);
    prompt.on_death();
    std::printf("game_flow: on_death(restart_prompt) error=\"%s\" state=%s\n",
                prompt.last_error().c_str(), name_of(prompt.state()).c_str());
    CHECK(!prompt.last_error().empty());
    CHECK_EQ(prompt.state(), PlayerFlow::restart_prompt);

    // tick() in `restart_prompt`: no transition, only the tick counter moves.
    const int left = prompt.ticks_left();
    const int deaths = prompt.deaths();
    const std::uint64_t elapsed = prompt.ticks_elapsed();
    for (int t = 0; t < 100; ++t) {
        prompt.tick();
    }
    std::printf("game_flow: 100 ticks in restart_prompt: state=%s left=%d deaths=%d "
                "elapsed=%llu\n",
                name_of(prompt.state()).c_str(), prompt.ticks_left(), prompt.deaths(),
                static_cast<unsigned long long>(prompt.ticks_elapsed()));
    CHECK_EQ(prompt.state(), PlayerFlow::restart_prompt);
    CHECK_EQ(prompt.ticks_left(), left);
    CHECK_EQ(prompt.deaths(), deaths);
    CHECK_EQ(prompt.ticks_elapsed(), elapsed + 100);
    CHECK(!prompt.respawn_due());
}

// --- boundaries: zero-length timers ---------------------------------------
void test_boundaries() {
    FlowConfig zero_death;
    zero_death.death_ticks = 0;
    GameFlow a;
    a.configure(zero_death);
    a.on_death();
    a.tick();
    std::printf("game_flow: death_ticks=0: after 1 tick state=%s due=%d\n",
                name_of(a.state()).c_str(), a.respawn_due() ? 1 : 0);
    CHECK_EQ(a.state(), PlayerFlow::dead);
    CHECK(a.respawn_due());

    FlowConfig zero_respawn;
    zero_respawn.respawn_ticks = 0;
    GameFlow b;
    b.configure(zero_respawn);
    b.on_death();
    for (int t = 1; t <= 137; ++t) {
        b.tick();
    }
    CHECK(b.respawn_due());
    b.on_respawn();
    CHECK_EQ(b.state(), PlayerFlow::respawning);
    CHECK_EQ(b.ticks_left(), 0);
    b.tick();
    std::printf("game_flow: respawn_ticks=0: after on_respawn + 1 tick state=%s\n",
                name_of(b.state()).c_str());
    CHECK_EQ(b.state(), PlayerFlow::alive);

    FlowConfig one_tick;
    one_tick.death_ticks = 1;
    GameFlow c;
    c.configure(one_tick);
    c.on_death();
    c.tick();
    CHECK_EQ(c.state(), PlayerFlow::dying);
    CHECK_EQ(c.ticks_left(), 0);
    c.tick();
    std::printf("game_flow: death_ticks=1: due after tick 2 = %d\n",
                c.respawn_due() ? 1 : 0);
    CHECK(c.respawn_due());
}

// --- determinism ----------------------------------------------------------
std::string run_script() {
    FlowConfig config;
    config.lives = 2;
    GameFlow flow;
    flow.configure(config);
    std::string trace;
    auto record = [&flow, &trace] {
        char cell[64];
        std::snprintf(cell, sizeof(cell), "%d/%d/%d;", static_cast<int>(flow.state()),
                      flow.ticks_left(), flow.respawn_due() ? 1 : 0);
        trace += cell;
    };
    flow.on_death();
    record();
    for (int t = 0; t < 300; ++t) {
        flow.tick();
        if (flow.respawn_due()) {
            flow.on_respawn();
        }
        record();
    }
    flow.on_death();
    record();
    for (int t = 0; t < 200; ++t) {
        flow.tick();
        record();
    }
    return trace;
}

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void test_determinism() {
    const std::string first = run_script();
    const std::string second = run_script();
    const std::uint64_t a = fnv1a64(first);
    const std::uint64_t b = fnv1a64(second);
    std::printf("game_flow: determinism: len=%zu hash1=%016llx hash2=%016llx equal=%d\n",
                first.size(), static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b), first == second ? 1 : 0);
    CHECK_EQ(first, second);
    CHECK_EQ(a, b);
    CHECK(!first.empty());
}

} // namespace

int main() {
    test_constants();
    test_timeline();
    test_due_pulse();
    test_lives();
    test_prompt_restart();
    test_return_to_hq();
    test_negatives();
    test_boundaries();
    test_determinism();

    if (g_failures != 0) {
        std::fprintf(stderr, "game_flow: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("game_flow: ok (%d checks)\n", g_checks);
    return 0;
}
