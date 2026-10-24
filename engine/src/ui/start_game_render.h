#pragma once

// One frame of the START GAME screen (13) as a 320x200 RGBA canvas (`M8.7`
// slice 5).
//
// Division of labour
// ------------------
// `ui/save_menu` is the state: the 20 parsed slots, their names and the
// highlight.  This module only *paints* screen 13 with the same building blocks
// as `ui/save_menu_render` (screen 14), `ui/level_select_render` (screen 17) and
// `ui/menu_render`: the atlases of `FRONTEND/BMP.IDX` (title font 5, rows 0,
// highlight 4), the optional 196x131 `PLATE` of `0x486050`, the `FILESLOT`
// 124x10 slot bars of `0x48ab90`, the `MENUARRW` 16x8 scroll arrows and the
// 320x200 mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3).  Nothing is decoded or
// measured twice (`render/text_layout` owns the atlases and the advances).
//
// Proven geometry (`0x48aa60`, docs/UI_SCREENS.md §2/§3.6, §5.1)
// -------------------------------------------------------------
// The handler is short (from `0x48aa60` to the `ret` at `0x48ab8f`); every
// number below is one of its sites (`docs/START_GAME_RENDER.md` §1):
//
//   * `0x48aa63` stores `rows 8` into `0x67f10c` and `0x48aa6d` sets the step
//     `0x662d58 = 10`; the eight selectable rows are the two entries (items 0/1)
//     and the six visible save slots (items 2..7);
//   * `0x486050` copies the 196x131 `PLATE` into the compose buffer `0x6865f8`;
//     every text call writes into that copy, so 196x131 is a hard clip;
//   * the title is `0x485dd0(font=5, text=0x4bdc20 "START GAME", centered=1,
//     x=38, row=12, item=-1)` at `0x48aab6`..`0x48aaca`; `centered` makes
//     `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`) and `item == -1` makes
//     the row absolute, so the title is centred at y = 12;
//   * item 0 (`New Single Player Game`, `0x4bdc08`) is
//     `0x485dd0(font=0, centered=0, x=35, row=23, item=0)`
//     (`0x48aacd`..`0x48aae1`) -> y = 23;
//   * item 1 (`New Multiplayer Game`, `0x4bdbf0`) is
//     `0x485dd0(font=0, centered=0, x=35, row=-33, item=1)`
//     (`0x48aae4`..`0x48aaf3`); a negative row is the absolute form
//     (`y = -row`, §3), so y = 33 — not an offset of the step;
//   * the slot loop `0x48aafb`..`0x48ab5d` walks `i = 0..5`:
//     - the name pointer is `0x6c2fe0 + (scroll + i) * 0x13` (`lea edx,[eax+eax*8]`
//       then `[eax+edx*2+0x6c2fe0]`, i.e. the 20-entry table of §5.1);
//     - `0x48ab90` first copies the `FILESLOT` band (124x10, see the next
//       paragraph) to `0x688707 + i*0x86c`, i.e. (35, 43 + 11*i);
//     - the text is `0x485dd0(font=0, centered=0, x=39, row=-(44 + 11*i),
//       item=i+2)`, so the slot rows are at y = 44 + 11*i, x = 39;
//     - `sub edi,0xb` (row -= 11) and `cmp edi,0xffffff92; jg` (-110) bound the
//       loop to exactly six rows;
//   * `ESC TO CANCEL` (`0x4bd9d8`) is `0x485dd0(font=0, centered=0, x=35,
//     row=-(4 + 11*6) = -110, item=-1)` (`0x48ab5f`..`0x48ab7a`), i.e. (35, 110):
//     the row follows the slot rows one step below the last one (99);
//   * the scroll `0x68ccdc` is the same window of §5.1: `scroll > 0`
//     (`0x48aa81`) blits `MENUARRW` **up** with `0x485d00` to `0x68681f`
//     (stride `0xc4` = 196), and `scroll + 6 >= 20` (`cmp eax,0x14` at
//     `0x48aa9f`) skips the **down** arrow, otherwise `0x485cc0` paints it at
//     `0x68c33b`.  Both anchors sit inside the `PLATE` copy `0x6865f8`:
//     `0x68681f - 0x6865f8 = 2*196 + 159` and `0x68c33b - 0x6865f8 =
//     121*196 + 159`, so column 159, rows 2 and 121 — the same two rectangles as
//     screens 14/17.  The arrows are painted before any text;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the text is not empty the face becomes 4.
//     There is no bar, box or `MENULIT` marker on this screen either.
//
// Negative rows (proven, not a scroll offset)
// -------------------------------------------
// The `-33`, `-44 … -99` and `-110` of §3.6 are *absolute* screen rows, not a
// list offset: `0x485dd0` computes `dest = 0x6865f8 + x - 196*row` for
// `row < 0` (`0x485e2b`..`0x485e3d`; `196*row` is `((3*row)<<4 + row)<<2`), so
// `y = -row`.  Only the slot *window* moves with the scroll — via the name
// pointer `0x6c2fe0 + (scroll + i)*0x13` and the fixed `i`-dependent rows.
//
// The `FILESLOT` slot band (proven geometry, slice 9)
// ---------------------------------------------------
// `0x48ab21` calls `0x48ab90(dest = 0x688707 + i*0x86c, stride = 0xc4, name)`;
// `0x48ab90` copies ten rows (`mov ebx,0xa` at `0x48abab`) of `0x1f` dwords
// (`push 0x1f` at `0x48abb0`, a dword count: `rep movs` of 31 dwords = 124
// bytes) from the `FILESLOT` buffer `0x661298` (source stride `0x7c` = 124 at
// `0x48abbe`) to the plate copy with the destination stride `0xc4` = 196.  So
// the band is the **whole 124x10 `FILESLOT` image** at (35, 43 + 11*i) of the
// 196x131 band (`0x688707 - 0x6865f8 = 43*196 + 35`), and `0x86c = 11*196` is
// the row step.  The band is copied before its own text (`0x48ab26` follows the
// call), so the name is painted over the bar; it is a plain `rep movs` — no
// transparency, index 0 would be copied too (the shipped art has none).
//
// Orientation follows the loader, not `parse_bmp`: `0x47efe0` reads
// `width*height` pixel bytes of the BMP verbatim (`0x47f0f8`, `fread`), so
// buffer row `r` is file row `r`, and the forward `rep movs` writes it to
// screen row `43 + i*11 + r`.  `parse_bmp` normalises BMP rows to top-down, so
// screen row `r` is `parse_bmp` row `height - 1 - r` — the same `flipV` the
// pager proved on readable headers (`docs/PAGER_RENDER.md` §2.4) and the same
// convention as the `MENUARRW` blit in this renderer.
//
// Row text (model, not a new format)
// ----------------------------------
// A slot band shows the table entry of its slot: the title of the save file for
// an occupied slot and `::: empty slot :::` (`0x4bc3b0`, §5.1) for an empty one.
// The model carries more per slot (`SaveSlotView::level`/`seconds`), but the
// original row prints only the name, so nothing else is invented into the
// picture.
//
// Canvas, background and colours
// ------------------------------
// `StartGameRenderOptions::background` names a `FRONTEND/BMP.IDX` entry painted
// at (0,0) before the text; an empty string means the opaque black canvas.  The
// proven panel of screen 13 is `PLATE` (196x131, `menu_panel_asset(Screen::
// StartGame)`), but it is never painted implicitly.  Glyphs are clipped to the
// 196x131 band, the buffer `0x485dd0` writes into.  Colours come from the atlas
// palette: title font 5 -> (155,15,15) dark red, rows font 0 -> (19,15,15) dark
// grey, highlight font 4 -> (239,239,239) white (the `FONTS4` faces,
// docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90` (slice 8, docs/START_GAME_RENDER.md §6)
// ----------------------------------------------------------------
// `StartGameRenderOptions::fade_lut` is the **exact** path of the sibling
// renderers (`ui/pager_render.cpp::FadeSource`, docs/PAGER_RENDER.md §2.6): the
// table of `0x682f90` is index -> index, so it is applied to the palette index
// of each painted pixel **before** the palette lookup
// (`render/hud_lut.h::map`).  The engine blits the composed 8-bpp buffer
// through `LUT[k]`, so the table must see the index the pixel was painted with,
// not the colour it became: this module keeps no index canvas, it maps at the
// pixel write (`PaletteFade` in the .cpp).  Levels 0..15 are taken as they are,
// so 12..15 brighten (they are real tables, not aliases of 11).  A pixel with
// no palette index (the black canvas when no background is asked for) stays
// black.
//
// The table either comes pre-loaded (`fade_lut_table`, built in memory by
// `build_hud_lut`) or from a 4096-byte dump (`fade_lut_dump`,
// docs/HUD_LUT.md §2).  Without both the call is a text refusal prefixed
// `start_game_render:`, never a silently unfaded frame.  The RGBA model
// (`fade`/`fade_level` of `ui/pager_render`) is not part of this screen's
// options: only the exact path is offered here.
//
// What is *not* drawn (docs/START_GAME_RENDER.md §5)
// -------------------------------------------------
// The `updown`/`select` sounds, the mouse cursor, the name-entry dialog (15);
// screen 13 has no level preview.  The `FILESLOT` band is drawn (slice 9) —
// `StartGameRenderOptions::fileslot`, on by default.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"start_game_render: "`: a slot list with no
// rows to draw, an empty game directory, a font atlas `FRONTEND/BMP.IDX` does
// not provide, a background entry that is missing/broken/not 8 bpp, a
// `FILESLOT` band that is missing/broken/not 124x10 while `fileslot` is on, and
// an exact fade without a table and a dump (or with an unreadable/wrong-sized
// dump).  On failure `rgba` is cleared, `width`/`height` are 0 (never a stale or
// half-drawn picture) and `*stats` is zeroed.  `stats`/`error` may be null.  The
// call only reads the installation, is deterministic and does not depend on the
// current working directory.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "ui/save_menu.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kStartGameCanvasWidth = 320;
inline constexpr int kStartGameCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kStartGameBackground = 0x000000FFu;

