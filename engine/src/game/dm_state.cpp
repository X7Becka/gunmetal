// Deathmatch match state (`M9.2`, slice 2; docs/DM_STATE.md).
//
// Pure rules module: the proven facts and every model decision are listed in
// `game/dm_state.h`.  This file only turns them into code -- no runtime, no
// VM, no clock read, no RNG, no allocation on the tick path except the
// `std::string` of an error note.
//
// The two `configure()` forms share `install()`:
//   * `configure(settings, &error)` validates first and refuses the whole
//     record on a violation (previous settings and the match stay), which is
//     what a caller that parses a config wants;
//   * `configure(settings)` clamps instead (a match must start even with a bad
//     limiter) and stores a one-line note about what was clamped.

#include "game/dm_state.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

namespace gm {
namespace {

/// True for the four values the executable compares against
/// (`DM_RULES.md` §2.1); everything else is not a mode.
bool known_mode(int value) noexcept {
    return value >= static_cast<int>(DmMode::campaign) && value <= static_cast<int>(DmMode::hunt);
}

/// First reason why `settings` cannot be used as is, or `nullptr`.
///
/// The mode is checked as an integer because `DmMode` is a plain enum class and
/// a caller can cast an arbitrary value into it; the flag mask is limited to
/// `kDmKnownFlags` (only 0x01..0x20 have a proven meaning, `DM_RULES.md` §2.2).
const char* settings_error(const DmSettings& settings) noexcept {
    if (!known_mode(static_cast<int>(settings.mode))) {
        return "dm: unknown game mode value (expected 0..3, DM_RULES.md 2.1)";
    }
    if ((settings.flags & ~kDmKnownFlags) != 0) {
        return "dm: unknown mp_flags bits (only 0x01..0x20 are proven, DM_RULES.md 2.2)";
    }
    if (settings.frag_limit < 0) {
        return "dm: negative frag_limit (0 means no limit, DM_RULES.md 3.4)";
    }
    if (settings.time_limit_ticks < 0) {
        return "dm: negative time_limit_ticks (0 means no limit, DM_RULES.md 3.4)";
    }
    if (settings.respawn_delay_ticks < 0) {
        return "dm: negative respawn_delay_ticks (DM_RULES.md 4.3)";
    }
    if (settings.handicap < 0) {
        return "dm: negative handicap (0 means off, DM_RULES.md 5)";
    }
    return nullptr;
}

/// Clamp `settings` into the accepted range and append a note per clamp.
DmSettings clamped_settings(const DmSettings& settings, std::string& notes) {
    DmSettings out = settings;
    if (!known_mode(static_cast<int>(out.mode))) {
        notes += "dm: mode value ";
        notes += std::to_string(static_cast<int>(out.mode));
        notes += " clamped to campaign (0..3, DM_RULES.md 2.1); ";
        out.mode = DmMode::campaign;
    }
    if ((out.flags & ~kDmKnownFlags) != 0) {
        char bits[32];
        std::snprintf(bits, sizeof(bits), "0x%02x", out.flags & ~kDmKnownFlags);
        notes += "dm: unknown mp_flags bits ";
        notes += bits;
        notes += " dropped (DM_RULES.md 2.2); ";
        out.flags &= kDmKnownFlags;
    }
    if (out.frag_limit < 0) {
        notes += "dm: frag_limit ";
        notes += std::to_string(out.frag_limit);
        notes += " clamped to 0 (no limit, DM_RULES.md 3.4); ";
        out.frag_limit = 0;
    }
    if (out.time_limit_ticks < 0) {
        notes += "dm: time_limit_ticks ";
        notes += std::to_string(out.time_limit_ticks);
        notes += " clamped to 0 (no limit, DM_RULES.md 3.4); ";
        out.time_limit_ticks = 0;
    }
    if (out.respawn_delay_ticks < 0) {
        notes += "dm: respawn_delay_ticks ";
        notes += std::to_string(out.respawn_delay_ticks);
        notes += " clamped to 0 (DM_RULES.md 4.3); ";
        out.respawn_delay_ticks = 0;
    }
    if (out.handicap < 0) {
        notes += "dm: handicap ";
        notes += std::to_string(out.handicap);
        notes += " clamped to 0 (off, DM_RULES.md 5); ";
        out.handicap = 0;
    }
    return out;
}

} // namespace

int dm_handicap_percent(int frag_diff) noexcept {
    // `DM_RULES.md` §5: the comparisons are `cmp eax, [max - N]` + `jge`, i.e.
    // the step applies while `max - mine > N` (strictly greater).
    for (const DmHandicapStep& step : kDmHandicapLadder) {
        if (frag_diff > step.diff_gt) {
            return step.percent;
        }
    }
    return 0;
}

void DmState::install(const DmSettings& settings) {
    settings_ = settings;
    players_.clear();
    elapsed_ = 0;
    time_left_ = settings_.time_limit_ticks; // 0 = no match clock
    frag_limit_reached_ = false;
}

void DmState::configure(const DmSettings& settings) {
    std::string notes;
    const DmSettings fixed = clamped_settings(settings, notes);
    install(fixed);
    last_error_ = std::move(notes);
}

bool DmState::configure(const DmSettings& settings, std::string* error) {
    if (const char* why = settings_error(settings)) {
        last_error_ = why;
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }
    install(settings);
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

int DmState::add_player(int team) {
    if (player_count() >= kDmMaxPlayers) {
        // `DM_RULES.md` §4.1: exactly 8 slots per map, slot = player.
        last_error_ = "dm: no free player slot (8 slots, DM_RULES.md 4.1)";
        return -1;
    }
    if (team < 0) {
        last_error_ = "dm: negative team id clamped to 0 (DM_RULES.md 6)";
        team = 0;
    }
    DmPlayer player;
    player.team = team;
    players_.push_back(player);
    return player_count() - 1;
}

void DmState::on_kill(int victim, int killer) {
    if (finished()) {
        last_error_ = "dm: kill ignored, the match is finished";
        return;
    }
    if (victim < 0 || victim >= player_count()) {
        last_error_ = "dm: victim " + std::to_string(victim) + " outside 0.." +
                      std::to_string(player_count() - 1) + ", kill ignored";
        return;
    }
    DmPlayer& target = players_[static_cast<std::size_t>(victim)];
    if (!target.alive) {
        // `0x453700`: `action_state == 0 || == 7` returns at once.
        last_error_ = "dm: victim " + std::to_string(victim) +
                      " is already dead, kill ignored (DM_RULES.md 3.2)";
        return;
    }
    last_error_.clear();

    // `0x4537af`: a killer outside 0..7 or the victim itself jumps to the
    // victim-penalty branch `0x453875`; the same slot list is the player list
    // here, so "outside" means outside `player_count()` (world kill).
    const bool valid_killer = killer >= 0 && killer < player_count() && killer != victim;
    if (valid_killer) {
        DmPlayer& attacker = players_[static_cast<std::size_t>(killer)];
        if (dm_has_flag(settings_.flags, DmFlag::team) && attacker.team == target.team) {
            // `0x4537c8` team branch: the guard tests the **victim's** frags
            // (`0x4537f5`) and the decrement hits the **killer** (`0x4537fd`).
            // No clamp on the killer is proven, so the value may go below 0.
            if (target.frags > 0) {
                --attacker.frags;
            }
        } else {
            // `0x453812`: `frags[killer] += 1`; the victim's frags stay.
            ++attacker.frags;
        }
        attacker.score = attacker.frags; // MODEL: score mirrors frags
    } else if (target.frags > 0) {
        // `0x453875`/`0x45387f`: suicide or a world kill costs the victim one
        // frag, but only when it had one ("нет фрагов — нечего отнимать").
        --target.frags;
    }

    // MODEL: death bookkeeping (the original has no death counter, §5) and the
    // respawn timer (`+0x33d4 = 0` at 0x45372f, 136 ticks to respawn, §4.3).
    ++target.deaths;
    target.score = target.frags;
    target.respawn_ticks = std::max(0, settings_.respawn_delay_ticks);
    target.alive = target.respawn_ticks == 0;

    if (settings_.frag_limit > 0) {
        for (const DmPlayer& player : players_) {
            if (player.frags >= settings_.frag_limit) {
                frag_limit_reached_ = true;
                break;
            }
        }
    }
}

void DmState::tick(std::uint64_t ticks) {
    for (std::uint64_t step = 0; step < ticks; ++step) {
        if (finished()) {
            return; // the match clock stops with the match
        }
        ++elapsed_;
        for (DmPlayer& player : players_) {
            if (player.alive) {
                continue;
            }
            if (player.respawn_ticks > 0) {
                --player.respawn_ticks;
            }
            if (player.respawn_ticks == 0) {
                player.alive = true; // `0x452730(player, 0, 1)` respawn (§4.3)
            }
        }
        if (settings_.time_limit_ticks > 0 && time_left_ > 0) {
            --time_left_; // `0x457230` subtracts the tick delta (§3.4)
        }
    }
}

bool DmState::finished() const {
    if (frag_limit_reached_) {
        return true;
    }
    return settings_.time_limit_ticks > 0 && time_left_ <= 0;
}

int DmState::respawn_left(int player) const {
    if (player < 0 || player >= player_count()) {
        return -1;
    }
    const DmPlayer& entry = players_[static_cast<std::size_t>(player)];
    return entry.alive ? 0 : entry.respawn_ticks;
}

bool DmState::is_friendly(int a, int b) const {
    if (a < 0 || b < 0 || a >= player_count() || b >= player_count() || a == b) {
        return false;
    }
    // `DM_RULES.md` §3.2 gates the team-kill check on `mp_flags & 1`; extending
    // it to CTF (`& 2`, §2.2) is a model (see the header).
    const bool team_game = dm_has_flag(settings_.flags, DmFlag::team) ||
                           dm_has_flag(settings_.flags, DmFlag::ctf);
    if (!team_game) {
        return false;
    }
    return players_[static_cast<std::size_t>(a)].team ==
           players_[static_cast<std::size_t>(b)].team;
}

int DmState::winner() const {
    if (!finished() || players_.empty()) {
        return -1;
    }
    int best = 0;
    int best_frags = players_.front().frags;
    bool tie = false;
    for (int index = 1; index < player_count(); ++index) {
        const int frags = players_[static_cast<std::size_t>(index)].frags;
        if (frags > best_frags) {
            best = index;
            best_frags = frags;
            tie = false;
        } else if (frags == best_frags) {
            tie = true;
        }
    }
    // MODEL: a draw has no tie-break here; the original only sorts the score
    // table by frags (`0x486afc`, §3.3) and never ends a DM match.
    return tie ? -1 : best;
}

std::string_view DmState::mode_name(DmMode mode) noexcept {
    switch (mode) {
    case DmMode::campaign:
        return "campaign";
    case DmMode::normal:
        return "normal";
    case DmMode::classic:
        return "classic";
    case DmMode::hunt:
        return "hunt";
    }
    return "unknown";
}

int DmState::time_left() const {
    if (settings_.time_limit_ticks <= 0) {
        return 0;
    }
    return time_left_;
}

const DmPlayer* DmState::player(int index) const {
    if (index < 0 || index >= player_count()) {
        return nullptr;
    }
    return &players_[static_cast<std::size_t>(index)];
}

} // namespace gm
