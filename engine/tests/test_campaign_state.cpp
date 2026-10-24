// Campaign progress (`M9.1`, slice 2; docs/CAMPAIGN_STATE.md).
//
// Pins the pure module `game/campaign_state.h` against the numbers of
// `docs/CAMPAIGN_FLOW.md`; the only game file it reads is `GUNMETAL.HI`
// (read only, copied to a temporary file for the write side).  Every number it
// checks is printed.
//
//   * counter `ds:0x80f9cc`: new game 1, `load()` clamps 0/-5 to 1, the 24
//     `NEXT_LEVEL` increments walk 1 -> 25 without a gap and the 25th is
//     refused with a text (§2/§3);
//   * the 36-row transition table: 24 missions, 5 HQs, 4 DM, 3 secret levels,
//     the 60 `NEW_LEVEL` + 6 `NEXT_LEVEL` sites = 66 (§3/§8), the six
//     exceptions by real level names (`level0`, `level00`, `level000`,
//     `level0a`, `level0b` = COMMAND POST, `level19` = WASTELAND);
//   * a full campaign walk: 24 missions `level1`..`level24`, the counter is
//     `m+1` during mission `m`, 23 HQ `NEXT_LEVEL` executions, `level24` has
//     no exit (§1/§2/§7);
//   * "Restart level from HQ": decrement (`0x44295e`) and the clamp at 1
//     (`0x442973`);
//   * objectives 25/51/35/48 and at most 30 per level (§4); secrets 101
//     declared / 105 sites, `player+0x33cc`, no cap and no dedupe (§5);
//   * `GUNMETAL.HI`: the shipped 576-byte, 9-record file round-trips byte for
//     byte, editing one time changes only its 4 bytes, `best_time()` is the
//     case-insensitive minimum per name (§6);
//   * negatives: unknown level, a DM level, `level24`, `NEXT_LEVEL` at counter
//     25, a missing/short/oversized `.hi`, out-of-range record indices, a
//     negative time, an empty/61-byte/NUL name and an unwritable path -- all
//     refused with a text and without touching the state.
#include "game/campaign_state.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

int g_failures = 0;
int g_checks = 0;
int g_hi_records = 0;

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

/// Read a whole file; empty when it cannot be opened.
std::string read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string out;
    char buffer[4096];
    while (in.read(buffer, static_cast<std::streamsize>(sizeof(buffer))) || in.gcount() > 0) {
        out.append(buffer, static_cast<std::size_t>(in.gcount()));
    }
    return out;
}

bool write_bytes(const std::string& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

/// FNV-1a 64 of a file, the project's usual fingerprint.
std::uint64_t fnv1a64(std::string_view data) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// A scratch file with a per-process suffix (`std::filesystem` keeps this
/// stdlib-only).  `/var/tmp` is the project's scratch disk; `GM_CAMPAIGN_TMPDIR`
/// overrides it and the system temp dir is the last resort.
std::string temp_file(const std::string& leaf) {
    std::string base = "/var/tmp";
    if (const char* env = std::getenv("GM_CAMPAIGN_TMPDIR"); env != nullptr && *env != '\0') {
        base = env;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(base, ec)) {
        base = std::filesystem::temp_directory_path(ec).string();
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&g_checks)));
    return base + "/gm_campaign_state" + suffix + "_" + leaf;
}

/// Mission number of `level<n>`; 0 for every other stem.
int mission_of(std::string_view stem) {
    if (stem.size() <= 5 || stem.substr(0, 5) != "level") {
        return 0;
    }
    int value = 0;
    for (std::size_t i = 5; i < stem.size(); ++i) {
        if (stem[i] < '0' || stem[i] > '9') {
            return 0;
        }
        value = value * 10 + (stem[i] - '0');
    }
    return value;
}

std::string joined(const std::array<int, 9>& values, int count) {
    std::string line;
    for (int i = 0; i < count; ++i) {
        line += (i == 0 ? "" : ",") + std::to_string(values[static_cast<std::size_t>(i)]);
    }
    return line;
}

