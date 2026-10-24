#pragma once

// One frame of a mission briefing: `SML/brief/<level>.SME` -> 320x200 RGBA
// (`M8.3` slice 3).
//
// Division of labour
// ------------------
// Nothing is decoded or laid out here.  `ui/briefing` already maps a level to
// its file (the same case-insensitive scan of `SML/brief` the engine loader
// does, `0x497310`), decodes the `.sme` and paginates it into 267x167 pages
// with `SmlPager`; `ui/pager_render` already paints the current page with the
// font atlases of `FRONTEND/BMP.IDX` into the 320x200 buffer of the front end
// (`0x6d8fc0`).  This module is the glue: load through the index, show one
// page, hand the pager to the renderer.  The page box, the fonts, the wrapping
// and the pixel statistics are therefore the pager's, not a second model
// (docs/BRIEFING.md §1, docs/PAGER_RENDER.md §2).
//
// Page numbering
// --------------
// `BriefingRenderOptions::page` is **1-based**, exactly like `--page N` of the
// pager CLI (docs/PAGER_RENDER.md §3): 1 is the first page and
// `BriefingRenderStats::pages` the last.  There is no implicit "0 = first":
// 0, a negative value and anything above the page count are refused with a
// text.  The field defaults to 0 only because the options struct must stay
// value-initialisable; a caller that wants the first page must say
// `page = 1`, so a forgotten field can never silently render page 1.
//
// Background and fade
// -------------------
// Both layers are the pager's, not a briefing-specific model: `background`
// paints the same `HELPSCRN` screen before the text, `fade`/`fade_level` run
// the same fade model (0 = black, 11 = identity, 12..15 clamped; the curve
// `0x682f90` / `0x483290`, docs/PAGER_RENDER.md §2.4/§2.5), and
// `fade_lut`/`fade_lut_level`/`fade_lut_dump`/`fade_lut_table` the **exact**
// tables of `0x682f90` applied to the palette index (docs/PAGER_RENDER.md
// §2.6).  All four LUT fields are forwarded to `PageRenderOptions` unchanged,
// so the exact path, its level 11 identity and the real 12..15 tables are the
// pager's, not a second implementation; `fade_lut` wins over `fade` there.
// A LUT without a table and without a dump is a refusal, never a silently
// unfaded frame.  The *mission briefing* screen of the original also loads
// `BRIEFBG2` into the same 320x200 buffer (docs/UI_SCREENS.md §9), but binding
// that asset to the briefing renderer is not proven and this slice
// deliberately mirrors `--pager`; the open question is listed in
// docs/BRIEFING_RENDER.md §5.
//
// What is *not* drawn
// -------------------
// `<I>` illustrations (`SML/<f>.bmp`; the layout already reserves their float
// slots, so the text sits where it belongs), a page cursor/highlight, the
// `updown` sound, and the par time / `YOUR BEST` column — neither is part of
// a `.sme` (docs/BRIEFING.md §5).
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"briefing_render: "`: missing game
// directory, `SML/brief`, level or file; an empty/broken `.sme`; a page
// outside `1..pages`; missing `FRONTEND/BMP.IDX`; a pager/render failure.  On
// failure `rgba` is cleared and `width`/`height` are 0 (never a stale picture),
// and `*stats` is zeroed.  `stats` and `error` may be null.  The call only
// reads the installation, is deterministic and does not depend on the current
// working directory.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// Which page and which optional layers of one briefing frame.
struct BriefingRenderOptions {
    /// Paint the 320x200 pager screen (`HELPSCRN`) before the text.
    bool background = false;
    /// 1-based page number; 0 and everything outside `1..pages` is refused.
    int page = 0;
    /// Walk the finished frame through the fade curve of `0x682f90`.
    bool fade = false;
    /// Fade level 0..15 (clamped by the renderer); 0 = black, 11 = identity.
    int fade_level = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (docs/PAGER_RENDER.md §2.6) instead of the RGBA model above.  Takes
    /// precedence over `fade`; needs a table, either pre-loaded
    /// (`fade_lut_table`) or read from `fade_lut_dump`.
    bool fade_lut = false;
    /// Level of the exact path, 0..15; 11 is the identity and 12..15 are real
    /// tables (they brighten), unlike `fade_level`.
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2); used
    /// only when `fade_lut_table` is null.  A missing/wrong-sized dump is a
    /// refusal, never a silently unfaded frame.
    std::string fade_lut_dump;
    /// A table already in memory; wins over `fade_lut_dump` (so this path
    /// needs no file at all).  Only read during the call.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `pages` is the page count of the whole briefing (`Briefing::page_count`);
/// the other fields describe the rendered page and are exactly the
/// `PageRenderStats` values of `ui/pager_render` (docs/PAGER_RENDER.md §1):
/// `lines` visual lines, `glyphs` characters resolved in the atlas (spaces
/// included), `missing_glyphs` characters without a glyph (skipped, never
/// fatal), `ink_pixels` canvas pixels covered by glyph pixels (unique) and
/// `background_pixels` non-black pixels of the screen bitmap (0 without
/// `background`).
struct BriefingRenderStats {
    int pages = 0;
    int lines = 0;
    int glyphs = 0;
    int missing_glyphs = 0;
    int ink_pixels = 0;
    int background_pixels = 0;
};

/// Render page `options.page` (1-based) of `game/SML/brief/<level>.SME` into
/// `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The file is found by `load_briefing()` (same search, decoding and
/// validation as `ui/briefing`), then opened a second time by `SmlPager` for
/// the renderer; both read only the same bytes, so the page counts agree.
/// Returns false + `*error` on every refusal listed in the header comment, and
/// true with `width == 320`, `height == 200` and `rgba.size() == 64000`
/// otherwise.
bool render_briefing(const std::filesystem::path& game, const std::string& level,
                     const BriefingRenderOptions& options,
                     std::vector<std::uint32_t>& rgba, int& width, int& height,
                     BriefingRenderStats* stats = nullptr,
                     std::string* error = nullptr);

} // namespace gm
