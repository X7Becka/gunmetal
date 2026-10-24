#pragma once

// Level-selection screen (`M8.4`/`M9.1`, slice 2): the 36 levels of screen 17,
// `SELECT LEVEL FOR NEW GAME`, with their campaign unlock state, best time and
// secret counts.
//
// The screen itself (`docs/UI_SCREENS.md` §2/§3.7, canon `docs/FORMATS.md` §15)
//
//   * is state id **17**, handler `0x48a3c0`, title `SELECT LEVEL FOR NEW GAME`
//     (`0x4bd9e8`), step 8 (`mov [0x662d58], 8` at `0x48a3ce`), x 14
//     (`push 0xe` at `0x48a481`), y = -(27 + 8*i) (`0xffffffe5` = -27) and
//     `ESC TO CANCEL` (`0x4bd9d8`, row 111 = `0xffffff91`);
//   * shows 10 rows at a time (`lea ecx,[ebx+0xa]` at `0x48a408`, the same
//     window/clamp for the arrows at `0x48a438`), the row count is the runtime
//     dword `0x68ddb0` (`mov [0x67f10c], eax` at `0x48a3c9`);
//   * is reached from `NUMBER OF LOCAL PLAYERS` (21 -> 17, `0x48dca6`) and
//     leaves for `SET DIFFICULTY LEVEL` (17 -> 23, `0x48e281`); Esc has no §4
//     row, so the model's `back()` event is ours;
//   * does **not** use a level table: `0x48b170` calls the directory scanner
//     `0x48b1c0` with the mask `S?W` (`0x4bdca8`) and the pattern `"%s\*.%s"`
//     (`0x4bdccc`), which walks `<game>\*.sbw`, strips the extension at the
//     first `.` (`0x48b2ca`..`0x48b2f8`) and stores `char*` stems in the array
//     `0x68cda8` with the count in `0x68ddb0`; `0x49ff70` then sorts that array
//     with the comparator `0x48b350`;
//   * the comparator (disassembled for this module) is two `strncmp(name,
//     "LEVEL", 5)` calls (`0x415ff0`, string `0x4bdcd4` at `0x48b35b`/`0x48b375`):
//     a name that starts with `LEVEL` sorts **before** one that does not; when
//     both or neither match, the lengths decide (`0x48b387`..`0x48b39d`:
//     `-1` shorter first, `+1` longer, `0` equal);
//   * looks the display text up at draw time only: `0x48a4c0(stem)` builds
//     `sprintf(buf, "%s - ", stem)` (`0x4bda04`) and appends the rest of the
//     matching `LVLNAMES.SLU` line (at most 30 characters, `cmp ecx,0x1e` at
//     `0x48a542`); a stem without a line (the five HQs, `temple`) shows the stem
//     itself.  The byte compare against the file is case-sensitive.
//
// Data of the shipped installation: 36 `.sbw` files, and `LVLNAMES.SLU` has 30
// lines (`LEVEL1`..`LEVEL24`, `SECRT1`, `SECRT2`, `DM1`..`DM4`) — no HQs, no
// `DM5`/`TEMPLE` (`FORMATS.md` §15).
//
// Proven campaign facts this module builds its unlock rules on
// (`docs/CAMPAIGN_FLOW.md` §1..§3, `game/campaign_state.h`):
//
//   * the counter `ds:0x80f9cc` is the number of the **next** mission: a mission
//     `N` is played while the counter is `N + 1` (§2), so `N < counter` means
//     "completed" and `N <= counter` means "reached or current";
//   * 24 missions `level1`..`level24`, five HQs `level0`, `level00`, `level000`,
//     `level0a`, `level0b`, four DM maps and three secret levels (36, §8);
//   * the acts fix which HQ a mission returns to (§3): 1..8 -> `level00`,
//     9..16 -> `level000`, 17..18 -> `level0a`, 20..23 -> `level0b`; `level0` is
//     the starting HQ of a new game (`0x442a0e`), `level19` has no HQ (it runs
//     `NEXT_LEVEL` itself, `0xf5a0`), and `level24` has no campaign exit (§7);
//   * the secret levels hang off two missions with an unconditional
//     `NEW_LEVEL`: `level4` -> `secrt1` (`0xde21`) and `level16` -> `secrt2`
//     (`0xe44d`) (`FORMATS.md` §15);
//   * `temple` is the patch DM map `DM5 - TEMPLE`; it is not on the campaign
//     path and has no `LVLNAMES.SLU` line (`FORMATS.md` §15).
//
// Model, not a port -- every such place is marked `MODEL`:
//
//   * **Order.**  `levels()` is in *campaign* order: missions `level1`..`level24`
//     by number, then the five HQs, then the secret levels, then `dm1`..`dm4`
//     (the DM maps last, as a separate block).  The screen's real order is the
//     directory scan sorted by `0x48b350` above, and it depends on the *case* of
//     the file names on disk: `0x415ff0` compares bytes, so on this install
//     (lowercase `level1.sbw`) the `LEVEL` branch never fires and only the
//     length rule is visible, while a FAT/CD install with `LEVEL1.SBW` sorts all
//     `LEVEL*` names first.  With equal lengths the comparator returns 0 and
//     `0x49ff70` (a median-of-three quicksort with insertion sort for <= 8
//     elements) leaves the relative order unspecified.  None of that is
//     reproduced: the campaign chain is what a level select of the campaign
//     needs, and it is deterministic.
//   * **Unlock.**  `unlocked` is `counter >= threshold`, where the threshold of
//     mission `N` is `N`, of `level0` 1, of `level00` 9, `level000` 17,
//     `level0a` 19, `level0b` 24 (= the counter while the last mission of the
//     act before it is played), of `secrt1` 5 and `secrt2` 17 (the counter
//     while their host mission `level4`/`level16` is played).  DM maps and
//     `temple` are always selectable (they are not on the campaign path, and
//     screen 17 lists every `.sbw`, §15).  The original has **no lock at all**:
//     the list is the whole directory.  The counter cannot distinguish "the
//     last mission of the act is in progress" from "it is finished" (both are
//     the same value), so a threshold says "reachable at this counter", not
//     "the player has been there".
//   * `completed` is `mission < counter` and is defined for missions only; no
//     field of the original records that a HQ, secret or DM map was visited
//     (the counter is the whole progress state).
//   * `best_seconds` is the `Records` minimum per level name (§6 of
//     `CAMPAIGN_FLOW.md`; `SET_LEVEL_NAME`, not the stem), `-1` when the name
//     has no record or no `Records` were given.
//   * `secrets_declared` is the `SET_LEVEL_SECRETS` number of §8 (101 over the
//     corpus); `secrets_found` is the module tally `CampaignState::secrets()`
//     for the mission being played (`mission == counter - 1`), 0 for every other
//     level -- the tally is reset at each level start (`0x45280f`), so it can
//     only describe the current level.
//   * `name` is the **file stem** (`level1`, `level0b`, `dm3`): that is the key
//     the scanner produces and what `load_level` (`0x442ef0`) opens, and it is
//     what `launch` hands back.  The printable titles (`SET_LEVEL_NAME` via
//     `campaign_level(name)->name`, or the `LVLNAMES.SLU` line of `0x48a4c0`)
//     are a rendering concern and are not stored.
//   * `move()` clamps at both ends and does **not** skip locked entries: the
//     original list has no locks, and a cursor that silently jumps over rows
//     would make `activate()`'s `locked` answer unreachable.
//   * `build()` refuses a counter below 1 (the original clamps to >= 1 only on
//     the restart path, `0x442973`; a save can carry anything) and refuses
//     nothing else: a null `Records` just leaves every time at `-1`.
//
// Pure state: no pixels, no fonts, no `.sbw` scan, no file I/O at all (the
// `GUNMETAL.HI` data arrives through `Records`).  Not wired into the frontend,
// `GameRuntime` or the CLI, and it draws no thumbnails/previews.  See
// `docs/LEVEL_SELECT.md`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

