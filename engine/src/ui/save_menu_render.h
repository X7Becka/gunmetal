#pragma once

// One frame of the SAVE GAME screen (14) as a 320x200 RGBA canvas (`M8.7`
// slice 3).
//
// Division of labour
// ------------------
// `ui/save_menu` is the state: the 20 parsed slots, the six-row window, the
// highlight and the load/save/back event.  This module only *paints* that
// state with the same building blocks as `ui/level_select_render` and
// `ui/menu_render`: the atlases of `FRONTEND/BMP.IDX` (font 0 rows, font 4
// highlight), the optional 196x131 `PLATE`, the `FILESLOT` 124x10 slot bars
// (slice 10) and the 320x200 mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3).
// Nothing is decoded or measured twice (`render/text_layout` owns the atlases
// and the advances).
//
// Proven geometry (docs/UI_SCREENS.md §3.6, §5.1)
// -----------------------------------------------
// The numbers below are the source-of-truth table of the screen handler, not a
// picture guess:
//
//   * `rows 6`, step 12; the slot rows start at y = 28 and step 12
//     (`y = 28 + 12*i`, i = 0..5), the slot name sits at x = 42;
//   * `ESC TO CANCEL` is row 101 at x = 38;
//   * 20 slots exist and 6 are visible, the scroll `0x68ccdc` is bounded by
//     0..13 (`scroll > 0` and `scroll + 6 < 20` in `0x48aa60`/`0x48abd0`,
//     §5.1) - the window of the model (`SaveMenu::visible_first()`), slot 19
//     is never reachable (`SaveMenu::kLastReachable` = 18);
//   * the highlight is the cursor row in font 4 (the same font-switch rule as
//     screen 17, docs/UI_SCREENS.md §3); there is no bar, box or `MENULIT`
//     marker;
//   * `::: empty slot :::` is the label of a slot whose name cannot be read
//     (`0x4bc3b0`, §5.1) - here `SaveMenu::label()` already answers it.
//
// The scroll arrows are the same `MENUARRW` sprite screen 17 uses: `0x48abf1`
// blits it up with `0x485d00` when `scroll 0x68ccdc > 0`, `0x48ac07` blits it
// down with `0x485cc0` when `scroll + 6 < 20`, both at the destination
// `0x68681f` / `0x68c33b` with stride 0xc4 (196) — inside the copy of `PLATE`
// at `0x6865f8` that is column 159, rows 2 and 121
// (`0x68681f - 0x6865f8 = 2*196 + 159`, `0x68c33b - 0x6865f8 = 121*196 + 159`).
// `0x485d00` reads the 16x8 sprite backwards, so the up arrow is the down
// arrow rotated by 180 degrees (docs/SAVE_MENU_RENDER.md §1).
//
// Row text (model, not a new format)
// ----------------------------------
// Each band shows `SaveMenu::label(row)`: the save title for an occupied slot
// and `::: empty slot :::` for an empty one.  That is exactly the text of the
// original rows (`0x6c2fe0 + slot*0x13`, §5.1).  The model also carries the
// level name and the elapsed seconds (`SaveSlotView::level`/`seconds`), but the
// original row does not print them, so they are not invented into the picture
// here.
//
// `FILESLOT` slot bars (`M8.7` slice 10, docs/SAVE_MENU_RENDER.md §1)
// ------------------------------------------------------------------
// `0x48ac5d` calls `0x48ab90(dest, 0xc4, name)` before the text of row `i`,
// with `dest = 0x687aca + i*0x930`; that is the same 124x10 `FILESLOT` entry
// screen 13 uses (buffer `0x661298`, `0x48ab90` = ten rows of `0x1f` dwords =
// 124 bytes, stride `0xc4` = 196).  Relative to the plate copy `0x6865f8`,
// `0x14d2 = 27*196 + 38` and `0x930 = 12*196`, so the bar of row `i` is the
// whole sprite at (38, 27 + 12*i): one pixel above the text row (y 28 + 12*i)
// and four left of its x = 42 — the same insets screen 13 has (43/44, 35/39).
// The bar is opaque (the copy has no index test, and the shipped art has no
// index 0), so the name is painted over the bar, never over the plate.
//
// Canvas, background and colours
// ------------------------------
// `SaveMenuRenderOptions::background` names a `FRONTEND/BMP.IDX` entry painted
// at (0,0) before the text; an empty string means the opaque black canvas.
// The proven panel of screen 14 is `PLATE` (196x131, the same asset screen 17
// and the menu screens use, `menu_panel_asset(Screen::SaveGame)`), but it is
// never painted implicitly.  Glyphs are clipped to the 196x131 band, the
// buffer the original text renderer writes into (`0x485dd0`, docs/UI_SCREENS.md
// §3).  Colours come from the atlas palette: row font 0 -> (19,15,15) dark
// grey, highlight font 4 -> (239,239,239) white (the `FONTS4` faces,
// docs/MENU_RENDER.md §3).
//
// Exact fade (`M8.7` slice 8, docs/SAVE_MENU_RENDER.md §3b)
// ---------------------------------------------------------
// `SaveMenuRenderOptions::fade_lut` is the **exact** fade of `0x682f90`: every
// pixel keeps the palette index it came from (`PLATE`, `MENUARRW`, glyph
// atlas), the index goes through `render/hud_lut.h::map(level, index)` before
// the palette lookup, and only then becomes `0xRRGGBBAA`.  The table is either
// caller-supplied (`fade_lut_table`, e.g. `build_hud_lut` over `GAMEPAL`) or
// read from the 4096-byte dump (`fade_lut_dump`); without both the call refuses
// with `save_menu_render: fade LUT needs a table ...`.  There is no RGBA model
// in this module, so `fade_lut` is the only fade path and 12..15 brighten
// instead of aliasing level 11.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"save_menu_render: "`: a slot list with no
// rows to draw, an empty game directory, a font atlas `FRONTEND/BMP.IDX` does
// not provide, a background entry that is missing/broken/not 8 bpp, a
// `FILESLOT` entry that is missing/broken/not 124x10 while `fileslot` is on, a
// fade LUT without a table and a dump (`fade LUT ...`).  On
// failure `rgba` is cleared and `width`/`height` are 0 (never a stale or
// half-drawn picture) and `*stats` is zeroed.  `stats`/`error` may be null.
// The call only reads the installation, is deterministic and does not depend
// on the current working directory.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "ui/save_menu.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kSaveMenuCanvasWidth = 320;
inline constexpr int kSaveMenuCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kSaveMenuBackground = 0x000000FFu;

