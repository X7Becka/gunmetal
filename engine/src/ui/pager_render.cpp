#include "ui/pager_render.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {
namespace {

/// Closed pager / no current page: one text for every overload.
constexpr const char* kNoPageError = "pager_render: no page (the pager is closed)";

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

/// Source palette index of one painted pixel, kept only when the exact fade
/// tables are requested: `0x682f90` is an index -> index table, so the index
/// has to survive until after the frame is composed and only then become a
/// colour.  `palette == nullptr` marks an untouched canvas pixel (the black
/// background of the plain path), which has no index and is left alone.
struct FadeSource {
    const std::vector<std::uint8_t>* palette = nullptr;
    std::uint8_t index = 0;
};

/// Palette colour of one index as `0xRRGGBBAA`.  The BMP reserved byte sits in
/// the alpha slot of every palette in this module, so alpha is forced to 255
/// exactly like `bmp_to_rgba` in `main.cpp`.
std::uint32_t pack_palette(const std::vector<std::uint8_t>& palette,
                           std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// Palette colour of one glyph index.  Index 0 is transparent and never reaches
/// this function.
std::uint32_t palette_colour(const FontAtlas& atlas, std::size_t index) {
    return pack_palette(atlas.palette, index);
}

/// Same for a screen bitmap: 8 bpp index -> `0xRRGGBBAA`, alpha 255.  Unlike a
/// glyph, index 0 of a full screen is opaque black, not transparent
/// (`gm/hud.py::read_panel` keeps the BMP palette too).
std::uint32_t screen_colour(const BmpImage& image, std::size_t index) {
    return pack_palette(image.palette, index);
}

std::string lowered(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

/// One 320x200 screen of `<game>/FRONTEND/BMP.IDX` (`HELPSCRN`, `LOADPAGE`,
/// `BRIEFBG2`, ...), decoded with `res/bmp` like `gm/hud.py::read_panel` does
/// for the HUD panels.
std::optional<BmpImage> load_background(const std::filesystem::path& game,
                                        const std::string& name,
                                        std::string& error) {
    const std::filesystem::path index = game / "FRONTEND" / "BMP.IDX";
    std::string open_error;
    auto archive = BigFile::open(index, open_error);
    if (!archive) {
        error = "background: " + open_error;
        return std::nullopt;
    }
    const std::string wanted = lowered(name);
    const BigEntry* entry = nullptr;
    for (const BigEntry& candidate : archive->entries()) {
        const std::string candidate_name = lowered(candidate.name);
        if (candidate_name == wanted || candidate_name == wanted + ".bmp") {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        error = "background \"" + name + "\" not in " + index.string();
        return std::nullopt;
    }
    std::string read_error;
    auto blob = archive->read(*entry, read_error);
    if (!blob) {
        error = "background \"" + name + "\": " + read_error;
        return std::nullopt;
    }
    std::string bmp_error;
    auto image = parse_bmp(*blob, bmp_error);
    if (!image) {
        error = "background \"" + name + "\": " + bmp_error;
        return std::nullopt;
    }
    if (image->width != kPagerCanvasWidth || image->height != kPagerCanvasHeight) {
        error = "background \"" + name + "\" is " + std::to_string(image->width) + "x" +
                std::to_string(image->height) + ", expected 320x200";
        return std::nullopt;
    }
    if (image->palette.size() < 256u * 4u || image->pixels.size() <
            static_cast<std::size_t>(kPagerCanvasWidth) * kPagerCanvasHeight) {
        error = "background \"" + name + "\": short bitmap or palette";
        return std::nullopt;
    }
    return image;
}

/// Paint the screen bitmap as the canvas: 320x200, `0xRRGGBBAA`.
///
/// `parse_bmp` normalises the BMP rows to top-down, but the engine's loader
/// (`0x47efe0`) copies the file rows straight into the 8-bpp buffer and the
/// screen keeps that order, so the picture the player sees is the vertical
/// mirror of the parsed image (`gm/hud.py::engine_order` for `PANEL`).  The
/// shipped titles confirm it: `flipV(HELPSCRN)` reads `HELP`,
/// `flipV(LOADPAGE)` reads `LOADING`, `flipV(BRIEFBG2)` reads
/// `MISSION OBJECTIVES`; the unflipped rows read them upside down.
void paint_background(const BmpImage& image, std::vector<std::uint32_t>& rgba,
                      PageRenderStats& stats, std::vector<FadeSource>* source) {
    for (int y = 0; y < kPagerCanvasHeight; ++y) {
        const int source_row = kPagerCanvasHeight - 1 - y;
        for (int x = 0; x < kPagerCanvasWidth; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(source_row) * kPagerCanvasWidth +
                static_cast<std::size_t>(x);
            const std::uint8_t palette_index = image.pixels[index];
            const std::uint32_t colour = screen_colour(image, palette_index);
            const std::size_t at = static_cast<std::size_t>(y) * kPagerCanvasWidth +
                                   static_cast<std::size_t>(x);
            rgba[at] = colour;
            if (source != nullptr) {
                (*source)[at] = FadeSource{&image.palette, palette_index};
            }
            if ((colour >> 8u) != 0u) {
                ++stats.background_pixels;
            }
        }
    }
}

/// One channel of the fade table, level 0..15: `value * min(level, 11) / 11`,
/// rounded.  Level 11 is exactly `value` and 12..15 are clamped to it (the
/// tables there are really brighter, docs/HUD_LUT.md §2 — that is what the
/// exact path below fixes), level 0 is black — the curve
/// `0x483290(dst, k/11, 1)` documents.  Only the curve is proven; the engine's
/// requantisation of the scaled colour back to a palette index was never
/// dumped, so this is our model (docs §2.5).
std::uint8_t fade_channel(std::uint32_t value, int level) {
    const int table = std::clamp(level, 0, kPagerFadeLevels - 1);
    const int factor = std::min(table, kPagerFadeMax);
    return static_cast<std::uint8_t>(
        (value * static_cast<std::uint32_t>(factor) + kPagerFadeMax / 2) /
        static_cast<std::uint32_t>(kPagerFadeMax));
}

/// Apply `fade_channel` to the whole frame (the original blits the composed
/// 320x200 buffer through one LUT, so background and text fade together).
void apply_fade(std::vector<std::uint32_t>& rgba, int level) {
    for (std::uint32_t& colour : rgba) {
        const std::uint32_t r = fade_channel((colour >> 24u) & 0xFFu, level);
        const std::uint32_t g = fade_channel((colour >> 16u) & 0xFFu, level);
        const std::uint32_t b = fade_channel((colour >> 8u) & 0xFFu, level);
        colour = (r << 24u) | (g << 16u) | (b << 8u) | 0xFFu;
    }
}

/// The exact fade of `0x682f90`: index -> index through the dumped table, then
/// the palette of the pixel's own source.  A mapped index outside that palette
/// is refused with text (the dump writes 0..255, so it only happens with a
/// short palette, never with the shipped atlases or screens).
bool apply_fade_lut(std::vector<std::uint32_t>& rgba,
                    const std::vector<FadeSource>& source, const HudLut& lut,
                    int level, std::string& error) {
    for (std::size_t at = 0; at < rgba.size(); ++at) {
        const FadeSource& pixel = source[at];
        if (pixel.palette == nullptr) {
            // No palette index: the untouched black canvas stays black.
            continue;
        }
        const std::uint8_t mapped = map(lut, level, pixel.index);
        if (static_cast<std::size_t>(mapped) * 4u >= pixel.palette->size()) {
            error = "pager_render: fade LUT level " + std::to_string(level) +
                    " maps index " + std::to_string(pixel.index) + " to " +
                    std::to_string(mapped) + ", outside the " +
                    std::to_string(pixel.palette->size() / 4u) + "-entry palette";
            return false;
        }
        rgba[at] = pack_palette(*pixel.palette, mapped);
    }
    return true;
}

/// BT.601 luma of an `0xRRGGBBAA` pixel.
int luma_of(std::uint32_t colour) {
    const int r = static_cast<int>((colour >> 24u) & 0xFFu);
    const int g = static_cast<int>((colour >> 16u) & 0xFFu);
    const int b = static_cast<int>((colour >> 8u) & 0xFFu);
    return (77 * r + 150 * g + 29 * b) >> 8;
}

/// Compose and draw one frame.  `background` is null for the plain black
/// canvas (slice 3), otherwise a decoded 320x200 screen.
bool render_frame(const SmlPager& pager, const FontLookup& fonts,
                  std::vector<std::uint32_t>& rgba, int& width, int& height,
                  const PageRenderOptions& options, const BmpImage* background,
                  PageRenderStats& stats, std::string& error) {
    width = kPagerCanvasWidth;
    height = kPagerCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kPagerBackground);
    stats = PageRenderStats{};

    // The exact tables are resolved before anything is painted, so a missing
    // or broken dump is an error and not a silently unfaded frame.  The index
    // map is kept only for this path (64000 sources, cheap next to the RGBA
    // canvas) and only for pixels that really came from a palette.
    HudLut owned_lut;
    const HudLut* lut = nullptr;
    std::vector<FadeSource> source;
    if (options.fade_lut) {
        if (options.fade_lut_table != nullptr) {
            lut = options.fade_lut_table;
        } else if (!options.fade_lut_dump.empty()) {
            std::string lut_error;
            if (!load_hud_lut(options.fade_lut_dump, owned_lut, &lut_error)) {
                error = "pager_render: " + lut_error;
                return false;
            }
            lut = &owned_lut;
        } else {
            error = "pager_render: fade LUT needs a table (fade_lut_table) or a "
                    "dump (fade_lut_dump)";
            return false;
        }
        source.assign(rgba.size(), FadeSource{});
    }

    if (background != nullptr) {
        paint_background(*background, rgba, stats,
                         options.fade_lut ? &source : nullptr);
    }

    const LayoutPage* page = pager.current_page();
    if (page == nullptr) {
        error = kNoPageError;
        return false;
    }
    if (!fonts) {
        error = "pager_render: no font source (pass a FontLookup)";
        return false;
    }

    const PageRange range = range_of(pager, *page);
    stats.lines = static_cast<int>(page->lines.size());
    std::vector<char> ink(static_cast<std::size_t>(width) * height, 0);

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
                    // Glyph row 0 is the TOP row: `0x47faf0` writes it at the
                    // destination row and keeps going down (`gm/hud.py::draw_text`,
                    // `docs/HUD_AUDIT.md` §2.9).  The first version of this
                    // renderer had the rows inverted, which mirrored every glyph
                    // vertically while leaving the ink count unchanged.
                    const int y = kPagerPageY + run.y + row;
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
                        const std::size_t at =
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x);
                        rgba[at] = palette_colour(*atlas, index);
                        if (options.fade_lut) {
                            source[at] = FadeSource{&atlas->palette, index};
                        }
                        ink[at] = 1;
                    }
                }
            }
            pen += atlas->advance_of(ch);
        }
    }

    // Text statistics over the final glyph pixels (unique: overlapping glyphs
    // do not inflate the count, and the luma is the one that is visible).
    stats.contrast_min = 0;
    stats.contrast_max = 0;
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        if (!ink[i]) {
            continue;
        }
        const int luma = luma_of(rgba[i]);
        if (stats.ink_pixels == 0) {
            stats.contrast_min = luma;
            stats.contrast_max = luma;
        } else {
            stats.contrast_min = std::min(stats.contrast_min, luma);
            stats.contrast_max = std::max(stats.contrast_max, luma);
        }
        ++stats.ink_pixels;
    }

    if (options.fade_lut) {
        // Exact path first: the table is index -> index, so it runs on the
        // composed frame before each source palette is applied (the engine
        // blits the whole 8-bpp buffer through `LUT[k]`).
        if (!apply_fade_lut(rgba, source, *lut, options.fade_lut_level, error)) {
            return false;
        }
    } else if (options.fade) {
        apply_fade(rgba, options.fade_level);
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

/// The canvas is reset even on a failure so a caller never sees a stale
/// buffer (slice 3 contract).
void reset_canvas(std::vector<std::uint32_t>& rgba, int& width, int& height) {
    width = kPagerCanvasWidth;
    height = kPagerCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kPagerBackground);
}

} // namespace

