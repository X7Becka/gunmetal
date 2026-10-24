#pragma once

// Deathmatch match state (`M9.2`, slice 2): modes, `mp_flags`, frags, respawn,
// team friendliness and the handicap ladder.
//
// The rules come from `docs/DM_RULES.md`, which was taken from the shipped
// `gundx.exe` (read only) and the 36 `.sbt` scripts; this module is the C++
// counterpart of the runnable Python model `tools/re/dm_rules.py`.  Proven in
// the original:
//
//   * `game_mode` = `ds:0x83d2c8` (`DM_RULES.md` §2.1): 0 campaign, 1 normal
//     DM, 2 classic DM, 3 Scavenger Hunt.  Checks: `0x4421c0` (!=0),
//     `0x45eb1c` (==1), `0x45eb06` (==2), `0x4421d0` (==3);
//   * `mp_flags` = `ds:0x83d184` (§2.2, parser `0x44a719`..`0x44a811`):
//     0x01 `team` (`/team`, `0x44a752`), 0x02 `ctf` (`/flag`, `0x44a777`),
//     0x04 `no_monsters` (`0x44a79c`), 0x08 `freeze` (`0x44a7c1`),
//     0x10 `no_buy` (`/nobuyammo`, `0x44a80b`), 0x20 `handicap`
//     (`0x44a7e6`);
//   * frags = `player+0x3270` (§3.1; the HUD `KILL` number is read at
//     `0x492fdd`) and only `player_killed` `0x4536e0` writes them (§3.2):
//     a valid killer gets `+1` (`0x453812`), a team kill gives `-1` to the
//     **killer** and only when the **victim** had a frag (`0x4537f5`/
//     `0x4537fd`), a suicide or a world kill gives `-1` to the victim when it
//     had one (`0x453875`/`0x45387f`).  A normal kill leaves the victim's
//     frags untouched: the `-1` branch is entered only for an invalid or
//     self killer (`0x4537af` -> `0x453875`);
//   * a death sets `action_state = 7` and `+0x33d4 = 0` (`0x453763`,
//     `0x45372f`), the timer runs up to `0x88` = 136 ticks and respawns
//     (`0x443b0a`, §4.3); a map has 8 player slots (§4.1);
//   * the shipped deathmatch has **no frag limit and no match time limit**
//     (§3.4): nothing in the code compares frags with N, the level countdown
//     (`ds:0x80d898`, §3.4) is set by two campaign maps only;
//   * the handicap ladder (§5, `0x452e9a`..`0x452f71`): with
//     `diff = max_other_frags - mine`, `diff > 50/30/25/20/15/10/7/4` grants
//     5x / 4x / 3x / 2x / 1x / 1/2 / 1/4 / 1/10 of the starting cash
//     (`ds:0x83d310`);
//   * teams: the team-kill branch is gated by `mp_flags & 0x1` (§3.2), the
//     team id is `player+0x344c` (§6); CTF is `mp_flags & 0x2` (§2.2).
//
// Model, not a port -- every such place is marked `MODEL`:
//
//   * `DmPlayer::deaths` and `DmPlayer::score`: the original has a single i32
//     score (`+0x3270`) and no per-player death counter (§5: "отдельного
//     «поля очков» нет").  `score` mirrors `frags`; `deaths` counts the
//     `on_kill()` deaths of this module.
//   * `frag_limit`/`time_limit_ticks`: the shipped DM has neither (§3.4), so
//     both are opt-in engine match rules and `0` means "no limit".  The
//     countdown *direction* follows `ds:0x80d898` (`0x457230` subtracts the
//     tick delta); the values are the caller's.
//   * the respawn timer counts **down** here, while `+0x33d4` counts up to
//     `0x88` (§4.3).  Only the default magnitude (136 ticks) is the proven
//     one, the direction is a model.
//   * applying the team-kill rule to CTF (`mp_flags & 0x2`) is a model: the
//     original branch tests bit `0x1` only (§3.2).
//   * `handicap`: a caller input.  The proven ladder is
//     `dm_handicap_percent()`; the cash side (`cash = salvage + starting cash
//     (+ bonus)`, §4.4/§5) is deliberately not modelled -- there is no cash
//     field in `DmPlayer`.
//   * the `kills_by_victim[8]` matrix (§3.1) and the kill message
//     (`0x4936a0`, §3.2) are not modelled.
//
// The class is pure match state: no runtime, no VM, no save, no clock, no RNG,
// no rendering and no network.  The kill event `-7 SYSTEM_PLAYER_KILLED`
// (`docs/EVENTS.md` §4.1) is **not** posted here: the original counts the frag
// inside `player_killed` itself, the event only feeds scripts/effects (§3.2).
//
// Limits (docs/DM_STATE.md): not wired into `GameRuntime`/`main.cpp`, no spawn
// point selection (`SET_DEATHMATCH_START`, §4.1/§4.2), no weapons or items by
// `mp_flags` (no-buy/freeze/hunt arsenal, §2.4/§7), no bots (the game has none,
// §6) and no DM level chain (`dm1 -> ... -> level24 -> dm1`, §3.4).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

