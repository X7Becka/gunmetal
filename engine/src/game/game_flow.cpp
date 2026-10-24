// Player death / respawn / "game over" flow (`M9.5`, slice 1).
//
// Pure deterministic state machine, no game files and no runtime: the facts
// and the modelled parts are documented in `game_flow.h` and
// `docs/GAME_FLOW.md`.  The tick order is fixed:
//
//   * the one-tick `respawn_due()` pulse is cleared at the start of `tick()`;
//   * `dying` counts the timer down and only on the *next* tick decides
//     `dead` (due) or `restart_prompt` (lives exhausted, MODEL), which is the
//     "136 ticks of `action_state == 7`, respawn on tick 137" arithmetic of
//     `docs/GAME_OVER.md` §2.1;
//   * `respawning` counts down and flips to `alive` on the tick the counter
//     reaches 0, so `on_respawn()` on tick 137 plus 136 ticks is `alive` after
//     tick 273;
//   * `alive`, `dead` and `restart_prompt` hold their state (`dead` waits for
//     the caller's `on_respawn()`), but still count the tick.
#include "game/game_flow.h"

#include <string>
#include <utility>

namespace gm {
namespace {

/// First invalid setting of `config`, or `nullptr` when it is valid.
const char* config_error(const FlowConfig& config) {
    if (config.death_ticks < 0) {
        // GAME_OVER.md §2.1: the timer is a tick count, never negative.
        return "game_flow: death_ticks must be >= 0 (GAME_OVER.md 2.1)";
    }
    if (config.respawn_ticks < 0) {
        // MODEL: the respawning window is a tick count too.
        return "game_flow: respawn_ticks must be >= 0 (model, GAME_OVER.md 2.1)";
    }
    if (config.lives < -1) {
        return "game_flow: lives must be -1 (unlimited) or >= 0 (model, "
               "GAME_OVER.md 2.2)";
    }
    return nullptr;
}

/// Clamped copy of `config` plus a one-line note per clamp.
FlowConfig clamped_config(const FlowConfig& in, std::string& notes) {
    FlowConfig out = in;
    if (out.death_ticks < 0) {
        notes += "game_flow: death_ticks ";
        notes += std::to_string(out.death_ticks);
        notes += " clamped to 0 (GAME_OVER.md 2.1); ";
        out.death_ticks = 0;
    }
    if (out.respawn_ticks < 0) {
        notes += "game_flow: respawn_ticks ";
        notes += std::to_string(out.respawn_ticks);
        notes += " clamped to 0 (model); ";
        out.respawn_ticks = 0;
    }
    if (out.lives < -1) {
        notes += "game_flow: lives ";
        notes += std::to_string(out.lives);
        notes += " clamped to -1, unlimited (model); ";
        out.lives = -1;
    }
    return out;
}

} // namespace

std::string_view GameFlow::state_name(PlayerFlow state) noexcept {
    switch (state) {
    case PlayerFlow::alive:
        return "alive";
    case PlayerFlow::dying:
        return "dying";
    case PlayerFlow::dead:
        return "dead";
    case PlayerFlow::respawning:
        return "respawning";
    case PlayerFlow::restart_prompt:
        return "restart_prompt";
    }
    return "unknown";
}

void GameFlow::install(const FlowConfig& config) {
    config_ = config;
    state_ = PlayerFlow::alive;
    ticks_left_ = 0;
    lives_ = config.lives;
    respawn_due_ = false;
    deaths_ = 0;
    respawns_ = 0;
    restarts_ = 0;
    hq_returns_ = 0;
    ticks_elapsed_ = 0;
}

void GameFlow::configure(const FlowConfig& config) {
    std::string notes;
    const FlowConfig fixed = clamped_config(config, notes);
    install(fixed);
    last_error_ = std::move(notes);
}

bool GameFlow::configure(const FlowConfig& config, std::string* error) {
    if (const char* why = config_error(config)) {
        last_error_ = why;
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }
    install(config);
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void GameFlow::on_death() {
    if (state_ == PlayerFlow::dying) {
        // The original guard of `player_killed` (`0x453700`) returns for
        // `action_state == 7`, so a second death in the same life cannot reset
        // the running timer (`docs/GAME_OVER.md` §2.1).  No-op, timer kept.
        last_error_ =
            "game_flow: already dying, death ignored (player_killed 0x453700)";
        return;
    }
    if (state_ == PlayerFlow::dead || state_ == PlayerFlow::restart_prompt) {
        last_error_ = "game_flow: death ignored in state ";
        last_error_ += state_name(state_);
        last_error_ += ", restart()/return_to_hq() first";
        return;
    }
    last_error_.clear();
    // MODEL: the life budget is spent on the death itself; `-1` is unlimited
    // and the shipped game has no lives at all (GAME_OVER.md §2.2).
    if (config_.campaign && lives_ > 0) {
        --lives_;
    }
    state_ = PlayerFlow::dying;
    ticks_left_ = config_.death_ticks;
    respawn_due_ = false;
    ++deaths_;
}

void GameFlow::tick() {
    // Exactly-one-tick pulse: cleared here, set only by the expiry below.
    respawn_due_ = false;
    switch (state_) {
    case PlayerFlow::dying:
        if (ticks_left_ > 0) {
            // 0x443849..0x443853: `death_timer += dt`; here the model counts
            // down from `death_ticks` (GAME_OVER.md §2.1).
            --ticks_left_;
        } else if (config_.campaign && config_.lives >= 0 && lives_ == 0) {
            // MODEL: no lives left, so no respawn -- the flow stops at the
            // restart prompt (the only thing close to "game over", §0.2).
            state_ = PlayerFlow::restart_prompt;
        } else {
            // 0x443b0a: `death_timer >= 0x88` -> 0x452670 respawn.
            state_ = PlayerFlow::dead;
            respawn_due_ = true;
        }
        break;
    case PlayerFlow::respawning:
        if (ticks_left_ > 0) {
            --ticks_left_;
        }
        if (ticks_left_ == 0) {
            state_ = PlayerFlow::alive;
        }
        break;
    case PlayerFlow::alive:
    case PlayerFlow::dead:
    case PlayerFlow::restart_prompt:
        // `dead` waits for `on_respawn()`; nothing else runs a timer.
        break;
    }
    ++ticks_elapsed_;
}

void GameFlow::on_respawn() {
    if (state_ != PlayerFlow::dead) {
        last_error_ = "game_flow: on_respawn() outside dead (state ";
        last_error_ += state_name(state_);
        last_error_ += "), ignored";
        return;
    }
    last_error_.clear();
    state_ = PlayerFlow::respawning;
    ticks_left_ = config_.respawn_ticks;
    respawn_due_ = false;
    ++respawns_;
}

bool GameFlow::prompt_restart() {
    if (state_ == PlayerFlow::dying || state_ == PlayerFlow::dead) {
        last_error_.clear();
        state_ = PlayerFlow::restart_prompt;
        ticks_left_ = 0;
        respawn_due_ = false;
        return true;
    }
    // MODEL: the original opens the menu only while dead (0x443af9); Esc from
    // the level (0x48b54c) is outside this module.
    last_error_ = "game_flow: prompt_restart() needs dying/dead (0x443af9), state ";
    last_error_ += state_name(state_);
    return false;
}

void GameFlow::restart() {
    last_error_.clear();
    state_ = PlayerFlow::alive;
    ticks_left_ = 0;
    respawn_due_ = false;
    lives_ = config_.lives;
    ++restarts_;
}

void GameFlow::return_to_hq() {
    last_error_.clear();
    state_ = PlayerFlow::alive;
    ticks_left_ = 0;
    respawn_due_ = false;
    lives_ = config_.lives;
    ++hq_returns_;
}

} // namespace gm
