// Level-selection screen (`M8.4`/`M9.1`, slice 2; docs/LEVEL_SELECT.md).
//
// Pins the pure module `ui/level_select.h` against the numbers of
// `docs/CAMPAIGN_FLOW.md` §8, `docs/UI_SCREENS.md` §2/§3.7 and `docs/FORMATS.md`
// §15.  The only file it reads is the shipped `GUNMETAL.HI` (read only, never
// written), and it does not need the installation to pass.
//
//   * the list: 36 rows = 24 missions + 5 HQs + 3 secret levels + 4 DM maps, in
//     campaign order, `level1` first and `dm4` last, every name a real row of
//     the 36-level table;
//   * the 101 declared secrets of §8, 25 levels declaring at least one, at most
//     6 on a level;
//   * unlock thresholds (MODEL, derived from the counter): counter 1/8/17/25 ->
//     7/14/27/36 selectable and 0/7/16/24 completed missions;
//   * best times from the real `GUNMETAL.HI`: `TESTING RANGE` 18 (not 146),
//     `POWER STATION` 109, `NATAKA TOWER` 7, `OIL RIG` 11, `OIL REFINERY` 40,
//     `CADTLELTL` 6, `KELLER CORE` 1570, and -1 for a level without a record;
//   * cursor and events: clamped moves, locked rows are not skipped, `launch`
//     carries the stem, `back` is its own event;
//   * negatives: no list (never built), unknown level, a locked row -- each with
//     a one-line text; determinism of two builds.
#include "ui/level_select.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "game/campaign_state.h"
#include "ui/records.h"