/// `game_mode` (`ds:0x83d2c8`, `DM_RULES.md` §2.1).  The numeric values are the
/// ones the executable compares against; no other value exists (a mode outside
/// 0..3 makes the thing mask return 1, §2.3).
enum class DmMode : std::uint8_t {
    campaign = 0, ///< `0x4421c0`: `game_mode != 0` is DEATHMATCH_GAME
    normal = 1,   ///< `0x45eb1c`: `cmp ecx,1 / sete`
    classic = 2,  ///< `0x45eb06`: `cmp ecx,2 / sete`
    hunt = 3,     ///< `0x4421d0`: `cmp ecx,3 / sete`
};

/// `mp_flags` bits (`ds:0x83d184`, `DM_RULES.md` §2.2).  Only the bits the
/// executable parses from the command line; `kDmKnownFlags` is their union.
enum class DmFlag : std::uint32_t {
    none = 0x00,
    team = 0x01,        ///< `/team` `0x44a752`; enables the team-kill branch
    ctf = 0x02,         ///< `/flag` `0x44a777` (CAPTURE_THE_FLAG_GAME)
    no_monsters = 0x04, ///< `/nomonsters` `0x44a79c`; hides thing mask 0x20
    freeze = 0x08,      ///< `/freeze` `0x44a7c1`
    no_buy = 0x10,      ///< `/nobuyammo` `0x44a80b`
    handicap = 0x20,    ///< `/handicap` `0x44a7e6`; gates the §5 ladder
};

/// Bits with a proven meaning (`team|ctf|no_monsters|freeze|no_buy|handicap`).
inline constexpr std::uint32_t kDmKnownFlags = 0x3F;

/// The `mp_flags` value of one bit (`DmFlag::team` -> `0x01`).
constexpr std::uint32_t dm_flag_bit(DmFlag flag) noexcept {
    return static_cast<std::uint32_t>(flag);
}

/// True when `flags` (`DmSettings::flags`) carries `flag`.
constexpr bool dm_has_flag(std::uint32_t flags, DmFlag flag) noexcept {
    return (flags & dm_flag_bit(flag)) != 0;
}

/// Number of player slots of one map: 8 (`DM_RULES.md` §4.1).
inline constexpr int kDmMaxPlayers = 8;

/// Respawn delay of the original: `+0x33d4 >= 0x88` = 136 fixed ticks
/// (`DM_RULES.md` §4.3, `0x443b0a`; ~1.49 s at 91 Hz).
inline constexpr int kDmRespawnTicks = 136;

/// One step of the handicap ladder (`DM_RULES.md` §5): the bonus applies while
/// `diff > diff_gt`, where `diff = max_frags_of_the_others - mine`.
struct DmHandicapStep {
    int diff_gt;   ///< strict lower bound of the frag difference
    int percent;   ///< bonus in percent of `ds:0x83d310` (starting cash)
};

