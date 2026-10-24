// Deathmatch match state (`M9.2`, slice 2; docs/DM_STATE.md).
//
// No game files: this test pins the pure module `game/dm_state.h` against the
// facts of `docs/DM_RULES.md` and prints every number it checks.
//
//   * modes and flags: `game_mode` 0..3 (§2.1), the six `mp_flags` bits
//     (§2.2), `is_friendly` in team and CTF modes (§3.2/§6);
//   * one kill (`player_killed` 0x4536e0, §3.2): `frags[killer] += 1`, the
//     victim's frags do **not** change, deaths += 1, the respawn timer starts
//     at 136 (§4.3) and reaches 0 exactly after 136 ticks;
//   * suicide/world kill (`killer == victim` or an outside slot): the victim
//     loses a frag only when it had one (`0x453875`);
//   * team kill: the killer (not the victim) loses a frag, and only while the
//     victim had one (`0x4537f5`/`0x4537fd`); a killer's frags are not clamped;
//   * frag limit: the match ends exactly on the reaching kill, the winner is
//     that player, an equal score is a draw (via the time limit, because the
//     first player to reach the limit ends the match);
//   * time limit: 0 disables the clock, a set limit runs out exactly at N
//     ticks and freezes the match clock afterwards;
//   * the handicap ladder (§5) on all 18 boundary points of
//     `tools/re/dm_rules.py`;
//   * negatives: negative limits/delay/handicap and unknown flags are refused
//     with a text by the validating `configure()` and clamped with a note by
//     the plain one; a ninth player, an out-of-range victim, a kill on a dead
//     victim and a kill after the end are refused with a text.
#include "game/dm_state.h"

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

using gm::DmFlag;
using gm::DmMode;
using gm::DmSettings;
using gm::DmState;

constexpr std::uint32_t kTeam = gm::dm_flag_bit(DmFlag::team);
constexpr std::uint32_t kCtf = gm::dm_flag_bit(DmFlag::ctf);

std::string name_of(DmMode mode) {
    return std::string(DmState::mode_name(mode));
}

/// Print every player of `state` as `slot:frags/deaths/score/respawn/alive`.
std::string players_line(const DmState& state) {
    std::string line;
    for (int i = 0; i < state.player_count(); ++i) {
        const gm::DmPlayer* p = state.player(i);
        char cell[96];
        std::snprintf(cell, sizeof(cell), "%s%d:%d/%d/%d/%d/%d", i == 0 ? "" : " ", i,
                      p->frags, p->deaths, p->score, p->respawn_ticks, p->alive ? 1 : 0);
        line += cell;
    }
    return line;
}

// --- modes and flags -------------------------------------------------------
void test_modes_and_flags() {
    std::printf("dm_state: modes: campaign=%d normal=%d classic=%d hunt=%d\n",
                static_cast<int>(DmMode::campaign), static_cast<int>(DmMode::normal),
                static_cast<int>(DmMode::classic), static_cast<int>(DmMode::hunt));
    CHECK_EQ(name_of(DmMode::campaign), "campaign");
    CHECK_EQ(name_of(DmMode::normal), "normal");
    CHECK_EQ(name_of(DmMode::classic), "classic");
    CHECK_EQ(name_of(DmMode::hunt), "hunt");
    CHECK_EQ(name_of(static_cast<DmMode>(4)), "unknown");
    CHECK_EQ(static_cast<int>(DmMode::campaign), 0);
    CHECK_EQ(static_cast<int>(DmMode::hunt), 3);

    std::printf("dm_state: flags: team=0x%02x ctf=0x%02x no_monsters=0x%02x freeze=0x%02x "
                "no_buy=0x%02x handicap=0x%02x known=0x%02x\n",
                gm::dm_flag_bit(DmFlag::team), gm::dm_flag_bit(DmFlag::ctf),
                gm::dm_flag_bit(DmFlag::no_monsters), gm::dm_flag_bit(DmFlag::freeze),
                gm::dm_flag_bit(DmFlag::no_buy), gm::dm_flag_bit(DmFlag::handicap),
                gm::kDmKnownFlags);
    CHECK_EQ(gm::dm_flag_bit(DmFlag::team), 0x01u);
    CHECK_EQ(gm::dm_flag_bit(DmFlag::ctf), 0x02u);
    CHECK_EQ(gm::dm_flag_bit(DmFlag::no_monsters), 0x04u);
    CHECK_EQ(gm::dm_flag_bit(DmFlag::freeze), 0x08u);
    CHECK_EQ(gm::dm_flag_bit(DmFlag::no_buy), 0x10u);
    CHECK_EQ(gm::dm_flag_bit(DmFlag::handicap), 0x20u);
    CHECK_EQ(gm::kDmKnownFlags, 0x3Fu);

    const std::uint32_t all = gm::kDmKnownFlags;
    CHECK(gm::dm_has_flag(all, DmFlag::team));
    CHECK(gm::dm_has_flag(all, DmFlag::ctf));
    CHECK(gm::dm_has_flag(all, DmFlag::no_monsters));
    CHECK(gm::dm_has_flag(all, DmFlag::freeze));
    CHECK(gm::dm_has_flag(all, DmFlag::no_buy));
    CHECK(gm::dm_has_flag(all, DmFlag::handicap));
    CHECK(!gm::dm_has_flag(0, DmFlag::team));
    CHECK(!gm::dm_has_flag(kCtf, DmFlag::team));
}

