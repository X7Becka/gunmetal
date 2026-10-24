#pragma once

// Player death / respawn / "game over" flow (`M9.5`, slice 1).
//
// The original has **no separate game-over state**.  A death puts the player
// into `action_state = 7` and after a fixed 136-tick timer the normal respawn
// runs inside the same level; the only "game over" screen is the
// `RESTART GAME` menu (frontend state 6).  This module is the pure state
// machine of that flow; every fact and every modelled part is marked below.
//
// Proven in `gundx.exe` (read only, `docs/GAME_OVER.md`):
//
//   * `player_killed` `0x4536e0` sets `action_state = 7` (`0x453763`) and
//     `death_timer` `player+0x33d4` = 0 (`0x45372f`); every frame of state 7
//     adds the tick delta to the timer (`0x443849`..`0x443853`) -- §2.1;
//   * at `death_timer >= 0x88` = **136** ticks the normal respawn `0x452670`
//     is called (`0x443b0a`..`0x443b1e`): in a campaign `SET_PLAYER_START`, in
//     DM the farthest slot (§2.1, `docs/DM_RULES.md` §4.2/§4.3).  A tick is
//     1/91 s (`Sleep(11)`, `docs/PLAYER_GHIDRA.md`), so 136 ticks ~ 1.49 s;
//   * there is no `GAME OVER` screen, no life counter, no death counter and no
//     autosave on death (full scans, §0.1/§2.2); the respawn keeps the level
//     and does not touch `mission_counter` (`ds:0x80f9cc`, §2.2);
//   * the only restart screen is the menu `RESTART GAME` (frontend state 6):
//     Activate (bit `0x8000`) while dead opens it (`0x443af9`), Esc opens it
//     from the level (`0x48b54c`); its four items are "Reload last saved game"
//     (`0x48d9e9`), "Restart level with same R.P.V." (`0x48d9fd` ->
//     `0x4428f0(0)`), "Restart level from HQ" (`0x48da09` -> `0x4428f0(1)`)
//     and "Quit to title" (`0x48da31`) -- §0.2/§4.1;
//   * while `action_state == 7` the optional status panel draws
//     `PRESS ACTIVATE TO RESTART` (string `0x4bc2c8`, §2.3).
//
// Model, not a port -- every such place is marked `MODEL` below:
//
//   * `lives`: the game has no life counter (§2.2).  `lives = -1` (the
//     default) means "unlimited" and is the proven behaviour (every death is
//     followed by the 136-tick respawn); `lives >= 0` is an opt-in engine rule,
//     and `0` after a death turns the expiry into `restart_prompt` instead of a
//     respawn.
//   * `campaign`: only gates the `lives` rule.  The original behaves the same
//     in campaign and DM (death -> respawn, `0x443b0a`), so with
//     `campaign = false` the `lives` counter is kept and never prompts.
//   * `respawn_ticks`: the original respawns **immediately** when the death
//     timer expires (`0x452670` in `0x443b1e`); there is no second delay.
//     The `respawning` phase is the module's own "the respawn is running"
//     window, and its default 136 is the same proven magnitude as the death
//     timer (the direction/counter split is a model, as in
//     `docs/DM_STATE.md` §2.3).
//   * the death timer counts **down** here, `player+0x33d4` counts up to
//     `0x88` in the game (§2.1): only the magnitude (136) is proven.
//   * `prompt_restart()` is the Activate-while-dead path (`0x443af9`) without
//     its gates ([0x83d620]/[0x83d940]/[0x5a46d8], §7.3) and without the
//     menu itself; it only enters `restart_prompt`.
//   * `restart()` / `return_to_hq()`: the menu items `0x4428f0(0/1)` load a
//     level and copy a 0x3510-byte player snapshot (§4.2).  None of that is
//     modelled: only the flow transition, the timer reset and the restored
//     `lives` count.
//
// The object is pure flow state: no runtime, no VM, no clock, no RNG, no
// rendering, no save, no level loading, no menu drawing.  `tick()` is one
// fixed tick (1/91 s); two runs of the same call sequence are identical.
//
// Limits (`docs/GAME_FLOW.md` §5): not wired into `GameRuntime`/`main.cpp`,
// nothing shows the menu, no death animation/corpse/sound (the original has no
// death jingle either, §2.3), no save/autosave, no spawn-point selection
// (`0x452670` itself), no Esc-while-alive, no credits/level-end screen, and no
// HQ snapshot / mission counter / cash (§4.2).