/// The proven ladder, strongest first: 5/4/3/2/1, then ½/¼/⅒ (§5).
inline constexpr DmHandicapStep kDmHandicapLadder[] = {
    {50, 500}, // 0x452e9a..0x452eb0: base + 5 * start
    {30, 400}, // 0x452eb5..0x452ec9: base + 4 * start
    {25, 300}, // 0x452ece..0x452ee4: base + 3 * start
    {20, 200}, // 0x452ee9..0x452efd: base + 2 * start
    {15, 100}, // 0x452f02..0x452f15: base + 1 * start
    {10, 50},  // 0x452f17..0x452f31: base + start / 2
    {7, 25},   // 0x452f33..0x452f51: base + start / 4
    {4, 10},   // 0x452f53..0x452f71: base + start / 10
};

/// Bonus of the trailing player in percent of the starting cash
/// (`DM_RULES.md` §5).  `frag_diff <= 4` gives `0`.
int dm_handicap_percent(int frag_diff) noexcept;

/// Match settings.  `0` in a limit means "none" (the shipped DM has neither,
/// `DM_RULES.md` §3.4).  `configure()` validates or clamps the values.
struct DmSettings {
    DmMode mode = DmMode::campaign; ///< `ds:0x83d2c8` (§2.1)
    std::uint32_t flags = 0;        ///< `ds:0x83d184` bits of `DmFlag` (§2.2)
    /// MODEL: frags of the first player to reach it end the match (§3.4: the
    /// original has no such check).
    int frag_limit = 0;
    /// MODEL: ticks of the match clock; the countdown direction follows
    /// `ds:0x80d898` (§3.4), but no DM map sets it.
    int time_limit_ticks = 0;
    /// Respawn delay in fixed ticks; 136 is the original's `0x88` (§4.3).
    int respawn_delay_ticks = kDmRespawnTicks;
    /// MODEL: caller input for the §5 ladder (0 = off).  The ladder itself is
    /// `dm_handicap_percent()`; no cash is modelled here.
    int handicap = 0;
};

/// Match state of one player slot.  `frags` is the only score field of the
/// original (`+0x3270`, §3.1); `deaths`/`score` are the model fields above.
struct DmPlayer {
    int frags = 0;         ///< `player+0x3270` (§3.1/§3.2)
    int deaths = 0;        ///< MODEL: per-player death counter
    int score = 0;         ///< MODEL: mirrors `frags` (no separate field, §5)
    int respawn_ticks = 0; ///< ticks left until respawn (`+0x33d4` model)
    bool alive = true;     ///< `action_state != 7` (§4.3)
    int team = 0;          ///< `player+0x344c` (§6)
};

/// Deterministic match state of one deathmatch.
///
/// `configure()` starts a new match (players are dropped, the clock is reset),
/// `add_player()` fills the 8 slots (§4.1), `on_kill()` applies the frag rules
/// of `player_killed` (§3.2) and `tick()` advances the respawn and match
/// clocks.  Every mutation that has to refuse something records a one-line
/// reason in `last_error()`.
class DmState {
public:
    /// Largest accepted `add_player()` list (`DM_RULES.md` §4.1).
    static constexpr int kMaxPlayers = kDmMaxPlayers;

    /// Start a new match with `settings`, clamping invalid values and keeping
    /// a note about every clamp in `last_error()`.  Players and the match
    /// clock are dropped, so the object is back at "no players, tick 0".
    void configure(const DmSettings& settings);

    /// The validating form: returns `false` and writes a one-line reason for an
    /// unknown mode value, unknown flag bits, a negative limit/delay/handicap;
    /// the previous settings and the running match are left untouched.
    bool configure(const DmSettings& settings, std::string* error);

