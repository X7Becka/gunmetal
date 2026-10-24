// Level-selection screen (`M8.4`/`M9.1`, slice 2; docs/LEVEL_SELECT.md).
//
// The evidence classes and every model decision live in `ui/level_select.h`.
// This file holds the one table the screen needs (kind, mission number, unlock
// counter and declared secrets of the 36 shipped levels) and the cursor/event
// logic.  No I/O: the hi-score values arrive through `Records`, the campaign
// state through `CampaignState`.

#include "ui/level_select.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "game/campaign_state.h"
#include "ui/records.h"

namespace gm {
namespace {

/// One row of the list.  `stem` is the `.sbw` name without the extension (the
/// scanner of `0x48b1c0` keeps it), `unlock_counter` is the `MODEL` threshold of
/// the header (`0` = always selectable, DM maps and `temple`), `secrets` is the
/// `SET_LEVEL_SECRETS` value of `CAMPAIGN_FLOW.md` §8.
struct Rule {
    std::string_view stem;
    LevelKind kind;
    int mission;
    int unlock_counter;
    int secrets;
};

/// Campaign order (`MODEL`, header): missions 1..24, the five HQs, the three
/// secret levels, the four DM maps last.
constexpr Rule kRules[] = {
    {"level1", LevelKind::mission, 1, 1, 4},
    {"level2", LevelKind::mission, 2, 2, 5},
    {"level3", LevelKind::mission, 3, 3, 6},
    {"level4", LevelKind::mission, 4, 4, 5},
    {"level5", LevelKind::mission, 5, 5, 4},
    {"level6", LevelKind::mission, 6, 6, 6},
    {"level7", LevelKind::mission, 7, 7, 4},
    {"level8", LevelKind::mission, 8, 8, 3},
    {"level9", LevelKind::mission, 9, 9, 4},
    {"level10", LevelKind::mission, 10, 10, 3},
    {"level11", LevelKind::mission, 11, 11, 1},
    {"level12", LevelKind::mission, 12, 12, 6},
    {"level13", LevelKind::mission, 13, 13, 3},
    {"level14", LevelKind::mission, 14, 14, 3},
    {"level15", LevelKind::mission, 15, 15, 5},
    {"level16", LevelKind::mission, 16, 16, 5},
    {"level17", LevelKind::mission, 17, 17, 3},
    {"level18", LevelKind::mission, 18, 18, 6},
    {"level19", LevelKind::mission, 19, 19, 2},
    {"level20", LevelKind::mission, 20, 20, 3},
    {"level21", LevelKind::mission, 21, 21, 6},
    {"level22", LevelKind::mission, 22, 22, 4},
    {"level23", LevelKind::mission, 23, 23, 3},
    {"level24", LevelKind::mission, 24, 24, 3},
    // Five HQs; the threshold is the counter while the last mission of the act
    // before them is played (CAMPAIGN_FLOW.md §3): act 1 ends at level8, so
    // level00 is reachable from counter 9, and so on.  level0 is the start.
    {"level0", LevelKind::headquarters, 0, 1, 0},
    {"level00", LevelKind::headquarters, 0, 9, 0},
    {"level000", LevelKind::headquarters, 0, 17, 0},
    {"level0a", LevelKind::headquarters, 0, 19, 0},
    {"level0b", LevelKind::headquarters, 0, 24, 0},
    // Secret levels: the gates live in level4 (counter 5) and level16 (17).
    {"secrt1", LevelKind::secret, 0, 5, 4},
    {"secrt2", LevelKind::secret, 0, 17, 0},
    {"temple", LevelKind::secret, 0, 0, 0},
    // Deathmatch maps: off the campaign path, always selectable.
    {"dm1", LevelKind::deathmatch, 0, 0, 0},
    {"dm2", LevelKind::deathmatch, 0, 0, 0},
    {"dm3", LevelKind::deathmatch, 0, 0, 0},
    {"dm4", LevelKind::deathmatch, 0, 0, 0},
};

static_assert(sizeof(kRules) / sizeof(kRules[0]) ==
                  static_cast<std::size_t>(LevelSelect::kLevelCount),
              "one row per shipped level (CAMPAIGN_FLOW.md 8)");

/// The rule of *stem*, ASCII case-insensitive (`RECORDS`/engine file naming);
/// nullptr when the name is not one of the 36.
const Rule* find_rule(std::string_view stem) noexcept {
    for (const Rule& rule : kRules) {
        if (record_name_equal(rule.stem, stem)) {
            return &rule;
        }
    }
    return nullptr;
}

} // namespace

// --- static facts ----------------------------------------------------------

LevelKind LevelSelect::kind_of(std::string_view stem) {
    const Rule* rule = find_rule(stem);
    return rule != nullptr ? rule->kind : LevelKind::unknown;
}

int LevelSelect::mission_of(std::string_view stem) {
    const Rule* rule = find_rule(stem);
    return rule != nullptr ? rule->mission : 0;
}

int LevelSelect::declared_secrets(std::string_view stem) {
    const Rule* rule = find_rule(stem);
    return rule != nullptr ? rule->secrets : 0;
}

int LevelSelect::unlock_counter(std::string_view stem) {
    const Rule* rule = find_rule(stem);
    return rule != nullptr ? rule->unlock_counter : -1;
}

// --- build -----------------------------------------------------------------

void LevelSelect::build(const CampaignState& campaign, const Records* records) {
    levels_.clear();
    selected_ = 0;
    counter_ = 0;
    event_ = LevelSelectEvent::none;
    launched_.clear();
    last_error_.clear();

    const int counter = campaign.counter();
    if (counter < kCampaignFirstCounter) {
        last_error_ = "campaign counter " + std::to_string(counter) + " is below " +
                      std::to_string(kCampaignFirstCounter) +
                      ": no level can be selected (CAMPAIGN_FLOW.md 2)";
        return;
    }
    counter_ = counter;

    // The tally `0x4536b0` belongs to the level being played and is reset at
    // every level start (`0x45280f`): when the counter is 1 the start HQ is
    // current (mission index 0), so no mission owns the tally.
    const int current_mission = campaign.mission_index();

    levels_.reserve(static_cast<std::size_t>(kLevelCount));
    for (const Rule& rule : kRules) {
        LevelEntry entry;
        entry.name = std::string(rule.stem);
        entry.kind = rule.kind;
        entry.mission = rule.mission;
        entry.unlocked = rule.unlock_counter == 0 || counter >= rule.unlock_counter;
        entry.completed = rule.kind == LevelKind::mission && rule.mission < counter;
        entry.secrets_declared = rule.secrets;
        entry.secrets_found =
            rule.kind == LevelKind::mission && rule.mission == current_mission
                ? campaign.secrets()
                : 0;
        entry.best_seconds = -1;
        if (records != nullptr) {
            // `GUNMETAL.HI` is keyed by `SET_LEVEL_NAME` (not the stem): the
            // screen's best time is the minimum of that name (0x45c8d6).
            if (const CampaignLevel* level = campaign_level(rule.stem); level != nullptr) {
                entry.best_seconds = records->best_time(level->name);
            }
        }
        levels_.push_back(std::move(entry));
    }
}

// --- cursor ----------------------------------------------------------------

void LevelSelect::move(int delta) {
    event_ = LevelSelectEvent::none;
    last_error_.clear();
    if (levels_.empty()) {
        return;
    }
    const int last = static_cast<int>(levels_.size()) - 1;
    // Clamp, do not wrap and do not skip: the original list has no locks and a
    // wrapping/skipping cursor would be our invention (header `MODEL`).
    selected_ = std::clamp(selected_ + delta, 0, last);
}

bool LevelSelect::select(std::string_view stem, std::string* error) {
    const std::size_t count = levels_.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (record_name_equal(levels_[i].name, stem)) {
            selected_ = static_cast<int>(i);
            event_ = LevelSelectEvent::none;
            launched_.clear();
            last_error_.clear();
            return true;
        }
    }
    event_ = LevelSelectEvent::none;
    launched_.clear();
    return fail("unknown level " + std::string(stem), error);
}

bool LevelSelect::activate(std::string* error) {
    if (levels_.empty()) {
        event_ = LevelSelectEvent::none;
        launched_.clear();
        return fail("no levels to select: the campaign state was refused (see build())", error);
    }
    const LevelEntry& entry = levels_[static_cast<std::size_t>(selected_)];
    if (!entry.unlocked) {
        const int threshold = unlock_counter(entry.name);
        event_ = LevelSelectEvent::locked;
        launched_.clear();
        return fail("level " + entry.name + " is locked: the campaign counter is " +
                        std::to_string(counter_) + ", it opens at " + std::to_string(threshold),
                    error);
    }
    event_ = LevelSelectEvent::launch;
    launched_ = entry.name;
    last_error_.clear();
    return true;
}

void LevelSelect::back() {
    event_ = LevelSelectEvent::back;
    launched_.clear();
    last_error_.clear();
}

std::optional<LevelEntry> LevelSelect::current() const {
    if (levels_.empty()) {
        return std::nullopt;
    }
    return levels_[static_cast<std::size_t>(selected_)];
}

bool LevelSelect::fail(std::string text, std::string* error) {
    last_error_ = std::move(text);
    if (error != nullptr) {
        *error = last_error_;
    }
    return false;
}

} // namespace gm
