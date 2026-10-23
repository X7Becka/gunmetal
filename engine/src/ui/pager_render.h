#pragma once

// Rendering of one SML pager page into an RGBA canvas (`M8.1` slice 3).
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
// What is *not* drawn (docs/PAGER_RENDER.md §5): the `HELPSCRN`/`LOADPAGE`
// background bitmap, `<I>` illustrations (`SML/<f>.bmp`), the `updown` sound,
// a selection/cursor highlight and the 320x200 interlace of the original.
//
// Errors are text (`std::string* error`) so the CLI can print `[error] pager:
// ...` and return 2: a closed pager, a font whose atlas is missing from the
// provider, a metrics-only atlas and a page whose fonts cannot be loaded all
// fail instead of producing a silently empty picture.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

/// Opaque `#000`: the canvas background before any glyph is drawn.
inline constexpr std::uint32_t kPagerBackground = 0x000000FFu;

/// Numbers a render produced.  `ink_pixels` counts the canvas pixels that are
/// not `kPagerBackground` (the picture is neither empty nor a solid fill).
struct PageRenderStats {
    int lines = 0;           ///< `LayoutLine`s of the current page
    int glyphs = 0;          ///< characters resolved in the atlas (spaces too)
    int missing_glyphs = 0;  ///< characters with no glyph (skipped, never fatal)
    int ink_pixels = 0;      ///< pixels that differ from the background
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

/// Same, loading the page's atlases from `<game>/FRONTEND/BMP.IDX` through
/// `load_font` (the CLI path; the files are only read).  A font the engine
/// never initialises cannot occur here: the layout would have refused the
/// document first.
bool render_page(const SmlPager& pager, const std::filesystem::path& game,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 PageRenderStats* stats = nullptr, std::string* error = nullptr);

} // namespace gm
