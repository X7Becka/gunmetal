// Campaign progress (`M9.1`, slice 2; docs/CAMPAIGN_STATE.md).
//
// Pure state module: the proven facts and every model decision are listed in
// `game/campaign_state.h`.  This file holds the 36-row transition table of
// `docs/CAMPAIGN_FLOW.md` §8 (the 60 `NEW_LEVEL` + 6 `NEXT_LEVEL` sites), the
// counter steps of §2/§3 and the 0x40-byte `GUNMETAL.HI` codec of §6.  No
// runtime, no VM, no clock, no RNG; the only I/O is the two hi-score calls.
//
// The table keeps every target of the shipped scripts, including the branches
// the campaign does not take (the `DEATHMATCH_GAME` ones and the two secret
// levels), because their site counts are what makes 66 checkable: the sum of
// `new_level_sites` is 60 and exactly six rows carry a `NEXT_LEVEL` site.

#include "game/campaign_state.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

namespace gm {
namespace {

/// ASCII lowercase, no locale: stems are compared the way the engine names
/// files, and names the way `0x416850` compares them.
char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// `GUNMETAL.HI` stores the seconds as a little-endian i32 (§6); both helpers
/// spell the byte order out instead of reinterpreting memory.
std::int32_t read_i32(const unsigned char* p) noexcept {
    const std::uint32_t value = static_cast<std::uint32_t>(p[0]) |
                                (static_cast<std::uint32_t>(p[1]) << 8) |
                                (static_cast<std::uint32_t>(p[2]) << 16) |
                                (static_cast<std::uint32_t>(p[3]) << 24);
    return static_cast<std::int32_t>(value);
}

void write_i32(unsigned char* p, std::int32_t value) noexcept {
    const std::uint32_t raw = static_cast<std::uint32_t>(value);
    p[0] = static_cast<unsigned char>(raw & 0xFF);
    p[1] = static_cast<unsigned char>((raw >> 8) & 0xFF);
    p[2] = static_cast<unsigned char>((raw >> 16) & 0xFF);
    p[3] = static_cast<unsigned char>((raw >> 24) & 0xFF);
}

// --- the 36 shipped levels -------------------------------------------------
//
// `docs/CAMPAIGN_FLOW.md` §8, one row per level.  `new_level_sites` counts every
// `NEW_LEVEL` in the completion script(s) of that level (campaign, deathmatch
// and secret branches), `next_level_sites` the `NEXT_LEVEL` ones.  Column
// meanings:
//   * `target` is the HQ a mission returns to (its `NEW_LEVEL` stem); the HQ
//     then runs `NEXT_LEVEL`, which is why the counter picks the mission;
//   * `campaign_alt` is `level6`'s second accepted HQ (§3: two `-8`
//     sub-scripts, `0xdad9` and `0xdc02`);
//   * `campaign_end` is `level24`: `-8` `0xf799` is `IF DEATHMATCH_GAME {
//     NEW_LEVEL "DM1" }` and nothing else (§7);
//   * DM and secret levels are `other`: their `NEW_LEVEL` branches exist in the
//     data but not in the campaign.
constexpr CampaignLevel kLevels[] = {
    {"level0", "HEADQUARTERS", CampaignRole::hq, 0, 1, CampaignExit::next_level, "", ""},
    {"level00", "HEADQUARTERS", CampaignRole::hq, 0, 1, CampaignExit::next_level, "", ""},
    {"level000", "HEADQUARTERS", CampaignRole::hq, 0, 1, CampaignExit::next_level, "", ""},
    {"level0a", "HEADQUARTERS", CampaignRole::hq, 0, 1, CampaignExit::next_level, "", ""},
    {"level0b", "COMMAND POST", CampaignRole::hq, 0, 1, CampaignExit::next_level, "", ""},
    {"level1", "TESTING RANGE", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level00", ""},
    {"level2", "POWER STATION", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level00", ""},
    {"level3", "NATAKA TOWER", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level00", ""},
    {"level4", "OIL RIG", CampaignRole::mission, 3, 0, CampaignExit::new_level, "level00", ""},
    {"level5", "OIL REFINERY", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level00", ""},
    {"level6", "CADTLELTL", CampaignRole::mission, 4, 0, CampaignExit::new_level, "level00",
     "level0a"},
    {"level7", "GHOST TOWN", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level00", ""},
    {"level8", "ARGUS BASE", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level00", ""},
    {"level9", "VIRUS LAB", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level000", ""},
    {"level10", "SPACEPORT", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level000", ""},
    {"level11", "ISLAND FORTRESS", CampaignRole::mission, 2, 0, CampaignExit::new_level,
     "level000", ""},
    {"level12", "SPACE STATION", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level000",
     ""},
    {"level13", "MINING ASTEROID", CampaignRole::mission, 2, 0, CampaignExit::new_level,
     "level000", ""},
    {"level14", "DIVINE LOGIC", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level000",
     ""},
    {"level15", "SPARTA", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level000", ""},
    {"level16", "CULT MOON", CampaignRole::mission, 3, 0, CampaignExit::new_level, "level000", ""},
    {"level17", "WEAPONS FACILITY", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level0a",
     ""},
    {"level18", "NATAKA ARCOLOGY", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level0a",
     ""},
    {"level19", "WASTELAND", CampaignRole::mission, 1, 1, CampaignExit::next_level, "", ""},
    {"level20", "BASE ATTACK", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level0b", ""},
    {"level21", "THE RUINS", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level0b", ""},
    {"level22", "TELEPORT DEPOT", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level0b",
     ""},
    {"level23", "TANK FACTORY", CampaignRole::mission, 2, 0, CampaignExit::new_level, "level0b", ""},
    {"level24", "KELLER CORE", CampaignRole::mission, 1, 0, CampaignExit::campaign_end, "", ""},
    {"dm1", "TRAILER PARK", CampaignRole::deathmatch, 1, 0, CampaignExit::other, "", ""},
    {"dm2", "FOREST", CampaignRole::deathmatch, 1, 0, CampaignExit::other, "", ""},
    {"dm3", "NIAC COMBAT ARENA", CampaignRole::deathmatch, 1, 0, CampaignExit::other, "", ""},
    {"dm4", "DEATHMATCH 'TILL DAWN", CampaignRole::deathmatch, 1, 0, CampaignExit::other, "", ""},
    {"secrt1", "SECRET LEVEL - FOUNDRY", CampaignRole::secret, 2, 0, CampaignExit::other, "", ""},
    {"secrt2", "SECRET LEVEL - PROCESSOR", CampaignRole::secret, 2, 0, CampaignExit::other, "",
     ""},
    {"temple", "DM5 - TEMPLE", CampaignRole::secret, 2, 0, CampaignExit::other, "", ""},
};

static_assert(sizeof(kLevels) / sizeof(kLevels[0]) == static_cast<std::size_t>(kCampaignLevelCount),
              "one row per shipped level (CAMPAIGN_FLOW.md 8)");

/// Read a whole file into *out*; false when it cannot be opened or read.
bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char buffer[4096];
    while (in.read(buffer, static_cast<std::streamsize>(sizeof(buffer))) || in.gcount() > 0) {
        out.append(buffer, static_cast<std::size_t>(in.gcount()));
    }
    return !in.bad();
}

} // namespace

std::span<const CampaignLevel> campaign_levels() noexcept {
    return std::span<const CampaignLevel>(kLevels, sizeof(kLevels) / sizeof(kLevels[0]));
}

const CampaignLevel* campaign_level(std::string_view stem) noexcept {
    for (const CampaignLevel& row : kLevels) {
        if (ascii_iequals(stem, row.stem)) {
            return &row;
        }
    }
    return nullptr;
}

bool CampaignState::fail(std::string text, std::string* error) const {
    last_error_ = std::move(text);
    if (error != nullptr) {
        *error = last_error_;
    }
    return false;
}

void CampaignState::new_game() {
    // `0x442a0e`/`0x442a4c`/`0x442cf7`: name LEVEL0 from `0x4ab6d4`, counter 1.
    counter_ = kCampaignFirstCounter;
    stats_ = CampaignStats{};
    next_stem_ = "level0";
    last_error_.clear();
}

void CampaignState::load(int counter) {
    last_error_.clear();
    stats_ = CampaignStats{};
    next_stem_.clear(); // the level comes from the save header, not the counter
    if (counter < kCampaignFirstCounter) {
        // `0x45586e` reads F2 as is; the clamp is the same floor the restart
        // path uses (`0x442973`), applied here so the model never has a
        // meaningless counter.
        counter_ = kCampaignFirstCounter;
        last_error_ = "campaign: counter " + std::to_string(counter) +
                      " clamped to 1 (CAMPAIGN_FLOW.md 2)";
        return;
    }
    counter_ = counter;
}

bool CampaignState::new_level() {
    last_error_.clear();
    if (counter_ >= kCampaignLastCounter) {
        // MODEL: `0x457b26` increments blindly; there is no `level25` in the
        // shipped 24-mission campaign (§1), so the model stops here.
        return fail("campaign: counter 25 is level24, the campaign has no mission 26 "
                    "(CAMPAIGN_FLOW.md 1/7)",
                    nullptr);
    }
    ++counter_; // `0x457b26`..`0x457b2f`
    return true;
}

bool CampaignState::next_level(const std::string& level) {
    last_error_.clear();
    const CampaignLevel* row = campaign_level(level);
    if (row == nullptr) {
        return fail("campaign: unknown level '" + level +
                        "' (24 missions + 5 HQs, CAMPAIGN_FLOW.md 1/8)",
                    nullptr);
    }
    switch (row->exit) {
    case CampaignExit::new_level:
        // `0x457af3`: copy the stem, the counter does not move (§3).
        next_stem_ = std::string(row->target);
        return true;
    case CampaignExit::next_level:
        // `0x457b17`: `sprintf(buf, "LEVEL%ld", counter)`, then the increment.
        if (counter_ >= kCampaignLastCounter) {
            return fail("campaign: NEXT_LEVEL at counter 25 would load LEVEL25, there is no such "
                        "mission (CAMPAIGN_FLOW.md 1)",
                        nullptr);
        }
        next_stem_ = "level" + std::to_string(counter_);
        ++counter_;
        if (row->role == CampaignRole::hq) {
            ++stats_.hq_visits;
        }
        return true;
    case CampaignExit::campaign_end:
        // §7: `level24`'s only exit is the DEATHMATCH_GAME branch.
        return fail("campaign: level24 (KELLER CORE) has no campaign exit; its -8 only leaves for "
                    "DEATHMATCH_GAME (CAMPAIGN_FLOW.md 7)",
                    nullptr);
    case CampaignExit::other:
        return fail("campaign: '" + level +
                        "' is not on the campaign path (CAMPAIGN_FLOW.md 1/8)",
                    nullptr);
    }
    return false;
}

void CampaignState::restart_level_from_hq() {
    last_error_.clear();
    ++stats_.restarts;
    if (counter_ <= kCampaignFirstCounter) {
        // 0x442973: the clamp keeps the counter at 1.
        last_error_ = "campaign: counter already 1, the restart clamp keeps it there "
                      "(CAMPAIGN_FLOW.md 2)";
        return;
    }
    --counter_; // 0x44295e, the `param & 1` branch of 0x4428f0
}

void CampaignState::on_mission_complete() {
    ++stats_.missions;
}

void CampaignState::add_secret() {
    // 0x4536b0: `*(int*)(0x81c40c + i * 0x3510) += 1`, one per site.
    ++stats_.secrets;
}

void CampaignState::reset_level_secrets() {
    // 0x45280f: the tally is cleared on every level start (§5).
    stats_.secrets = 0;
}

void CampaignState::on_objective() {
    ++stats_.objectives_done;
}

// --- GUNMETAL.HI (§6) ------------------------------------------------------

bool CampaignState::load_hi(const std::string& path, std::string* error) {
    last_error_.clear();
    std::string data;
    if (!read_file(path, data)) {
        return fail("campaign: cannot read GUNMETAL.HI '" + path + "'", error);
    }
    if (data.size() % static_cast<std::size_t>(kHiRecordSize) != 0) {
        return fail("campaign: GUNMETAL.HI '" + path + "' is " + std::to_string(data.size()) +
                        " bytes, not a multiple of 0x40 (CAMPAIGN_FLOW.md 6)",
                    error);
    }
    const std::size_t count = data.size() / static_cast<std::size_t>(kHiRecordSize);
    if (count > static_cast<std::size_t>(kHiRecords)) {
        return fail("campaign: GUNMETAL.HI has " + std::to_string(count) +
                        " records, the model holds 9 (576 bytes, CAMPAIGN_FLOW.md 6)",
                    error);
    }

    // Parse into locals: a refused file leaves the loaded table untouched.
    std::array<int, kHiRecords> times{};
    std::array<std::string, kHiRecords> names{};
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* record =
            reinterpret_cast<const unsigned char*>(data.data()) +
            i * static_cast<std::size_t>(kHiRecordSize);
        times[i] = read_i32(record);
        const char* raw = data.data() + i * static_cast<std::size_t>(kHiRecordSize) + 4;
        std::size_t length = 0;
        while (length < static_cast<std::size_t>(kHiNameSize) && raw[length] != '\0') {
            ++length;
        }
        names[i].assign(raw, length);
    }
    best_times = times;
    best_names = names;
    hi_count_ = static_cast<int>(count);
    return true;
}

bool CampaignState::save_hi(const std::string& path, std::string* error) const {
    last_error_.clear();
    if (path.empty()) {
        return fail("campaign: save_hi needs a path (CAMPAIGN_FLOW.md 6)", error);
    }
    std::string data(static_cast<std::size_t>(hi_count_) *
                         static_cast<std::size_t>(kHiRecordSize),
                     '\0');
    for (int i = 0; i < hi_count_; ++i) {
        const std::string& name = best_names[static_cast<std::size_t>(i)];
        if (name.size() > static_cast<std::size_t>(kHiNameSize)) {
            return fail("campaign: GUNMETAL.HI name is " + std::to_string(name.size()) +
                            " bytes, the field is 60 (CAMPAIGN_FLOW.md 6)",
                        error);
        }
        if (name.find('\0') != std::string::npos) {
            return fail("campaign: GUNMETAL.HI name contains a NUL (CAMPAIGN_FLOW.md 6)", error);
        }
        unsigned char* record = reinterpret_cast<unsigned char*>(data.data()) +
                                static_cast<std::size_t>(i) *
                                    static_cast<std::size_t>(kHiRecordSize);
        write_i32(record, best_times[static_cast<std::size_t>(i)]);
        // The rest of the 60 bytes is already NUL padding.
        std::copy(name.begin(), name.end(), data.begin() +
                                               static_cast<std::ptrdiff_t>(i * kHiRecordSize + 4));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail("campaign: cannot write GUNMETAL.HI '" + path + "'", error);
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        return fail("campaign: write error on GUNMETAL.HI '" + path + "'", error);
    }
    return true;
}

bool CampaignState::set_hi_record(int index, int seconds, const std::string& name,
                                  std::string* error) {
    last_error_.clear();
    if (index < 0 || index >= hi_count_) {
        return fail("campaign: GUNMETAL.HI record " + std::to_string(index) + " out of range [0, " +
                        std::to_string(hi_count_) + ") (CAMPAIGN_FLOW.md 6)",
                    error);
    }
    if (seconds < 0) {
        return fail("campaign: negative GUNMETAL.HI time " + std::to_string(seconds) +
                        " (CAMPAIGN_FLOW.md 6)",
                    error);
    }
    if (name.empty()) {
        return fail("campaign: empty GUNMETAL.HI name (SET_LEVEL_NAME, CAMPAIGN_FLOW.md 6)", error);
    }
    if (name.size() > static_cast<std::size_t>(kHiNameSize)) {
        return fail("campaign: GUNMETAL.HI name is " + std::to_string(name.size()) +
                        " bytes, the field is 60 (CAMPAIGN_FLOW.md 6)",
                    error);
    }
    if (name.find('\0') != std::string::npos) {
        return fail("campaign: GUNMETAL.HI name contains a NUL (CAMPAIGN_FLOW.md 6)", error);
    }
    best_times[static_cast<std::size_t>(index)] = seconds;
    best_names[static_cast<std::size_t>(index)] = name;
    return true;
}

bool CampaignState::set_hi_time(int index, int seconds, std::string* error) {
    if (index < 0 || index >= hi_count_) {
        last_error_.clear();
        return fail("campaign: GUNMETAL.HI record " + std::to_string(index) + " out of range [0, " +
                        std::to_string(hi_count_) + ") (CAMPAIGN_FLOW.md 6)",
                    error);
    }
    return set_hi_record(index, seconds, best_names[static_cast<std::size_t>(index)], error);
}

int CampaignState::best_time(std::string_view name) const {
    // 0x45c8d6: minimum over the records whose name matches (0x416850,
    // case-insensitive); duplicates keep the better time.
    int best = -1;
    for (int i = 0; i < hi_count_; ++i) {
        if (!ascii_iequals(best_names[static_cast<std::size_t>(i)], name)) {
            continue;
        }
        if (best < 0 || best_times[static_cast<std::size_t>(i)] < best) {
            best = best_times[static_cast<std::size_t>(i)];
        }
    }
    return best;
}

} // namespace gm
