// Shield / armor regeneration model (`M6.1.3`, `docs/REGEN.md`).
//
// The original has no readable regeneration values (opcode 174 writes two
// doubles that nothing in 1.36 reads), so nothing here is a port: this is the
// pure rule/tick arithmetic documented in `game/regen.h`.  `double` only, a
// fixed order of operations and no clock/RNG, so the result is deterministic
// and independent of the platform.
#include "game/regen.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gm {
namespace {

const char* check_rule(const RegenRule& rule) {
    if (rule.delay_ticks < 0) {
        return "regen: delay_ticks must be >= 0";
    }
    if (rule.delay_ticks > kRegenMaxDelayTicks) {
        return "regen: delay_ticks must be <= 1000000";
    }
    if (!std::isfinite(rule.rate_per_tick)) {
        return "regen: rate_per_tick must be finite";
    }
    if (rule.rate_per_tick < 0.0) {
        return "regen: rate_per_tick must be >= 0";
    }
    if (rule.rate_per_tick > kRegenMaxRatePerTick) {
        return "regen: rate_per_tick must be <= 1000000";
    }
    if (!rule.shields && !rule.armor) {
        return "regen: a rule must enable shields, armor or both";
    }
    return nullptr;
}

int room(int value, int max_value) {
    if (max_value <= 0 || value >= max_value) {
        return 0;
    }
    return max_value - std::max(value, 0);
}

} // namespace

const char* regen_rule_error(const RegenRule& rule) { return check_rule(rule); }

bool apply_regen(Vitals& vitals, RegenState& state, const RegenRule& rule,
                 bool damaged_this_tick) {
    if (check_rule(rule) != nullptr) {
        return false;
    }

    if (damaged_this_tick) {
        // The damage tick itself restores nothing and a new window starts on
        // the next tick, exactly like the "delay after the last hit" reading
        // of the model.  The carry of the previous window is not inherited.
        state.since_damage = 0;
        state.carry = 0.0;
        return true;
    }

    if (state.since_damage < std::numeric_limits<int>::max()) {
        ++state.since_damage;
    }
    if (state.since_damage <= rule.delay_ticks) {
        return true; // still inside the delay window
    }

    // Deficit check first: a full gauge set drops the accumulated fraction,
    // so it cannot burst out later when damage finally arrives.
    bool deficit = false;
    if (rule.shields) {
        for (std::size_t i = 0; i < vitals.shields.size(); ++i) {
            if (room(vitals.shields[i], vitals.shield_max) > 0) {
                deficit = true;
                break;
            }
        }
    }
    if (!deficit && rule.armor && room(vitals.armor, vitals.armor_max) > 0) {
        deficit = true;
    }
    if (!deficit) {
        state.carry = 0.0;
        return true;
    }

    const double budget = state.carry + rule.rate_per_tick;
    const double whole = std::floor(budget);
    state.carry = budget - whole; // always in [0, 1)
    int remaining = static_cast<int>(whole);
    if (remaining <= 0) {
        return true;
    }

    // Fixed order: the four shield quadrants (front, right, back, left), then
    // the armor.  Only whole hundredths move; anything left after the last
    // deficit is dropped.
    if (rule.shields) {
        for (std::size_t i = 0; i < vitals.shields.size() && remaining > 0; ++i) {
            const int free = room(vitals.shields[i], vitals.shield_max);
            if (free <= 0) {
                continue;
            }
            const int take = std::min(free, remaining);
            vitals.shields[i] += take;
            remaining -= take;
        }
    }
    if (rule.armor && remaining > 0) {
        const int free = room(vitals.armor, vitals.armor_max);
        if (free > 0) {
            const int take = std::min(free, remaining);
            vitals.armor += take;
            remaining -= take;
        }
    }
    return true;
}

void reset_regen(RegenState& state) {
    state.since_damage = 0;
    state.carry = 0.0;
}

} // namespace gm