/// `START GAME` (`0x4bdc20`): the title pushed at `0x48aabe`.
inline constexpr std::string_view kStartGameTitle = "START GAME";
/// Item 0 (`0x4bdc08`), state 23.
inline constexpr std::string_view kStartGameSinglePlayer = "New Single Player Game";
/// Item 1 (`0x4bdbf0`), state 19.
inline constexpr std::string_view kStartGameMultiplayer = "New Multiplayer Game";
/// `ESC TO CANCEL` of screens 13/14/17 (`0x4bd9d8`).
inline constexpr std::string_view kStartGameCancel = "ESC TO CANCEL";
/// The panel of screen 13 (`menu_panel_asset(Screen::StartGame)`).
inline constexpr std::string_view kStartGamePanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kStartGamePanelWidth = 196;
inline constexpr int kStartGamePanelHeight = 131;

/// `0x67f10c = 8` (`0x48aa63`): the two entries and the six slot rows.
inline constexpr int kStartGameRows = 8;
/// `0x662d58 = 10` (`0x48aa6d`): the row step of the two entries.
inline constexpr int kStartGameItemStep = 10;
/// Selectable entries above the slots: items 0 and 1.
inline constexpr int kStartGameItems = 2;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kStartGameTitleFont = 5;
inline constexpr int kStartGameRowFont = 0;
inline constexpr int kStartGameSelectedFont = 4;