// --- counter ---------------------------------------------------------------
void test_counter() {
    gm::CampaignState state;
    state.new_game();
    std::printf("campaign_state: new game: counter=%d next=%s mission=%d last=%d\n",
                state.counter(), state.next_stem().c_str(), state.mission_index(),
                state.at_last_mission() ? 1 : 0);
    CHECK_EQ(state.counter(), 1);
    CHECK_EQ(state.next_stem(), "level0");
    CHECK_EQ(state.mission_index(), 0);
    CHECK(!state.at_last_mission());
    CHECK(state.last_error().empty());

    state.load(0);
    const int zero = state.counter();
    CHECK_EQ(zero, 1);
    CHECK(!state.last_error().empty());
    const std::string zero_error = state.last_error();
    state.load(-5);
    const int negative = state.counter();
    CHECK_EQ(negative, 1);
    CHECK(!state.last_error().empty());
    state.load(7);
    const int seven = state.counter();
    CHECK_EQ(seven, 7);
    CHECK(state.last_error().empty());
    CHECK(state.next_stem().empty()); // the saved level name is not the counter
    state.load(25);
    CHECK_EQ(state.counter(), 25); // no upper clamp: the save field is read as is
    std::printf("campaign_state: load: 0->%d (-5)->%d 7->%d 25->%d error0=\"%s\"\n", zero,
                negative, seven, state.counter(), zero_error.c_str());

    // `new_level()` is the `0x457b26` increment: 24 steps take 1 to 25 with no
    // gap, the 25th step (counter 25 = level24) is refused with a text.
    state.new_game();
    int steps = 0;
    bool monotonic = true;
    for (int expected = 2; expected <= 25; ++expected) {
        if (!state.new_level() || state.counter() != expected) {
            monotonic = false;
        }
        ++steps;
    }
    const bool past_end = state.new_level();
    const int after_end = state.counter();
    std::printf("campaign_state: increments: %d steps 1->%d monotonic=%d 25th=%d counter=%d "
                "error=\"%s\"\n",
                steps, steps + 1, monotonic ? 1 : 0, past_end ? 1 : 0, after_end,
                state.last_error().c_str());
    CHECK_EQ(steps, 24);
    CHECK(monotonic);
    CHECK_EQ(state.counter(), 25);
    CHECK(!past_end);
    CHECK_EQ(after_end, 25);
    CHECK(!state.last_error().empty());
    CHECK(state.at_last_mission());

    // A clean call clears the note again.
    state.load(3);
    CHECK(state.new_level());
    CHECK(state.last_error().empty());
}