bool render_page(const SmlPager& pager, const FontLookup& fonts,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 const PageRenderOptions& options, PageRenderStats* stats,
                 std::string* error) {
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    width = kPagerCanvasWidth;
    height = kPagerCanvasHeight;
    PageRenderStats counts;
    // The closed-pager error wins over the missing-directory one and over the
    // background check (slice-3 contract); handled here, not by delegating to
    // the plain overload, which would call this one again.
    if (pager.current_page() == nullptr) {
        reset_canvas(rgba, width, height);
        err = kNoPageError;
        if (stats != nullptr) {
            *stats = PageRenderStats{};
        }
        return false;
    }
    if (options.background) {
        reset_canvas(rgba, width, height);
        err = "pager_render: background \"" + options.background_asset +
              "\" needs a game directory";
        if (stats != nullptr) {
            *stats = counts;
        }
        return false;
    }
    if (!render_frame(pager, fonts, rgba, width, height, options, nullptr, counts,
                      err)) {
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

bool render_page(const SmlPager& pager, const FontLookup& fonts,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 PageRenderStats* stats, std::string* error) {
    return render_page(pager, fonts, rgba, width, height, PageRenderOptions{}, stats,
                       error);
}

bool render_page(const SmlPager& pager, const std::filesystem::path& game,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 const PageRenderOptions& options, PageRenderStats* stats,
                 std::string* error) {
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    // A closed pager fails before any file is read (and resets the buffer):
    // delegate, so every overload reports the same text.
    if (pager.current_page() == nullptr) {
        return render_page(pager, FontLookup{}, rgba, width, height,
                           PageRenderOptions{}, stats, &err);
    }
    if (game.empty()) {
        reset_canvas(rgba, width, height);
        err = "pager_render: no game directory for the font atlases";
        if (stats != nullptr) {
            *stats = PageRenderStats{};
        }
        return false;
    }

    // The screen bitmap is read before the atlases: a wrong background name is
    // reported as such, not as a font problem.
    std::optional<BmpImage> background;
    if (options.background) {
        std::string background_error;
        background = load_background(game, options.background_asset, background_error);
        if (!background) {
            reset_canvas(rgba, width, height);
            err = "pager_render: " + background_error;
            if (stats != nullptr) {
                *stats = PageRenderStats{};
            }
            return false;
        }
    }

    std::map<int, FontAtlas> owned;
    std::string load_error;
    for (const int number : page_fonts(pager)) {
        auto atlas = load_font(game, number, load_error);
        if (!atlas) {
            reset_canvas(rgba, width, height);
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

    PageRenderStats counts;
    if (!render_frame(pager, lookup, rgba, width, height, options,
                      background ? &*background : nullptr, counts, err)) {
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
    return render_page(pager, game, rgba, width, height, PageRenderOptions{}, stats,
                       error);
}

} // namespace gm
