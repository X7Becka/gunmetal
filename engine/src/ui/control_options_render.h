#pragma once

// One frame of screen 10, `CONTROL OPTIONS` (`M8.8` slice 17).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/mouse_setup_render`
// 22, `ui/player_setup_render` 36, `ui/keycfg_render` 30/34/35): the caller owns
// the state (the list cursor `[0x68ccf4]`, the two selected key sets
// `[0x5a4910]`/`[0x4aa488]`, the key-set names at `0x6c2058 + set*0x108`, the
// joystick slots `0x4bc540`/`0x4bc544`, the mouse "used by" index `[0x5a4938]`)
// and this module paints it into the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3).  Nothing here reads the keyboard, the joystick,
// `GUNMETAL.CFG` or sounds, and nothing is written back.
//
// The handler is `0x488ba0`
// -------------------------
// The screen map (docs/UI_SCREENS.md §2) lists `0x488ba0` for id 10; the
// dispatcher `0x487e71` computes `eax = [0x4bc440] - 5` and jumps through
// `0x4883d8 + eax*4`, so id 10 is slot index 5 (`0x4883ec` = `0x487ec0`),
// whose stub is `call 0x488ba0; jmp 0x487f64` (no argument pushed — the handler
// reads the globals itself).  Body `0x488ba0`..`0x488e83` (`ret`):
//
//   0x488ba3  [0x67f10c] = 10       rows, the handler's own counter
//   0x488bad  [0x662d58] = 9        step
//   0x488bb7  call 0x486050         copy the 196x131 PLATE into 0x6865f8
//   0x488bbc  title   font 5, centred, x 20 (ignored), row 14, item -1
//   0x488bd0  item 0  sprintf "Player 1: %s" of `0x6c2058 + [0x5a4910]*0x108`
//   0x488c15  item 1  set < 1 -> "View %s" (`0x4bd5dc`), else "Assign %s"
//   0x488c63  item 2  sprintf "Player 2: %s" of `0x6c2058 + [0x4aa488]*0x108`
//   0x488ca8  item 3  same View/Assign split for player 2
//   0x488cf9  item 4  "Keyjamming" (`0x4bd5b4`)
//   0x488d0d  ... device sanity: an assigned slot whose device byte
//                    `[0x4bf7e0 + dev*0x7c]` has bit 0 clear is reset to -1
//                    (`0x488d2b`/`0x488d5a`, side effect, §"not drawn")
//   0x488d74  item 5  sprintf "1: %.25s" (0x4bd58c) or "Player 1 Joystick:
//                    Not Used" (0x4bd598) when `[0x4bc540] == -1`
//   0x488dca  item 6  sprintf "2: %.25s" (0x4bd564) or "Player 2 Joystick:
//                    Not Used" (0x4bd570) when `[0x4bc544] == -1`
//   0x488e16  item 7  "Joystick Centering" (`0x4bd550`)
//   0x488e2a  item 8  sprintf "Mouse: %s" (0x4bd544) of
//                    `0x4bc548 + [0x5a4938]*4`
//   0x488e64  item 9  "Done" (`0x4bceb4`)
//   0x488e7b  call 0x484b10         key-set validation into 0x83c980/0x83ca00
//                                   (side effect, §"not drawn")
//
// Everything below was read from that listing (`tools/re/disrange.py`); the
// string pool is `0x4bd544`..`0x4bd5f4` and the mouse table `0x4bc548`.
//
// Proven geometry
// ---------------
//   * Every item is passed with `row = 27` (`push 0x1b` at `0x488bff`,
//     `0x488c55`, `0x488c92`, `0x488ce8`, `0x488cfb`, `0x488dab`, `0x488e01`,
//     `0x488e18`, `0x488e53`, `0x488e66`), and `0x485dd0` computes
//     `y = row + item*step` for `row >= 0` (`0x485e4b`..`0x485e5c`), so item
//     `i` sits at `y = 27 + 9*i` = 27, 36, ..., 108.
//   * `x = 20` (`push 0x14`) for items 0, 2, 4..9 and `x = 32` (`push 0x20`)
//     for the two `Assign`/`View` lines 1 and 3.
//   * `item = -1` (title) makes `0x485dd0` ignore both the cursor compare and
//     the absolute-row formula: `row = 14`, `y = 14` (`0x485e09`).
//   * Title `CONTROL OPTIONS` (`0x4bd5f4`), font 5, `centered = 1`, so
//     `x = (196 - width(font 5, title))/2` (`0x485de2`..`0x485dfa`); the pushed
//     `x = 20` is ignored on that path.
//   * The highlight is `0x485e0d`..`0x485e19`: when `item == [0x68ccf4]` **and
//     the caller's font is 0**, the font becomes 4.  No handler-side colouring
//     exists on screen 10 (unlike screens 36/22), so exactly the cursor line is
//     white.
//
// Text sources and ranges
// -----------------------
//   * `[0x5a4910]` = player 1's key set (`.bss`, 0 at start; docs/UI_SCREENS.md
//     §7: 16 sets, `0..15`), `[0x4aa488]` = player 2's (`.data` = **1**).
//   * The name is the `name[16]` of the set record: `0x6c2058 + set*0x108`
//     (`(set*32 + set)*8 + 0x6c2058` at `0x488bda`..`0x488be3`), i.e. the
//     `+0xf8` field of the 0x108-byte profile (primary keys `+0x00`, secondary
//     `+0x7c`, name `+0xf8`; docs/KEYCFG_RENDER.md §2).  `.bss` until a
//     `KEYCFG/*.KEY` file is loaded, so the API default is empty.
//   * `Assign`/`View`: `cmp eax,1; jge` (`0x488c15`, `0x488ca8`) — set index
//     `>= 1` prints `Assign %s` (`0x4bd5d0`), `0` (and any negative value)
//     prints `View %s` (`0x4bd5dc`).
//   * Joysticks: `-1` is `Not Used` (`.data` default of both slots); otherwise
//     the DirectInput name at `0x4bf7f0 + dev*0x7c` through `"1: %.25s"` /
//     `"2: %.25s"` (25 bytes, docs/UI_SCREENS.md §3.1).
//   * Mouse: `0x4bc548 + [0x5a4938]*4`, whose first three pointers are
//     `Not Used` (`0x4bca50`), `Player 1` (`0x4bc944`), `Player 2`
//     (`0x4bc938`) — the same `Used By` table screen 22 shows (docs/
//     MOUSE_SETUP_RENDER.md §2.3).  `[0x5a4938]` is `.bss` (0 = `Not Used`)
//     and is only ever written with 0..2 (`idiv 3` at `0x48c31b`); the model
//     clamps the index to 0..2, the original would walk into the key-action
//     names.
//
// No widgets, no arrows, no scrolling
// -----------------------------------
// `0x488ba0` contains no call to `0x486070` (`SLIDE`), `0x4860b0` (`KNOB`),
// `0x4833e0` (`CHECK`/`NOCHECK`), `0x485cc0`/`0x485d00` (`MENUARRW`) or any of
// the list-window helpers: the ten rows always fit (`27 + 9*9 = 108`, glyph
// cell 13 -> 121 < 131), there is nothing to scroll and no value to drag.
// Therefore `sliders`, `checks` and `arrows` are always 0 — the §2 asset list of
// screen 10 is `plate` + fonts only.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE` (196x131, copied into `0x6865f8` by
// `0x486050` at `0x488bb7`) and `FONTS4` faces 5 (title), 0 (items) and 4 (the
// cursor line).  All assets are validated even when `background` is empty,
// because the original composes the frame on the plate copy; the canvas is
// opaque black unless `background` names an entry (`"PLATE"` reproduces the
// panel).  Colours come from the atlas palettes: font 5 -> (155,15,15) dark
// red, font 0 -> (19,15,15) dark grey, font 4 -> (239,239,239) white
// (docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/mouse_setup_render`/`ui/player_setup_render`:
// every painted palette index goes through `render/hud_lut.h::map(level, index)`
// **before** its colour is looked up.  The table comes pre-loaded
// (`fade_lut_table`) or from a 4096-byte dump (`fade_lut_dump`, docs/HUD_LUT.md
// §2); with neither the call refuses, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"control_options_render: "`: an empty game
// directory, a missing `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken/not-196x131 `PLATE`, a background entry that is
// missing/broken/not 8 bpp, and an exact fade without a table and a dump (or
// with an unreadable dump).  Out-of-range *values* (set indices, joystick slots,
// mouse type, names longer than their field) are clamped/truncated, not refused:
// they only change a text and the screen stays paintable.  On failure `rgba` is
// cleared, `width`/`height` are 0 (never a stale or half-drawn picture) and
// `*stats` is zeroed.  `stats`/`error` may be null.  The call only reads the
// installation, is deterministic and does not depend on the current directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the key poll, the cursor moves, Esc), UI sounds, and the two handler
// side effects: `0x488d0d`..`0x488d65` resets an assigned joystick slot whose
// device byte `[0x4bf7e0 + dev*0x7c]` lost bit 0 (and clears `[0x5a4918 +
// dev*4]`), and `0x484b10` revalidates both key sets into `0x83c980`/`0x83ca00`.
// The model treats a slot `>= 0` as an enumerated device; presence bits are
// DirectInput output, not a shipped byte.  Esc is not part of the handler
// either: the front-end escape path of `0x48e4ca` compares every state that
// returns to 10 (16, 22, 26, 30, 34, 36), then 18 -> 12, and state 10 falls to
// the default `[0x4bc440] = 1` at `0x48e58c`, i.e. Esc leaves to the main menu.
// The `Done` item takes the generic item path `0x48e49c`, which stores the
// caller's previous state; nothing here handles keys.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kControlOptionsCanvasWidth = 320;
inline constexpr int kControlOptionsCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kControlOptionsBackground = 0x000000FFu;