// --- the 36-row transition table ------------------------------------------
void test_table() {
    const auto rows = gm::campaign_levels();
    int missions = 0;
    int hqs = 0;
    int deathmatch = 0;
    int secrets = 0;
    int new_sites = 0;
    int next_sites = 0;
    std::string next_rows;
    for (const gm::CampaignLevel& row : rows) {
        switch (row.role) {
        case gm::CampaignRole::hq:
            ++hqs;
            break;
        case gm::CampaignRole::mission:
            ++missions;
            break;
        case gm::CampaignRole::deathmatch:
            ++deathmatch;
            break;
        case gm::CampaignRole::secret:
            ++secrets;
            break;
        }
        new_sites += row.new_level_sites;
        next_sites += row.next_level_sites;
        if (row.next_level_sites != 0) {
            next_rows += (next_rows.empty() ? "" : ", ") + std::string(row.stem) + "(" +
                         std::string(row.name) + ")";
        }
    }
    std::printf("campaign_state: table: levels=%d missions=%d hqs=%d dm=%d secret=%d "
                "new_level_sites=%d next_level_sites=%d total=%d\n",
                static_cast<int>(rows.size()), missions, hqs, deathmatch, secrets, new_sites,
                next_sites, new_sites + next_sites);
    std::printf("campaign_state: NEXT_LEVEL exceptions (%d): %s\n", next_sites,
                next_rows.c_str());
    CHECK_EQ(static_cast<int>(rows.size()), 36);
    CHECK_EQ(missions, 24);
    CHECK_EQ(hqs, 5);
    CHECK_EQ(deathmatch, 4);
    CHECK_EQ(secrets, 3);
    CHECK_EQ(new_sites, 60);
    CHECK_EQ(next_sites, 6);
    CHECK_EQ(new_sites + next_sites, 66);
    CHECK_EQ(gm::kCampaignMissions, 24);
    CHECK_EQ(gm::kCampaignHqs, 5);
    CHECK_EQ(gm::kCampaignLevelCount, 36);
    CHECK_EQ(gm::kCampaignNewLevelSites, 60);
    CHECK_EQ(gm::kCampaignNextLevelSites, 6);
    CHECK_EQ(gm::kCampaignTransitionSites, 66);
    // The six exceptions: five HQs + level19 (§2/§3).
    CHECK_EQ(next_rows,
             "level0(HEADQUARTERS), level00(HEADQUARTERS), level000(HEADQUARTERS), "
             "level0a(HEADQUARTERS), level0b(COMMAND POST), level19(WASTELAND)");

    // Real names and site counts of the awkward rows (§3/§8).
    const gm::CampaignLevel* level6 = gm::campaign_level("level6");
    const gm::CampaignLevel* level4 = gm::campaign_level("level4");
    const gm::CampaignLevel* level16 = gm::campaign_level("level16");
    const gm::CampaignLevel* level19 = gm::campaign_level("level19");
    const gm::CampaignLevel* level24 = gm::campaign_level("level24");
    const gm::CampaignLevel* hq_b = gm::campaign_level("level0b");
    CHECK(level6 != nullptr && level4 != nullptr && level16 != nullptr && level19 != nullptr &&
          level24 != nullptr && hq_b != nullptr);
    CHECK_EQ(level6->name, "CADTLELTL");
    CHECK_EQ(level6->new_level_sites, 4);
    CHECK_EQ(level6->target, "level00");
    CHECK_EQ(level6->campaign_alt, "level0a"); // the second -8 sub-script (§3)
    CHECK_EQ(level4->new_level_sites, 3);      // secrt1, level5 (DM), level00
    CHECK_EQ(level4->target, "level00");
    CHECK_EQ(level16->new_level_sites, 3); // level17 (DM), level000, secrt2
    CHECK_EQ(level19->role, gm::CampaignRole::mission);
    CHECK_EQ(level19->exit, gm::CampaignExit::next_level);
    CHECK_EQ(level19->new_level_sites, 1);
    CHECK_EQ(level19->next_level_sites, 1);
    CHECK_EQ(level24->exit, gm::CampaignExit::campaign_end);
    CHECK_EQ(level24->new_level_sites, 1); // only the DM1 branch
    CHECK_EQ(hq_b->name, "COMMAND POST");
    std::printf("campaign_state: rows: level6=%s sites=%d hq=%s alt=%s | level4=%d level16=%d | "
                "level19=%s next=%d | level24=%s exit=end\n",
                std::string(level6->name).c_str(), level6->new_level_sites,
                std::string(level6->target).c_str(), std::string(level6->campaign_alt).c_str(),
                level4->new_level_sites, level16->new_level_sites,
                std::string(level19->name).c_str(), level19->next_level_sites,
                std::string(level24->name).c_str());

    // Every mission target is an HQ or, for level19, the counter; the acts of
    // §3 are the documented ones.
    CHECK_EQ(gm::campaign_level("level1")->target, "level00");
    CHECK_EQ(gm::campaign_level("level9")->target, "level000");
    CHECK_EQ(gm::campaign_level("level17")->target, "level0a");
    CHECK_EQ(gm::campaign_level("level20")->target, "level0b");
    CHECK_EQ(gm::campaign_level("level23")->target, "level0b");
    CHECK(gm::campaign_level("level24")->target.empty());
    CHECK_EQ(gm::campaign_level("dm1")->exit, gm::CampaignExit::other);
    CHECK_EQ(gm::campaign_level("secrt2")->exit, gm::CampaignExit::other);
    CHECK_EQ(gm::campaign_level("LEVEL19"), gm::campaign_level("level19")); // case-insensitive
    CHECK(gm::campaign_level("nosuchlevel") == nullptr);
    CHECK(gm::campaign_level("") == nullptr);
}