class CampaignState;
class Records;

/// What a level of the shipped corpus is (`docs/CAMPAIGN_FLOW.md` §8).
enum class LevelKind {
    mission,      ///< `level1`..`level24`
    headquarters, ///< `level0`, `level00`, `level000`, `level0a`, `level0b`
    secret,       ///< `secrt1`, `secrt2`, `temple`
    deathmatch,   ///< `dm1`..`dm4`
    unknown,      ///< not a shipped level
};

/// What `activate()`/`back()` decided; `none` after `build()`/`move()`.
enum class LevelSelectEvent {
    none,
    launch, ///< the level may be loaded (`launched()` says which)
    locked, ///< the selected level is not unlocked yet (`last_error()` says why)
    back,   ///< `back()`: leave the screen
};

/// One row of the list.  Field meanings and their evidence class are in the
/// header comment; `name` is the file stem.
struct LevelEntry {
    std::string name;        ///< file stem: `level1`, `level0b`, `dm3`
    LevelKind kind = LevelKind::unknown;
    int mission = 0;         ///< 1..24 for a mission, 0 otherwise
    bool unlocked = false;   ///< selectable at the current counter (`MODEL`)
    bool completed = false;  ///< `mission < counter` (`MODEL`)
    int best_seconds = -1;   ///< `GUNMETAL.HI` minimum for the level name, -1 = none
    int secrets_found = 0;   ///< tally of the mission being played, else 0
    int secrets_declared = 0;///< `SET_LEVEL_SECRETS` of the level (§8), 0 if none
};