/// The panel and the text band: `0x488bb7` -> `0x486050` copies the 196x131
/// `PLATE` into `0x6865f8`, so this is also the glyph clip.
inline constexpr std::string_view kControlOptionsPanelName = "PLATE";
inline constexpr int kControlOptionsPanelWidth = 196;
inline constexpr int kControlOptionsPanelHeight = 131;

/// `0x4bd5f4`, font 5, centred, row 14.
inline constexpr std::string_view kControlOptionsTitle = "CONTROL OPTIONS";

/// docs/UI_SCREENS.md §3: title 5, items 0, the highlight face 4 that
/// `0x485dd0` swaps in for the cursor line (`0x485e15`/`0x485e19`).
inline constexpr int kControlOptionsTitleFont = 5;
inline constexpr int kControlOptionsRowFont = 0;
inline constexpr int kControlOptionsSelectedFont = 4;

/// `[0x67f10c] = 10` at `0x488ba3` and `[0x662d58] = 9` at `0x488bad`.
inline constexpr int kControlOptionsRows = 10;
inline constexpr int kControlOptionsRowStep = 9;

/// Title anchors: `push 0xe` (`row`) and `push 0x14` (`x`, ignored because
/// `centered = 1`).
inline constexpr int kControlOptionsTitleY = 14;
inline constexpr int kControlOptionsTitleX = 20;