// --- friendliness ----------------------------------------------------------
void test_friendly() {
    std::printf("dm_state: friendly: default respawn=136 max_players=%d\n",
                DmState::kMaxPlayers);
    CHECK_EQ(gm::kDmRespawnTicks, 136);

    DmState team;
    DmSettings settings;
    settings.mode = DmMode::normal;
    settings.flags = kTeam;
    team.configure(settings);
    CHECK_EQ(team.add_player(0), 0);
    CHECK_EQ(team.add_player(0), 1);
    CHECK_EQ(team.add_player(1), 2);
    CHECK_EQ(team.add_player(1), 3);
    CHECK_EQ(team.player_count(), 4);
    const bool t01 = team.is_friendly(0, 1);
    const bool t02 = team.is_friendly(0, 2);
    const bool t23 = team.is_friendly(2, 3);
    const bool t00 = team.is_friendly(0, 0);
    const bool t0x = team.is_friendly(0, 99);
    std::printf("dm_state: friendly(team): (0,1)=%d (0,2)=%d (2,3)=%d (0,0)=%d (0,99)=%d\n",
                t01 ? 1 : 0, t02 ? 1 : 0, t23 ? 1 : 0, t00 ? 1 : 0, t0x ? 1 : 0);
    CHECK(t01);
    CHECK(!t02);
    CHECK(t23);
    CHECK(!t00); // a self pair is not "friendly"; self-kill is its own rule
    CHECK(!t0x);

    // The original tests `mp_flags & 1` only (§3.2); applying it to CTF is the
    // documented model.
    DmState ctf;
    settings.flags = kCtf;
    ctf.configure(settings);
    ctf.add_player(0);
    ctf.add_player(0);
    ctf.add_player(1);
    const bool c01 = ctf.is_friendly(0, 1);
    const bool c02 = ctf.is_friendly(0, 2);
    std::printf("dm_state: friendly(ctf): (0,1)=%d (0,2)=%d\n", c01 ? 1 : 0, c02 ? 1 : 0);
    CHECK(c01);
    CHECK(!c02);
    CHECK(!ctf.is_friendly(0, 0));

    // No team flag: nobody is friendly, even with equal team ids.
    DmState plain;
    settings.flags = 0;
    plain.configure(settings);
    plain.add_player(0);
    plain.add_player(0);
    CHECK(!plain.is_friendly(0, 1));
}