/// Title row (`push 0xc` at `0x48aab8`), centred in the band.
inline constexpr int kStartGameTitleY = 12;
/// `x = 35` of both entries (`push 0x23`), y = 23 and 33.
inline constexpr int kStartGameItemX = 35;
inline constexpr int kStartGameItemFirstY = 23;
/// `x = 39` of the slot rows (`push 0x27` at `0x48ab37`), `y = 44 + 11*i`.
inline constexpr int kStartGameSlotX = 39;
inline constexpr int kStartGameSlotFirstY = 44;
/// `sub edi,0xb` at `0x48ab50`: the slot rows step 11.
inline constexpr int kStartGameSlotStep = 11;
/// Six visible slots: the loop bound `row > -110` of `0x48ab5a`, and the
/// `scroll + 6 < 20` window of §5.1.
inline constexpr int kStartGameVisible = SaveMenu::kVisible;
/// Largest window start (0..13), the `SaveSlotList::kMaxScroll` of §5.1.
inline constexpr int kStartGameMaxFirst = SaveMenu::kMaxFirst;

/// `ESC TO CANCEL` at (35, 110): `row = -(4 + 11*6)` (`0x48ab5f`..`0x48ab6e`).
inline constexpr int kStartGameCancelX = 35;
inline constexpr int kStartGameCancelY = 110;

/// The `FILESLOT` slot bar of screens 13/14 (`0x48ab90`, `0x661298`), a 124x10
/// 8 bpp image.  `0x48ab21` copies it to `0x688707 + i*0x86c` of the plate copy
/// `0x6865f8`, i.e. (35, 43 + 11*i) with the whole 124x10 image; the copy is a
/// plain `rep movs` (31 dwords = 124 bytes per row, source stride 124, ten rows).
inline constexpr std::string_view kStartGameFileslotName = "FILESLOT";
inline constexpr int kStartGameFileslotWidth = 124;
inline constexpr int kStartGameFileslotHeight = 10;
inline constexpr int kStartGameFileslotX = 35;
/// `0x688707 - 0x6865f8 = 43*196 + 35`: the bar's top row, one row above the
/// slot text (`kStartGameSlotFirstY`), so the 10-row bar frames the 8-row glyph
/// cell.
inline constexpr int kStartGameFileslotFirstY = 43;

