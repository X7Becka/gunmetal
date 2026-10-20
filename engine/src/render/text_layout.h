#pragma once

// SML text layout: font atlases, metrics, word wrap, centring and pagination.
//
// This is the C++ port of the front-end text engine in ``gundx.exe``:
//
//   * font table ``0x65ddd0`` (31 records of ``0x1c`` bytes) filled by
//     ``0x47fe30`` from the ``FONTS1..4`` / ``NUMFONTS`` atlases;
//   * ``0x47fa50`` (measure) and ``0x47faf0`` (draw), both indexing the table;
//   * the layout loop ``0x497880`` with the tag handlers ``0x497d7c`` ..
//     ``0x498012`` (``r/p/c/f/x/s/k/m/i``);
//   * pagination ``0x498600`` / ``0x4986b0`` / ``0x498750``.
//
// Everything that is *proven* from the binary is noted at the definition; the
// few approximations are called out in ``docs/TEXT_LAYOUT.md``.  The module
// only reads shipped assets (``FRONTEND/BMP.IDX``) and never writes anything.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/render.h"
#include "res/sml.h"

namespace gm {

// ------------------------------------------------------------------ fonts --

/// The engine accepts font numbers ``0..30`` (``0x47fa50``: ``< 0`` or
/// ``> 0x1e`` returns width 0).
inline constexpr int kFontNumbers = 31;

/// Font used by the layout before any ``<f>`` tag (``0x497a99``).
inline constexpr int kDefaultFont = 1;

/// ``0x4983c3``: line height is 18 for font 3 and 10 for every other number.
inline constexpr int kBigFont = 3;
inline constexpr int kDefaultLineHeight = 10;
inline constexpr int kDefaultBigLineHeight = 18;

/// One populated slot of the engine font table.
///
/// ``number``     logical number (what ``<f s=N>`` and ``FontAtlas::number`` use)
/// ``atlas``      ``FRONTEND/BMP.IDX`` entry name
/// ``cell``       glyph cell size in pixels (``w*h`` bytes inside a glyph block)
/// ``first/count`` byte range covered by the table (``0x65ddd8``/``0x65dddc``)
/// ``columns``    glyphs per atlas row (the divisor in ``0x47fe30``)
/// ``column_pitch``/``row_pitch``  grid pitch in pixels
/// ``origin_x``/``origin_y``      grid origin in engine (bottom-up) pixels
/// ``space_advance``              advance stored for ``' '`` (``0x47ff5b``)
/// ``extra_before``/``extra_after``  per-character tracking added by ``0x47fa50``
/// ``ink_trim``   true: advance = ink width (small/body fonts); false: fixed cell
struct FontSpec {
    int number = 0;
    const char* atlas = "";
    int cell_width = 0;
    int cell_height = 0;
    int first_char = 0;
    int glyph_count = 0;
    int columns = 0;
    int column_pitch = 0;
    int row_pitch = 0;
    int origin_x = 0;
    int origin_y = 0;
    int space_advance = 0;
    int extra_before = 0;
    int extra_after = 0;
    bool ink_trim = true;
};

/// Slot of the engine table used by logical font ``number`` (the layout maps
/// 1 -> 0 and 2 -> 5 before touching the table), or nullptr when the loader
/// never initialises it (font 6 and 13..30 have no atlas in ``gundx.exe``).
const FontSpec* font_spec(int number);

/// Raw slot of the ``0x65ddd0`` table for a number, *without* the 1 -> 0 and
/// 2 -> 5 mapping.  ``load_font`` honours the mapping; this is for tables and
/// documentation.
const FontSpec* font_table_spec(int number);

/// The layout's measure/draw wrappers map two logical fonts to other slots
/// (``0x497b10`` / ``0x497b60``): 1 -> 0, 2 -> 5, everything else unchanged.
/// ``<f s=2>`` therefore renders with the FONTS4 body face, not with FONTS1.
int logical_font_number(int number);

/// ``0x4983c3``: font 3 -> 18 px, anything else -> 10 px.  The values can be
/// overridden through ``LayoutOptions``.
int font_line_height(int number, int small = kDefaultLineHeight,
                     int big = kDefaultBigLineHeight);

/// One extracted glyph.
///
/// ``pixels`` holds ``width * cell_height`` palette indices in engine buffer
/// order: row 0 is the *bottom* row of the glyph (the atlas is stored
/// bottom-up and the blitter keeps that order).  Index 0 is transparent.
struct FontGlyph {
    int advance = 0;
    int width = 0;
    std::vector<std::uint8_t> pixels;
};

/// A loaded font: bitmap metadata plus the per-glyph advances (``0x65dde0``)
/// and bitmaps that ``0x47fe30`` builds at start-up.
struct FontAtlas {
    int number = 0;  ///< logical number (as requested)
    int source = 0;  ///< table slot actually extracted (``logical_font_number``)
    std::string atlas;  ///< ``FRONTEND/BMP.IDX`` entry name

    int width = 0;   ///< atlas bitmap width in pixels
    int height = 0;  ///< atlas bitmap height in pixels
    int cell_width = 0;
    int cell_height = 0;
    int first_char = 0;
    int last_char = 0;
    int columns = 0;

    /// 256-colour palette from the atlas BMP, RGBA quads (may be empty for a
    /// metrics-only atlas).
    std::vector<std::uint8_t> palette;

