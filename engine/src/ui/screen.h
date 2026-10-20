#pragma once

// One SML/SME screen: markup -> laid-out pages -> RGB.
//
// This is the runtime the front-end pager would call after ``load_sml``:
//
//   * ``res/sml``            - decoding and the flat tag stream (no re-parse here);
//   * ``render/text_layout`` - fonts, metrics, word wrap, ``<I>`` floats, pages;
//   * ``render/sprite``      - the indexed blitter used for ``<I>`` bitmaps;
//   * ``res/bmp``/``res/bigfile`` - loose ``SML/*.bmp`` and the ``FRONTEND/BMP.IDX``
//                              fallback, so no second BMP reader exists.
//
// ``Screen`` owns no window, no SDL and no global state: load a document, lay it
// out, navigate pages/lines, draw into an RGB buffer.  It only reads the game
// installation.
//
// Rendering policy (fixed, see docs/SCREEN_CPP.md)
// ------------------------------------------------
// * The page origin is ``(0,0)`` of *target*; *target* must be at least the
//   page size (``layout(page_width, page_height)``).  Rendering composites on
//   top of the existing pixels - the caller owns the background.
// * Text uses the glyph palette of its font atlas, exactly like
//   ``text_layout::draw_text`` (the proven front-end path).  ``<I>`` bitmaps
//   use their own BMP palette unless the caller passes a complete 1024-byte
//   RGBA palette, which then overrides every image.
// * Images are anchored to the ``LayoutImage`` float slot (the model of
//   docs/TEXT_LAYOUT.md §3.6), *not* to the ``x``/``y`` attributes.  The slot is
//   64x64 by default and every shipped ``<I>`` asset is 64x64, so ``scale`` is
//   1 for the whole corpus.  An optional ``s``/``scale`` attribute selects an
//   integer zoom anchored at the slot's top-left and clipped to the page; the
//   text layout does not reserve the zoomed rectangle (fixed limitation).
// * Selection is a one-pixel rectangle in ``kScreenSelectionColor`` around the
//   chosen line.  The engine picks menu items with font 4 (docs/UI.md §6.2);
//   the SML pager itself has no line cursor, so the frame is a UI-layer policy,
//   not a claim about the briefing pager.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/render.h"
#include "render/text_layout.h"
#include "res/sml.h"

namespace gm {

/// Briefing pager extents, ``0x4972f0(0x10b, 0xa7)`` (docs/UI.md §5.5).
inline constexpr int kScreenPageWidth = 267;
inline constexpr int kScreenPageHeight = 167;

/// ``<I>`` assets in the shipment are 64x64; the layout slot uses the same size.
inline constexpr int kScreenImageSize = 64;

/// RGB of the selection frame (UI-layer policy, see the header comment).
inline constexpr std::uint8_t kScreenSelectionColor[3] = {255, 255, 0};

/// ``sprite::blit`` needs 256 RGBA quads; a non-empty palette must be complete.
inline constexpr std::size_t kScreenPaletteBytes = 256u * 4u;

/// A decoded ``<I>`` bitmap.  ``pixels`` is top-down palette indices (the
/// ``Sprite`` order used by ``render/sprite``); ``palette`` is RGBA.
struct ScreenImageAsset {
    std::string name;    ///< ``f=`` as written in the markup
    std::string source;  ///< resolved path or ``FRONTEND/BMP.IDX:<entry>``
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> palette;
};

/// One ``<I>`` placement on a page.
struct ScreenImageSlot {
    int page = 0;
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int scale = 1;
};

/// One visual line of a page, with its text assembled from the runs that share
/// the baseline (``<s>``, ``<k>`` and centred columns can emit several runs).
struct ScreenLine {
    int page = 0;
    int index = 0;  ///< line index inside the page
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int font = kDefaultFont;  ///< font of the first run on the line
    std::size_t first = 0;    ///< byte range in ``SmlDocument::text``
    std::size_t last = 0;
    std::string text;
};

/// Observable state; ``error`` is the message of the last failed operation.
struct ScreenState {
    bool loaded = false;
    bool laid_out = false;
    std::string source;  ///< path or the name passed to the bytes overload
    std::size_t tags = 0;
    std::size_t elements = 0;
    std::size_t pages = 0;
    std::size_t lines = 0;
    std::size_t images = 0;  ///< ``<I>`` slots over all pages
    int page = 0;            ///< current page for navigation/render() overload
    int rendered_page = -1;
    bool has_selection = false;
    std::size_t selected = 0;  ///< index into ``lines()``
    std::size_t drawn_lines = 0;
    std::size_t drawn_images = 0;
    std::uint64_t rgb_hash = 0;  ///< FNV-1a64 of the last rendered target RGB
    /// True while the documented approximations stay in force (image rectangle
    /// from the float model, selection frame policy).
    bool image_geometry_hypothesis = true;
    bool selection_style_hypothesis = true;
    std::string error;
};

/// Immutable-ish screen runtime: created by one of the ``load`` factories.
class Screen {
public:
    Screen() = default;

