#pragma once

// UI sound events (ROADMAP M8.6, slice 2): menu event -> which front-end sample
// would play.  This unit is pure data and mapping - it never mixes, loads or
// plays anything; a caller turns a `UiSoundEvent` into a playback request.
//
// Source of truth: docs/UI_SOUNDS.md (checked against `gundx.exe` and the
// shipped `sounds/SOU.IDX` by tools/re/ui_sounds.py):
//
//   * §1.1 - the seven loader names of `0x48f770`, their handle globals
//     `0x4bc44c..0x4bc464` and the fact that `vfront`/`vback` are loaded but
//     never read by any play site;
//   * §1.2 - the `sounds/SOU.IDX` record index of every blob (0-based);
//   * §2.1 - one event code per `frontend_input` (`0x48b4d0`) call: 1 ->
//     `updown` (`0x48e60c`), 2 -> `select` (`0x48e639`), gated by `0x4aa384`;
//   * §2.3 - the screen/action table; §2.4 - `selectm` on every Enter (the
//     27 ms highlight callback `0x486700`); §2.5 - `menuon` is the title-art
//     animation callback `0x486720`, not a key response.
//
// What is proved by the document:
//
//   * Up/Down -> `updown`, and only when the selection really moved (the
//     "value hit its limit" branch leaves event 0, `0x48ce70`);
//   * Enter on screens 1..37 except 16 (`KEY JAMMING` uses keys as data) ->
//     `select` now and `selectm` one frame later, so both are audible;
//   * Left/Right set event 2 (`select`) *before* the per-item switch
//     (`0x48bd79`), with overrides: `Switch Left Right` (SOUND OPTIONS item 4)
//     plays `vleft` only (`0x48c6c0`, pan = left), the volume items play
//     `updown` only when the value changed, and GRAPHICS OPTIONS `Border`
//     plays `updown`;
//   * Esc -> silence (`0x48b4e1`; the Esc epilogue `0x48e4ca..0x48e5ad`
//     writes no event), and so does every key on screen 16.
//
// Model, not proved (marked again at the call site in docs/UI_SOUNDS_STATE.md):
//
//   * `menu_on` is emitted on Enter that changes the screen.  In the original
//     `menuon` is the callback of the title-art appearance animation, so "Enter
//     that opens a submenu" is this unit's invention to make the shell audible
//     end to end; no key branch writes a `menuon` request;
//   * `Left`/`Right` on a screen/item the document does not single out play the
//     default `select` (the generic event 2).
//
// Limits: no volume, priority, pan, mixer or voice data - the original's
// request tuple (`vol 0x30`, `pan 0x80`, rate `0x2b11`, prio `0x3e8`) belongs
// to the playback layer; `back` (`vback`, idx 636) is never emitted because the
// doc proves nobody reads the handle.

#include "res/sbt.h"
#include "ui/frontend_menu.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gm {

/// The front-end samples of docs/UI_SOUNDS.md §1.1.  `vfront` is deliberately
/// absent: the enum names only sounds a trigger can produce, and neither
/// `vfront` nor `vback` is ever played.
enum class UiSound {
    up_down,  ///< `updown`, handle `0x4bc454`
    select,   ///< `select`, handle `0x4bc458`
    select_m, ///< `selectm`, handle `0x4bc450`
    menu_on,  ///< `menuon`, handle `0x4bc44c`
    v_left,   ///< `vleft`, handle `0x4bc45c`, pan = left
    back,     ///< `vback`, handle `0x4bc464`; loaded, never played (model)
};

/// One concrete sound of the front-end: a logical sound plus the game's data.
struct UiSoundEvent {
    UiSound sound = UiSound::up_down;
    /// Loader key as written in the archive/`SOUND_SAMPLE_TABLE` (`updown`).
    std::string_view name = {};
    /// `sounds/SOU.IDX` record index (0-based, docs/UI_SOUNDS.md §1.2).
    int table_index = -1;
};

/// One row of the §1.1/§1.2 sample table.
struct UiSoundSample {
    UiSound sound = UiSound::up_down;
    std::string_view name = {};
    int table_index = -1;
};

/// The six samples a trigger can name, in enum order.  `UPDOWN=581`,
/// `SELECT=510`, `SELECTM=511`, `MENUON=425`, `VLEFT=804`, `VBACK=636`.
std::span<const UiSoundSample> ui_sound_samples();

/// Row for one sound, nullptr for an out-of-range value.
const UiSoundSample* ui_sound_sample(UiSound sound);

/// The primary sound of one key event, `nullopt` for silence.
///
/// * Up/Down: `up_down` when `moved` is true, silence otherwise;
/// * Enter: `select` on screens 1..37 except 16 (`KEY JAMMING`), silence there
///   and on the driver states (intro/pager/briefing/exit);
/// * Left/Right: `select` by default (proved event 2); `v_left` is item
///   specific and therefore only reachable through `ui_sounds_for`, which
///   knows the highlighted item;
/// * Escape/Backspace/Text/Other: silence (proved for Esc, §2.3).
///
/// `moved` is the caller's observation that the action changed something
/// (selection moved, value changed).  The function never re-implements
/// `FrontendMenu`; `from`/`to` are the screens before/after the event.
std::optional<UiSoundEvent> ui_sound_for(const KeyEvent& key, Screen from,
                                         Screen to, bool moved);

/// All sounds of one key event, in playback order.  Enter on an audible screen
/// yields `select`, `select_m` and (model) `menu_on` when the screen changed;
/// Up/Down and Left/Right yield at most one event; Esc yields none.
struct UiSoundPlan {
    std::vector<UiSoundEvent> events;

    bool empty() const { return events.empty(); }
    std::size_t size() const { return events.size(); }
};

/// Plan for `key` on `menu`.  The state is read from a **copy**: `feed()` runs
/// on it to obtain the resulting screen and whether the selection moved, so
/// the automaton is not duplicated and the caller's menu is untouched.
/// Item-specific overrides (`Switch Left Right`, the two volume rows, the
/// GRAPHICS OPTIONS rows) use `menu.selected_id()`.
///
/// `moved` is the host's observation that the action changed something: the
/// automaton does not own volume values, so a Left/Right on a volume row is
/// "not moved" (silence) unless the host passes `moved = true`.  When the
/// argument is `nullopt` the plan derives it from `feed()` (selection/screen).
UiSoundPlan ui_sounds_for(const FrontendMenu& menu, const KeyEvent& key,
                          std::optional<bool> moved = std::nullopt);

// --- level data cross-check ------------------------------------------------

/// Slot of the level's `SOUND_SAMPLE_TABLE` whose name equals `name`
/// (case-insensitive, like the loader's 8-byte uppercase key) and whose
/// `count > 0`, or -1.  An empty/absent name or a count of 0 is a miss; an
/// empty table never matches and never crashes.
int table_slot(const SoundSampleTable& table, std::string_view name);

/// True when the sample with this `sounds/SOU.IDX` record index is present in
/// the level's `SOUND_SAMPLE_TABLE` with `count > 0` and the slot name matches
/// the documented one.  Unknown/negative indices are false.
///
/// `index` is the **archive record** index from docs/UI_SOUNDS.md §1.2, not a
/// `SOUND_SAMPLE_TABLE` slot: the shipped tables hold the samples in slots
/// 0..6 while the archive records are 425..804.  The check therefore joins the
/// documented index to the real level table by name; the archive side (that
/// record N really is that name) is verified by the test through `BigFile`.
bool table_has(const SoundSampleTable& table, int index);

} // namespace gm