// --- one completion step per level ----------------------------------------
void test_next_level() {
    gm::CampaignState state;
    state.new_game();
    CHECK(state.next_level("level0"));
    std::printf("campaign_state: steps: level0 -> %s counter=%d\n", state.next_stem().c_str(),
                state.counter());
    CHECK_EQ(state.next_stem(), "level1");
    CHECK_EQ(state.counter(), 2);
    CHECK_EQ(state.stats().hq_visits, 1);

    // A mission returns to its HQ and leaves the counter alone (§3).
    CHECK(state.next_level("level1"));
    CHECK_EQ(state.next_stem(), "level00");
    CHECK_EQ(state.counter(), 2);
    CHECK_EQ(state.stats().hq_visits, 1);

    // The HQ runs NEXT_LEVEL: name from the counter, then the increment.
    CHECK(state.next_level("level00"));
    std::printf("campaign_state: steps: level1 -> %s -> %s counter=%d hq_visits=%d\n", "level00",
                state.next_stem().c_str(), state.counter(), state.stats().hq_visits);
    CHECK_EQ(state.next_stem(), "level2");
    CHECK_EQ(state.counter(), 3);
    CHECK_EQ(state.stats().hq_visits, 2);

    // level19 is the mission without an HQ: it advances the counter itself.
    state.load(20);
    CHECK(state.next_level("level19"));
    std::printf("campaign_state: level19 at counter 20 -> %s counter=%d hq_visits=%d\n",
                state.next_stem().c_str(), state.counter(), state.stats().hq_visits);
    CHECK_EQ(state.next_stem(), "level20");
    CHECK_EQ(state.counter(), 21);
    CHECK_EQ(state.stats().hq_visits, 0); // not an HQ

    // level24 has no campaign exit (§7).
    state.load(25);
    const std::string before = state.next_stem();
    const bool ended = state.next_level("level24");
    std::printf("campaign_state: level24: next=%d counter=%d error=\"%s\"\n", ended ? 1 : 0,
                state.counter(), state.last_error().c_str());
    CHECK(!ended);
    CHECK_EQ(state.counter(), 25);
    CHECK_EQ(state.next_stem(), before);
    CHECK(state.last_error().find("no campaign exit") != std::string::npos);
}

// --- the whole campaign ----------------------------------------------------
void test_campaign_walk() {
    gm::CampaignState state;
    state.new_game();
    CHECK_EQ(state.next_stem(), "level0");

    std::string current = state.next_stem();
    std::array<int, 9> hq_visits{}; // level0, level00, level000, level0a, level0b, level19, ...
    int hq_total = 0;
    int increments = 0;
    int last_counter = state.counter();
    int missions = 0;
    std::string sequence;
    bool order_ok = true;
    bool acts_ok = true;
    bool counter_ok = true;

    while (true) {
        const gm::CampaignLevel* row = gm::campaign_level(current);
        CHECK(row != nullptr);
        if (row == nullptr) {
            break;
        }
        if (row->role == gm::CampaignRole::hq) {
            if (row->stem == "level0") {
                ++hq_visits[0];
            } else if (row->stem == "level00") {
                ++hq_visits[1];
            } else if (row->stem == "level000") {
                ++hq_visits[2];
            } else if (row->stem == "level0a") {
                ++hq_visits[3];
            } else {
                ++hq_visits[4];
            }
            ++hq_total;
            if (!state.next_level(current)) {
                order_ok = false;
                break;
            }
            sequence += row->stem;
            sequence += " ";
            current = state.next_stem();
            if (state.counter() != last_counter + 1) {
                counter_ok = false;
            }
            last_counter = state.counter();
            ++increments;
            continue;
        }
        // A mission: `level<m>` at counter `m+1`.
        const int m = mission_of(current);
        ++missions;
        state.on_mission_complete();
        if (m != missions) {
            order_ok = false;
        }
        if (state.counter() != m + 1) {
            counter_ok = false;
        }
        sequence += current;
        sequence += " ";
        if (missions == 24) {
            const bool exits = state.next_level(current);
            if (exits) {
                acts_ok = false;
            }
            break;
        }
        if (!state.next_level(current)) {
            order_ok = false;
            break;
        }
        // The act table of §3: the mission returns to its own HQ (level19 goes
        // straight to level20).
        std::string expected;
        if (m <= 8) {
            expected = "level00";
        } else if (m <= 16) {
            expected = "level000";
        } else if (m <= 18) {
            expected = "level0a";
        } else if (m == 19) {
            expected = "level20";
        } else {
            expected = "level0b";
        }
        if (state.next_stem() != expected) {
            acts_ok = false;
        }
        // level19 advances the counter itself, every other mission does not.
        const int expected_counter = (m == 19) ? m + 2 : m + 1;
        if (state.counter() != expected_counter) {
            counter_ok = false;
        }
        if (m == 19) {
            ++increments;
            last_counter = state.counter();
        }
        current = state.next_stem();
    }

    std::printf("campaign_state: walk: missions=%d increments=%d counter=%d hq_visits=%d\n",
                missions, increments, state.counter(), hq_total);
    std::printf("campaign_state: hq visits: level0=%d level00=%d level000=%d level0a=%d "
                "level0b=%d total=%d\n",
                hq_visits[0], hq_visits[1], hq_visits[2], hq_visits[3], hq_visits[4], hq_total);
    std::printf("campaign_state: order=%d acts=%d counter=%d sequence=%.240s\n", order_ok ? 1 : 0,
                acts_ok ? 1 : 0, counter_ok ? 1 : 0, sequence.c_str());
    CHECK_EQ(missions, 24);
    CHECK_EQ(increments, 24);
    CHECK_EQ(state.counter(), 25);
    CHECK(state.at_last_mission());
    CHECK_EQ(state.stats().missions, 24);
    CHECK_EQ(state.stats().hq_visits, 23);
    CHECK_EQ(hq_total, 23);
    CHECK_EQ(hq_visits[0], 1);
    CHECK_EQ(hq_visits[1], 8);
    CHECK_EQ(hq_visits[2], 8);
    CHECK_EQ(hq_visits[3], 2);
    CHECK_EQ(hq_visits[4], 4);
    CHECK(order_ok);
    CHECK(acts_ok);
    CHECK(counter_ok);
    CHECK(!state.last_error().empty()); // level24 refused the exit
}