namespace {

int g_failures = 0;
int g_checks = 0;
int g_hi_loaded = 0;

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

[[maybe_unused]] constexpr int kKindCount = 5;

int count_kind(const std::vector<gm::LevelEntry>& levels, gm::LevelKind kind) {
    int count = 0;
    for (const gm::LevelEntry& entry : levels) {
        if (entry.kind == kind) {
            ++count;
        }
    }
    return count;
}

int count_unlocked(const std::vector<gm::LevelEntry>& levels) {
    int count = 0;
    for (const gm::LevelEntry& entry : levels) {
        count += entry.unlocked ? 1 : 0;
    }
    return count;
}

int count_completed(const std::vector<gm::LevelEntry>& levels) {
    int count = 0;
    for (const gm::LevelEntry& entry : levels) {
        count += entry.completed ? 1 : 0;
    }
    return count;
}

int declared_total(const std::vector<gm::LevelEntry>& levels) {
    int total = 0;
    for (const gm::LevelEntry& entry : levels) {
        total += entry.secrets_declared;
    }
    return total;
}

int declaring_levels(const std::vector<gm::LevelEntry>& levels) {
    int count = 0;
    for (const gm::LevelEntry& entry : levels) {
        count += entry.secrets_declared > 0 ? 1 : 0;
    }
    return count;
}

int max_declared(const std::vector<gm::LevelEntry>& levels) {
    int value = 0;
    for (const gm::LevelEntry& entry : levels) {
        if (entry.secrets_declared > value) {
            value = entry.secrets_declared;
        }
    }
    return value;
}

/// Index of the row with *stem*, -1 when absent.
int index_of(const std::vector<gm::LevelEntry>& levels, std::string_view stem) {
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[i].name == stem) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const gm::LevelEntry* find(const std::vector<gm::LevelEntry>& levels, std::string_view stem) {
    const int index = index_of(levels, stem);
    return index < 0 ? nullptr : &levels[static_cast<std::size_t>(index)];
}

bool unlocked(const std::vector<gm::LevelEntry>& levels, std::string_view stem) {
    const gm::LevelEntry* entry = find(levels, stem);
    return entry != nullptr && entry->unlocked;
}

int best(const std::vector<gm::LevelEntry>& levels, std::string_view stem) {
    const gm::LevelEntry* entry = find(levels, stem);
    return entry == nullptr ? -2 : entry->best_seconds;
}

/// Unique names joined by commas (the printed dump of a build).
std::string joined_names(const std::vector<gm::LevelEntry>& levels) {
    std::string out;
    for (const gm::LevelEntry& entry : levels) {
        out += (out.empty() ? "" : ",") + entry.name;
    }
    return out;
}

/// Everything the state derives, for the determinism check.
std::string dump(const gm::LevelSelect& select) {
    std::string out;
    for (const gm::LevelEntry& entry : select.levels()) {
        out += entry.name;
        out += entry.unlocked ? "+" : "-";
        out += entry.completed ? "c" : ".";
        out += std::to_string(entry.mission);
        out += ":";
        out += std::to_string(entry.best_seconds);
        out += "/";
        out += std::to_string(entry.secrets_found);
        out += "/";
        out += std::to_string(entry.secrets_declared);
        out += ";";
    }
    out += "sel=" + std::to_string(select.selected());
    out += ";launched=" + select.launched();
    return out;
}

/// The shipped `GUNMETAL.HI`; `GUNMETAL_DIR` overrides the install root and the
/// mirror under `~/.cache/gunmetal-re/game` is the second try.
std::filesystem::path shipped_hi() {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    std::error_code ec;
    for (const fs::path& root : roots) {
        const fs::path file = root / "GUNMETAL.HI";
        if (fs::is_regular_file(file, ec) && !ec) {
            return file;
        }
    }
    return {};
}

// --- the list --------------------------------------------------------------
void test_list() {
    gm::CampaignState campaign;
    campaign.load(25); // level24 is being played: everything is reachable

    gm::LevelSelect select;
    select.build(campaign, nullptr);
    const std::vector<gm::LevelEntry>& levels = select.levels();

    CHECK(select.last_error().empty());
    CHECK(select.event() == gm::LevelSelectEvent::none);
    CHECK_EQ(levels.size(), static_cast<std::size_t>(gm::LevelSelect::kLevelCount));
    CHECK_EQ(static_cast<int>(levels.size()), gm::kCampaignLevelCount);
    CHECK_EQ(count_kind(levels, gm::LevelKind::mission), gm::kCampaignMissions);
    CHECK_EQ(count_kind(levels, gm::LevelKind::headquarters), gm::kCampaignHqs);
    CHECK_EQ(count_kind(levels, gm::LevelKind::secret), 3);
    CHECK_EQ(count_kind(levels, gm::LevelKind::deathmatch), 4);
    CHECK_EQ(count_kind(levels, gm::LevelKind::unknown), 0);

    // Order: 24 missions, 5 HQs, 3 secret levels, 4 DM maps (MODEL, header).
    static constexpr std::array<std::string_view, 5> kHqs = {"level0", "level00", "level000",
                                                             "level0a", "level0b"};
    static constexpr std::array<std::string_view, 3> kSecret = {"secrt1", "secrt2", "temple"};
    static constexpr std::array<std::string_view, 4> kDm = {"dm1", "dm2", "dm3", "dm4"};
    CHECK_EQ(levels.front().name, "level1");
    CHECK_EQ(levels.front().mission, 1);
    CHECK(levels.front().kind == gm::LevelKind::mission);
    CHECK_EQ(levels.back().name, "dm4");
    CHECK(levels.back().kind == gm::LevelKind::deathmatch);
    CHECK_EQ(levels.back().mission, 0);
    for (int i = 0; i < gm::kCampaignMissions; ++i) {
        const gm::LevelEntry& entry = levels[static_cast<std::size_t>(i)];
        CHECK_EQ(entry.name, "level" + std::to_string(i + 1));
        CHECK_EQ(entry.mission, i + 1);
        CHECK(entry.kind == gm::LevelKind::mission);
    }
    for (std::size_t i = 0; i < kHqs.size(); ++i) {
        const gm::LevelEntry& entry =
            levels[static_cast<std::size_t>(gm::kCampaignMissions) + i];
        CHECK_EQ(entry.name, kHqs[i]);
        CHECK(entry.kind == gm::LevelKind::headquarters);
        CHECK_EQ(entry.mission, 0);
    }
    for (std::size_t i = 0; i < kSecret.size(); ++i) {
        const gm::LevelEntry& entry = levels[static_cast<std::size_t>(gm::kCampaignMissions) +
                                             kHqs.size() + i];
        CHECK_EQ(entry.name, kSecret[i]);
        CHECK(entry.kind == gm::LevelKind::secret);
    }
    for (std::size_t i = 0; i < kDm.size(); ++i) {
        const gm::LevelEntry& entry = levels[static_cast<std::size_t>(gm::kCampaignMissions) +
                                             kHqs.size() + kSecret.size() + i];
        CHECK_EQ(entry.name, kDm[i]);
        CHECK(entry.kind == gm::LevelKind::deathmatch);
    }

    // Every row is a real table row and the kind matches the proven role.
    for (const gm::LevelEntry& entry : levels) {
        const gm::CampaignLevel* row = gm::campaign_level(entry.name);
        CHECK(row != nullptr);
        if (row == nullptr) {
            continue;
        }
        gm::LevelKind want = gm::LevelKind::secret;
        switch (row->role) {
        case gm::CampaignRole::mission:
            want = gm::LevelKind::mission;
            break;
        case gm::CampaignRole::hq:
            want = gm::LevelKind::headquarters;
            break;
        case gm::CampaignRole::deathmatch:
            want = gm::LevelKind::deathmatch;
            break;
        case gm::CampaignRole::secret:
            want = gm::LevelKind::secret;
            break;
        }
        CHECK(entry.kind == want);
    }
    // No duplicate names, and the same set as the 36-level table.
    std::vector<std::string> sorted;
    for (const gm::LevelEntry& entry : levels) {
        sorted.push_back(entry.name);
    }
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK_EQ(static_cast<int>(gm::campaign_levels().size()), gm::kCampaignLevelCount);

    // Secrets of §8: 101 declared over 25 levels, at most 6 on one level.
    CHECK_EQ(declared_total(levels), gm::kCampaignSecretDeclared);
    CHECK_EQ(declared_total(levels), 101);
    CHECK_EQ(declaring_levels(levels), 25);
    CHECK_EQ(max_declared(levels), 6);
    CHECK_EQ(gm::LevelSelect::declared_secrets("level3"), 6);
    CHECK_EQ(gm::LevelSelect::declared_secrets("level1"), 4);
    CHECK_EQ(gm::LevelSelect::declared_secrets("secrt2"), 0); // no SET_LEVEL_SECRETS
    CHECK_EQ(gm::LevelSelect::declared_secrets("level0"), 0);

    // Static facts of the stems.
    CHECK(gm::LevelSelect::kind_of("level0b") == gm::LevelKind::headquarters);
    CHECK(gm::LevelSelect::kind_of("temple") == gm::LevelKind::secret);
    CHECK(gm::LevelSelect::kind_of("nosuch") == gm::LevelKind::unknown);
    CHECK_EQ(gm::LevelSelect::mission_of("level24"), 24);
    CHECK_EQ(gm::LevelSelect::mission_of("level19"), 19);
    CHECK_EQ(gm::LevelSelect::mission_of("level0"), 0);
    CHECK_EQ(gm::LevelSelect::mission_of("dm1"), 0);
    CHECK_EQ(gm::LevelSelect::unlock_counter("nosuch"), -1);

    // At counter 25 everything is unlocked and all 24 missions are completed.
    CHECK_EQ(count_unlocked(levels), gm::LevelSelect::kLevelCount);
    CHECK_EQ(count_completed(levels), gm::kCampaignMissions);
    for (const gm::LevelEntry& entry : levels) {
        CHECK_EQ(entry.best_seconds, -1); // no Records handed in
    }

    std::printf("level_select: list levels=%d missions=%d hq=%d secret=%d dm=%d unknown=%d "
                "declared=%d declaring=%d max=%d first=%s last=%s\n",
                static_cast<int>(levels.size()), count_kind(levels, gm::LevelKind::mission),
                count_kind(levels, gm::LevelKind::headquarters),
                count_kind(levels, gm::LevelKind::secret),
                count_kind(levels, gm::LevelKind::deathmatch),
                count_kind(levels, gm::LevelKind::unknown), declared_total(levels),
                declaring_levels(levels), max_declared(levels), levels.front().name.c_str(),
                levels.back().name.c_str());
}

// --- unlock rules ----------------------------------------------------------
void test_unlock() {
    struct Case {
        int counter;
        int unlocked;
        int completed;
    };
    static constexpr std::array<Case, 4> kCases = {
        {{1, 7, 0}, {8, 15, 7}, {17, 27, 16}, {25, 36, 24}}};
    for (const Case& row : kCases) {
        gm::CampaignState campaign;
        campaign.load(row.counter);
        gm::LevelSelect select;
        select.build(campaign, nullptr);
        const std::vector<gm::LevelEntry>& levels = select.levels();
        const int got_unlocked = count_unlocked(levels);
        const int got_completed = count_completed(levels);
        std::printf("level_select: counter=%d unlocked=%d completed=%d (%s)\n", row.counter,
                    got_unlocked, got_completed,
                    row.counter == 1 ? "start HQ, only mission 1" : "campaign progress");
        CHECK_EQ(got_unlocked, row.unlocked);
        CHECK_EQ(got_completed, row.completed);
        if (got_unlocked != row.unlocked) {
            std::fprintf(stderr, "  unlocked list: %s\n", joined_names(levels).c_str());
        }
    }

    // Spot checks at the documented act boundaries (CAMPAIGN_FLOW.md §3).
    auto build_at = [](int counter, gm::LevelSelect& select) {
        gm::CampaignState campaign;
        campaign.load(counter);
        select.build(campaign, nullptr);
    };

    gm::LevelSelect at1;
    build_at(1, at1);
    CHECK(unlocked(at1.levels(), "level1"));
    CHECK(!unlocked(at1.levels(), "level2"));
    CHECK(unlocked(at1.levels(), "level0"));
    CHECK(!unlocked(at1.levels(), "level00"));
    CHECK(!unlocked(at1.levels(), "secrt1"));
    CHECK(unlocked(at1.levels(), "temple"));
    for (const std::string_view dm : {"dm1", "dm2", "dm3", "dm4"}) {
        CHECK(unlocked(at1.levels(), dm));
    }
    // The 4 DM maps + temple + level0 + level1 are the seven of counter 1.
    CHECK_EQ(count_unlocked(at1.levels()), 7);

    gm::LevelSelect at4;
    build_at(4, at4);
    CHECK(!unlocked(at4.levels(), "secrt1")); // gate lives in level4 (counter 5)
    gm::LevelSelect at5;
    build_at(5, at5);
    CHECK(unlocked(at5.levels(), "secrt1")); // level4 is being played
    CHECK(unlocked(at5.levels(), "level4"));
    CHECK(unlocked(at5.levels(), "level5")); // counter 5 names the next mission
    CHECK(!unlocked(at5.levels(), "level6"));
    CHECK(!unlocked(at5.levels(), "level00"));

    gm::LevelSelect at8;
    build_at(8, at8);
    CHECK(unlocked(at8.levels(), "level8"));
    CHECK(unlocked(at8.levels(), "secrt1"));
    CHECK(!unlocked(at8.levels(), "level9"));
    CHECK(!unlocked(at8.levels(), "level00"));
    CHECK(!unlocked(at8.levels(), "level000"));

    gm::LevelSelect at9;
    build_at(9, at9);
    CHECK(unlocked(at9.levels(), "level00"));
    CHECK(!unlocked(at9.levels(), "level000"));

    gm::LevelSelect at16;
    build_at(16, at16);
    CHECK(!unlocked(at16.levels(), "secrt2")); // gate lives in level16 (counter 17)

    gm::LevelSelect at17;
    build_at(17, at17);
    CHECK(unlocked(at17.levels(), "level000"));
    CHECK(unlocked(at17.levels(), "secrt1"));
    CHECK(unlocked(at17.levels(), "secrt2")); // level16 is being played
    CHECK(!unlocked(at17.levels(), "level0a"));

    gm::LevelSelect at19;
    build_at(19, at19);
    CHECK(unlocked(at19.levels(), "level0a"));
    CHECK(!unlocked(at19.levels(), "level0b"));

    gm::LevelSelect at24;
    build_at(24, at24);
    CHECK(unlocked(at24.levels(), "level0b"));
    CHECK(unlocked(at24.levels(), "level23"));
    CHECK(unlocked(at24.levels(), "level24")); // counter 24 names the next mission
    CHECK(find(at24.levels(), "level23")->completed);
    CHECK(!find(at24.levels(), "level24")->completed);

    gm::LevelSelect at25;
    build_at(25, at25);
    for (const gm::LevelEntry& entry : at25.levels()) {
        CHECK(entry.unlocked);
    }

    // Completed is `mission < counter` and only missions carry it.
    const std::vector<gm::LevelEntry>& e8 = at8.levels();
    CHECK(find(e8, "level7") != nullptr && find(e8, "level7")->completed);
    CHECK(find(e8, "level8") != nullptr && !find(e8, "level8")->completed);
    CHECK(find(e8, "level0") != nullptr && !find(e8, "level0")->completed);
    CHECK(find(e8, "secrt1") != nullptr && !find(e8, "secrt1")->completed);
    CHECK(find(e8, "dm1") != nullptr && !find(e8, "dm1")->completed);

    std::printf("level_select: thresholds level1=1 level8=8 level9=9 level24=24 level0=1 "
                "level00=9 level000=17 level0a=19 level0b=24 secrt1=5 secrt2=17 temple=0 dm1=0\n");
    CHECK_EQ(gm::LevelSelect::unlock_counter("level19"), 19);
    CHECK_EQ(gm::LevelSelect::unlock_counter("level0b"), 24);
    CHECK_EQ(gm::LevelSelect::unlock_counter("secrt1"), 5);
    CHECK_EQ(gm::LevelSelect::unlock_counter("secrt2"), 17);
    CHECK_EQ(gm::LevelSelect::unlock_counter("temple"), 0);
    CHECK_EQ(gm::LevelSelect::unlock_counter("dm4"), 0);
}

// --- best times from the real file ----------------------------------------
void test_times() {
    const std::filesystem::path hi = shipped_hi();
    if (hi.empty()) {
        std::printf("level_select: times skipped (GUNMETAL.HI not installed)\n");
        return;
    }
    gm::Records records;
    std::string error;
    CHECK(records.load(hi, &error));
    if (!error.empty()) {
        std::printf("level_select: GUNMETAL.HI = %s: %s\n", hi.string().c_str(), error.c_str());
    }
    CHECK_EQ(static_cast<int>(records.rows().size()), gm::Records::kShippedRecords);
    g_hi_loaded = records.loaded() ? 1 : 0;

    gm::CampaignState campaign;
    campaign.load(25);
    gm::LevelSelect select;
    select.build(campaign, &records);
    const std::vector<gm::LevelEntry>& levels = select.levels();

    // GUNMETAL.HI of the installation (CAMPAIGN_FLOW.md §6):
    // 146 TESTING RANGE, 1570 KELLER CORE, 18 TESTING RANGE, 109 POWER STATION,
    // 18/7 NATAKA TOWER, 11 OIL RIG, 40 OIL REFINERY, 6 CADTLELTL.
    CHECK_EQ(best(levels, "level1"), 18);   // TESTING RANGE, minimum of 146 and 18
    CHECK_EQ(best(levels, "level2"), 109);  // POWER STATION
    CHECK_EQ(best(levels, "level3"), 7);    // NATAKA TOWER, minimum of 18 and 7
    CHECK_EQ(best(levels, "level4"), 11);   // OIL RIG
    CHECK_EQ(best(levels, "level5"), 40);   // OIL REFINERY
    CHECK_EQ(best(levels, "level6"), 6);    // CADTLELTL
    CHECK_EQ(best(levels, "level24"), 1570); // KELLER CORE
    // Levels without a record, and the ones the file never names.
    CHECK_EQ(best(levels, "level7"), -1);
    CHECK_EQ(best(levels, "level8"), -1);
    CHECK_EQ(best(levels, "level19"), -1);
    CHECK_EQ(best(levels, "level0"), -1);
    CHECK_EQ(best(levels, "secrt1"), -1);
    CHECK_EQ(best(levels, "dm1"), -1);

    int with_time = 0;
    for (const gm::LevelEntry& entry : levels) {
        with_time += entry.best_seconds >= 0 ? 1 : 0;
    }
    CHECK_EQ(with_time, 7); // the seven names of the shipped file

    // Without a table every time is -1, and the file itself is not touched.
    gm::LevelSelect without;
    without.build(campaign, nullptr);
    CHECK_EQ(best(without.levels(), "level1"), -1);

    std::printf("level_select: best level1=%d level2=%d level3=%d level4=%d level5=%d "
                "level6=%d level24=%d level7=%d level0=%d with_time=%d hi=%d\n",
                best(levels, "level1"), best(levels, "level2"), best(levels, "level3"),
                best(levels, "level4"), best(levels, "level5"), best(levels, "level6"),
                best(levels, "level24"), best(levels, "level7"), best(levels, "level0"),
                with_time, g_hi_loaded);
}

// --- cursor and events -----------------------------------------------------
void test_navigation() {
    gm::CampaignState campaign;
    campaign.load(1); // only mission 1, the start HQ, temple and the DM maps
    gm::LevelSelect select;
    select.build(campaign, nullptr);
    CHECK_EQ(select.selected(), 0);
    CHECK(select.current().has_value());
    CHECK_EQ(select.current()->name, "level1");

    // Locked rows are not skipped: one step from level1 lands on level2.
    select.move(1);
    CHECK_EQ(select.selected(), 1);
    CHECK_EQ(select.current()->name, "level2");
    CHECK(!select.current()->unlocked);
    std::string error;
    CHECK(!select.activate(&error));
    CHECK(select.event() == gm::LevelSelectEvent::locked);
    CHECK(error.find("locked") != std::string::npos);
    CHECK(error.find("level2") != std::string::npos);
    CHECK(error.find("counter is 1") != std::string::npos);
    CHECK(select.launched().empty());

    // Clamp at both ends, never wrap.
    select.move(1000);
    CHECK_EQ(select.selected(), static_cast<int>(select.levels().size()) - 1);
    CHECK_EQ(select.current()->name, "dm4");
    select.move(-1000);
    CHECK_EQ(select.selected(), 0);
    select.move(0);
    CHECK_EQ(select.selected(), 0);

    // A successful activation carries the stem.
    CHECK(select.activate(&error));
    CHECK(select.event() == gm::LevelSelectEvent::launch);
    CHECK_EQ(select.launched(), "level1");
    CHECK(select.last_error().empty());

    // A move clears the event.
    select.move(1);
    CHECK(select.event() == gm::LevelSelectEvent::none);

    // select() is case-insensitive and refuses unknown names.
    CHECK(select.select("LEVEL2"));
    CHECK_EQ(select.selected(), 1);
    CHECK(select.select("level20"));
    CHECK_EQ(select.selected(), 19);
    CHECK(select.current()->mission == 20);
    CHECK(select.select("level1"));
    CHECK_EQ(select.selected(), 0);
    error.clear();
    CHECK(!select.select("nosuch", &error));
    CHECK_EQ(select.selected(), 0);
    CHECK(error.find("unknown level") != std::string::npos);
    std::printf("level_select: unknown level: error=\"%s\"\n", error.c_str());

    // Esc is its own event and drops the launch.
    select.back();
    CHECK(select.event() == gm::LevelSelectEvent::back);
    CHECK(select.launched().empty());

    // Never built: the list is empty and activate() explains itself.
    gm::LevelSelect empty;
    CHECK(empty.levels().empty());
    CHECK(!empty.current().has_value());
    CHECK_EQ(empty.selected(), 0);
    empty.move(3);
    CHECK(empty.levels().empty());
    error.clear();
    CHECK(!empty.activate(&error));
    CHECK(error.find("no levels") != std::string::npos);
    std::printf("level_select: empty list: error=\"%s\"\n", error.c_str());

    // A counter below 1 is refused by build(); CampaignState cannot produce one
    // (load() clamps to 1, `0x442973`), the guard is for a future caller.
    gm::CampaignState clamped;
    clamped.load(0);
    CHECK_EQ(clamped.counter(), 1);
    CHECK_EQ(gm::kCampaignFirstCounter, 1);
}

// --- secrets ---------------------------------------------------------------
void test_secrets() {
    gm::CampaignState campaign;
    campaign.load(5); // level4 is being played
    campaign.add_secret();
    campaign.add_secret();
    campaign.add_secret();
    CHECK_EQ(campaign.secrets(), 3);

    gm::LevelSelect select;
    select.build(campaign, nullptr);
    const std::vector<gm::LevelEntry>& levels = select.levels();
    const gm::LevelEntry* level4 = find(levels, "level4");
    CHECK(level4 != nullptr);
    if (level4 != nullptr) {
        CHECK_EQ(level4->secrets_found, 3);
        CHECK_EQ(level4->secrets_declared, 5);
    }
    CHECK_EQ(find(levels, "level3")->secrets_found, 0);
    CHECK_EQ(find(levels, "level5")->secrets_found, 0);
    CHECK_EQ(find(levels, "level0")->secrets_found, 0);

    // The tally is reset at every level start (`0x45280f`), so a rebuild after
    // a counter step no longer attributes it to level4.
    gm::CampaignState next;
    next.load(6);
    next.add_secret();
    gm::LevelSelect moved;
    moved.build(next, nullptr);
    CHECK_EQ(find(moved.levels(), "level5")->secrets_found, 1);
    CHECK_EQ(find(moved.levels(), "level4")->secrets_found, 0);

    std::printf("level_select: secrets level4=%d/%d current_level5=%d declared_total=%d "
                "declaring=%d\n",
                level4 != nullptr ? level4->secrets_found : -1,
                level4 != nullptr ? level4->secrets_declared : -1,
                find(moved.levels(), "level5")->secrets_found, declared_total(levels),
                declaring_levels(levels));
}

// --- determinism -----------------------------------------------------------
void test_determinism() {
    gm::CampaignState campaign;
    campaign.load(13);
    campaign.add_secret();
    gm::Records records; // empty table, no file read: times stay -1
    gm::LevelSelect first;
    first.build(campaign, &records);
    gm::LevelSelect second;
    second.build(campaign, &records);
    const std::string a = dump(first);
    const std::string b = dump(second);
    CHECK_EQ(a, b);
    CHECK_EQ(count_unlocked(first.levels()), count_unlocked(second.levels()));

    // A rebuild resets the selection and the event.
    first.move(7);
    CHECK(first.selected() != 0);
    first.build(campaign, &records);
    CHECK_EQ(first.selected(), 0);
    CHECK(first.event() == gm::LevelSelectEvent::none);

    std::printf("level_select: determinism dump=%d chars unlocked=%d selected=%d\n",
                static_cast<int>(a.size()), count_unlocked(first.levels()), first.selected());
}

} // namespace

int main() {
    test_list();
    test_unlock();
    test_times();
    test_navigation();
    test_secrets();
    test_determinism();

    if (g_failures != 0) {
        std::fprintf(stderr, "level_select: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("level_select: ok (%d checks) levels=36 missions=24 hq=5 secret=3 dm=4 "
                "declared=101 first=level1 last=dm4 hi=%d\n",
                g_checks, g_hi_loaded);
    return 0;
}