/// `ESC TO CANCEL` of screens 13/14 (docs/UI_SCREENS.md §3.6).
inline constexpr std::string_view kSaveMenuCancel = "ESC TO CANCEL";
/// The panel of screen 14 (`menu_panel_asset(Screen::SaveGame)`).
inline constexpr std::string_view kSaveMenuPanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kSaveMenuPanelWidth = 196;
inline constexpr int kSaveMenuPanelHeight = 131;

/// Rows of the screen (`§3.6: rows 6`), the same six rows the model exposes.
inline constexpr int kSaveMenuRows = SaveMenu::kVisible;
/// docs/UI_SCREENS.md §3: rows 0, highlight 4.
inline constexpr int kSaveMenuRowFont = 0;
inline constexpr int kSaveMenuSelectedFont = 4;

/// `(save slot name)` at x = 42 (`§3.6`).
inline constexpr int kSaveMenuRowX = 42;
/// First slot row: y = 28 (`§3.6`), step 12 below.
inline constexpr int kSaveMenuFirstRowY = 28;
/// `§3.6: rows 6, шаг 12`.
inline constexpr int kSaveMenuRowStep = 12;
/// `ESC TO CANCEL` at row 101, x = 38 (`§3.6`).
inline constexpr int kSaveMenuCancelX = 38;
inline constexpr int kSaveMenuCancelY = 101;

/// The `FILESLOT` band of the slot rows (`0x48ab90`, same asset as screen 13):
/// a 124x10 8 bpp sprite of `FRONTEND/BMP.IDX`.
inline constexpr std::string_view kSaveMenuFileslotName = "FILESLOT";
inline constexpr int kSaveMenuFileslotWidth = 124;
inline constexpr int kSaveMenuFileslotHeight = 10;
/// `0x687aca - 0x6865f8 = 27*196 + 38` and `add ebx,0x930` at `0x48ac93` with
/// `0x930 = 12*196`: the bar of row i is at (38, 27 + 12*i), i.e. one pixel
/// above the text row and four left of its x 42.
inline constexpr int kSaveMenuFileslotX = 38;
inline constexpr int kSaveMenuFileslotFirstY = 27;