/// Item anchors: `push 0x1b` (`row` -> y 27 for item 0), `push 0x14` (`x` 20)
/// and `push 0x20` (`x` 32) for the two `Assign`/`View` lines 1 and 3.
inline constexpr int kControlOptionsFirstRowY = 27;
inline constexpr int kControlOptionsRowX = 20;
inline constexpr int kControlOptionsSubRowX = 32;

/// Key sets: 16 profiles of 0x108 bytes, name `name[16]` at `0x6c2058 + set*0x108`
/// (docs/UI_SCREENS.md §7, docs/KEYCFG_RENDER.md §2).
inline constexpr int kControlOptionsKeySets = 16;
inline constexpr int kControlOptionsKeyNameChars = 16;

/// `"1: %.25s"` / `"2: %.25s"` of the joystick names.
inline constexpr int kControlOptionsJoystickNameChars = 25;

/// `0x4bd5e4` / `0x4bd5c0`: the two key-set summary lines.
inline constexpr std::string_view kControlOptionsPlayer1Format = "Player 1: %s";
inline constexpr std::string_view kControlOptionsPlayer2Format = "Player 2: %s";

/// `0x4bd5dc` (set 0) / `0x4bd5d0` (any other set).
inline constexpr std::string_view kControlOptionsViewFormat = "View %s";
inline constexpr std::string_view kControlOptionsAssignFormat = "Assign %s";

/// `0x4bd5b4`, `0x4bd550`, `0x4bceb4`.
inline constexpr std::string_view kControlOptionsKeyJamming = "Keyjamming";
inline constexpr std::string_view kControlOptionsCentering = "Joystick Centering";
inline constexpr std::string_view kControlOptionsDone = "Done";

/// `0x4bd598` / `0x4bd570`: the two `Not Used` joystick lines.
inline constexpr std::string_view kControlOptionsJoystick1Unused =
    "Player 1 Joystick: Not Used";
inline constexpr std::string_view kControlOptionsJoystick2Unused =
    "Player 2 Joystick: Not Used";

/// `"1: %.25s"` / `"2: %.25s"` prefixes.
inline constexpr std::string_view kControlOptionsJoystick1Prefix = "1: ";
inline constexpr std::string_view kControlOptionsJoystick2Prefix = "2: ";

/// `0x4bd544`: `Mouse: %s`.
inline constexpr std::string_view kControlOptionsMouseFormat = "Mouse: %s";

/// `0x4bc548`, the three entries `[0x5a4938]` can select (`idiv 3` at
/// `0x48c31b`); the rest of that table aliases key actions.
inline constexpr std::string_view kControlOptionsMouseNames[3] = {
    "Not Used",  // 0x4bca50
    "Player 1",  // 0x4bc944
    "Player 2",  // 0x4bc938
};