#include <cstdint>
#include <string>
#include <string_view>

namespace gm {

/// Ticks from `player_killed` to the respawn: `player+0x33d4 >= 0x88` = 136
/// (`docs/GAME_OVER.md` §2.1; `0x45372f` resets the timer, `0x443b0a` compares
/// with `0x88`, `0x443b1e` calls `0x452670`).  At 91 Hz that is ~1.49 s.
inline constexpr int kFlowDeathTicks = 136;

/// MODEL: length of the module's own `respawning` window.  The original has
/// no second delay -- it respawns on the expiry tick (§2.1) -- so the value
/// only mirrors the proven magnitude of the death timer (`kFlowDeathTicks`).
inline constexpr int kFlowRespawnTicks = 136;

/// Flow of one player slot.  `alive`/`dying`/`dead`/`respawning` follow
/// `action_state` 7 and the `0x33d4` timer (§2.1); `restart_prompt` is the
/// modelled "no lives left" stop, the only thing close to a game-over state in
/// the original (the `RESTART GAME` menu, §0.2).
enum class PlayerFlow : std::uint8_t {
    alive = 0,          ///< playing; `action_state != 7`
    dying = 1,          ///< `action_state == 7`, `death_ticks` running
    dead = 2,           ///< timer expired, `respawn_due()`; waits for `on_respawn()`
    respawning = 3,     ///< MODEL: `respawn_ticks` window after the respawn
    restart_prompt = 4, ///< MODEL: lives exhausted / no respawn (menu state 6)
};

/// Configuration of one flow.  `configure()` validates or clamps the values.
struct FlowConfig {
    /// Ticks of `action_state == 7` before the respawn is due (§2.1).
    /// Proven default: 136.
    int death_ticks = kFlowDeathTicks;
    /// MODEL: ticks of the `respawning` window (136 = same magnitude, §2.1).
    int respawn_ticks = kFlowRespawnTicks;
    /// MODEL: only gates `lives`; the original behaves the same in both modes.
    bool campaign = true;
    /// MODEL: `-1` = unlimited (the proven behaviour, §2.2); `0..N` = an
    /// engine rule: after the death that spends the last life the flow stops in
    /// `restart_prompt` instead of respawning.
    int lives = -1;
};

/// Deterministic death/respawn/restart flow of one player.
///
/// `on_death()` enters `dying`, `tick()` runs the timers, `on_respawn()` is the
/// caller's acknowledgement of `respawn_due()` (`0x452670`), `prompt_restart()`
/// is the Activate-while-dead path (`0x443af9`), `restart()`/`return_to_hq()`
/// are the two level-restart menu items (`0x4428f0(0/1)`).  Every call that has
/// to refuse something records a one-line reason in `last_error()`.
///
/// Exact timing with the defaults (death on tick 0, `docs/GAME_FLOW.md` §4):
///
///   * ticks 1..136: `dying`, `ticks_left()` 135..0 (136 observed ticks);
///   * tick 137: `dead`, `respawn_due()` true (exactly one tick);
///   * `on_respawn()`: `respawning`, `ticks_left()` = 136;
///   * ticks 138..273: the respawn timer runs, and after tick **273** the
///     state is `alive` (136 ticks from the due tick).
class GameFlow {
public:
    /// Start a new flow with `config`, clamping invalid values and keeping a
    /// note about every clamp in `last_error()`: negative timers become 0 and a
    /// `lives` below -1 becomes -1.  The state is reset to `alive`, the timers
    /// and the counters are dropped, and `lives()` becomes `config.lives`.
    void configure(const FlowConfig& config);

    /// The validating form: returns `false` and writes a one-line reason for a
    /// negative `death_ticks`/`respawn_ticks` or a `lives < -1`; the previous
    /// configuration and the running flow are left untouched.
    bool configure(const FlowConfig& config, std::string* error);

    /// `player_killed` (`0x4536e0`): enter `dying` with `death_ticks` on the
    /// timer and count one death.  A positive `lives` budget is spent here
    /// (MODEL); `-1` is never touched.
    ///
    /// A repeated call while already `dying` is a **no-op** that keeps the
    /// running timer (the original can kill a player only once per life: the
    /// `player_killed` guard at `0x453700` returns for `action_state == 7`), and
    /// writes a reason to `last_error()`.  `dead` and `restart_prompt` refuse
    /// too; `alive` and `respawning` start a new death.
    void on_death();