/// Scroll arrows: the `MENUARRW` entry of `FRONTEND/BMP.IDX`, an 8 bpp 16x8
/// index sprite (the same asset and anchors as screen 17).
inline constexpr std::string_view kSaveMenuArrowName = "MENUARRW";
inline constexpr int kSaveMenuArrowWidth = 16;
inline constexpr int kSaveMenuArrowHeight = 8;
/// `0x68681f` and `0x68c33b` inside the 196x131 plate copy `0x6865f8`:
/// column 159, rows 2 (up) and 121 (down).
inline constexpr int kSaveMenuArrowX = 159;
inline constexpr int kSaveMenuArrowUpY = 2;
inline constexpr int kSaveMenuArrowDownY = 121;

/// Optional parts of one frame.
struct SaveMenuRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 14, but it is
    /// never painted implicitly.
    std::string background;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (docs/SAVE_MENU_RENDER.md §3b).  Every glyph, arrow and background pixel
    /// keeps the palette index of its source asset, the index goes through
    /// `render/hud_lut.h::map(level, index)` before the palette lookup, and
    /// only then becomes `0xRRGGBBAA`.  Levels 12..15 are real tables (they
    /// brighten), not aliases of 11.  Needs a table, either pre-loaded
    /// (`fade_lut_table`) or read from `fade_lut_dump`.
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

    /// Paint the `FILESLOT` 124x10 bar at (38, 27 + 12*i) under every visible
    /// slot row, as `0x48ac5d` does (on by default: the original paints it
    /// unconditionally).  The asset is mandatory: a missing or differently
    /// sized entry is a text failure.  `false` is a deliberate text-only frame
    /// (partial install, stands), not a way to hide a broken asset.
    bool fileslot = true;
};

/// Numbers of one frame.
///
/// `rows` is the number of slot bands painted (`min(6, slots-slot window)`),
/// `scroll` the first visible slot (the model's `visible_first()`, 0..13),
/// `selected_row` the absolute slot index carrying the highlight (0..18),
/// `ink_pixels` the canvas pixels covered by at least one glyph pixel (unique,
/// independent of the background), `missing_glyphs` the characters with no
/// glyph in the atlas (skipped, never fatal), `slot_rows` the number of the
/// painted bands whose slot is occupied (a real save, not `::: empty slot :::`)
/// and `arrows` the number of `MENUARRW` sprites painted (0..2: up when
/// `scroll > 0`, down when `scroll + 6 < 20`), `fileslot_bands` the number of
/// `FILESLOT` 124x10 bars copied (one per painted row while `fileslot` is on,
/// 0 without it).  The arrow and `FILESLOT` pixels are *not* counted in
/// `ink_pixels`, which stays "glyph ink".
struct SaveMenuRenderStats {
    int rows = 0;
    int scroll = 0;
    int selected_row = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int slot_rows = 0;
    int arrows = 0;
    int fileslot_bands = 0;
};

/// Paint screen 14 of the current state of *menu* into `rgba` as a 320x200
/// `0xRRGGBBAA` canvas.
///
/// Row `i` of the six-row window sits at (`kSaveMenuRowX`,
/// `kSaveMenuFirstRowY + kSaveMenuRowStep * i`) and carries
/// `menu.label(i)`; the cursor slot uses font 4 instead of font 0 and
/// `ESC TO CANCEL` sits at (`kSaveMenuCancelX`, `kSaveMenuCancelY`).  With
/// `options.fileslot` every row gets the 124x10 `FILESLOT` bar at
/// (`kSaveMenuFileslotX`, `kSaveMenuFileslotFirstY + kSaveMenuRowStep * i`)
/// before its text, so the name lies on the bar.  Every glyph is clipped to the
/// 196x131 text band.  Returns false + `*error` on each refusal of the header
/// comment, and true with `width == 320`, `height == 200` and
/// `rgba.size() == 64000` otherwise.
bool render_save_menu(const std::filesystem::path& game, const SaveMenu& menu,
                      const SaveMenuRenderOptions& options,
                      std::vector<std::uint32_t>& rgba, int& width, int& height,
                      SaveMenuRenderStats* stats = nullptr,
                      std::string* error = nullptr);

} // namespace gm
