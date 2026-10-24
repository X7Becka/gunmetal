#pragma once

// One frame of screen 23, `SET DIFFICULTY LEVEL` (`M8.8` slice 2).
//
// Division of labour
// ------------------
// There is no state object for this screen: it is a flat list of six ranks, so
// this module paints the whole 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3) from the options alone.  The building blocks are the same as
// `ui/start_game_render` (screen 13), `ui/level_select_render` (17) and
// `ui/keycfg_render` (30/34/35): the `FONTS4` atlases of `FRONTEND/BMP.IDX`
// (title face 5, rows 0, highlight 4), the optional 196x131 `PLATE` of
// `0x486050`, and the exact fade tables of `0x682f90`.  Nothing is decoded or
// measured twice: `render/text_layout` owns the atlases and the advances.
//
// Proven geometry (`0x48a9a0`, docs/UI_SCREENS.md §2, docs/DIFFICULTY_RENDER.md
// §1)
// -------
// The handler is nine instructions plus six text calls (from `0x48a9a0` to the
// `ret` at `0x48aa5a`); every number below is one of its sites:
//
//   * `0x48a9a0` stores `rows 6` into `0x67f10c`, `0x48a9aa` sets the step
//     `0x662d58 = 15`; `0x48a9b4` calls `0x486050`, which copies the 196x131
//     `PLATE` into the compose buffer `0x6865f8` — every text call writes into
//     that copy, so 196x131 is a hard clip;
//   * the title is `0x485dd0(font=5, text=0x4bdbd8 "SET DIFFICULTY LEVEL",
//     centered=1, x=38, row=14, item=-1)` (`0x48a9b9`..`0x48a9c8`);
//     `centered` makes `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`) and
//     `item == -1` makes the row absolute, so the title is centred at y = 14;
//   * the six ranks are `0x485dd0(font=0, centered=0, x=40, row=27,
//     item=0..5)` (`0x48a9d0`..`0x48aa57`), i.e. `y = 27 + 15*item`:
//     27 `Private` (`0x4bdbd0`), 42 `Corporal` (`0x4bdbc4`),
//     57 `Lieutenant` (`0x4bdbb8`), 72 `Captain` (`0x4bdbb0`),
//     87 `Major` (`0x4bdba8`), 102 `Cancel` (`0x4bda0c`);
//   * there is no other call: no arrow (`MENUARRW`), no slider, no checkbox and
//     **no separate `ESC` line** on this screen (`ESC TO CANCEL` belongs to
//     13/14/17).  The way back is item 5 `Cancel`;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the text is not empty the face becomes 4.
//     There is no bar, box or `MENULIT` marker;
//   * the cursor is a dword of the front end (`0x68ccf4`, docs/UI.md §6.2), not
//     a per-screen field.  Both entries into state 23 write the *same* initial
//     value **2** (`Lieutenant`): `0x48daa0` from `START GAME` item 0 and
//     `0x48e28b` from the level list.  Zeroing on the way to the player-name
//     screen (`0x48e3bd`) shows it is a plain index into the six rows.
//     `DifficultyRenderOptions::cursor` is that index; the original default 2
//     is `kDifficultyDefaultCursor`.
//
// What the ranks mean (model, not this renderer)
// ----------------------------------------------
// Selecting a rank copies a 16-byte rank name into the callsign buffer
// `0x4bc2b8` (jump table `0x48e960`, cases `0x48e2d6` `Private` …
// `0x48e35e` `Major`; `0x48e378` is the out-of-range fallback `First Seaman`,
// `0x4bdd14`) and continues to the player-name screen 24 (`0x48e3c7`).
// `Cancel` is the `cursor >= rows - 1` branch (`0x48e2b5`..`0x48e2b7` →
// `0x48e473`), not a rank.  This module paints the *frame*; it does not read or
// write any of those globals and has no game-difficulty state.
//
// Colours, canvas and background
// ------------------------------
// `DifficultyRenderOptions::background` names a `FRONTEND/BMP.IDX` entry painted
// at (0,0) before the text; an empty string means the opaque black canvas.  The
// proven panel of screen 23 is `PLATE` (`menu_panel_asset(Screen::Difficulty)`),
// but it is never painted implicitly.  Glyphs are clipped to the 196x131 band
// `0x485dd0` writes into.  Colours come from the atlas palette: title font 5 ->
// (155,15,15) dark red, rows font 0 -> (19,15,15) dark grey, highlight font 4 ->
// (239,239,239) white (the `FONTS4` faces, docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// `fade_lut` is the same exact path as `ui/start_game_render` and
// `ui/pager_render.cpp::FadeSource`: the tables are index -> index, so the level
// is applied to the palette index of each painted pixel **before** the palette
// lookup (`render/hud_lut.h::map`).  Levels 0..15 are taken as they are (12..15
// brighten, they are real tables).  A pixel with no palette index (the black
// canvas) stays black.  The table either comes pre-loaded (`fade_lut_table`) or
// from a 4096-byte dump (`fade_lut_dump`, docs/HUD_LUT.md §2); without both the
// call is a text refusal, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"difficulty_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX` or one of the faces 5/0/4, a
// background entry that is missing/broken/not 8 bpp, and an exact fade without a
// table and a dump (or with an unreadable/wrong-sized dump).  On failure `rgba`
// is cleared, `width`/`height` are 0 (never a stale or half-drawn picture) and
// `*stats` is zeroed.  `stats`/`error` may be null.  The call only reads the
// installation, is deterministic and does not depend on the current working
// directory.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kDifficultyCanvasWidth = 320;
inline constexpr int kDifficultyCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kDifficultyBackground = 0x000000FFu;