/// The level list of screen 17 as a headless state machine.
class LevelSelect {
public:
    /// Levels of the shipped corpus (`CAMPAIGN_FLOW.md` §8).
    static constexpr int kLevelCount = 36;
    /// Rows the screen shows at once (`0x48a408`); not used by the state,
    /// exposed so a renderer needs no magic number.
    static constexpr int kVisibleRows = 10;

    /// Fill the list from *campaign* (counter, mission tally) and the optional
    /// hi-score table.  Clears the selection, the event and the last error.  A
    /// counter below 1 refuses the whole build: the list stays empty and a text
    /// lands in `last_error()`; *records* may be null (then every best time is
    /// `-1`).
    void build(const CampaignState& campaign, const Records* records);

    /// The rows in campaign order (36 for a shipped build, 0 after a refusal).
    const std::vector<LevelEntry>& levels() const { return levels_; }

    /// Index into `levels()` of the highlighted row (0 when empty).
    int selected() const { return selected_; }

    /// Move the cursor by *delta* rows, clamped to `0..size()-1`; locked rows
    /// are **not** skipped.  No-op on an empty list.  Clears the event/error.
    void move(int delta);

    /// Enter on the selected row.  Returns true and raises `launch` (with
    /// `launched()` set) for an unlocked row; returns false and raises `locked`
    /// with a one-line reason for a locked one; returns false with a text and
    /// no event when the list is empty.
    bool activate(std::string* error = nullptr);

    /// Put the cursor on the row whose stem is *stem* (ASCII case-insensitive,
    /// as the engine names files).  Returns false, with a text, for an unknown
    /// name; the cursor and the list are left alone.
    bool select(std::string_view stem, std::string* error = nullptr);

    /// Esc: raises `back` (screen 17 has no documented Esc row; the caller
    /// decides where to go).  Clears `launched()` and the last error.
    void back();

    /// The last `activate()`/`back()` result (`none` after `build()`/`move()`).
    LevelSelectEvent event() const { return event_; }

    /// Stem of the last `launch` (empty otherwise).
    const std::string& launched() const { return launched_; }

    /// Copy of the selected row, `std::nullopt` when the list is empty.
    std::optional<LevelEntry> current() const;

    /// Reason of the last refusal (empty when the last call was clean).
    const std::string& last_error() const { return last_error_; }
    void clear_error() { last_error_.clear(); }

    // --- static facts of one stem (also used by the test) -------------------

    /// Kind of *stem* (`unknown` for anything that is not one of the 36).
    static LevelKind kind_of(std::string_view stem);
    /// Mission number 1..24 for `level1`..`level24`, 0 otherwise.
    static int mission_of(std::string_view stem);
    /// `SET_LEVEL_SECRETS` of *stem* (`CAMPAIGN_FLOW.md` §8); 0 when none.
    static int declared_secrets(std::string_view stem);
    /// Campaign counter at which *stem* becomes selectable; 0 = always (DM
    /// maps, `temple`), -1 = not a shipped level.  `MODEL`, see the header.
    static int unlock_counter(std::string_view stem);

private:
    /// Store *text* as the last error and return false.
    bool fail(std::string text, std::string* error);

    std::vector<LevelEntry> levels_;
    int selected_ = 0;
    int counter_ = 0; ///< counter of the last `build()` (0 before it)
    LevelSelectEvent event_ = LevelSelectEvent::none;
    std::string launched_;
    std::string last_error_;
};

} // namespace gm
