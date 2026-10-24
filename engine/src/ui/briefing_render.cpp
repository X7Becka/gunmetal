#include "ui/briefing_render.h"

#include <string>
#include <string_view>

#include "ui/briefing.h"
#include "ui/pager_render.h"
#include "ui/sml_pager.h"

namespace gm {
namespace {

constexpr std::string_view kPrefix = "briefing_render: ";

/// Strip the module prefix of a nested error so the caller sees one
/// `"briefing_render: ..."` chain instead of `"briefing_render: briefing: ..."`.
/// Only the two prefixes of the direct dependencies are removed; an inner
/// `"sml_pager:"` / `"text_layout:"` reason stays visible on purpose.
std::string detail_of(std::string_view message) {
    constexpr std::string_view kBriefing = "briefing: ";
    constexpr std::string_view kRender = "pager_render: ";
    if (message.rfind(kBriefing, 0) == 0) {
        return std::string(message.substr(kBriefing.size()));
    }
    if (message.rfind(kRender, 0) == 0) {
        return std::string(message.substr(kRender.size()));
    }
    return std::string(message);
}

std::string page_error(int page, int pages) {
    return std::string(kPrefix) + "no page " + std::to_string(page) + " (1.." +
           std::to_string(pages) + ")";
}

/// Failure contract: no stale pixels and no stale numbers for the caller.
void clear(std::vector<std::uint32_t>& rgba, int& width, int& height,
           BriefingRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = BriefingRenderStats{};
    }
}

} // namespace

bool render_briefing(const std::filesystem::path& game, const std::string& level,
                     const BriefingRenderOptions& options,
                     std::vector<std::uint32_t>& rgba, int& width, int& height,
                     BriefingRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    // The index owns the file search, the decoding and the "is this a
    // briefing" rules (docs/BRIEFING.md §2).  Its failures are re-prefixed, not
    // duplicated.
    Briefing briefing;
    std::string index_error;
    if (!load_briefing(game, level, briefing, &index_error)) {
        err = std::string(kPrefix) + detail_of(index_error);
        return false;
    }

    const int pages = briefing.page_count();
    if (options.page < 1 || options.page > pages) {
        err = page_error(options.page, pages);
        return false;
    }

    // Open the same file the index read, so page numbers and text come from
    // one document.  `source` is an existing path, which `SmlPager::open`
    // takes as-is.
    SmlPager pager(game);
    std::string pager_error;
    if (!pager.open(briefing.source, &pager_error)) {
        err = std::string(kPrefix) + detail_of(pager_error);
        return false;
    }
    if (pager.page_count() != pages) {
        err = std::string(kPrefix) + "page count changed between loads (" +
              std::to_string(pager.page_count()) + " != " + std::to_string(pages) + ")";
        return false;
    }
    // `show_page(0)` is a no-op (and returns false: "changed only"), and a
    // fresh pager already sits on page 0, so only a real move is required.
    if (options.page - 1 != pager.page() && !pager.show_page(options.page - 1)) {
        // Unreachable after the range check, but the contract stays a text.
        err = page_error(options.page, pages);
        return false;
    }

    // Exactly the pager layers of `--pager`: HELPSCRN, the fade model and the
    // exact fade tables.  The precedence (LUT over the model) and every
    // refusal stay in `render_page`; forwarding the fields is all this layer
    // does, so a briefing frame cannot fade differently from a pager page.
    PageRenderOptions render_options;
    render_options.background = options.background;
    render_options.background_asset = kPagerBackgroundAsset;
    render_options.fade = options.fade;
    render_options.fade_level = options.fade_level;
    render_options.fade_lut = options.fade_lut;
    render_options.fade_lut_level = options.fade_lut_level;
    render_options.fade_lut_dump = options.fade_lut_dump;
    render_options.fade_lut_table = options.fade_lut_table;

    PageRenderStats page_stats;
    std::string render_error;
    if (!render_page(pager, game, rgba, width, height, render_options, &page_stats,
                     &render_error)) {
        clear(rgba, width, height, stats);
        err = std::string(kPrefix) + detail_of(render_error);
        return false;
    }

    BriefingRenderStats counts;
    counts.pages = pages;
    counts.lines = page_stats.lines;
    counts.glyphs = page_stats.glyphs;
    counts.missing_glyphs = page_stats.missing_glyphs;
    counts.ink_pixels = page_stats.ink_pixels;
    counts.background_pixels = page_stats.background_pixels;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