    /// Append a player to the first free slot and return its index, or `-1`
    /// when 8 slots are taken (`DM_RULES.md` §4.1).  A negative `team` is
    /// clamped to 0 with a note.  The player is alive with zero frags.
    int add_player(int team = 0);

    /// Players added since `configure()` (0..8).
    int player_count() const { return static_cast<int>(players_.size()); }

    /// Apply one death: `victim` dies, `killer` is the slot that caused it.
    ///
    /// Proven rules (`player_killed` `0x4536e0`, `DM_RULES.md` §3.2):
    ///   * an already dead victim is ignored (`0x453700` returns for
    ///     `action_state == 0 || 7`);
    ///   * invalid killer (outside the player list) or `killer == victim`:
    ///     `frags[victim] -= 1` only when it was `> 0` (`0x453875`);
    ///   * team flag and equal teams: `frags[killer] -= 1` only when the
    ///     victim had a frag (`0x4537f5`) -- the guard tests the *victim* and
    ///     the killer's frags are not clamped (`0x4537fd`), which is modelled
    ///     as is;
    ///   * otherwise `frags[killer] += 1` (`0x453812`);
    ///   * the victim's own frags stay as they are on a valid kill.
    ///
    /// Model: any death increments `deaths`, sets `score = frags`, sets
    /// `respawn_ticks = respawn_delay_ticks` and `alive = false` (a zero delay
    /// respawns at once).  A kill after `finished()` is ignored.  A refused
    /// kill writes the reason to `last_error()`.
    void on_kill(int victim, int killer);

    /// Advance the match clock `ticks` steps (default 1).  Every step
    /// decrements the respawn timer of every dead player by one and flips it
    /// alive at 0, then decrements `time_left()`.  The clock stops once the
    /// match is `finished()`, so a later `tick()` changes nothing.
    void tick(std::uint64_t ticks = 1);

    /// True once a frag limit was reached or the time limit ran out.  A limit
    /// of 0 never finishes the match; the shipped DM has neither (§3.4).
    bool finished() const;

    /// Ticks left on `player`'s respawn timer; `0` for a living player, `-1`
    /// for an index outside `0..player_count()-1`.
    int respawn_left(int player) const;

    /// True when `a` and `b` are different existing players of the same team
    /// and `team` or `ctf` is set (`DM_RULES.md` §3.2/§6; the CTF part is a
    /// model).  False for a self pair, a missing flag or an invalid index.
    bool is_friendly(int a, int b) const;

    /// Index of the player with the most frags once `finished()`, `-1` for a
    /// draw, an empty match or a match that is still running.
    int winner() const;

    /// Name of a mode: `"campaign"`, `"normal"`, `"classic"`, `"hunt"`, or
    /// `"unknown"` for a value outside 0..3.
    static std::string_view mode_name(DmMode mode) noexcept;

    const DmSettings& settings() const { return settings_; }

    /// Ticks of the match clock left; `0` when no time limit is set and after
    /// the limit ran out.
    int time_left() const;

    /// Fixed ticks executed by `tick()` since `configure()` (0 before the
    /// first tick; it stops growing when the match is finished).
    std::uint64_t ticks_elapsed() const { return elapsed_; }

    /// Player `index`, or `nullptr` outside `0..player_count()-1`.
    const DmPlayer* player(int index) const;

    /// All players in slot order.
    std::span<const DmPlayer> players() const { return players_; }

    /// Reason of the last refusal/clamp (empty when the last call was clean).
    const std::string& last_error() const { return last_error_; }

    /// Drop the stored reason without touching the match.
    void clear_error() { last_error_.clear(); }

private:
    /// Install already validated settings and restart the match.
    void install(const DmSettings& settings);

    DmSettings settings_{};
    std::vector<DmPlayer> players_;
    std::uint64_t elapsed_ = 0;
    int time_left_ = 0;
    /// Set by `on_kill()` when a player reaches `frag_limit`.
    bool frag_limit_reached_ = false;
    std::string last_error_;
};

} // namespace gm