// --- restart from HQ -------------------------------------------------------
void test_restart() {
    gm::CampaignState state;
    state.load(2);
    state.restart_level_from_hq();
    const int from_two = state.counter();
    state.restart_level_from_hq();
    const int at_one = state.counter();
    CHECK_EQ(from_two, 1);
    CHECK_EQ(at_one, 1);
    CHECK(!state.last_error().empty());
    const int restarts_before_load = state.stats().restarts;
    state.load(25);
    state.restart_level_from_hq();
    const int from_last = state.counter();
    std::printf("campaign_state: restart: 2->%d, 1->%d (clamp 0x442973), 25->%d restarts=%d "
                "(load resets the module counters to %d)\n",
                from_two, at_one, from_last, restarts_before_load, state.stats().restarts);
    CHECK_EQ(from_last, 24);
    CHECK_EQ(restarts_before_load, 2);
    CHECK_EQ(state.stats().restarts, 1); // load() resets the module counters
}

// --- objectives and secrets ------------------------------------------------
void test_objectives_and_secrets() {
    std::printf("campaign_state: objectives: levels=%d lines=%d declarations=%d complete_sites=%d "
                "max_per_level=%d\n",
                gm::kCampaignObjectiveLevels, gm::kCampaignObjectiveLines,
                gm::kCampaignObjectiveDeclarations, gm::kCampaignObjectiveCompleteSites,
                gm::kCampaignMaxObjectives);
    CHECK_EQ(gm::kCampaignObjectiveLevels, 25);
    CHECK_EQ(gm::kCampaignObjectiveLines, 51);
    CHECK_EQ(gm::kCampaignObjectiveDeclarations, 35);
    CHECK_EQ(gm::kCampaignObjectiveCompleteSites, 48);
    CHECK_EQ(gm::kCampaignMaxObjectives, 30); // 0x45bdf6 `cmp eax,0x1e`

    gm::CampaignState state;
    state.new_game();
    for (int i = 0; i < gm::kCampaignObjectiveLines; ++i) {
        state.on_objective();
    }
    CHECK_EQ(state.objectives_done(), 51);

    std::printf("campaign_state: secrets: declared=%d sites=%d offset=0x%x\n",
                gm::kCampaignSecretDeclared, gm::kCampaignSecretSites,
                gm::kCampaignSecretOffset);
    CHECK_EQ(gm::kCampaignSecretDeclared, 101);
    CHECK_EQ(gm::kCampaignSecretSites, 105);
    CHECK_EQ(gm::kCampaignSecretOffset, 0x33CCu);

    // level3: 6 declared, 8 increment sites -- the tally may pass the
    // declaration (§5), so neither a cap nor a dedupe is modelled.
    for (int i = 0; i < 8; ++i) {
        state.add_secret();
    }
    const int level3 = state.secrets();
    CHECK_EQ(level3, 8);
    state.reset_level_secrets(); // 0x45280f on every level start
    const int after_reset = state.secrets();
    // secrt2/temple: sites without a declaration -> "found / 0" (§5/§10).
    state.add_secret();
    state.add_secret();
    const int secret_level = state.secrets();
    std::printf("campaign_state: tally: level3 6 declared/8 sites -> %d, reset -> %d, "
                "secrt2 0 declared/2 sites -> %d\n",
                level3, after_reset, secret_level);
    CHECK_EQ(after_reset, 0);
    CHECK_EQ(secret_level, 2);
    CHECK_EQ(state.objectives_done(), 51); // untouched by the secret tally
}