/// Scroll arrows: the `MENUARRW` entry of `FRONTEND/BMP.IDX`, an 8 bpp 16x8
/// index sprite (the same asset and anchors as screens 14/17).
inline constexpr std::string_view kStartGameArrowName = "MENUARRW";
inline constexpr int kStartGameArrowWidth = 16;
inline constexpr int kStartGameArrowHeight = 8;
/// `0x68681f`/`0x68c33b` inside the 196x131 plate copy `0x6865f8`: column 159,
/// rows 2 (up) and 121 (down).
inline constexpr int kStartGameArrowX = 159;
inline constexpr int kStartGameArrowUpY = 2;
inline constexpr int kStartGameArrowDownY = 121;

/// Optional parts of one frame.
struct StartGameRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 13, but it is
    /// never painted implicitly.
    std::string background;
    /// First slot of the six-row window (`0..kStartGameMaxFirst`, the
    /// `scroll 0x68ccdc` of §5.1).  Values outside the range are clamped; the
    /// effective value is `StartGameRenderStats::first_slot`.  The caller
    /// normally passes `SaveMenu::visible_first()`.
    int first_slot = 0;
    /// Paint the `FILESLOT` 124x10 bar under every visible slot row (35,
    /// 43 + 11*i), as `0x48ab90` does.  On by default: the original paints it
    /// unconditionally, the plate copy `0x6865f8` is its canvas.  The asset
    /// comes from `FRONTEND/BMP.IDX`; when it is missing/not 124x10 the call is
    /// a `start_game_render:` refusal — set this to `false` for a text-only
    /// frame (or a partial install), never to paper over a broken asset.
    bool fileslot = true;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (slice 8, docs/START_GAME_RENDER.md §6).  Needs a table, either
    /// pre-loaded (`fade_lut_table`) or read from `fade_lut_dump`; without both
    /// the call fails with `start_game_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten), unlike the RGBA model's clamp to 11.
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
/// `rows` is the selectable rows painted, `kStartGameItems + slot_rows` (the
/// proven `rows 8` while six slots are visible), `first_slot` the effective
/// window start (0..13), `slot_rows` the number of slot bands painted
/// (`min(6, slots - first_slot)`), `fileslot_bands` the number of `FILESLOT`
/// 124x10 bars copied (`== slot_rows` while `fileslot` is on, 0 otherwise),
/// `ink_pixels` the canvas pixels covered by at least one glyph pixel (unique,
/// independent of the background), `missing_glyphs` the characters with no
/// glyph in the atlas (skipped, never fatal) and `arrows` the number of
/// `MENUARRW` sprites painted (0..2: up when `first_slot > 0`, down when
/// `first_slot + 6 < 20`).  The arrow and `FILESLOT` pixels are *not* counted
/// in `ink_pixels`, which stays "glyph ink".
struct StartGameRenderStats {
    int rows = 0;
    int first_slot = 0;
    int slot_rows = 0;
    int fileslot_bands = 0;
    int ink_pixels = 0;
    int arrows = 0;
    int missing_glyphs = 0;
};

/// Paint screen 13 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kStartGameTitleY` (font 5), the two entries sit at
/// (`kStartGameItemX`, `kStartGameItemFirstY`) and y 33 (font 0), slot band `i`
/// of the window at (`kStartGameSlotX`, `kStartGameSlotFirstY +
/// kStartGameSlotStep * i`) carrying the table entry of slot `first_slot + i`,
/// and `ESC TO CANCEL` at (`kStartGameCancelX`, `kStartGameCancelY`).  The band
/// of the slot `menu.selected()` uses font 4 instead of font 0 when that slot is
/// inside the window.  With `options.fileslot` every visible row also gets the
/// `FILESLOT` 124x10 bar at (`kStartGameFileslotX`, `kStartGameFileslotFirstY +
/// kStartGameSlotStep * i`), painted before its own text.  Every glyph (and the
/// bar) is clipped to the 196x131 text band.  With `options.fade_lut` every
/// painted palette index (background, arrows, bar, glyphs) goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_start_game(const std::filesystem::path& game, const SaveMenu& menu,
                       const StartGameRenderOptions& options,
                       std::vector<std::uint32_t>& rgba, int& width, int& height,
                       StartGameRenderStats* stats = nullptr,
                       std::string* error = nullptr);

} // namespace gm
