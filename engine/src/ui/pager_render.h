#pragma once

// Rendering of one SML pager page into an RGBA canvas (`M8.1` slices 3-4,
// exact fade in `M8.5` slice 5).
//
// Division of labour
// ------------------
// `render/text_layout` paginates and measures, `ui/sml_pager` holds the cursor,
// this module only *paints the current page*: it walks the `LayoutRun`s that
// belong to `SmlPager::current_page()`, looks every byte up in the font atlas
// the layout used (the same `FRONTEND/BMP.IDX` faces) and writes the palette
// colour of each non-zero glyph pixel into a 320x200 buffer.  Nothing is
// re-measured: `run.x`/`run.y`/`run.font` come from the layout, exactly like
// `text_layout::draw_text` (`0x47faf0`).
//
// Canvas and coordinates
// ----------------------
// The page is 267x167 (`0x4972f0(0x10b, 0xa7)`, docs/UI.md §5.5) and the
// shipped screen is a 320x200 mode-13h buffer (`0x6d8fc0`, docs/UI.md §6.3), so
// the canvas is 320x200 and the page sits at its top-left corner: the 53 px
// right and 33 px bottom margins stay background.  The *screen* origin of the
// pager box is not documented (docs/UI.md §8 item 5: "точные координаты
// экранного origin ... нужна сверка с кадром игры"), so no guessed offset is
// applied.  Text is drawn with the atlas palette of its font (index 0 is
// transparent, like `0x47faf0`), never with the shared `GAMEPAL`.
//
// Pixels are packed `0xRRGGBBAA` (byte order of a PNG RGBA row), alpha 255:
// the atlases carry the reserved BMP byte in alpha, which is 0, so the
// compositor forces opaque pixels just like `bmp_to_rgba` in `main.cpp`.
//
// Background and fade (`M8.1` slice 4, docs/PAGER_RENDER.md §2.4/§2.5)
// ---------------------------------------------------------------------
// `PageRenderOptions::background` paints a 320x200 8-bpp screen of
// `FRONTEND/BMP.IDX` *before* the text: the documented pager background is
// `HELPSCRN` (docs/UI.md §6.3: the pager loads `SML\HELP.SML` into `0x6d8fc0`
// and draws `HELPSCRN`); `LOADPAGE` and `BRIEFBG2` are loaded into the same
// buffer by the neighbouring screens and are selectable by name, but binding
// *them* to the pager is not proven.  The asset is decoded like
// `gm/hud.py::read_panel` (BMP palette, index 0 opaque here, unlike glyphs)
// and flipped vertically: the shipped BMPs store their rows in file order and
// the engine buffer keeps that order, so `flipV(parse_bmp)` is the picture the
// player sees (verified on the readable `HELP`/`LOADING`/`MISSION OBJECTIVES`
// titles of the three assets).
//
// Fade (`M8.1` slice 4 / `M8.5` slice 5, docs/PAGER_RENDER.md §2.5/§2.6)
// ----------------------------------------------------------------
// Two paths, because the engine's tables are index -> index:
//
//  * `PageRenderOptions::fade` walks the finished RGBA frame through the fade
//    *curve* of the engine (`0x682f90`, 16 tables of 256 bytes built by
//    `0x483290(dst, k/11, 1)`).  The curve is documented (factor `k/11`,
//    identity at k=11, black at k=0) but the requantisation of a colour back to
//    a palette index is not, so this path multiplies the RGB channels and
//    rounds — a **model**, and it clamps 12..15 to 11.
//  * `PageRenderOptions::fade_lut` is the **exact** path: every painted pixel
//    keeps the palette index it came from (glyph atlas or screen bitmap), the
//    index goes through `render/hud_lut.h::map(level, index)` *before* the
//    palette lookup, and only then becomes `0xRRGGBBAA`.  Levels 0..15 are
//    taken from the table as they are, so 12..15 brighten instead of aliasing
//    11.  It needs the dump of `0x682f90` (`~/.cache/gunmetal-re/hud_lut.bin`,
//    `tools/win/dump_lut.lua`, docs/HUD_LUT.md) either pre-loaded or by path.
//    Untouched canvas pixels (the black background when no screen bitmap is
//    asked for) have no palette index and stay black.
//
// Known defect of slice 3, kept on purpose
// ----------------------------------------
// Glyph row `r` is written to `y + cell_height - 1 - r`, while the engine
// blitter (`0x47faf0`) and the screen-verified `gm/hud.py::draw_text` write it
// to `y + r`: the letters come out vertically mirrored (docs/PAGER_RENDER.md
// §5, verified on `F`/`L`).  Fixing it changes every slice-3 PNG byte for
// byte, and slice 4 had to keep that path identical, so the one-line fix is
// queued in §6, not applied here.
//
// What is *not* drawn (docs/PAGER_RENDER.md §5): `<I>` illustrations
// (`SML/<f>.bmp`), the `updown` sound, a selection/cursor highlight and the
// 320x200 interlace of the original.
//
// Errors are text (`std::string* error`) so the CLI can print `[error] pager:
// ...` and return 2: a closed pager, a font whose atlas is missing from the
// provider, a metrics-only atlas and a page whose fonts cannot be loaded all
// fail instead of producing a silently empty picture.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "ui/sml_pager.h"

