#pragma once

// One frame of screen 26, `JOYSTICK CENTERING` (`M8.8` slice 10,
// docs/JOYSTICK_CENTERING_RENDER.md).
//
// Division of labour
// ------------------
// This module is stateless: it paints the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3) from the options alone, exactly like
// `ui/video_mode_render` (37), `ui/game_type_render` (19), `ui/difficulty_render`
// (23) and `ui/local_players_render` (21).  The input (Esc/Enter closing the
// screen, the `updown`/`select` sounds) belongs to the caller.  The building
// blocks are the `FONTS4` atlases of `FRONTEND/BMP.IDX` (title face 5, rows
// face 0, highlight face 4), the optional 196x131 `PLATE` copied by `0x486050`
// and the exact fade tables of `0x682f90`.  Nothing is decoded or measured
// twice: `render/text_layout` owns the atlases and the advances.
//
// Proven geometry (`0x488f70`, docs/UI_SCREENS.md §2/§3.8)
// -------------------------------------------------------
// The handler runs from `0x488f70` to its `ret` at `0x489013` (the next
// function starts at `0x489020`, `LOCAL PLAYER ONE/TWO NAME`).  The jump table
// `0x4883d8` reaches it through a stub that only `call`s it (`0x487e95`); the
// CONTROL OPTIONS dispatcher calls it directly at `0x48d274`.  It is one
// `rows`/`step` pair, one `PLATE` copy and six `0x485dd0` calls:
//
//   * `0x488f70` stores `rows 1` into `0x67f10c` and `0x488f7a` sets the step
//     `0x662d58 = 0`.  There is **one item** (id 0) and **no row spacing**, so
//     every line's `y` is exactly the `row` argument it is pushed with; five
//     of the six calls pass `item 0`.  The handler is the only menu screen of
//     this family with five separate information lines and no selectable item;
//   * `0x488f84` calls `0x486050`, which copies the 196x131 `PLATE` into the
//     compose buffer `0x6865f8` (6419 dwords = 25676 bytes); every text call
//     writes into that copy, so 196x131 is a hard clip (§3);
//   * the title is `0x485dd0(font=5, text=0x4bd6ac "JOYSTICK CENTERING",
//     centered=1, x=38, row=14, item=-1)` at `0x488f89`..`0x488f9d`.
//     `centered` makes `x = (196 - measure(text)) / 2`
//     (`0x485de2`..`0x485dfa`, integer division) and `item == -1` zeroes
//     `item*step` (`0x485e04`..`0x485e09`), so the title sits at y = 14;
//   * the five information lines, font 0, `centered=0`, `item=0`, in call
//     order (`0x488fa0`..`0x489010`):
//
//     | # | x  | y  | text                        | VA        |
//     |---|----|----|-----------------------------|-----------|
//     | 1 | 36 | 25 | `Spiral the stick of the`   | `0x4bd694`|
//     | 2 | 36 | 35 | `joystick through all of`   | `0x4bd67c`|
//     | 3 | 36 | 45 | `it's positions.`           | `0x4bd66c`|
//     | 4 | 41 | 86 | `Press Esc or Enter`        | `0x4bd658`|
//     | 5 | 41 | 97 | `to close`                  | `0x4bd64c`|
//
//     `36 = 0x24` and `41 = 0x29` are the two `x` arguments of §3.8; the
//     `y` values are the `row` immediates `0x19`, `0x23`, `0x2d`, `0x56`,
//     `0x61` (`25`, `35`, `45`, `86`, `97`).  The paragraphs are 10 px apart,
//     the hint is 9 px; the gap 45 -> 86 is part of the layout, not a blank
//     item.  `item 0` on all five lines is what makes the highlight quirk of
//     the next paragraph reach the whole text at once;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the caller passed font 0 the face becomes
//     4, and `0x485e04` shows that only `item == -1` skips the comparison.
//     The title passes -1 and is never highlighted; all five lines pass 0, so
//     `[0x68ccf4] == 0` whitens the **whole body** at once.  That is a quirk of
//     the shipped handler, modelled here, not a cursor: the screen has nothing
//     to select;
//   * there is no `MENUARRW` call (`0x485cc0`/`0x485d00` are not reached) and
//     no scroll window: the last line ends at y 97 + 7 = 104 of a 131 px band.
//
// The screen has no data at all
// -----------------------------
// All six strings are static literals of the exe; there is no `%s`, `%d` or
// any buffer push in `0x488f70`..`0x489013` (the only pushes are the six
// `0x485dd0` argument sets and the last `ret` cleans them).  Unlike screen 10
// (`Player 1: %s`, selected key set) or screen 24/25 (player names), this
// screen reads no level name, no statistics and no config field.  The module
// therefore has **no substitution fields**: inventing one would be a model the
// binary does not support (`docs/JOYSTICK_CENTERING_RENDER.md` §1.1).
//
// Entry, cursor and what the screen does
// --------------------------------------
// The only entry is CONTROL OPTIONS (10) item 7 `Joystick Centering`
// (`docs/UI_SCREENS.md` §3.1, dispatcher table `0x48e888` slot 7 ->
// `0x48d22e`): that path sets the eight dwords `0x4a5ae8`..`0x4a5b04`
// (centring state, field names unproven) to -1 (`0x48d22e`..`0x48d25a`),
// stores 7 into `0x660788` (`0x48d260`), writes state `0x1a` (`0x48d26a`),
// calls `0x488f70` itself (`0x48d274`) and jumps into the shared input handler
// (`0x48e4a6`).  It does
// **not** write `0x68ccf4`, so the cursor the screen sees is the one of screen
// 10 — item id 7 at that moment (`kJoystickShippedCursor`).  Since no line
// has item 7, the first shipped frame has no highlight; only a caller that
// passes 0 gets the white body.  The handler itself only paints; the joystick
// centring (the calibration pass behind this menu item) is not part of this
// module, and Esc/Enter closing the screen back to state 10 (`0x48e599`) is
// the caller's job.
//
// Canvas, background and colours
// ------------------------------
// `JoystickRenderOptions::background` names a `FRONTEND/BMP.IDX` entry
// painted at (0,0) before the text; an empty string means the opaque black
// canvas.  The proven panel of screen 26 is `PLATE` (196x131,
// `menu_panel_asset(Screen::JoystickCentering)`), but it is never painted
// implicitly, like screens 13/14/17/19/21/23/30/37.  Glyphs are clipped to the
// 196x131 band `0x485dd0` writes into.  Colours come from the atlas palette:
// title font 5 -> (155,15,15) dark red, rows font 0 -> (19,15,15) dark grey,
// highlight font 4 -> (239,239,239) white (the `FONTS4` faces,
// docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// `fade_lut` is the same exact index -> index path as `ui/video_mode_render`
// and `ui/game_type_render`: the table is applied to the palette index of each
// painted pixel **before** the palette lookup (`render/hud_lut.h::map`).  The
// table either comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  Without both the call is a text
// refusal prefixed `joystick_centering_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"joystick_centering_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX` or `FONTS4` atlas, a
// background entry that is missing/broken/not 8 bpp, and an exact fade without
// a table and a dump (or with an unreadable/wrong-sized dump).  On failure
// `rgba` is cleared, `width`/`height` are 0 (never a stale or half-drawn
// picture) and `*stats` is zeroed.  `stats`/`error` may be null.  The call only
// reads the installation, is deterministic and does not depend on the current
// working directory.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kJoystickCanvasWidth = 320;
inline constexpr int kJoystickCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kJoystickBackground = 0x000000FFu;