// --- one kill and the respawn timer ---------------------------------------
void test_kill_and_respawn() {
    DmState state;
    DmSettings settings;
    settings.mode = DmMode::normal;
    state.configure(settings);
    state.add_player(0);
    state.add_player(1);
    CHECK(!state.finished());

    state.on_kill(1, 0);
    const gm::DmPlayer* killer = state.player(0);
    const gm::DmPlayer* victim = state.player(1);
    std::printf("dm_state: kill: %s | killer_frags=%d victim_frags=%d victim_deaths=%d "
                "victim_respawn=%d victim_alive=%d respawn_left(0)=%d respawn_left(1)=%d\n",
                players_line(state).c_str(), killer->frags, victim->frags, victim->deaths,
                victim->respawn_ticks, victim->alive ? 1 : 0, state.respawn_left(0),
                state.respawn_left(1));
    CHECK_EQ(killer->frags, 1);
    CHECK_EQ(killer->score, 1);
    CHECK_EQ(victim->frags, 0); // §3.2: a valid kill does not subtract from the victim
    CHECK_EQ(victim->deaths, 1);
    CHECK_EQ(victim->respawn_ticks, gm::kDmRespawnTicks);
    CHECK(!victim->alive);
    CHECK_EQ(state.respawn_left(0), 0);
    CHECK_EQ(state.respawn_left(1), 136);
    CHECK_EQ(state.respawn_left(-1), -1);
    CHECK_EQ(state.respawn_left(2), -1);

    state.tick();
    std::printf("dm_state: after 1 tick: left=%d alive=%d elapsed=%llu\n",
                state.respawn_left(1), state.player(1)->alive ? 1 : 0,
                static_cast<unsigned long long>(state.ticks_elapsed()));
    CHECK_EQ(state.respawn_left(1), 135);
    CHECK(!state.player(1)->alive);
    CHECK_EQ(state.ticks_elapsed(), 1u);

    state.tick(134);
    CHECK_EQ(state.respawn_left(1), 1);
    CHECK(!state.player(1)->alive);
    state.tick();
    std::printf("dm_state: after 136 ticks: left=%d alive=%d elapsed=%llu\n",
                state.respawn_left(1), state.player(1)->alive ? 1 : 0,
                static_cast<unsigned long long>(state.ticks_elapsed()));
    CHECK_EQ(state.respawn_left(1), 0);
    CHECK(state.player(1)->alive);
    CHECK_EQ(state.ticks_elapsed(), 136u);

    // A kill on a dead victim is ignored (`0x453700`): kill again, then try a
    // second kill while the victim is still dead.
    state.on_kill(1, 0);
    CHECK_EQ(state.player(0)->frags, 2);
    CHECK(!state.player(1)->alive);
    state.on_kill(1, 0);
    CHECK_EQ(killer->frags, 2);
    CHECK(!state.last_error().empty());
    std::printf("dm_state: kill on dead victim: error=\"%s\" killer_frags=%d\n",
                state.last_error().c_str(), killer->frags);
    CHECK_EQ(state.player(1)->deaths, 2);
    std::printf("dm_state: state after the kills: %s\n", players_line(state).c_str());
}

// --- suicide and world kills ---------------------------------------------
void test_self_and_world() {
    DmState state;
    DmSettings settings;
    settings.mode = DmMode::classic;
    settings.respawn_delay_ticks = 0; // instant respawn keeps the kills coming
    state.configure(settings);
    state.add_player(0);
    state.add_player(1);
    CHECK_EQ(state.player(0)->frags, 0);

    state.on_kill(0, 0); // self-kill without a frag: nothing to take
    std::printf("dm_state: self-kill at 0 frags: %s\n", players_line(state).c_str());
    CHECK_EQ(state.player(0)->frags, 0); // `0x453875` guard: frags > 0
    CHECK_EQ(state.player(0)->deaths, 1);
    CHECK(state.player(0)->alive); // zero delay: instant respawn
    CHECK_EQ(state.player(0)->score, 0);

    state.on_kill(1, 0); // victim 1 dies, killer 0 gets the frag
    state.on_kill(0, 0); // now killer 0 self-kills: -1
    std::printf("dm_state: self-kill at 1 frag: %s\n", players_line(state).c_str());
    CHECK_EQ(state.player(0)->frags, 0);
    CHECK_EQ(state.player(0)->deaths, 2);

    state.on_kill(1, 0); // frag 1 again
    state.on_kill(0, 0);
    CHECK_EQ(state.player(0)->frags, 0);
    CHECK_EQ(state.player(0)->deaths, 3);

    // A world kill (`killer` outside the player list) behaves like a suicide.
    state.on_kill(1, 0);
    CHECK_EQ(state.player(0)->frags, 1);
    state.on_kill(1, 99);
    std::printf("dm_state: world kill (killer=99): %s\n", players_line(state).c_str());
    CHECK_EQ(state.player(1)->frags, 0); // had none: nothing to take
    CHECK_EQ(state.player(1)->deaths, 4); // three kills above + this world kill
    CHECK_EQ(state.player(0)->frags, 1); // untouched
}