    /// ``advance[i]`` belongs to ``first_char + i``; already includes the
    /// ``extra_before``/``extra_after`` tracking of ``0x47fa50``.
    std::vector<int> advance;
    std::vector<FontGlyph> glyphs;

    bool has_glyphs() const { return !glyphs.empty(); }

    /// Width of one character; 0 for codes outside ``first_char..last_char``
    /// (the engine skips them in ``0x47fa50``).
    int advance_of(int ch) const;
    const FontGlyph* glyph(int ch) const;

    /// Sum of ``advance_of`` over a latin-1 byte string, exactly like
    /// ``0x47fa50`` (including the ``+extra`` tracking already folded in).
    int measure(std::string_view text) const;
};

/// Load ``number`` from ``<game>/FRONTEND/BMP.IDX``.
///
/// Fails (``nullopt`` + ``error``) for numbers the engine never initialises,
/// a missing archive, a missing/renamed entry, an atlas that is too small for
/// its declared grid, or a non-8-bit BMP.
std::optional<FontAtlas> load_font(const std::filesystem::path& game, int number,
                                   std::string& error);

/// Same, but from raw BMP bytes (tests, synthetic atlases).
std::optional<FontAtlas> load_font_bmp(int number, std::span<const std::uint8_t> bmp,
                                       std::string& error);

// ----------------------------------------------------------------- layout --

struct LayoutOptions {
    int width = 267;   ///< page width in pixels (briefing pager: 267)
    int height = 167;  ///< page height in pixels (briefing pager: 167)
    int line_height = kDefaultLineHeight;      ///< height for fonts != 3
    int big_line_height = kDefaultBigLineHeight; // height for font 3
    int max_pages = 256;  ///< pagination safety limit (engine: 100)
    int image_width = 64;   ///< ``<I>`` corpus size (all shipped images are 64x64)
    int image_height = 64;
    bool wrap = true;  ///< false: measure only, never break long lines
};

/// One drawn text run: byte range in ``SmlDocument::text`` plus the normalised
/// body (CR/LF -> space; ``<k>`` -> "Soldier", ``<s>`` -> " ").
struct LayoutRun {
    int x = 0;
    int y = 0;
    int width = 0;
    int font = kDefaultFont;
    std::size_t first = 0;
    std::size_t last = 0;
    std::string text;
};

/// One laid-out line: origin, measured width, height and byte range.
struct LayoutLine {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = kDefaultLineHeight;
    int font = kDefaultFont;
    std::size_t first = 0;
    std::size_t last = 0;
};

/// An ``<I>`` element.  Images are laid out as floats: a left image indents
/// the column start, a right image moves the right edge in (see docs).
struct LayoutImage {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    char align = 'l';
    std::string name;
    std::size_t pos = 0;
};

struct LayoutPage {
    int index = 0;
    std::size_t start = 0;  ///< first byte of the page in ``SmlDocument::text``
    std::size_t end = 0;    ///< first byte that did not fit (0 = document end)
    std::vector<LayoutLine> lines;
    std::vector<LayoutImage> images;
    int used_height = 0;  ///< last line bottom / image bottom
};

struct LayoutResult {
    int width = 0;
    int height = 0;
    int line_height = kDefaultLineHeight;
    int font = kDefaultFont;
    bool truncated = false;  ///< ``max_pages`` reached
    std::vector<LayoutPage> pages;
    std::vector<LayoutRun> runs;  ///< every run of every page, in order
};

/// Measure one string with the atlas (helper for tests / callers).
int measure_text(const FontAtlas& font, std::string_view text);

/// Layout every page of ``doc`` with a single atlas.
///
/// This convenience overload measures *all* runs with ``font`` and uses the
/// font number only for the line height.  Use the lookup overload when the
/// document switches fonts (``<f s=3>``).
///
/// ``error`` (if non-null) is set for: empty text, non-positive width/height,
/// a font without glyphs, or a page-limit overflow (``truncated`` is also set).
LayoutResult layout_text(const SmlDocument& doc, const FontAtlas& font,
                         const LayoutOptions& options = {},
                         std::string* error = nullptr);

/// Atlas provider: return the atlas for a logical font number or nullptr.
using FontLookup = std::function<const FontAtlas*(int)>;

/// Layout with a provider, so ``<f s=N>`` picks the matching metrics.
/// ``default_font`` is the font used before the first ``<f>`` tag
/// (the engine starts at 1, ``0x497a99``).
LayoutResult layout_text(const SmlDocument& doc, const FontLookup& fonts,
                         int default_font, const LayoutOptions& options = {},
                         std::string* error = nullptr);

/// Metrics-only bounding box of a layout.
struct TextBounds {
    int width = 0;
    int height = 0;
    int lines = 0;
    int pages = 0;
};
TextBounds text_bounds(const LayoutResult& layout);

/// Draw every run into an RGB target at ``(origin_x, origin_y)``.
///
/// Glyphs are blitted with the atlas palette (index 0 = transparent).  Returns
/// false + ``error`` when the target has no/badly sized RGB buffer, a run
/// references a font with no atlas, or the atlas carries no glyph pixels
/// (metrics-only).  Layout positions are page-relative.
bool draw_text(RenderResult& target, const LayoutResult& layout, const FontAtlas& font,
               int origin_x, int origin_y, std::string& error);

/// Same, with a provider so ``<f s=N>`` runs use their own atlas.
bool draw_text(RenderResult& target, const LayoutResult& layout, const FontLookup& fonts,
               int origin_x, int origin_y, std::string& error);

} // namespace gm