/// Optional parts of one frame.
struct ControlOptionsRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of the screen and is never
    /// painted implicitly.
    std::string background;

    /// The cursor `[0x68ccf4]`: the **item index** the highlight is on
    /// (`0..9`).  Values outside the range simply highlight nothing; `-1` is
    /// the documented way to ask for a frame without a white line.
    int cursor = 0;

    /// `[0x5a4910]` — player 1's key set, clamped to `0..15` (docs/UI_SCREENS.md
    /// §7; `.bss`, so `0` at start).  `0` prints `View %s`, `>= 1` prints
    /// `Assign %s` (`0x488c15`).  The clamp only affects that word: the name
    /// itself comes from `keyset_names[0]`.
    int keyset1 = 0;

    /// `[0x4aa488]` — player 2's key set, clamped to `0..15` (`.data` = `1`).
    int keyset2 = 1;

    /// The `name[16]` of the **selected** set of player 1 / player 2
    /// (`0x6c2058 + [set]*0x108`), NUL-truncated and capped at 16 bytes the way
    /// the loader stores them.  Empty by default: that field is `.bss` until a
    /// `KEYCFG/*.KEY` file is loaded, and this module never reads those files.
    std::array<std::string, 2> keyset_names = {"", ""};

    /// `[0x4bc540]` / `[0x4bc544]`: the joystick slots used by player 1/2,
    /// `-1` = `Not Used` (the `.data` value of both), otherwise the DirectInput
    /// device index `0..7`.
    int joystick1 = -1;
    int joystick2 = -1;

    /// The DirectInput names at `0x4bf7f0 + dev*0x7c`, printed through
    /// `"%.25s"`; empty by default (that field is `.bss`, filled by device
    /// enumeration).  Only drawn when the corresponding slot is assigned.
    std::string joystick_name1;
    std::string joystick_name2;

    /// `[0x5a4938]`, index into `kControlOptionsMouseNames` (clamped to 0..2;
    /// `.bss`, so `Not Used` at start).  It is the same variable screen 22 edits
    /// in its `Used By` row.
    int mouse_type = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level.
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `control_options_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten).
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read with
    /// `load_hud_lut`.  Used when `fade_lut_table` is null.
    std::string fade_lut_dump;
    /// A table already in memory (e.g. `build_hud_lut` output); wins over
    /// `fade_lut_dump`.  Only read during the call, the caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the handler's own counter `[0x67f10c]` (always 10); `sliders`,
/// `checks` and `arrows` are always 0 because `0x488ba0` paints no
/// `SLIDE`/`KNOB`, no `CHECK`/`NOCHECK` and no `MENUARRW`; `ink_pixels` the
/// canvas pixels covered by at least one glyph pixel (unique, independent of the
/// background; the same rule as the sibling modules); `missing_glyphs` the
/// characters with no glyph in the atlas (skipped, never fatal).
struct ControlOptionsRenderStats {
    int rows = 0;
    int sliders = 0;
    int checks = 0;
    int arrows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
};

/// `y` of item `item`: `kControlOptionsFirstRowY + kControlOptionsRowStep * item`
/// (`0x485e4b`..`0x485e5c`).  Item 9 lands on 108.
int control_options_row_y(int item);

/// `x` of item `item`: 32 for the two `Assign`/`View` lines (1 and 3), 20 for
/// everything else (`push 0x20` / `push 0x14`).
int control_options_row_x(int item);

/// `x` of the centred title: `max(0, (196 - width(font 5, title)) / 2)`
/// (`0x485de2`..`0x485dfa`).
int control_options_title_x(const FontAtlas& title_face);

/// Name of mouse type `index` (clamped to 0..2) from `0x4bc548`.
std::string_view control_options_mouse_name(int index);

/// The whole line of item `item` (0..9) as the handler's `sprintf` builds it
/// (`0x4bd5e4` ... `0x4bceb4`).  Out-of-range items give an empty string.
std::string control_options_item_text(const ControlOptionsRenderOptions& options,
                                       int item);

/// Paint screen 10 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The frame is painted in the handler's emission order: title (font 5,
/// centred, y 14), then items 0..9 at `y = 27 + 9*i` (`x` 20, or 32 for 1/3)
/// with the cursor item in font 4.  Every glyph is clipped to the 196x131 text
/// band.  With `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.  Returns false +
/// `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_control_options(const std::filesystem::path& game,
                            const ControlOptionsRenderOptions& options,
                            std::vector<std::uint32_t>& rgba, int& width, int& height,
                            ControlOptionsRenderStats* stats = nullptr,
                            std::string* error = nullptr);

} // namespace gm