// --- GUNMETAL.HI -----------------------------------------------------------
void test_hi() {
    const std::string path = "/mnt/d/games/Gunmetal 3dfx/GUNMETAL.HI";
    const std::string original = read_bytes(path);
    if (original.empty()) {
        std::printf("campaign_state: hi: GUNMETAL.HI not installed, skipped\n");
        g_hi_records = 0;
        return;
    }
    std::printf("campaign_state: hi: file %s bytes=%d records=%d fnv=0x%llx\n", path.c_str(),
                static_cast<int>(original.size()),
                static_cast<int>(original.size() / static_cast<std::size_t>(gm::kHiRecordSize)),
                static_cast<unsigned long long>(fnv1a64(original)));
    CHECK_EQ(static_cast<int>(original.size()), 576);
    CHECK_EQ(gm::kHiFileSize, 576);
    CHECK_EQ(gm::kHiRecordSize, 0x40);
    CHECK_EQ(gm::kHiNameSize, 0x3C);
    CHECK_EQ(gm::kHiRecords, 9);

    gm::CampaignState state;
    std::string error;
    const bool loaded = state.load_hi(path, &error);
    CHECK(loaded);
    CHECK(error.empty());
    g_hi_records = state.hi_count();
    std::printf("campaign_state: hi: records=%d times=%s\n", state.hi_count(),
                joined(state.best_times, state.hi_count()).c_str());
    CHECK_EQ(state.hi_count(), 9);
    CHECK_EQ(state.best_names[0], "TESTING RANGE");
    CHECK_EQ(state.best_times[0], 146);
    CHECK_EQ(state.best_times[1], 1570); // KELLER CORE
    CHECK_EQ(state.best_times[2], 18);   // TESTING RANGE, duplicate name
    CHECK_EQ(state.best_times[3], 109);
    CHECK_EQ(state.best_times[4], 18); // NATAKA TOWER, the worse record
    CHECK_EQ(state.best_times[5], 7);  // NATAKA TOWER, the minimum
    CHECK_EQ(state.best_times[6], 11);
    CHECK_EQ(state.best_times[7], 40);
    CHECK_EQ(state.best_times[8], 6);
    CHECK_EQ(state.best_names[8], "CADTLELTL");

    // The minimum per name, case-insensitive (`0x416850`/`0x45c8d6`, §6).
    const int testing = state.best_time("TESTING RANGE");
    const int testing_lower = state.best_time("testing range");
    const int keller = state.best_time("KELLER CORE");
    const int nataka = state.best_time("NATAKA TOWER");
    const int missing = state.best_time("NO SUCH LEVEL");
    std::printf("campaign_state: hi best: TESTING RANGE=%d lowercase=%d KELLER CORE=%d "
                "NATAKA TOWER=%d missing=%d\n",
                testing, testing_lower, keller, nataka, missing);
    CHECK_EQ(testing, 18);
    CHECK_EQ(testing_lower, 18);
    CHECK_EQ(keller, 1570);
    CHECK_EQ(nataka, 7);
    CHECK_EQ(missing, -1);

    // Read -> write is byte for byte the shipped file.
    const std::string roundtrip = temp_file("roundtrip.hi");
    CHECK(state.save_hi(roundtrip, &error));
    const std::string written = read_bytes(roundtrip);
    std::printf("campaign_state: hi round-trip: written=%d identical=%d\n",
                static_cast<int>(written.size()), written == original ? 1 : 0);
    CHECK_EQ(static_cast<int>(written.size()), 576);
    CHECK(written == original);

    // An edit of one time changes only that record's seconds.
    CHECK(state.set_hi_time(2, 5, &error));
    const std::string edited = temp_file("edited.hi");
    CHECK(state.save_hi(edited, &error));
    const std::string changed = read_bytes(edited);
    int diff = 0;
    int first = -1;
    int last = -1;
    if (changed.size() == original.size()) {
        for (std::size_t i = 0; i < original.size(); ++i) {
            if (changed[i] != original[i]) {
                ++diff;
                if (first < 0) {
                    first = static_cast<int>(i);
                }
                last = static_cast<int>(i);
            }
        }
    } else {
        diff = -1;
    }
    std::printf("campaign_state: hi edit: record=2 18->5 differing_bytes=%d first=0x%x last=0x%x\n",
                diff, first, last);
    CHECK_EQ(diff, 1);
    CHECK_EQ(first, 0x80); // record 2 starts at 2 * 0x40, name at +4
    CHECK_EQ(last, 0x80);

    // Re-reading the edited file sees the new minimum and keeps the rest.
    gm::CampaignState reread;
    CHECK(reread.load_hi(edited, &error));
    std::printf("campaign_state: hi reread: records=%d times=%s TESTING RANGE=%d\n",
                reread.hi_count(), joined(reread.best_times, reread.hi_count()).c_str(),
                reread.best_time("TESTING RANGE"));
    CHECK_EQ(reread.hi_count(), 9);
    CHECK_EQ(reread.best_times[2], 5);
    CHECK_EQ(reread.best_times[0], 146);
    CHECK_EQ(reread.best_time("TESTING RANGE"), 5);
    CHECK_EQ(reread.best_time("CADTLELTL"), 6);

    // Editing name + time touches only that record's name field (the baseline
    // is the already edited file, so record 2's time does not count again).
    CHECK(reread.set_hi_record(8, 6, "NEW NAME", &error));
    const std::string renamed = temp_file("renamed.hi");
    CHECK(reread.save_hi(renamed, &error));
    const std::string renamed_bytes = read_bytes(renamed);
    bool only_record8 = renamed_bytes.size() == changed.size();
    int renamed_diff = 0;
    if (only_record8) {
        for (std::size_t i = 0; i < changed.size(); ++i) {
            if (renamed_bytes[i] != changed[i]) {
                ++renamed_diff;
                if (i < 0x204 || i >= 0x240) { // record 8: 8*0x40 + 4 .. + 0x40
                    only_record8 = false;
                }
            }
        }
    }
    std::printf("campaign_state: hi rename: differing_bytes=%d inside_record8=%d\n", renamed_diff,
                only_record8 ? 1 : 0);
    CHECK(only_record8);
    CHECK(renamed_diff > 0);
    CHECK_EQ(renamed_bytes[0x204], 'N');
    CHECK_EQ(renamed_bytes[0x205], 'E');
    CHECK_EQ(renamed_bytes[0x206], 'W');
    CHECK_EQ(renamed_bytes[0x207], ' ');
    CHECK_EQ(renamed_bytes[0x208], 'N');
    CHECK_EQ(renamed_bytes[0x209], 'A');
    CHECK_EQ(renamed_bytes[0x20A], 'M');
    CHECK_EQ(renamed_bytes[0x20B], 'E');
    CHECK_EQ(renamed_bytes[0x20C], '\0');

    // Negatives: the table survives every refusal.
    const std::string missing_path = temp_file("missing.hi");
    std::error_code ignored;
    std::filesystem::remove(missing_path, ignored);
    const bool missing_ok = state.load_hi(missing_path, &error);
    std::printf("campaign_state: hi missing file: loaded=%d error=\"%s\" kept=%d\n",
                missing_ok ? 1 : 0, error.c_str(), state.hi_count());
    CHECK(!missing_ok);
    CHECK(!error.empty());
    CHECK_EQ(state.hi_count(), 9);

    const std::string short_path = temp_file("short.hi");
    CHECK(write_bytes(short_path, std::string(3, '\0')));
    error.clear();
    CHECK(!state.load_hi(short_path, &error));
    std::printf("campaign_state: hi short file: error=\"%s\"\n", error.c_str());
    CHECK(error.find("not a multiple of 0x40") != std::string::npos);
    CHECK_EQ(state.hi_count(), 9);

    const std::string big_path = temp_file("big.hi");
    CHECK(write_bytes(big_path, std::string(10 * 0x40, '\0')));
    error.clear();
    CHECK(!state.load_hi(big_path, &error));
    std::printf("campaign_state: hi oversized: error=\"%s\"\n", error.c_str());
    CHECK(error.find("10 records") != std::string::npos);
    CHECK_EQ(state.hi_count(), 9);

    error.clear();
    CHECK(!state.set_hi_time(-1, 3, &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!state.set_hi_time(9, 3, &error));
    CHECK(!error.empty());
    CHECK(error.find("out of range") != std::string::npos);
    error.clear();
    CHECK(!state.set_hi_time(0, -1, &error));
    CHECK(error.find("negative") != std::string::npos);
    error.clear();
    CHECK(!state.set_hi_record(0, 1, "", &error));
    CHECK(error.find("empty") != std::string::npos);
    error.clear();
    CHECK(!state.set_hi_record(0, 1, std::string(61, 'x'), &error));
    CHECK(error.find("60") != std::string::npos);
    error.clear();
    CHECK(!state.set_hi_record(0, 1, std::string("AB\0CD", 5), &error));
    CHECK(error.find("NUL") != std::string::npos);
    error.clear();
    CHECK(!state.save_hi("/nonexistent/gm/GUNMETAL.HI", &error));
    std::printf("campaign_state: hi refusals: index=-1/9 time=-1 name=empty/61/NUL path=missing, "
                "records kept=%d\n",
                state.hi_count());
    CHECK(error.find("cannot write") != std::string::npos);
    CHECK_EQ(state.best_times[2], 5); // the refused edits changed nothing

    std::error_code cleanup_error;
    std::filesystem::remove(roundtrip, cleanup_error);
    std::filesystem::remove(edited, cleanup_error);
    std::filesystem::remove(renamed, cleanup_error);
    std::filesystem::remove(short_path, cleanup_error);
    std::filesystem::remove(big_path, cleanup_error);
}

// --- negatives of the transition path -------------------------------------
void test_negatives() {
    gm::CampaignState state;
    state.new_game();
    state.load(2);

    const int before = state.counter();
    const bool unknown = state.next_level("nosuchlevel");
    std::printf("campaign_state: unknown level: next=%d counter=%d error=\"%s\"\n",
                unknown ? 1 : 0, state.counter(), state.last_error().c_str());
    CHECK(!unknown);
    CHECK_EQ(state.counter(), before);
    CHECK(state.last_error().find("unknown level") != std::string::npos);

    CHECK(!state.next_level("dm1"));
    std::printf("campaign_state: dm1: error=\"%s\"\n", state.last_error().c_str());
    CHECK(state.last_error().find("not on the campaign path") != std::string::npos);
    CHECK(!state.next_level("secrt1"));
    CHECK(state.last_error().find("not on the campaign path") != std::string::npos);

    // NEXT_LEVEL at counter 25 would load LEVEL25, which does not exist.
    state.load(25);
    CHECK(!state.next_level("level00"));
    std::printf("campaign_state: HQ at counter 25: error=\"%s\"\n", state.last_error().c_str());
    CHECK(state.last_error().find("LEVEL25") != std::string::npos);
    CHECK(!state.next_level("level19"));
    CHECK(state.last_error().find("LEVEL25") != std::string::npos);
    CHECK_EQ(state.counter(), 25);

    // A clean step clears the previous note.
    state.load(2);
    CHECK(state.next_level("level1"));
    CHECK(state.last_error().empty());
}

} // namespace

int main() {
    test_counter();
    test_table();
    test_next_level();
    test_campaign_walk();
    test_restart();
    test_objectives_and_secrets();
    test_hi();
    test_negatives();

    if (g_failures != 0) {
        std::fprintf(stderr, "campaign_state: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("campaign_state: ok (%d checks) counter=1..25 missions=24 next_level_sites=6 "
                "transitions=66 hq_visits=23 hi_records=%d\n",
                g_checks, g_hi_records);
    return 0;
}