// --- team kills ------------------------------------------------------------
void test_team_kill() {
    DmState state;
    DmSettings settings;
    settings.mode = DmMode::normal;
    settings.flags = kTeam;
    settings.respawn_delay_ticks = 0;
    state.configure(settings);
    state.add_player(0); // team 0
    state.add_player(1); // team 1
    state.add_player(0); // team 0
    state.add_player(1); // team 1

    state.on_kill(1, 0);
    state.on_kill(1, 0);
    state.on_kill(1, 2);
    state.on_kill(1, 2);
    std::printf("dm_state: team setup: %s\n", players_line(state).c_str());
    CHECK_EQ(state.player(0)->frags, 2);
    CHECK_EQ(state.player(2)->frags, 2);

    // Victim 0 has frags: the killer (2, same team) pays one, the victim keeps.
    state.on_kill(0, 2);
    std::printf("dm_state: team kill (victim 2 frags, killer 2): %s\n",
                players_line(state).c_str());
    CHECK_EQ(state.player(0)->frags, 2); // victim keeps its frags
    CHECK_EQ(state.player(0)->deaths, 1);
    CHECK_EQ(state.player(2)->frags, 1); // the killer pays
    state.on_kill(0, 2);
    CHECK_EQ(state.player(2)->frags, 0);
    CHECK_EQ(state.player(0)->frags, 2);
    // The original has no clamp on the killer (`0x4537fd`), only the victim's
    // frags are tested (`0x4537f5`); modelled as is.
    state.on_kill(0, 2);
    std::printf("dm_state: third team kill: %s\n", players_line(state).c_str());
    CHECK_EQ(state.player(2)->frags, -1);
    CHECK_EQ(state.player(2)->score, -1);

    // Victim without a frag: a team kill is free (the guard tests the victim).
    state.on_kill(1, 3);
    CHECK_EQ(state.player(3)->frags, 0);
    state.on_kill(1, 3); // victim 1 still has 0 frags: no penalty at all
    std::printf("dm_state: team kill with a 0-frag victim: %s\n",
                players_line(state).c_str());
    CHECK_EQ(state.player(3)->frags, 0);
    CHECK_EQ(state.player(1)->frags, 0);
    CHECK_EQ(state.player(1)->deaths, 6); // four setup kills + two more here
}

// --- frag limit ------------------------------------------------------------
void test_frag_limit() {
    DmState state;
    DmSettings settings;
    settings.mode = DmMode::normal;
    settings.frag_limit = 3;
    settings.respawn_delay_ticks = 0;
    state.configure(settings);
    state.add_player(0);
    state.add_player(1);

    state.on_kill(1, 0);
    state.on_kill(1, 0);
    std::printf("dm_state: frag limit 3 after 2 kills: frags=%d finished=%d winner=%d\n",
                state.player(0)->frags, state.finished() ? 1 : 0, state.winner());
    CHECK_EQ(state.player(0)->frags, 2);
    CHECK(!state.finished());
    CHECK_EQ(state.winner(), -1);

    state.on_kill(1, 0);
    std::printf("dm_state: frag limit 3 on the 3rd kill: frags=%d finished=%d winner=%d "
                "score=%d time_left=%d\n",
                state.player(0)->frags, state.finished() ? 1 : 0, state.winner(),
                state.player(0)->score, state.time_left());
    CHECK_EQ(state.player(0)->frags, 3);
    CHECK(state.finished());
    CHECK_EQ(state.winner(), 0);
    CHECK_EQ(state.player(0)->score, 3);

    // A kill after the end is refused with a text.
    state.on_kill(0, 1);
    CHECK_EQ(state.player(1)->frags, 0);
    CHECK(!state.last_error().empty());
    std::printf("dm_state: kill after the end: error=\"%s\"\n",
                state.last_error().c_str());

    // The match clock stops with the match.
    const std::uint64_t before = state.ticks_elapsed();
    state.tick(10);
    CHECK_EQ(state.ticks_elapsed(), before);

    // A draw is reachable through the time limit: both players have 1 frag.
    DmState draw;
    DmSettings timed;
    timed.mode = DmMode::normal;
    timed.time_limit_ticks = 1;
    timed.respawn_delay_ticks = 0;
    draw.configure(timed);
    draw.add_player(0);
    draw.add_player(1);
    draw.on_kill(1, 0);
    draw.on_kill(0, 1);
    draw.tick();
    std::printf("dm_state: draw: %s finished=%d winner=%d\n", players_line(draw).c_str(),
                draw.finished() ? 1 : 0, draw.winner());
    CHECK(draw.finished());
    CHECK_EQ(draw.player(0)->frags, 1);
    CHECK_EQ(draw.player(1)->frags, 1);
    CHECK_EQ(draw.winner(), -1);
}

