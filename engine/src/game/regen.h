#pragma once

// Shield / armor regeneration in the fixed 91 Hz player tick (`M6.1.3`).
//
// Proven data (`docs/PICKUP.md` §7, `docs/DAMAGE.md` §6, `docs/OBJECTIVES.md`
// §4.3):
//   * opcode 174 is `DEFINE_REGENERATION_MARKER`, signature `ODD`
//     (`0x45c375`): dword -> `ds:0x80d878` (the SOB index of the deathmatch
//     item marker), double -> `0x80f9e0`, double -> `0x80f830`;
//   * every one of the 36 shipped `.sbt` files carries exactly one
//     `(19, 3000.0, 50.0)`;
//   * the two doubles are written only by the opcode itself (`0x45c38a`,
//     `0x45c399`) and cleared by the level loader (`0x45713f`, `0x457153`);
//     no reader exists in 1.36, so the original never consumes them and the
//     shipped game has **no shield/armor regeneration in its tick**;
//   * the live part of the opcode is the SOB marker (`0x468840`), which has
//     nothing to do with player gauges.
//
// This module is therefore a **model**, not a port of a proven routine: the
// rule fields are the caller's and the mapping of the shipped
// `(3000.0, 50.0)` to (`delay_ticks`, `rate_per_tick`) is a hypothesis
// documented in `docs/REGEN.md` §3.  The function is pure (no VM, no save, no
// runtime, no clock), so a caller can drive it from the tick or from a test.
//
// Units: `Vitals` uses the save gauges, integer hundredths (`10000 = 100.00`,
// `docs/SAVE_FIELDS.md`).  `rate_per_tick` is in those hundredths per fixed
// 91 Hz tick; `delay_ticks` is a count of fixed ticks.

#include <cstdint>

#include "game/weapons.h" // Vitals

namespace gm {

/// Upper bound on `delay_ticks` (one fixed tick is 1/91 s; a million ticks is
/// ~3 hours of game time, far beyond any level).  Keeps `since_damage` and
/// `delay_ticks + 1` inside `int`.
inline constexpr int kRegenMaxDelayTicks = 1000000;
/// Upper bound on `rate_per_tick` in save hundredths (10^6 = the whole gauge
/// per tick five times over); larger values are a typo, not a rule.
inline constexpr double kRegenMaxRatePerTick = 1000000.0;

/// One regeneration rule.  Which pools it touches is explicit because the
/// opcode does not say (hypothesis, `docs/REGEN.md` §3):
///   * `shields` — the four quadrants of `Vitals::shields`, filled in the
///     fixed order front, right, back, left up to `shield_max`;
///   * `armor`   — `Vitals::armor`, filled after the shields up to
///     `armor_max`.
/// A rule with both flags false is invalid: "regenerate nothing" is not a
/// rule, it is the default (no rule installed).
struct RegenRule {
    /// Full ticks after the last damage tick with no restoration.  The first
    /// restoring tick is `delay_ticks + 1` ticks after the damage tick.
    int delay_ticks = 0;
    /// Restoration budget per tick, in save hundredths (may be fractional; the
    /// fractional part is carried, see `RegenState::carry`).
    double rate_per_tick = 0.0;
    bool shields = true;
    bool armor = true;
};

/// Per-player regeneration clock and fractional accumulator.  It is separate
/// from `Vitals`, so a caller can save/restore it and compare it in tests.
struct RegenState {
    /// Ticks since the last damage tick; `0` on the damage tick itself.  It
    /// saturates at `kRegenMaxDelayTicks + 1`, which is enough to pass any
    /// valid delay.
    int since_damage = 0;
    /// Fraction of a gauge hundredth not spent yet (`0 <= carry < 1`).  A
    /// fractional `rate_per_tick` therefore restores exactly
    /// `floor(carry + rate)` hundredths per tick, and the total over N ticks
    /// is `floor(N * rate)` as long as there is room to spend it.
    double carry = 0.0;
};

/// Why `rule` cannot be used, or `nullptr` when it is valid.  The text is a
/// one-line reason suitable for `set_regen(..., &error)`:
///   * `delay_ticks` negative or above `kRegenMaxDelayTicks`;
///   * `rate_per_tick` negative, not finite (NaN/inf) or above
///     `kRegenMaxRatePerTick`;
///   * neither `shields` nor `armor` enabled.
const char* regen_rule_error(const RegenRule& rule);

/// Apply one fixed tick of regeneration.  Pure: only `vitals` and `state` are
/// touched, and an invalid rule (`regen_rule_error(rule) != nullptr`) returns
/// `false` **without any mutation**.
///
/// `damaged_this_tick` is the caller's flag for "some gauge decreased in this
/// tick"; it restarts the delay and drops the fractional carry.  Otherwise
/// `state.since_damage` advances and, once it is greater than
/// `rule.delay_ticks`, `state.carry + rule.rate_per_tick` whole hundredths are
/// distributed: shields first (front, right, back, left), then armor, each
/// clamped to its `*_max`, so nothing ever overfills.  A pool that is already
/// full is skipped; the unused part of the budget is dropped (not banked).
///
/// Returns `true` for a valid rule, including a tick where nothing needed
/// restoration or the delay was still running.
bool apply_regen(Vitals& vitals, RegenState& state, const RegenRule& rule,
                 bool damaged_this_tick);

/// Return `state` to the state of a new life (`since_damage = 0`,
/// `carry = 0`).  `respawn()`/`set_vitals()` of `SimPlayer` call this.
void reset_regen(RegenState& state);

} // namespace gm