/// `JOYSTICK CENTERING` (`0x4bd6ac`), pushed at `0x488f91`; font 5, centred.
inline constexpr std::string_view kJoystickTitle = "JOYSTICK CENTERING";

/// Line 1 (`0x4bd694`) at (36, 25), pushed at `0x488fa8`.
inline constexpr std::string_view kJoystickLineOne = "Spiral the stick of the";
/// Line 2 (`0x4bd67c`) at (36, 35), pushed at `0x488fbf`.
inline constexpr std::string_view kJoystickLineTwo = "joystick through all of";
/// Line 3 (`0x4bd66c`) at (36, 45), pushed at `0x488fd6`.
inline constexpr std::string_view kJoystickLineThree = "it's positions.";
/// Line 4 (`0x4bd658`) at (41, 86), pushed at `0x488fed`.
inline constexpr std::string_view kJoystickLineFour = "Press Esc or Enter";
/// Line 5 (`0x4bd64c`) at (41, 97), pushed at `0x489004`.
inline constexpr std::string_view kJoystickLineFive = "to close";

/// The panel of screen 26 (`0x486050`; `menu_panel_asset(Screen::JoystickCentering)`).
inline constexpr std::string_view kJoystickPanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kJoystickPanelWidth = 196;
inline constexpr int kJoystickPanelHeight = 131;