    /// Advance exactly one fixed tick (1/91 s).  The one-tick `respawn_due()`
    /// pulse is cleared first, so it is never true on two ticks in a row.
    ///
    ///   * `dying`: the timer runs; on the tick *after* it reached 0 the flow
    ///     goes to `dead` (respawn due) or, with `campaign && lives == 0`, to
    ///     `restart_prompt`;
    ///   * `respawning`: the timer runs, and the state flips to `alive` on the
    ///     tick the counter reaches 0;
    ///   * `alive`, `dead`, `restart_prompt`: no transition (`dead` waits for
    ///     `on_respawn()`).
    void tick();

    /// Current state.
    PlayerFlow state() const { return state_; }

    /// Ticks left in the current state: `death_ticks`..0 while `dying`,
    /// `respawn_ticks`..0 while `respawning`, 0 for the rest.  On `dying` a
    /// value of 0 means "the expiry is processed on the next tick".
    int ticks_left() const { return ticks_left_; }

    /// True on the tick the death timer expired: the caller must respawn the
    /// player (`0x452670`, §2.1).  True for **exactly one tick** and never
    /// while `lives` are exhausted (that goes to `restart_prompt`).
    bool respawn_due() const { return respawn_due_; }

    /// Acknowledge the due respawn: `dead` -> `respawning` with
    /// `respawn_ticks` on the timer and one more respawn counted.  Refused (no
    /// transition, reason in `last_error()`) outside `dead`; the call is legal
    /// after a later `tick()` already cleared the one-tick pulse.
    void on_respawn();

    /// The Activate path of the menu (`0x443af9`): while `dying` or `dead`,
    /// stop the flow in `restart_prompt` and return `true`.  Returns `false`
    /// with a reason from any other state (already prompted, alive,
    /// respawning).  MODEL: the original also gates this on `[0x83d620]`,
    /// `[0x83d940]` and the `regen` cheat (§2.1/§7.3) and then opens state 6.
    bool prompt_restart();

    /// Menu item "Restart level..." (`0x4428f0(0/1)`, §4.1/§4.2): back to
    /// `alive`, timers dropped, `lives` restored from the configuration and one
    /// restart counted.  Allowed from any state (the menu opens from the level
    /// as well).  MODEL: the real item loads a level and copies a player
    /// snapshot; nothing of that happens here.
    void restart();

    /// Return to HQ (`Restart level from HQ` / the after-mission `NEW_LEVEL`
    /// chain, §4.2/§4.4): same reset as `restart()` with its own counter.
    /// MODEL: the HQ snapshot, the mission counter and the level load are
    /// outside this module.
    void return_to_hq();

    /// Name of a state: `"alive"`, `"dying"`, `"dead"`, `"respawning"`,
    /// `"restart_prompt"`, or `"unknown"` for a value outside 0..4.
    static std::string_view state_name(PlayerFlow state) noexcept;

    const FlowConfig& config() const { return config_; }

    /// MODEL: lives left, `-1` = unlimited (the config value before any death).
    int lives() const { return lives_; }

    /// Deaths accepted by `on_death()` since `configure()`.
    int deaths() const { return deaths_; }

    /// Successful `on_respawn()` calls since `configure()`.
    int respawns() const { return respawns_; }

    /// `restart()` calls since `configure()`.
    int restarts() const { return restarts_; }

    /// `return_to_hq()` calls since `configure()`.
    int hq_returns() const { return hq_returns_; }

    /// Ticks executed by `tick()` since `configure()` (0 before the first one;
    /// it keeps counting in `alive`/`dead`/`restart_prompt`).
    std::uint64_t ticks_elapsed() const { return ticks_elapsed_; }

    /// Reason of the last refusal/clamp (empty when the last call was clean).
    const std::string& last_error() const { return last_error_; }

    /// Drop the stored reason without touching the flow.
    void clear_error() { last_error_.clear(); }

private:
    /// Install already validated settings and restart the flow.
    void install(const FlowConfig& config);

    FlowConfig config_{};
    PlayerFlow state_ = PlayerFlow::alive;
    int ticks_left_ = 0;
    int lives_ = -1;
    bool respawn_due_ = false;
    int deaths_ = 0;
    int respawns_ = 0;
    int restarts_ = 0;
    int hq_returns_ = 0;
    std::uint64_t ticks_elapsed_ = 0;
    std::string last_error_;
};

} // namespace gm