// --- time limit ------------------------------------------------------------
void test_time_limit() {
    DmState state;
    DmSettings settings;
    settings.mode = DmMode::hunt;
    settings.time_limit_ticks = 5;
    state.configure(settings);
    state.add_player(0);
    CHECK_EQ(state.time_left(), 5);
    CHECK(!state.finished());

    state.tick(4);
    std::printf("dm_state: time limit 5 after 4 ticks: left=%d finished=%d elapsed=%llu\n",
                state.time_left(), state.finished() ? 1 : 0,
                static_cast<unsigned long long>(state.ticks_elapsed()));
    CHECK_EQ(state.time_left(), 1);
    CHECK(!state.finished());

    state.tick();
    std::printf("dm_state: time limit 5 after 5 ticks: left=%d finished=%d winner=%d "
                "elapsed=%llu\n",
                state.time_left(), state.finished() ? 1 : 0, state.winner(),
                static_cast<unsigned long long>(state.ticks_elapsed()));
    CHECK_EQ(state.time_left(), 0);
    CHECK(state.finished());
    CHECK_EQ(state.winner(), 0); // the only player has the most frags (0)
    CHECK_EQ(state.ticks_elapsed(), 5u);

    state.tick(100);
    CHECK_EQ(state.ticks_elapsed(), 5u);
    CHECK_EQ(state.time_left(), 0);

    // 0 = no match clock at all (the shipped DM, §3.4).
    DmState endless;
    DmSettings none;
    none.mode = DmMode::normal;
    endless.configure(none);
    endless.add_player(0);
    endless.tick(1000);
    std::printf("dm_state: time limit 0 after 1000 ticks: left=%d finished=%d elapsed=%llu\n",
                endless.time_left(), endless.finished() ? 1 : 0,
                static_cast<unsigned long long>(endless.ticks_elapsed()));
    CHECK_EQ(endless.time_left(), 0);
    CHECK(!endless.finished());
    CHECK_EQ(endless.ticks_elapsed(), 1000u);

    // A zero respawn delay respawns on the death tick itself.
    DmState instant;
    DmSettings quick;
    quick.respawn_delay_ticks = 0;
    instant.configure(quick);
    instant.add_player(0);
    instant.add_player(1);
    instant.on_kill(1, 0);
    CHECK(instant.player(1)->alive);
    CHECK_EQ(instant.player(1)->respawn_ticks, 0);
    CHECK_EQ(instant.respawn_left(1), 0);
}