/// `SET DIFFICULTY LEVEL` (`0x4bdbd8`): the title pushed at `0x48a9c1`.
inline constexpr std::string_view kDifficultyTitle = "SET DIFFICULTY LEVEL";

/// The six rows of `0x48a9d0`..`0x48aa57`, in `item` order (0..5).
inline constexpr std::string_view kDifficultyItems[6] = {
    "Private",     // item 0, `0x4bdbd0`
    "Corporal",    // item 1, `0x4bdbc4`
    "Lieutenant",  // item 2, `0x4bdbb8`
    "Captain",     // item 3, `0x4bdbb0`
    "Major",       // item 4, `0x4bdba8`
    "Cancel",      // item 5, `0x4bda0c` — the way back, not a rank
};
inline constexpr int kDifficultyItemCount = 6;

/// The panel of screen 23 (`menu_panel_asset(Screen::Difficulty)`).
inline constexpr std::string_view kDifficultyPanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kDifficultyPanelWidth = 196;
inline constexpr int kDifficultyPanelHeight = 131;

/// `0x67f10c = 6` (`0x48a9a0`): six ranks, `Cancel` included.
inline constexpr int kDifficultyRows = kDifficultyItemCount;
/// `0x662d58 = 15` (`0x48a9aa`): the row step of `0x485dd0`.
inline constexpr int kDifficultyItemStep = 15;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kDifficultyTitleFont = 5;
inline constexpr int kDifficultyRowFont = 0;
inline constexpr int kDifficultySelectedFont = 4;

/// Title row (`push 0xe` at `0x48a9bb`), centred in the band.
inline constexpr int kDifficultyTitleY = 14;
/// `x = 40` for every rank (`push 0x28`), base row 27 (`push 0x1b`), step 15.
inline constexpr int kDifficultyItemX = 40;
inline constexpr int kDifficultyItemFirstY = 27;

/// The cursor written on both entries into the screen (`0x48daa0`,
/// `0x48e28b`): item 2, `Lieutenant`.  `DifficultyRenderOptions::cursor`
/// defaults to 0 as a neutral API value; a caller that wants the original
/// first frame passes this constant.
inline constexpr int kDifficultyDefaultCursor = 2;

/// Optional parts of one frame.
struct DifficultyRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 23, but it is
    /// never painted implicitly.
    std::string background;
    /// `[0x68ccf4]`: index of the highlighted row, 0..5 (`0` = `Private`, `5` =
    /// `Cancel`).  Values outside the range are clamped; the effective value is
    /// `DifficultyRenderStats::rows`-independent and is not echoed in the
    /// stats — the highlight is visible in the pixels.  The original enters the
    /// screen with 2 (`kDifficultyDefaultCursor`).
    int cursor = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (docs/DIFFICULTY_RENDER.md §5).  Needs a table, either pre-loaded
    /// (`fade_lut_table`) or read from `fade_lut_dump`; without both the call
    /// fails with `difficulty_render: fade LUT needs ...`.
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
/// `rows` is the six painted rows (the proven `0x67f10c = 6`), `ink_pixels` the
/// canvas pixels covered by at least one glyph pixel (unique, independent of
/// the background), `missing_glyphs` the characters with no glyph in the atlas
/// (skipped, never fatal) and `arrows` the number of `MENUARRW` sprites painted.
/// Screen 23 has no scroll window and no arrow call at `0x48a9a0`, so `arrows`
/// is always 0; the field exists to keep the stats shape of the sibling
/// renderers, and the test pins the 0.
struct DifficultyRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// Paint screen 23 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kDifficultyTitleY` (font 5) and row `item` sits at
/// (`kDifficultyItemX`, `kDifficultyItemFirstY + kDifficultyItemStep * item`)
/// with `kDifficultyItems[item]` (font 0); the row `options.cursor` (clamped to
/// 0..5) uses font 4 instead.  Every glyph is clipped to the 196x131 text band.
/// With `options.fade_lut` every painted palette index (background, glyphs) goes
/// through `render/hud_lut.h::map` at the level before its colour is looked up;
/// the untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_difficulty(const std::filesystem::path& game,
                       const DifficultyRenderOptions& options,
                       std::vector<std::uint32_t>& rgba, int& width, int& height,
                       DifficultyRenderStats* stats = nullptr,
                       std::string* error = nullptr);

} // namespace gm