/// `0x67f10c = 1` (`0x488f70`): one item, the whole body (id 0).
inline constexpr int kJoystickRows = 1;
/// `0x662d58 = 0` (`0x488f7a`): no row spacing, `y = row` for every line.
inline constexpr int kJoystickItemStep = 0;
/// The item id all five lines pass (`push 0x0` before each text at
/// `0x488fa0`..`0x489004`).
inline constexpr int kJoystickItem = 0;

/// Number of information lines (`0x4bd694`..`0x4bd64c`).
inline constexpr int kJoystickLineCount = 5;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kJoystickTitleFont = 5;
inline constexpr int kJoystickRowFont = 0;
inline constexpr int kJoystickSelectedFont = 4;

/// Title row (`push 0xe` at `0x488f8b`), centred in the band; `x = 38`
/// (`push 0x26`) is the `centered` call's fallback argument.
inline constexpr int kJoystickTitleY = 14;
inline constexpr int kJoystickTitleX = 38;

/// `x` of the three paragraph lines (`push 0x24` = 36) and of the two hint
/// lines (`push 0x29` = 41), docs/UI_SCREENS.md §3.8.
inline constexpr int kJoystickBodyX = 36;
inline constexpr int kJoystickHintX = 41;

/// `[0x68ccf4]` of screen 10 when item 7 (`Joystick Centering`) is taken:
/// `0x48d22e` never rewrites it, so this is the cursor of the shipped frame
/// and no line (item 0) is highlighted.
inline constexpr int kJoystickShippedCursor = 7;

/// A cursor value that matches no item: `0x485e0d` compares exactly, so `-1`
/// (the `item == -1` value the title uses) paints the plain frame.
inline constexpr int kJoystickNoHighlight = -1;

/// One painted information line: the handler's call, named.
struct JoystickLine {
    std::string_view text;
    int x = 0;
    int y = 0;
    int item = kJoystickItem;
};

/// The five lines in the order `0x488fa0`..`0x489010` paints them.
inline constexpr JoystickLine kJoystickLines[kJoystickLineCount] = {
    {kJoystickLineOne, kJoystickBodyX, 25, kJoystickItem},
    {kJoystickLineTwo, kJoystickBodyX, 35, kJoystickItem},
    {kJoystickLineThree, kJoystickBodyX, 45, kJoystickItem},
    {kJoystickLineFour, kJoystickHintX, 86, kJoystickItem},
    {kJoystickLineFive, kJoystickHintX, 97, kJoystickItem},
};

/// Optional parts of one frame.
struct JoystickRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 26, but it is
    /// never painted implicitly.
    std::string background;
    /// The cursor `[0x68ccf4]`, compared with the item id of the five lines.
    /// All five pass item **0**, so `cursor == 0` paints the whole body with
    /// font 4 (the quirk of `0x485dd0`), while any other value — including the
    /// default and the shipped 7 — leaves font 0.  The title (`item -1`) is
    /// never highlighted.
    int cursor = kJoystickNoHighlight;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/video_mode_render`, docs/HUD_LUT.md §2b).  Needs a
    /// table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `joystick_centering_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten).
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read
    /// with `load_hud_lut`.  Used when `fade_lut_table` is null; a missing or
    /// wrong-sized file is a text refusal, never a silently unfaded frame.
    std::string fade_lut_dump;
    /// A table already in memory (e.g. `build_hud_lut` output); wins over
    /// `fade_lut_dump`.  Only read during the call, the caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the number of **items** (`0x67f10c = 1`), which is one text line
/// of the handler's model even though five information lines are painted; the
/// title is not an item.  `ink_pixels` is the canvas pixels covered by at least
/// one glyph pixel (unique, independent of the background), `missing_glyphs`
/// the characters with no glyph in the atlas (skipped, never fatal) and
/// `arrows` the number of scroll sprites painted — always 0, screen 26 has
/// none; the field keeps the stats shape of the sibling renderers and the test
/// pins the 0.
struct JoystickRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// Paint screen 26 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kJoystickTitleY` (font 5); the five information
/// lines sit at their proven (`x`, `y`) pairs in the handler's order — 25/35/45
/// at x 36, 86/97 at x 41 — with font 0, or font 4 for all of them when
/// `options.cursor == 0`.  Every glyph is clipped to the 196x131 text band.
/// With `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.  Returns false +
/// `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_joystick_centering(const std::filesystem::path& game,
                         const JoystickRenderOptions& options,
                         std::vector<std::uint32_t>& rgba, int& width, int& height,
                         JoystickRenderStats* stats = nullptr,
                         std::string* error = nullptr);

} // namespace gm