// --- handicap ladder -------------------------------------------------------
void test_handicap() {
    struct Point {
        int diff;
        int percent;
    };
    // The 18 boundary points of `tools/re/dm_rules.py` (self-test 5).
    const Point table[] = {
        {51, 500}, {50, 400}, {31, 400}, {30, 300}, {26, 300}, {25, 200},
        {21, 200}, {20, 100}, {16, 100}, {15, 50},  {11, 50},  {10, 25},
        {8, 25},   {7, 10},   {5, 10},   {4, 0},     {0, 0},    {-3, 0},
    };
    int bad = 0;
    std::printf("dm_state: handicap:");
    for (const Point& point : table) {
        const int got = gm::dm_handicap_percent(point.diff);
        std::printf(" %d->%d", point.diff, got);
        if (got != point.percent) {
            ++bad;
        }
        CHECK_EQ(got, point.percent);
    }
    std::printf(" (0 mismatches=%d)\n", bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(gm::dm_handicap_percent(1000), 500);
    CHECK_EQ(gm::dm_handicap_percent(-1000), 0);
}

// --- negatives -------------------------------------------------------------
void test_negatives() {
    DmState state;
    DmSettings good;
    good.mode = DmMode::normal;
    CHECK(state.configure(good, nullptr));

    // The validating form refuses the whole record and keeps the old one.
    DmSettings bad = good;
    bad.frag_limit = -1;
    std::string error;
    const bool accepted = state.configure(bad, &error);
    std::printf("dm_state: configure(frag_limit=-1): accepted=%d error=\"%s\" kept_limit=%d\n",
                accepted ? 1 : 0, error.c_str(), state.settings().frag_limit);
    CHECK(!accepted);
    CHECK(!error.empty());
    CHECK_EQ(state.settings().frag_limit, 0);
    CHECK_EQ(state.settings().time_limit_ticks, 0);

    bad = good;
    bad.time_limit_ticks = -5;
    CHECK(!state.configure(bad, &error));
    CHECK(!error.empty());
    bad = good;
    bad.respawn_delay_ticks = -136;
    CHECK(!state.configure(bad, &error));
    CHECK(!error.empty());
    bad = good;
    bad.handicap = -1;
    CHECK(!state.configure(bad, &error));
    CHECK(!error.empty());
    bad = good;
    bad.flags = 0x40;
    CHECK(!state.configure(bad, &error));
    CHECK(!error.empty());
    bad = good;
    bad.mode = static_cast<DmMode>(9);
    CHECK(!state.configure(bad, &error));
    CHECK(error.find("mode") != std::string::npos || error.find("game mode") != std::string::npos);

    // The plain form clamps instead and keeps a note.
    DmSettings clamped = good;
    clamped.frag_limit = -2;
    clamped.time_limit_ticks = -3;
    clamped.respawn_delay_ticks = -4;
    clamped.handicap = -5;
    clamped.flags = 0xFF;
    clamped.mode = static_cast<DmMode>(7);
    state.configure(clamped);
    std::printf("dm_state: clamped settings: mode=%d flags=0x%02x frag=%d time=%d respawn=%d "
                "handicap=%d error=\"%s\"\n",
                static_cast<int>(state.settings().mode), state.settings().flags,
                state.settings().frag_limit, state.settings().time_limit_ticks,
                state.settings().respawn_delay_ticks, state.settings().handicap,
                state.last_error().c_str());
    CHECK_EQ(static_cast<int>(state.settings().mode), 0); // campaign
    CHECK_EQ(state.settings().flags, gm::kDmKnownFlags);  // unknown bits dropped
    CHECK_EQ(state.settings().frag_limit, 0);
    CHECK_EQ(state.settings().time_limit_ticks, 0);
    CHECK_EQ(state.settings().respawn_delay_ticks, 0);
    CHECK_EQ(state.settings().handicap, 0);
    CHECK(!state.last_error().empty());
    CHECK_EQ(state.player_count(), 0); // configure restarts the match
    state.clear_error();
    CHECK(state.last_error().empty());

    // The 8-slot limit (§4.1).
    DmState full;
    full.configure(good);
    for (int i = 0; i < DmState::kMaxPlayers; ++i) {
        CHECK_EQ(full.add_player(i % 2), i);
    }
    const int ninth = full.add_player(0);
    std::printf("dm_state: slots: count=%d ninth=%d error=\"%s\"\n", full.player_count(), ninth,
                full.last_error().c_str());
    CHECK_EQ(full.player_count(), 8);
    CHECK_EQ(ninth, -1);
    CHECK(!full.last_error().empty());

    // A negative team id is clamped, not refused.
    DmState team;
    team.configure(good);
    CHECK_EQ(team.add_player(-3), 0);
    CHECK_EQ(team.player(0)->team, 0);
    CHECK(!team.last_error().empty());

    // An out-of-range victim is refused with a text and changes nothing.
    DmState range;
    range.configure(good);
    range.add_player(0);
    range.add_player(1);
    range.on_kill(2, 0);
    std::printf("dm_state: victim out of range: frags=%d/%d error=\"%s\"\n",
                range.player(0)->frags, range.player(1)->frags, range.last_error().c_str());
    CHECK_EQ(range.player(0)->frags, 0);
    CHECK_EQ(range.player(1)->frags, 0);
    CHECK(!range.last_error().empty());
    range.on_kill(-1, 0);
    CHECK_EQ(range.player(0)->frags, 0);
    CHECK(!range.last_error().empty());
    CHECK(range.player(2) == nullptr);
    CHECK(range.player(-1) == nullptr);
}

} // namespace

int main() {
    test_modes_and_flags();
    test_friendly();
    test_kill_and_respawn();
    test_self_and_world();
    test_team_kill();
    test_frag_limit();
    test_time_limit();
    test_handicap();
    test_negatives();

    if (g_failures != 0) {
        std::fprintf(stderr, "dm_state: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("dm_state: ok (%d checks)\n", g_checks);
    return 0;
}