    /// Load a screen file.  ``screen`` may be absolute, relative to the current
    /// directory, relative to *game*, or relative to *game*/SML; resolution is
    /// ASCII-case-insensitive after an exact miss (``HELP.SME`` -> ``help.sme``).
    ///
    /// Fails (``nullopt`` + *error*) on a missing/unreadable file or malformed
    /// markup (``parse_sml``: empty input, no ``<d>``/``</d>``, bracket
    /// imbalance, 512+ tags).
    static std::optional<Screen> load(const std::filesystem::path& screen,
                                      const std::filesystem::path& game,
                                      std::string& error);

    /// Load already-read bytes.  Both ``.sml`` and encoded ``.sme`` are handled
    /// by ``parse_sml`` (it tries the cipher and falls back to plain text).
    static std::optional<Screen> load(std::span<const std::uint8_t> bytes,
                                      const std::filesystem::path& game,
                                      std::string& error);

    /// Same, with a logical name used in ``state().source`` and errors.
    static std::optional<Screen> load(std::span<const std::uint8_t> bytes,
                                      std::string_view name,
                                      const std::filesystem::path& game,
                                      std::string& error);

    /// Lay the document out and resolve every ``<I>`` asset.
    ///
    /// Loads the logical fonts used by the document from ``FRONTEND/BMP.IDX``
    /// and the text through ``layout_text``.  Fails on a font the engine never
    /// initialises, a bad page size, an unreadable ``<I>`` bitmap (loose
    /// ``SML/<f>.bmp`` first, then ``FRONTEND/BMP.IDX``), or a page overflow the
    /// layout reports.  Resets page and selection to the first page/first line.
    bool layout(int page_width = kScreenPageWidth, int page_height = kScreenPageHeight);

    std::size_t pages() const { return layout_result_.pages.size(); }
    const std::vector<ScreenLine>& lines() const { return lines_; }
    const std::vector<ScreenImageSlot>& image_slots() const { return slots_; }
    const std::map<std::string, ScreenImageAsset>& image_assets() const { return images_; }
    const SmlDocument& document() const { return document_; }
    const LayoutResult& layout_result() const { return layout_result_; }
    const ScreenState& state() const { return state_; }

    int page_width() const { return page_width_; }
    int page_height() const { return page_height_; }
    int page() const { return state_.page; }
    bool has_selection() const { return state_.has_selection; }

    /// Select a line by its index in ``lines()`` and switch to its page.
    /// ``false`` + *error* when the index is out of range; the state is kept.
    bool select(std::size_t index);

    /// Move the selection by *delta* lines (positive = down/forward), clamping
    /// at both ends and following the page.  With no lines this fails unless
    /// *delta* is 0.
    bool scroll(int delta);

    /// Switch the current page (0-based).  Selection is left untouched.
    bool show_page(int page);

    /// Draw *page* onto *target* at ``(0,0)``.
    ///
    /// * *target* must have a well-sized ``width*height*3`` RGB buffer and be at
    ///   least the laid-out page size, otherwise the call fails before writing.
    /// * *palette* is either empty (each ``<I>`` uses its BMP palette) or a
    ///   256-entry RGBA table that overrides all image palettes.
    /// * Validation failures (layout not run, target too small, wrong RGB size,
    ///   bad palette, page out of range) leave *target* unchanged.
    bool render(RenderResult& target, std::span<const std::uint8_t> palette, int page,
                std::string& error);

    /// Draw the current page (``state().page``).
    bool render(RenderResult& target, std::span<const std::uint8_t> palette,
                std::string& error);

private:
    bool load_bytes(std::span<const std::uint8_t> bytes, std::string_view name,
                    const std::filesystem::path& game, std::string& error);
    bool load_fonts(std::string& error);
    bool load_images(std::string& error);
    bool load_image(const std::string& name, std::string& error);
    void build_lines();
    void clear_navigation();

    std::filesystem::path game_;
    SmlDocument document_;
    LayoutResult layout_result_;
    std::map<int, FontAtlas> fonts_;
    std::map<std::string, ScreenImageAsset> images_;
    std::vector<ScreenImageSlot> slots_;
    std::vector<ScreenLine> lines_;
    std::vector<std::vector<const LayoutRun*>> page_runs_;
    ScreenState state_;

    int page_width_ = 0;
    int page_height_ = 0;
};

} // namespace gm
