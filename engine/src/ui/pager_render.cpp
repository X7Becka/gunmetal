#include "ui/pager_render.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <utility>

namespace gm {
namespace {

/// Byte range of the current page in `SmlDocument::text`.  `LayoutPage::end`
/// is 0 for the last page (`text_layout` convention), so it means "document
/// end" here.
struct PageRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

PageRange range_of(const SmlPager& pager, const LayoutPage& page) {
    PageRange range;
    range.begin = page.start;
    const std::size_t size = pager.document().text.size();
    range.end = page.end == 0 ? size : std::min(page.end, size);
    if (range.end < range.begin) {
        range.end = range.begin;
    }
    return range;
}

/// A run belongs to the page whose byte range contains its start.  Pages are
/// laid out one after another, so this is an exact partition: a run never
/// spans two pages and the next page starts at `page.end`.
bool belongs(const LayoutRun& run, const PageRange& range) {
    return run.first >= range.begin && run.first < range.end;
}

/// Palette colour of one glyph index as `0xRRGGBBAA`.  Index 0 is
/// transparent and never reaches this function.  The BMP reserved byte sits in
/// the alpha slot of `FontAtlas::palette`, so alpha is forced to 255 exactly
/// like `bmp_to_rgba` in `main.cpp`.
std::uint32_t palette_colour(const FontAtlas& atlas, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(atlas.palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(atlas.palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(atlas.palette[base + 2]) << 8) | 0xFFu;
}

bool draw_page(const SmlPager& pager, const FontLookup& fonts,
               std::vector<std::uint32_t>& rgba, int width, int height,
               PageRenderStats& stats, std::string& error) {
    const LayoutPage* page = pager.current_page();
    if (page == nullptr) {
        error = "pager_render: no page (the pager is closed)";
        return false;
    }
    if (!fonts) {
        error = "pager_render: no font source (pass a FontLookup)";
        return false;
    }

    const PageRange range = range_of(pager, *page);
    stats.lines = static_cast<int>(page->lines.size());

    for (const LayoutRun& run : pager.layout().runs) {
        if (!belongs(run, range)) {
            continue;
        }
        const FontAtlas* atlas = fonts(run.font);
        if (atlas == nullptr) {
            error = "pager_render: no atlas for font " + std::to_string(run.font);
            return false;
        }
        if (!atlas->has_glyphs()) {
            error = "pager_render: font " + std::to_string(run.font) + " is metrics-only";
            return false;
        }
        const std::size_t palette_entries = atlas->palette.size() / 4u;
        if (palette_entries == 0) {
            error = "pager_render: font " + std::to_string(run.font) + " has no palette";
            return false;
        }

        int pen = kPagerPageX + run.x;
        for (const char raw : run.text) {
            const auto ch = static_cast<unsigned char>(raw);
            // The layout normalises CR/LF to spaces; skip them anyway so a
            // hand-built document cannot paint a control code.
            if (ch == '\r' || ch == '\n') {
                continue;
            }
            const FontGlyph* glyph = atlas->glyph(ch);
            if (glyph == nullptr) {
                ++stats.missing_glyphs;
                continue;
            }
            ++stats.glyphs;
            if (glyph->width > 0 && !glyph->pixels.empty()) {
                for (int row = 0; row < atlas->cell_height; ++row) {
                    // Glyph row 0 is the bottom row (engine buffer order).
                    const int y = kPagerPageY + run.y + atlas->cell_height - 1 - row;
                    if (y < 0 || y >= height) {
                        continue;
                    }
                    for (int column = 0; column < glyph->width; ++column) {
                        const std::uint8_t index =
                            glyph->pixels[static_cast<std::size_t>(row) * glyph->width +
                                          column];
                        if (index == 0 || index >= palette_entries) {
                            continue;
                        }
                        const int x = pen + column;
                        if (x < 0 || x >= width) {
                            continue;
                        }
                        rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                             static_cast<std::size_t>(x)] = palette_colour(*atlas, index);
                    }
                }
            }
            pen += atlas->advance_of(ch);
        }
    }

    for (const std::uint32_t value : rgba) {
        if (value != kPagerBackground) {
            ++stats.ink_pixels;
        }
    }
    return true;
}

/// Fonts the current page actually draws with: one per run plus one per line
/// (a merged line carries the font of its first run, a run may carry another).
std::vector<int> page_fonts(const SmlPager& pager) {
    std::vector<int> wanted;
    const LayoutPage* page = pager.current_page();
    if (page == nullptr) {
        return wanted;
    }
    const PageRange range = range_of(pager, *page);
    const auto add = [&wanted](int number) {
        if (std::find(wanted.begin(), wanted.end(), number) == wanted.end()) {
            wanted.push_back(number);
        }
    };
    for (const LayoutLine& line : page->lines) {
        add(line.font);
    }
    for (const LayoutRun& run : pager.layout().runs) {
        if (belongs(run, range)) {
            add(run.font);
        }
    }
    return wanted;
}

} // namespace

bool render_page(const SmlPager& pager, const FontLookup& fonts,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 PageRenderStats* stats, std::string* error) {
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    PageRenderStats counts;
    width = kPagerCanvasWidth;
    height = kPagerCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kPagerBackground);

    if (!draw_page(pager, fonts, rgba, width, height, counts, err)) {
        if (stats != nullptr) {
            *stats = counts;
        }
        return false;
    }
    if (stats != nullptr) {
        *stats = counts;
    }
    return true;
}

bool render_page(const SmlPager& pager, const std::filesystem::path& game,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 PageRenderStats* stats, std::string* error) {
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    // A closed pager fails before any font is read (and resets the buffer):
    // delegate, so both overloads report the same text.
    if (pager.current_page() == nullptr) {
        return render_page(pager, FontLookup{}, rgba, width, height, stats, &err);
    }
    if (game.empty()) {
        width = kPagerCanvasWidth;
        height = kPagerCanvasHeight;
        rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                    kPagerBackground);
        err = "pager_render: no game directory for the font atlases";
        if (stats != nullptr) {
            *stats = PageRenderStats{};
        }
        return false;
    }

    std::map<int, FontAtlas> owned;
    std::string load_error;
    for (const int number : page_fonts(pager)) {
        auto atlas = load_font(game, number, load_error);
        if (!atlas) {
            width = kPagerCanvasWidth;
            height = kPagerCanvasHeight;
            rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                        kPagerBackground);
            err = "pager_render: font " + std::to_string(number) + ": " + load_error;
            if (stats != nullptr) {
                *stats = PageRenderStats{};
            }
            return false;
        }
        owned.emplace(number, std::move(*atlas));
    }

    const FontLookup lookup = [&owned](int number) -> const FontAtlas* {
        const auto it = owned.find(number);
        return it == owned.end() ? nullptr : &it->second;
    };
    return render_page(pager, lookup, rgba, width, height, stats, &err);
}

} // namespace gm
