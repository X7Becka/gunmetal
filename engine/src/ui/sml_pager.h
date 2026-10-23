#pragma once

// Runtime cursor of the SML pager: state 2 (`0x487b69`) over an already
// paginated document.
//
// Division of labour
// ------------------
// `render/text_layout` owns pagination: it cuts the document into 267x167 pages
// exactly like `0x497880` / `0x498600` and stores the byte span of every page in
// `LayoutPage::start` / `LayoutPage::end`.  This module owns only the *cursor*:
// which page is current, which visual line of the document the current line
// cursor points at, and the status string.  It does not lay out, does not draw,
// does not read `FRONTEND/BMP.IDX` beyond the font atlases `load_font` needs,
// and never writes anything.
//
// Proven behaviour it mirrors (docs/UI.md §10, docs/TEXT_LAYOUT.md §3.5)
// ------------------------------------------------------------------------
// * `0x498600(page)` lays out one page and reports `bit0 = page > 0`,
//   `bit1 = end != 0` (there is a next page); C++ equivalent: `show_page()` and
//   `page_count()`.
// * `0x4986b0` = next page, `0x498750` = previous page: whole pages, no line
//   cursor.  `next_page()` / `prev_page()` / `first_page()` / `last_page()`.
// * The page number lives in `0x4bc43c`, the pager flag in `0x4bc3a8`
//   (docs/UI.md §10); `status()` exposes the page number as `"page 2/7"`.
//
// `<p>` is *not* a page break: `0x497da0` adds half a line of paragraph spacing
// and then a line feed; a page ends only when the next line would leave the
// 167 px box, or at `</d>`.  The pager therefore never re-interprets markup.
//
// Lines
// -----
// The shipped pager flips pages only; a line cursor is a UI-layer convenience
// (the same idea as `Screen::scroll`, docs/SCREEN_CPP.md) and is marked as such:
// `line()` is the absolute index of a visual line over the whole document,
// `scroll_next_line()` / `scroll_prev_line()` move it by one and follow the
// page.  This is *not* a claim about `0x4986b0`.
//
// Invariants
// ----------
// * Before a successful `open()` (and after `reset()`) there are 0 pages:
//   `page()` is 0, `page_count()` is 0, every transition returns false and
//   changes nothing, `status()` is `"0/0"`.
// * `next_page()` on the last page and `prev_page()` on the first page return
//   false and leave the whole cursor (page and line) untouched.
// * `first_page()` / `last_page()` / `show_page()` return true only when the
//   page actually changed; an out-of-range page is rejected without a change.
// * A failed `open()` leaves the pager exactly like `reset()` (closed, 0 pages)
//   and sets `error()`; it never leaves a half-laid-out document behind.
// * A document without text (`<d></d>`) is valid and has one page with no
//   lines: `status()` is `"page 1/1"`, line transitions return false.
//
// See docs/SML_PAGER.md for the test numbers and the limitations (no 320x200
// rendering, no atlases, no `UPDOWN` sound, no `<I>` bitmaps, no menu
// `0x485dd0`).

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "render/text_layout.h"
#include "res/sml.h"

namespace gm {

/// Pager extents of the briefing screen, `0x4972f0(0x10b, 0xa7)`
/// (docs/UI.md §5.5): 267x167 pixels.
inline constexpr int kSmlPagerWidth = 267;
inline constexpr int kSmlPagerHeight = 167;

/// Layout knobs of the pager.  The defaults are the shipped briefing screen;
/// everything is forwarded to `LayoutOptions`, nothing is re-derived here.
struct SmlPagerOptions {
    int width = kSmlPagerWidth;
    int height = kSmlPagerHeight;
    int line_height = kDefaultLineHeight;          ///< fonts != 3 (`0x4983c3`)
    int big_line_height = kDefaultBigLineHeight;   ///< font 3 (`0x4983c3`)
    int max_pages = 256;                           ///< text_layout safety limit
    int image_width = 64;                          ///< `<I>` float slot
    int image_height = 64;
    int default_font = kDefaultFont;               ///< before the first `<f>` tag
    bool wrap = true;                              ///< greedy word wrap
};

/// A cursor over one paginated SML document.
///
/// Fonts come either from a game install (a `FRONTEND/BMP.IDX` read on the
/// first `open()`, cached for later documents) or from a caller-supplied
/// `FontLookup` (tests, synthetic atlases, no installation).  The atlases of a
/// `FontLookup` are owned by the caller and must outlive the pager.
class SmlPager {
public:
    SmlPager() = default;