namespace gm {

/// The 320x200 front-end buffer the briefing screen lives in (`0x6d8fc0`).
inline constexpr int kPagerCanvasWidth = 320;
inline constexpr int kPagerCanvasHeight = 200;

/// Top-left corner of the 267x167 page inside the canvas.  Zero because the
/// screen origin is not proven (see the header comment).
inline constexpr int kPagerPageX = 0;
inline constexpr int kPagerPageY = 0;

/// Opaque `#000`: the canvas background before any glyph is drawn (when no
/// screen bitmap is requested, or for its own black pixels).
inline constexpr std::uint32_t kPagerBackground = 0x000000FFu;

/// The pager background of the original: `SML\HELP.SML` is loaded into the
/// 320x200 buffer `0x6d8fc0` and `HELPSCRN` is drawn there (docs/UI.md §6.3).
inline constexpr const char* kPagerBackgroundAsset = "HELPSCRN";

/// Fade tables of the engine (`0x682f90`): levels 0..15.  Level 11 is
/// `0x483290(dst, 11/11, 1)` = identity; 12..15 are **not** aliases of 11 —
/// they brighten (132/73/48/45 colour identities out of 256, docs/HUD_LUT.md
/// §2), which the RGBA model cannot express and the exact LUT path can.
inline constexpr int kPagerFadeLevels = 16;
/// Level whose factor is 1 (`k/11`): the RGBA model clamps 12..15 to it.
inline constexpr int kPagerFadeMax = 11;

/// Optional layers of one render.  The defaults reproduce slice 3 exactly
/// (black canvas, no fade), so an old caller keeps its bytes.
struct PageRenderOptions {
    /// Paint a 320x200 `FRONTEND/BMP.IDX` screen before the text.
    bool background = false;
    /// Which screen; only meaningful when `background` is true.  The proven
    /// pager background is `HELPSCRN`; `LOADPAGE`/`BRIEFBG2` load into the
    /// same buffer on their own screens (docs/UI.md §7.1).
    std::string background_asset = kPagerBackgroundAsset;
    /// Apply the fade *model* of `0x682f90` to the finished frame (RGBA path).
    bool fade = false;
    /// Fade level 0..15 of the model (clamped); 0 = black frame, 11 = identity.
    int fade_level = kPagerFadeMax;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (docs/PAGER_RENDER.md §2.6).  Takes precedence over `fade`: when both are
    /// set, the model is skipped.  Needs a table, either pre-loaded
    /// (`fade_lut_table`) or read from `fade_lut_dump`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten), unlike `fade_level`.
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read
    /// with `load_hud_lut`.  Used when `fade_lut_table` is null; a missing or
    /// wrong-sized file is an error, never a silently unfaded frame.
    std::string fade_lut_dump;
    /// A table already in memory; wins over `fade_lut_dump`.  The pointer is
    /// only read during the call, so the caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers a render produced.
///
/// `ink_pixels` counts the canvas pixels covered by at least one glyph pixel
/// (index != 0), so it does not depend on the background colour: with no
/// background it is the same number as slice 3 (`!= kPagerBackground`).
/// `contrast` is the luma range of exactly those pixels (BT.601 weights,
/// `(77*r + 150*g + 29*b) >> 8`), i.e. how well the text reads on the screen
/// bitmap; both are 0 when the page has no glyph pixels.
struct PageRenderStats {
    int lines = 0;           ///< `LayoutLine`s of the current page
    int glyphs = 0;          ///< characters resolved in the atlas (spaces too)
    int missing_glyphs = 0;  ///< characters with no glyph (skipped, never fatal)
    int ink_pixels = 0;      ///< pixels written by glyphs (unique)
    int background_pixels = 0; ///< non-black pixels of the screen bitmap
    int contrast_min = 0;    ///< darkest text pixel (luma 0..255)
    int contrast_max = 0;    ///< brightest text pixel (luma 0..255)
};

/// Draw the *current* page of `pager` into `rgba` (320x200, `0xRRGGBBAA`).
///
/// `fonts` is the atlas provider the caller passed to `SmlPager` (or an
/// equivalent one); it must return the faces the layout used, or the call
/// fails with `"pager_render: no atlas for font N"`.  `width`/`height` are set
/// to the canvas size and `rgba` is resized even on failure, so a caller never
/// sees a stale buffer.
///
/// `stats` and `error` may be null.  Returns false + `*error` when the pager is
/// closed (no current page), there is no font provider, an atlas is missing or
/// metrics-only (no glyph pixels), or `rgba` cannot be built.
bool render_page(const SmlPager& pager, const FontLookup& fonts,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 PageRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same with optional layers (`background`, `fade`, `fade_lut`).  This overload
/// has no game directory, so it cannot read a screen bitmap: `background ==
/// true` fails with `"pager_render: background ... needs a game directory"`
/// instead of silently drawing black.  A fade needs no files (the exact path
/// needs the dump or a `HudLut`, both caller-supplied) and works here.
bool render_page(const SmlPager& pager, const FontLookup& fonts,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 const PageRenderOptions& options, PageRenderStats* stats = nullptr,
                 std::string* error = nullptr);

/// Same, loading the page's atlases from `<game>/FRONTEND/BMP.IDX` through
/// `load_font` (the CLI path; the files are only read).  A font the engine
/// never initialises cannot occur here: the layout would have refused the
/// document first.
bool render_page(const SmlPager& pager, const std::filesystem::path& game,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 PageRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, additionally decoding `options.background_asset` from the same
/// `FRONTEND/BMP.IDX` (`HELPSCRN` by default) and applying `options.fade` or
/// `options.fade_lut`.  A missing/wrong-sized background is an error, never a
/// silently black frame; so is a missing/unreadable fade dump.
bool render_page(const SmlPager& pager, const std::filesystem::path& game,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 const PageRenderOptions& options, PageRenderStats* stats = nullptr,
                 std::string* error = nullptr);

} // namespace gm
