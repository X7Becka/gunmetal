#pragma once

// One frame of the level-selection screen, `SELECT LEVEL FOR NEW GAME`
// (screen 17), as a 320x200 RGBA canvas (`M8.4` slice 3).
//
// Division of labour
// ------------------
// `ui/level_select` is the state: the rows, their order, the unlock flags and
// the cursor.  This module only *paints* that state, with the same building
// blocks as `ui/menu_render`: the 196x131 `PLATE` of `FRONTEND/BMP.IDX`, the
// three `FONTS4` faces (title 5, rows 0, highlight 4), the `MENUARRW` 16x8
// scroll arrows and the 196 px band as the write target.  Nothing is decoded or
// laid out twice (`render/text_layout` owns the atlases and the advances).
//
// Proven geometry (`0x48a3c0`, docs/UI_SCREENS.md §3.8, docs/LEVEL_SELECT.md §1)
// -----------------------------------------------------------------------------
// The handler is 90 disassembled instructions; everything below comes from
// them, not from the pictures:
//
//   * `0x48a3c9` copies the row count `0x68ddb0` into `0x67f10c` and
//     `0x48a3ce` sets the step `0x662d58 = 8`;
//   * `0x48a3d8` calls `0x486050`, which copies the 196x131 `PLATE` from
//     `0x6d2b60` into the compose buffer `0x6865f8` (docs/UI_SCREENS.md §3/§9);
//     every text call below writes into that buffer, so the band 196x131 is a
//     hard clip, exactly as in `ui/menu_render`;
//   * the title is `0x485dd0(font=5, text=0x4bd9e8, centered=1, x=38, row=14,
//     item=-1)`; `0x485dd0` ignores `x` when `centered` is set and uses
//     `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`), so the title is centred
//     in the band at y = 14;
//   * the rows are drawn in a loop `0x48a46a`..`0x48a49f` with
//     `y = -(27 + 8*i)` (`0xffffffe5` at `0x48a46c` minus `i*step`),
//     `x = 14` (`push 0xe` at `0x48a481`), font 0 and `item = scroll + i`
//     (`0x48a478`); the negative row is the absolute form of the item renderer
//     (`y = -row`, docs/UI_SCREENS.md §3), hence rows start at y = 27 and step
//     8.  `0x485e0d`..`0x485e19` switches such a row to font 4 when
//     `item == [0x68ccf4]` (the cursor) and the text is not empty -- that is
//     the **only** highlight, no bar and no `MENULIT` marker;
//   * `0x48a45b`/`0x48a456` draw `min(count, 10)` rows and
//     `0x48a3f1`..`0x48a416` compute the window start:
//     `scroll = selected >= 5 ? selected - 5 : 0`, then
//     `if (scroll + 10 >= count) scroll = count - 10`, then `scroll >= 0`.
//     That is `level_select_scroll()` below: the cursor stays on the sixth
//     visible row while scrolling down and the rest of the list is clamped at
//     the bottom;
//   * the scroll arrows are `MENUARRW`, a 16x8 index sprite loaded into
//     `0x67fcb0` (`0x48fe59` -> `0x47efe0`, `push 0x8; push 0x10; push
//     0x67fcb0`): `0x48a418`..`0x48a42c` blits it **up** with `0x485d00` when
//     `scroll > 0` and `0x48a438`..`0x48a44d` blits it **down** with `0x485cc0`
//     when `scroll + 10 < count`; both calls pass the destination
//     `0x68681f` / `0x68c33b` and the stride `0xc4` (196).  Inside the copy of
//     `PLATE` at `0x6865f8` those addresses are rows 2 and 121 at column 159:
//     `0x68681f - 0x6865f8 = 2*196 + 159` and `0x68c33b - 0x6865f8 =
//     121*196 + 159`, i.e. two pixels from the top/bottom edge;
//   * `0x485cc0` reads the sprite forward (16 bytes x 8 rows, zero bytes
//     transparent) and `0x485d00` reads the same 128 bytes **backwards**
//     (`mov al,[edx-1]; dec edx` at `0x485d1a`), so the up arrow is the down
//     arrow rotated by 180 degrees — one asset, two directions;
//   * `ESC TO CANCEL` (`0x4bd9d8`) is `0x485dd0(font=0, centered=0, x=38,
//     row=111, item=-1)` at `0x48a4a1`..`0x48a4b0`;
//   * the display text of a row is `0x48a4c0(stem)` (`level_select_name()`
//     below): `sprintf(buf, "%s - ", stem)` (`0x4bda04`) plus the rest of the
//     `LVLNAMES.SLU` line that starts with the **byte-exact** `<stem> `
//     (`0x48a505`..`0x48a51a`), at most 30 characters in total
//     (`cmp ecx,0x1e`); a stem without such a line shows the stem itself
//     (`0x48a53b`, `0x48a575`).
//
// Names of the shipped install
// ----------------------------
// `LVLNAMES.SLU` is uppercase (`LEVEL1 Testing Range`) while the shipped
// `*.sbw` files are lowercase (`level1.sbw`), and the compare above is a byte
// compare, so on **this** installation every one of the 36 stems falls back to
// the bare stem.  The titles appear on an install whose level files are
// uppercase (`LEVEL1.SBW`, the CD layout); the renderer implements the proven
// rule and does not fold case (docs/LEVEL_SELECT_RENDER.md §1/§3).
//
// Canvas, background and colours
// ------------------------------
// The canvas is the 320x200 mode-13h front-end buffer (`0x6d8fc0`).  The
// origin of the `PLATE` copy on screen is not documented, so the band is
// painted at (0,0) — the same convention (and caveat) as `ui/menu_render` and
// `ui/pager_render`.  `LevelSelectRenderOptions::background` names a
// `FRONTEND/BMP.IDX` entry to paint first; empty means the opaque black canvas.
// The screen's proven panel is `PLATE` (`kLevelSelectPanelName`, from
// `0x486050`) but it is **not** painted implicitly, so a caller that wants the
// pixels of the original must ask for it.  Glyphs are written with the palette
// of their face: row 0 -> (19,15,15) dark grey, row 4 -> (239,239,239) white
// (cursor), row 5 -> (155,15,15) dark red (title) — the ship-verified colours
// of `ui/menu_render`'s `FONTS4`.
//
// Exact fade (`M8.4` slice 8, docs/LEVEL_SELECT_RENDER.md §4b)
// ----------------------------------------------------------
// `LevelSelectRenderOptions::fade_lut` is the **exact** fade of `0x682f90`:
// every pixel keeps the palette index it came from (`PLATE`, `MENUARRW`, glyph
// atlas), the index goes through `render/hud_lut.h::map(level, index)` before
// the palette lookup, and only then becomes `0xRRGGBBAA`.  The table is either
// caller-supplied (`fade_lut_table`, e.g. `build_hud_lut` over `GAMEPAL`) or
// read from the 4096-byte dump (`fade_lut_dump`); without both the call refuses
// with `level_select_render: fade LUT needs a table ...`.  There is no RGBA
// model in this module, so `fade_lut` is the only fade path and 12..15 brighten
// instead of aliasing level 11.
//
// What is *not* drawn (docs/LEVEL_SELECT_RENDER.md §6)
// ---------------------------------------------------
// The `unlocked` flag paints nothing (the original list has **no** locks, so
// there is no documented marker and a counter-1 frame is byte-identical to a
// counter-25 frame); the `updown` sound; the mouse cursor; a level
// preview/thumbnail (screen 17 has none) and everything of `M9` (no CLI yet).
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"level_select_render: "`: an empty row list
// (`LevelSelect::build` was never called or refused the campaign state), an
// empty game directory, a font atlas that `FRONTEND/BMP.IDX` does not provide,
// a background entry that is missing/broken/not 8 bpp.  A missing
// `LVLNAMES.SLU` is **not** an error: the names fall back to the stems, exactly
// like `0x48a4c0` without its file.  On failure `rgba` is cleared and
// `width`/`height` are 0 (never a stale or half-drawn picture) and `*stats` is
// zeroed.  `stats`/`error` may be null.  The call only reads the installation,
// is deterministic and does not depend on the current working directory.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "ui/level_select.h"

namespace gm {

/// The 320x200 front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kLevelSelectCanvasWidth = 320;
inline constexpr int kLevelSelectCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kLevelSelectBackground = 0x000000FFu;

/// Title of screen 17 (`0x4bd9e8`).
inline constexpr std::string_view kLevelSelectTitle = "SELECT LEVEL FOR NEW GAME";
/// The only non-row text of the screen (`0x4bd9d8`).
inline constexpr std::string_view kLevelSelectCancel = "ESC TO CANCEL";
/// The screen's proven panel (`0x486050` copies it into the compose buffer).
inline constexpr std::string_view kLevelSelectPanelName = "PLATE";
/// Display names of the levels (`0x4bc270` = `0x4bca9c`).
inline constexpr std::string_view kLevelSelectNamesFile = "LVLNAMES.SLU";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is the same
/// rectangle, because `0x485dd0` writes into that copy.
inline constexpr int kLevelSelectPanelWidth = 196;
inline constexpr int kLevelSelectPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, items 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kLevelSelectTitleFont = 5;
inline constexpr int kLevelSelectRowFont = 0;
inline constexpr int kLevelSelectSelectedFont = 4;

/// Row 14 of the title (`0x48a3e1`), centred in the band.
inline constexpr int kLevelSelectTitleY = 14;
/// `x = 14` of every row (`push 0xe` at `0x48a481`).
inline constexpr int kLevelSelectRowX = 14;
/// `y = 27` of the first row (`0xffffffe5` at `0x48a46c`).
inline constexpr int kLevelSelectFirstRowY = 27;
/// Step of the screen (`mov [0x662d58], 8` at `0x48a3ce`).
inline constexpr int kLevelSelectRowStep = 8;
/// `ESC TO CANCEL` at (38, 111) (`push 0x26`, `push 0xffffff91`).
inline constexpr int kLevelSelectCancelX = 38;
inline constexpr int kLevelSelectCancelY = 111;
/// Cap of the display buffer in `0x48a4c0` (`cmp ecx,0x1e`).
inline constexpr int kLevelSelectNameMaxChars = 30;

/// Scroll arrows: the `MENUARRW` entry of `FRONTEND/BMP.IDX` (`0x4be2fc`,
/// loaded into `0x67fcb0` by `0x48fe59`), an 8 bpp 16x8 index sprite.
inline constexpr std::string_view kLevelSelectArrowName = "MENUARRW";
inline constexpr int kLevelSelectArrowWidth = 16;
inline constexpr int kLevelSelectArrowHeight = 8;
/// Anchor inside the 196x131 plate copy `0x6865f8`: `0x68681f` is
/// `2*196 + 159` and `0x68c33b` is `121*196 + 159` (both blits pass the 0xc4
/// stride), i.e. column 159, rows 2 (up arrow) and 121 (down arrow).
inline constexpr int kLevelSelectArrowX = 159;
inline constexpr int kLevelSelectArrowUpY = 2;
inline constexpr int kLevelSelectArrowDownY = 121;

/// Optional parts of one frame.
struct LevelSelectRenderOptions {
    /// Paint the title (`0x485dd0` at `0x48a3dd`, font 5, centred).
    bool title = true;
    /// Paint `ESC TO CANCEL` (`0x485dd0` at `0x48a4a1`, font 0).
    bool cancel = true;
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `kLevelSelectPanelName` (`PLATE`) is the proven panel of
    /// screen 17, but it is never painted implicitly.
    std::string background;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (docs/LEVEL_SELECT_RENDER.md §4b).  Takes precedence over the RGBA
    /// *model* (this module never had one, so the flag is the only fade path):
    /// every glyph, arrow and background pixel keeps the palette index of its
    /// source asset, the index goes through `render/hud_lut.h::map(level,
    /// index)` before the palette lookup, and only then becomes `0xRRGGBBAA`.
    /// Levels 12..15 are real tables (they brighten), not aliases of 11.  Needs
    /// a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables, unlike the RGBA model of `ui/pager_render`.
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read
    /// with `load_hud_lut`.  Used when `fade_lut_table` is null; a missing or
    /// wrong-sized file is an error, never a silently unfaded frame.
    std::string fade_lut_dump;
    /// A table already in memory (e.g. `build_hud_lut` over `GAMEPAL`); wins
    /// over `fade_lut_dump`.  The pointer is only read during the call, so the
    /// caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the number of level rows painted (`min(levels().size(), 10)`),
/// `selected_row` the index of the cursor in `LevelSelect::levels()` (so a
/// caller can tell which window row carries the highlight),
/// `ink_pixels` the canvas pixels covered by at least one glyph pixel (unique,
/// independent of the background), `missing_glyphs` the characters with no
/// glyph in the atlas (skipped, never fatal) and `arrows` the number of
/// `MENUARRW` sprites painted (0..2: up when `scroll > 0`, down when
/// `scroll + 10 < count`).  The arrow pixels are *not* counted in
/// `ink_pixels`, which stays "glyph ink".
struct LevelSelectRenderStats {
    int rows = 0;
    int selected_row = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// Window start of the 10-row list (`0x48a3f1`..`0x48a416`), proven:
/// `scroll = selected >= 5 ? selected - 5 : 0`; when `scroll + 10 >= count`
/// the window is clamped to `count - 10`; the result is never negative.
///
/// *count* is the number of rows, *selected* the cursor index (clamped to the
/// list).  A list of at most 10 rows always starts at 0.
int level_select_scroll(int count, int selected);

/// Display text of one row (`0x48a4c0`), given the bytes of `LVLNAMES.SLU`.
///
/// The line must start with *stem* followed by a space, compared **byte for
/// byte** (the original has no case folding); the result is `"<stem> - "` plus
/// the rest of that line, at most `kLevelSelectNameMaxChars` characters in
/// total.  Without such a line (or with an empty *level_names*) the stem is the
/// display text.  A missing file therefore degrades to the stems, never to an
/// error.
std::string level_select_name(std::string_view stem, std::string_view level_names);

/// Paint screen 17 of the current state of *select* into `rgba` as a 320x200
/// `0xRRGGBBAA` canvas.
///
/// Row `i` of the visible window sits at (`kLevelSelectRowX`,
/// `kLevelSelectFirstRowY + kLevelSelectRowStep * i`), the cursor row uses
/// font 4 instead of font 0, the title is centred at `kLevelSelectTitleY` and
/// `ESC TO CANCEL` sits at (`kLevelSelectCancelX`, `kLevelSelectCancelY`).
/// Every glyph is clipped to the 196x131 text band.  Returns false + `*error`
/// on each refusal of the header comment, and true with `width == 320`,
/// `height == 200` and `rgba.size() == 64000` otherwise.
bool render_level_select(const std::filesystem::path& game, const LevelSelect& select,
                         const LevelSelectRenderOptions& options,
                         std::vector<std::uint32_t>& rgba, int& width, int& height,
                         LevelSelectRenderStats* stats = nullptr,
                         std::string* error = nullptr);

} // namespace gm