    /// Read the atlases from `game`/`FRONTEND/BMP.IDX` lazily on `open()`.
    explicit SmlPager(std::filesystem::path game, SmlPagerOptions options = {});

    /// Use a caller-owned atlas provider instead of the installation.
    SmlPager(FontLookup fonts, SmlPagerOptions options = {});

    /// Paginate `document` and put the cursor on its first page.
    ///
    /// Fails (false + `error`) when there is no font source, the layout reports
    /// an error (empty text, bad page size, a font the engine never initialises,
    /// the page limit), or no page is produced.  A failed call leaves 0 pages.
    bool open(const SmlDocument& document, std::string* error = nullptr);

    /// Read, decode and paginate one `.sml` / `.sme` file.
    ///
    /// An absolute or existing relative path is used as-is; otherwise it is
    /// resolved case-insensitively under `game`, then under `game`/`SML`
    /// (`HELP.SME` -> `help.sme`).  Decoding is `parse_sml` (it tries the
    /// positional cipher and falls back to plain markup).
    bool open(const std::filesystem::path& path, std::string* error = nullptr);

    /// Drop the document, pages and cursor.  Loaded atlases stay cached, so the
    /// next `open()` does not read `FRONTEND/BMP.IDX` again.
    void reset();

    /// True between a successful `open()` and `reset()`.
    bool opened() const { return opened_; }

    /// The message of the last failed operation; cleared on success.
    const std::string& error() const { return error_; }

    // ------------------------------------------------------------ pages --

    int page_count() const { return static_cast<int>(layout_.pages.size()); }
    int page() const { return page_; }

    /// Advance one page (`0x4986b0`): false on the last page, no change.
    bool next_page();
    /// Step back one page (`0x498750`): false on the first page, no change.
    bool prev_page();
    /// Jump to page 0 (`0x498600(0)`): false when already there / no pages.
    bool first_page();
    /// Jump to the last page: false when already there / no pages.
    bool last_page();
    /// Jump to page `page` (0-based); false when out of range, no change.
    /// Any successful page change puts the line cursor on the page's first line.
    bool show_page(int page);

    // ------------------------------------------------- current page data --

    /// Visual lines of the current page, in `text_layout` coordinates
    /// (page-relative pixels).  Empty when nothing is open.
    const std::vector<LayoutLine>& lines() const;
    /// `<I>` floats of the current page, in `text_layout` coordinates.
    const std::vector<LayoutImage>& images() const;
    /// The whole current page record, or nullptr when nothing is open.
    const LayoutPage* current_page() const;

    int page_width() const { return options_.width; }
    int page_height() const { return options_.height; }

    // ------------------------------------------------------------ lines --

    /// Visual lines over the whole document (sum over pages).
    int line_count() const { return line_count_; }

    /// Absolute 0-based index of the line cursor; 0 when there is no line.
    int line() const { return line_; }

    /// Move the line cursor down one visual line, following the page at a page
    /// boundary; false at the last line, no change.
    bool scroll_next_line();
    /// Move the line cursor up one visual line, following the page; false at the
    /// first line, no change.
    bool scroll_prev_line();

    // ----------------------------------------------------------- payload --

    /// The paginated document (owned copy of what `open()` received).
    const SmlDocument& document() const { return document_; }
    /// The whole layout (`text_layout` output: pages, lines, runs, images).
    const LayoutResult& layout() const { return layout_; }

    /// `"page 2/7"` for an open document, `"0/0"` when there are no pages.
    std::string status() const;

private:
    bool ensure_fonts(std::string& error);
    bool open_parsed(SmlDocument document, std::string* error);
    void build_line_index();
    void close();
    void fail(std::string message, std::string* error);
    FontLookup make_lookup();

    std::filesystem::path game_;
    SmlPagerOptions options_{};
    FontLookup external_fonts_;
    std::map<int, FontAtlas> owned_fonts_;

    SmlDocument document_;
    LayoutResult layout_;
    std::vector<int> page_first_line_;  ///< absolute first line per page, +1 sentinel
    int line_count_ = 0;
    int page_ = 0;
    int line_ = 0;
    bool opened_ = false;
    std::string error_;
};

} // namespace gm
