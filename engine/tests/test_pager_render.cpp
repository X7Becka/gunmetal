// Page rendering of the SML pager (`M8.1` slices 3-4, docs/PAGER_RENDER.md).
//
// The pager already knows the lines of the current page; this module paints
// them with the font atlases of `FRONTEND/BMP.IDX` into the 320x200 RGBA canvas
// of the front end (`0x6d8fc0`).  Slice 4 adds the `HELPSCRN` background
// (decoded and flipped like the engine buffer) and the fade curve of
// `0x682f90`.  The tests pin:
//
//   * the canvas is 320x200 (`0xRRGGBBAA`), the page box is 267x167 and the
//     picture is neither empty (`ink > 0`) nor a solid fill (`ink < w*h`);
//   * the numbers of a render: `lines` equals `SmlPager::lines().size()`,
//     `glyphs` equals an independent walk over the page's `LayoutRun`s, and
//     `missing_glyphs == 0` on `SML/help.sme` and `SML/CREDITS.SML` (the two
//     documents of the slice);
//   * determinism: two renders of the same page are byte for byte identical,
//     while two different pages are not;
//   * the game-directory overload (`load_font` from `FRONTEND/BMP.IDX`) paints
//     exactly the same bytes as the caller-supplied atlases;
//   * boundaries and negatives: a page out of range is refused by the pager, an
//     empty document (`<d></d>`) renders 0 ink, a closed pager, a missing font
//     provider and a provider without the page's font fail with a text;
//   * slice 4: an empty page under `--background` reproduces `HELPSCRN` byte
//     for byte (palette + row flip), the text adds the same ink pixels as on
//     the black canvas, nothing outside the 267x167 page box moves, `LOADPAGE`
//     is selectable by name, and fade 0 is black / 11 and 15 identity / 5 a
//     monotone darkening.
//
// The installation is only read and the runs skip cleanly when it is absent.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "res/sml.h"
#include "ui/pager_render.h"
#include "ui/sml_pager.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(long long got, long long want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

gm::SmlDocument parse_or_empty(const std::string& markup, bool& ok) {
    std::string error;
    auto doc = gm::parse_sml(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(markup.data()),
                                      markup.size()),
        gm::SmlSource::Plain, error);
    ok = doc.has_value();
    if (!ok) {
        std::cerr << "FAIL: cannot parse synthetic document: " << error << "\n";
        ++failures;
        return gm::SmlDocument{};
    }
    return *doc;
}

gm::FontLookup make_lookup(const std::map<int, gm::FontAtlas>& fonts) {
    return [&fonts](int number) -> const gm::FontAtlas* {
        const auto it = fonts.find(number);
        return it == fonts.end() ? nullptr : &it->second;
    };
}

/// Byte range of the current page (the same convention as the renderer:
/// `LayoutPage::end == 0` means the document end).
std::pair<std::size_t, std::size_t> page_range(const gm::SmlPager& pager) {
    const gm::LayoutPage* page = pager.current_page();
    if (page == nullptr) {
        return {0, 0};
    }
    const std::size_t size = pager.document().text.size();
    return {page->start, page->end == 0 ? size : std::min(page->end, size)};
}

/// The glyph count as an independent walk over the page's runs: every byte of
/// `LayoutRun::text` (the normalised body: CR/LF -> space, `<k>` -> "Soldier")
/// that resolves to a glyph, plus every byte that does not (missing).
struct ExpectedGlyphs {
    int glyphs = 0;
    int missing = 0;
};

ExpectedGlyphs expected_glyphs(const gm::SmlPager& pager,
                               const gm::FontLookup& lookup) {
    ExpectedGlyphs want;
    const auto [begin, end] = page_range(pager);
    for (const gm::LayoutRun& run : pager.layout().runs) {
        if (run.first < begin || run.first >= end) {
            continue;
        }
        const gm::FontAtlas* atlas = lookup(run.font);
        if (atlas == nullptr) {
            continue;
        }
        for (const char raw : run.text) {
            const auto ch = static_cast<unsigned char>(raw);
            if (ch == '\r' || ch == '\n') {
                continue;
            }
            if (atlas->glyph(ch) == nullptr) {
                ++want.missing;
            } else {
                ++want.glyphs;
            }
        }
    }
    return want;
}

int count_ink(const std::vector<std::uint32_t>& pixels) {
    int ink = 0;
    for (const std::uint32_t value : pixels) {
        if (value != gm::kPagerBackground) {
            ++ink;
        }
    }
    return ink;
}

std::string lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

/// One 320x200 screen of `FRONTEND/BMP.IDX` decoded independently of
/// `pager_render` (the same `res/bigfile` + `res/bmp` the module uses, but no
/// renderer code): palette colours, alpha 255, and the vertical flip of the
/// engine buffer order (`gm/hud.py::engine_order`).  `non_black` is the number
/// of pixels whose RGB is not zero, i.e. what `background_pixels` must report.
struct DecodedScreen {
    bool ok = false;
    int width = 0;
    int height = 0;
    int non_black = 0;
    std::vector<std::uint32_t> pixels;
    /// Palette indices in engine row order (`indices[y*w + x]`) and the BMP's
    /// own 256 RGBA quads: the exact-fade test needs the index of a pixel, not
    /// only its colour.
    std::vector<std::uint8_t> indices;
    std::vector<std::uint8_t> palette;
};

DecodedScreen decode_screen(const fs::path& game, const std::string& name) {
    DecodedScreen out;
    std::string error;
    auto archive = gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        std::cerr << "FAIL: screen " << name << ": " << error << "\n";
        ++failures;
        return out;
    }
    const std::string wanted = lower(name);
    const gm::BigEntry* entry = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (lower(candidate.name) == wanted) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        std::cerr << "FAIL: screen " << name << " not in BMP.IDX\n";
        ++failures;
        return out;
    }
    auto blob = archive->read(*entry, error);
    if (!blob) {
        std::cerr << "FAIL: screen " << name << ": " << error << "\n";
        ++failures;
        return out;
    }
    auto image = gm::parse_bmp(*blob, error);
    if (!image) {
        std::cerr << "FAIL: screen " << name << ": " << error << "\n";
        ++failures;
        return out;
    }
    if (image->palette.size() < 1024u ||
        image->pixels.size() < static_cast<std::size_t>(image->width) * image->height) {
        std::cerr << "FAIL: screen " << name << ": short bitmap or palette\n";
        ++failures;
        return out;
    }
    out.width = image->width;
    out.height = image->height;
    out.palette = image->palette;
    out.pixels.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    out.indices.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    for (int y = 0; y < out.height; ++y) {
        // The engine buffer keeps the file row order: the player sees the
        // vertical mirror of the top-down parsed image.
        const int source = out.height - 1 - y;
        for (int x = 0; x < out.width; ++x) {
            const std::uint8_t index =
                image->pixels[static_cast<std::size_t>(source) * out.width + x];
            out.indices[static_cast<std::size_t>(y) * out.width + x] = index;
            const std::uint32_t colour =
                (static_cast<std::uint32_t>(image->palette[index * 4u + 0]) << 24) |
                (static_cast<std::uint32_t>(image->palette[index * 4u + 1]) << 16) |
                (static_cast<std::uint32_t>(image->palette[index * 4u + 2]) << 8) |
                0xFFu;
            out.pixels[static_cast<std::size_t>(y) * out.width + x] = colour;
            if ((colour >> 8u) != 0u) {
                ++out.non_black;
            }
        }
    }
    out.ok = true;
    return out;
}

// ------------------------------------------- reference for the exact fade LUT

/// The dumped tables, like `test_hud_lut`: `GM_HUD_LUT`, else
/// `~/.cache/gunmetal-re/hud_lut.bin` (docs/HUD_AUDIT.md §2.8).
fs::path find_hud_lut_dump() {
    if (const char* env = std::getenv("GM_HUD_LUT"); env != nullptr && *env != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".cache" / "gunmetal-re" / "hud_lut.bin";
    }
    return {};
}

/// One canvas pixel of the independent reference: the palette index it was
/// painted with and which source palette that index belongs to.  `palette ==
/// -1` is the untouched black canvas (no palette index at all).
struct ReferencePixel {
    std::uint8_t index = 0;
    int palette = -1;
};

/// Rebuild the palette-index canvas of the same page independently of
/// `pager_render`: the `HELPSCRN` indices in the engine row order plus the
/// glyph indices of the page's runs, each remembering its own palette (screen
/// BMP first, then one entry per font atlas).  This is the content the exact
/// fade has to map; a renderer that faded RGB instead would not reproduce it.
std::vector<ReferencePixel> reference_indices(
    const gm::SmlPager& pager, const gm::FontLookup& lookup,
    const DecodedScreen& screen,
    std::vector<const std::vector<std::uint8_t>*>& palettes) {
    const std::size_t canvas =
        static_cast<std::size_t>(gm::kPagerCanvasWidth) * gm::kPagerCanvasHeight;
    std::vector<ReferencePixel> reference(canvas);
    palettes.clear();
    palettes.push_back(&screen.palette);
    if (!screen.ok ||
        screen.indices.size() !=
            static_cast<std::size_t>(screen.width) * screen.height) {
        return reference;
    }
    for (int y = 0; y < gm::kPagerCanvasHeight; ++y) {
        for (int x = 0; x < gm::kPagerCanvasWidth; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * gm::kPagerCanvasWidth +
                                   static_cast<std::size_t>(x);
            ReferencePixel& pixel = reference[at];
            pixel.index = screen.indices[at];
            pixel.palette = 0;
        }
    }

    const auto [begin, end] = page_range(pager);
    for (const gm::LayoutRun& run : pager.layout().runs) {
        if (run.first < begin || run.first >= end) {
            continue;
        }
        const gm::FontAtlas* atlas = lookup(run.font);
        if (atlas == nullptr || !atlas->has_glyphs() || atlas->palette.empty()) {
            continue;
        }
        int slot = -1;
        for (std::size_t i = 0; i < palettes.size(); ++i) {
            if (palettes[i] == &atlas->palette) {
                slot = static_cast<int>(i);
            }
        }
        if (slot < 0) {
            palettes.push_back(&atlas->palette);
            slot = static_cast<int>(palettes.size()) - 1;
        }
        const std::size_t palette_entries = atlas->palette.size() / 4u;
        int pen = gm::kPagerPageX + run.x;
        for (const char raw : run.text) {
            const auto ch = static_cast<unsigned char>(raw);
            if (ch == '\r' || ch == '\n') {
                continue;
            }
            const gm::FontGlyph* glyph = atlas->glyph(ch);
            if (glyph == nullptr) {
                continue;
            }
            if (glyph->width > 0 && !glyph->pixels.empty()) {
                for (int row = 0; row < atlas->cell_height; ++row) {
                    const int y = gm::kPagerPageY + run.y + row;
                    if (y < 0 || y >= gm::kPagerCanvasHeight) {
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
                        if (x < 0 || x >= gm::kPagerCanvasWidth) {
                            continue;
                        }
                        const std::size_t at =
                            static_cast<std::size_t>(y) * gm::kPagerCanvasWidth +
                            static_cast<std::size_t>(x);
                        reference[at] = ReferencePixel{
                            index, slot};
                    }
                }
            }
            pen += atlas->advance_of(ch);
        }
    }
    return reference;
}

/// `0xRRGGBBAA` of one palette entry, alpha forced opaque (the BMP reserved
/// byte sits there).  Out-of-range indices return black and are counted by the
/// caller, not silently wrapped.
std::uint32_t palette_pixel(const std::vector<std::uint8_t>& palette,
                            std::size_t index) {
    if ((index + 1u) * 4u > palette.size()) {
        return 0xFF000000u;
    }
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24u) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16u) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8u) | 0xFFu;
}

/// The frame the exact path must produce at `level`: every reference pixel is
/// its own index through `map`, then through its own source palette.
std::vector<std::uint32_t> reference_frame(
    const std::vector<ReferencePixel>& reference,
    const std::vector<const std::vector<std::uint8_t>*>& palettes,
    const gm::HudLut& lut, int level) {
    std::vector<std::uint32_t> out(reference.size(), gm::kPagerBackground);
    for (std::size_t at = 0; at < reference.size(); ++at) {
        const ReferencePixel& pixel = reference[at];
        if (pixel.palette < 0) {
            continue;
        }
        const std::uint8_t mapped = gm::map(lut, level, pixel.index);
        out[at] = palette_pixel(*palettes[static_cast<std::size_t>(pixel.palette)],
                                mapped);
    }
    return out;
}

/// The same frame under the engine's *single* palette assumption: the original
/// blits one 8-bpp buffer through `LUT[k]` with `FRONTEND/GAMEPAL`, so every
/// pixel - screen and glyph - is read from that one palette.  The renderer
/// keeps the source palettes instead; the difference is measured, not assumed.
std::vector<std::uint32_t> reference_frame_single(
    const std::vector<ReferencePixel>& reference,
    const std::vector<std::uint8_t>& palette, const gm::HudLut& lut, int level) {
    std::vector<std::uint32_t> out(reference.size(), gm::kPagerBackground);
    for (std::size_t at = 0; at < reference.size(); ++at) {
        if (reference[at].palette < 0) {
            continue;
        }
        out[at] = palette_pixel(palette, gm::map(lut, level, reference[at].index));
    }
    return out;
}

int count_black(const std::vector<std::uint32_t>& pixels) {
    int black = 0;
    for (const std::uint32_t value : pixels) {
        if ((value >> 8u) == 0u) {
            ++black;
        }
    }
    return black;
}

int count_diff(const std::vector<std::uint32_t>& a,
               const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) {
        return -1;
    }
    int diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++diff;
        }
    }
    return diff;
}

void test_closed_pager(const gm::FontLookup& lookup) {
    gm::SmlPager pager;
    std::vector<std::uint32_t> pixels;
    int width = 0;
    int height = 0;
    gm::PageRenderStats stats;
    std::string error;
    const std::string tag = "closed pager: ";

    check(!gm::render_page(pager, lookup, pixels, width, height, &stats, &error),
          tag + "render false");
    check(error.find("no page") != std::string::npos, tag + "error names the page");
    check_eq(width, gm::kPagerCanvasWidth, tag + "canvas width");
    check_eq(height, gm::kPagerCanvasHeight, tag + "canvas height");
    check_eq(static_cast<long long>(pixels.size()),
             static_cast<long long>(gm::kPagerCanvasWidth) * gm::kPagerCanvasHeight,
             tag + "buffer is 320x200");
    check_eq(count_ink(pixels), 0, tag + "buffer is background only");
    check_eq(stats.ink_pixels, 0, tag + "no ink");
    check_eq(stats.lines, 0, tag + "no lines");
}

void test_open_errors(const std::map<int, gm::FontAtlas>& fonts) {
    const gm::FontLookup lookup = make_lookup(fonts);

    // A valid document, but the caller forgot the atlas provider.
    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>AB</d>", ok);
        if (ok) {
            gm::SmlPager pager(lookup);
            std::string error;
            check(pager.open(doc, &error), "no provider: open");
            std::vector<std::uint32_t> pixels;
            int width = 0;
            int height = 0;
            gm::PageRenderStats stats;
            check(!gm::render_page(pager, gm::FontLookup{}, pixels, width, height, &stats,
                                   &error),
                  "no provider: render false");
            check(error.find("no font source") != std::string::npos,
                  "no provider: error text");
            check_eq(count_ink(pixels), 0, "no provider: no ink");
        }
    }

    // The provider knows nothing about the page's font: a named failure, not a
    // silently blank page.
    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>AB</d>", ok);
        if (ok) {
            gm::SmlPager pager(lookup);
            std::string error;
            check(pager.open(doc, &error), "missing atlas: open");
            const gm::FontLookup empty = [](int) -> const gm::FontAtlas* {
                return nullptr;
            };
            std::vector<std::uint32_t> pixels;
            int width = 0;
            int height = 0;
            gm::PageRenderStats stats;
            check(!gm::render_page(pager, empty, pixels, width, height, &stats, &error),
                  "missing atlas: render false");
            check(error.find("no atlas for font 1") != std::string::npos,
                  "missing atlas: error names font 1");
            check_eq(width, gm::kPagerCanvasWidth, "missing atlas: canvas width");
        }
    }

    // `<d></d>`: one page without lines -> a valid, completely empty picture.
    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d></d>", ok);
        if (ok) {
            gm::SmlPager pager(lookup);
            std::string error;
            check(pager.open(doc, &error), "empty body: open");
            std::vector<std::uint32_t> pixels;
            int width = 0;
            int height = 0;
            gm::PageRenderStats stats;
            check(gm::render_page(pager, lookup, pixels, width, height, &stats, &error),
                  "empty body: render true");
            check(error.empty(), "empty body: no error");
            check_eq(width, gm::kPagerCanvasWidth, "empty body: canvas width");
            check_eq(height, gm::kPagerCanvasHeight, "empty body: canvas height");
            check_eq(stats.lines, 0, "empty body: 0 lines");
            check_eq(stats.glyphs, 0, "empty body: 0 glyphs");
            check_eq(stats.missing_glyphs, 0, "empty body: 0 missing");
            check_eq(stats.ink_pixels, 0, "empty body: 0 ink");
            check_eq(count_ink(pixels), 0, "empty body: buffer is background only");
            std::cout << "  empty document <d></d>: lines=0 glyphs=0 missing=0 ink=0\n";
        }
    }
}

// One corpus document: every page rendered and cross-checked.  `expected_ink`
// pins the numbers of slice 3 page by page (docs/PAGER_RENDER.md §4), so the
// plain path cannot drift when the background/fade layers are added.
void test_corpus_file(const fs::path& game, const gm::FontLookup& lookup,
                      const std::string& name,
                      const std::vector<int>& expected_ink) {
    const std::string tag = name + ": ";
    std::string error;
    const auto bytes = read_file(game / "SML" / name);
    auto doc = gm::parse_sml(bytes, error);
    check(doc.has_value(), tag + "parses");
    if (!doc) {
        std::cerr << "FAIL: " << tag << error << "\n";
        return;
    }

    gm::SmlPager pager(lookup);
    check(pager.open(*doc, &error), tag + "open");
    if (!pager.opened()) {
        std::cerr << "FAIL: " << tag << error << "\n";
        return;
    }

    // Every document of the corpus is larger than one page except the very
    // short ones; both files of this test have several pages.
    check(pager.page_count() > 1, tag + "more than one page");

    std::vector<std::uint32_t> first_page;
    int glyphs_total = 0;
    int missing_total = 0;
    int ink_total = 0;
    for (int page = 0; page < pager.page_count(); ++page) {
        pager.show_page(page);
        const std::string page_tag = tag + "page " + std::to_string(page + 1) + "/" +
                                     std::to_string(pager.page_count()) + ": ";

        std::vector<std::uint32_t> pixels;
        int width = 0;
        int height = 0;
        gm::PageRenderStats stats;
        check(gm::render_page(pager, lookup, pixels, width, height, &stats, &error),
              page_tag + "render");
        check(error.empty(), page_tag + "no error");
        check_eq(width, gm::kPagerCanvasWidth, page_tag + "canvas width");
        check_eq(height, gm::kPagerCanvasHeight, page_tag + "canvas height");
        check_eq(static_cast<long long>(pixels.size()),
                 static_cast<long long>(width) * height, page_tag + "buffer size");

        // Numbers against the layout and against an independent walk.
        check_eq(stats.lines, static_cast<long long>(pager.lines().size()),
                 page_tag + "lines");
        const ExpectedGlyphs want = expected_glyphs(pager, lookup);
        check_eq(stats.glyphs, want.glyphs, page_tag + "glyphs");
        check_eq(stats.missing_glyphs, want.missing, page_tag + "missing glyphs");
        check(stats.glyphs > 0, page_tag + "some glyphs");
        check(pager.lines().empty() == (stats.glyphs == 0),
              page_tag + "glyphs follow the lines");

        // A page of text is neither empty nor a solid fill.
        check(stats.ink_pixels > 0, page_tag + "ink > 0");
        check(stats.ink_pixels < width * height, page_tag + "ink < w*h");
        check_eq(count_ink(pixels), stats.ink_pixels, page_tag + "ink recount");
        check_eq(stats.background_pixels, 0, page_tag + "plain canvas has no background");
        check(stats.contrast_max > stats.contrast_min,
              page_tag + "text has a luma range");
        if (page < static_cast<int>(expected_ink.size())) {
            check_eq(stats.ink_pixels, expected_ink[static_cast<std::size_t>(page)],
                     page_tag + "pinned ink of slice 3");
        }

        // Determinism: the same page twice, byte for byte.
        std::vector<std::uint32_t> again;
        gm::PageRenderStats again_stats;
        check(gm::render_page(pager, lookup, again, width, height, &again_stats, &error),
              page_tag + "second render");
        check(pixels == again, page_tag + "two renders are byte-identical");
        check_eq(again_stats.ink_pixels, stats.ink_pixels, page_tag + "second ink");
        check_eq(again_stats.glyphs, stats.glyphs, page_tag + "second glyphs");
        check_eq(again_stats.missing_glyphs, stats.missing_glyphs,
                 page_tag + "second missing");

        // Page 1 and page 2 of the same document must not be the same picture.
        if (page == 0) {
            first_page = pixels;
        } else {
            check(pixels != first_page, page_tag + "differs from page 1");
        }

        // The game-directory overload loads the atlases itself and must paint
        // exactly the same bytes.
        std::vector<std::uint32_t> loaded;
        gm::PageRenderStats loaded_stats;
        check(gm::render_page(pager, game, loaded, width, height, &loaded_stats, &error),
              page_tag + "game overload");
        check(loaded == pixels, page_tag + "game overload paints the same bytes");
        check_eq(loaded_stats.lines, stats.lines, page_tag + "game overload lines");
        check_eq(loaded_stats.glyphs, stats.glyphs, page_tag + "game overload glyphs");
        check_eq(loaded_stats.missing_glyphs, stats.missing_glyphs,
                 page_tag + "game overload missing");
        check_eq(loaded_stats.ink_pixels, stats.ink_pixels, page_tag + "game overload ink");

        std::cout << "  " << name << " page " << (page + 1) << "/" << pager.page_count()
                  << ": lines=" << stats.lines << " glyphs=" << stats.glyphs
                  << " missing=" << stats.missing_glyphs << " ink=" << stats.ink_pixels
                  << " contrast=" << stats.contrast_min << ".." << stats.contrast_max
                  << "\n";

        glyphs_total += stats.glyphs;
        missing_total += stats.missing_glyphs;
        ink_total += stats.ink_pixels;
    }

    // Index 0 of the glyph atlas is transparent: a page with text cannot be a
    // full-canvas fill, and the totals are consistent with the pages.
    check(glyphs_total > 0, tag + "total glyphs");
    check(ink_total > 0, tag + "total ink");
    std::cout << "  " << name << " total: glyphs=" << glyphs_total
              << " missing=" << missing_total << " ink=" << ink_total << "\n";
}

// Slice 4: the `HELPSCRN` background of the original and the fade curve.
void test_background_and_fade(const fs::path& game, const gm::FontLookup& lookup) {
    const int page_width = 267;  // 0x4972f0(0x10b, 0xa7), docs/UI.md §5.5
    const int page_height = 167;
    std::string error;
    const auto bytes = read_file(game / "SML" / "help.sme");
    auto doc = gm::parse_sml(bytes, error);
    check(doc.has_value(), "background: help.sme parses");
    if (!doc) {
        return;
    }
    gm::SmlPager pager(lookup);
    check(pager.open(*doc, &error), "background: help.sme opens");

    const DecodedScreen help_screen = decode_screen(game, "HELPSCRN");
    check(help_screen.ok, "background: HELPSCRN decodes");
    if (!help_screen.ok) {
        return;
    }
    check_eq(help_screen.width, gm::kPagerCanvasWidth, "background: HELPSCRN width");
    check_eq(help_screen.height, gm::kPagerCanvasHeight, "background: HELPSCRN height");
    check(help_screen.non_black > 0, "background: HELPSCRN is not black");

    // An empty document under the background must reproduce the screen
    // exactly: this pins the palette, the row flip and "background first".
    {
        bool ok = false;
        const gm::SmlDocument empty = parse_or_empty("<d></d>", ok);
        if (ok) {
            gm::SmlPager blank(lookup);
            check(blank.open(empty, &error), "background: empty document opens");
            gm::PageRenderOptions options;
            options.background = true;
            std::vector<std::uint32_t> pixels;
            int width = 0;
            int height = 0;
            gm::PageRenderStats stats;
            check(gm::render_page(blank, game, pixels, width, height, options, &stats,
                                  &error),
                  "background: empty render");
            check(pixels == help_screen.pixels,
                  "background: empty canvas is exactly HELPSCRN");
            check_eq(stats.background_pixels, help_screen.non_black,
                     "background: background_pixels");
            check_eq(stats.ink_pixels, 0, "background: no ink on an empty page");
            check_eq(stats.contrast_min, 0, "background: empty contrast min");
            check_eq(stats.contrast_max, 0, "background: empty contrast max");
            std::cout << "  HELPSCRN 320x200: non-black=" << help_screen.non_black
                      << ", empty page reproduces it byte for byte\n";
        }
    }

    // A page of text over it: the same glyph pixels as the black canvas, the
    // background untouched outside the 267x167 page box, deterministic.
    pager.show_page(0);
    std::vector<std::uint32_t> plain;
    int width = 0;
    int height = 0;
    gm::PageRenderStats plain_stats;
    check(gm::render_page(pager, game, plain, width, height, &plain_stats, &error),
          "background: plain render");
    {
        gm::PageRenderOptions options;
        options.background = true;
        std::vector<std::uint32_t> over;
        gm::PageRenderStats stats;
        check(gm::render_page(pager, game, over, width, height, options, &stats, &error),
              "background: render over HELPSCRN");
        check_eq(stats.ink_pixels, plain_stats.ink_pixels,
                 "background: same text pixels as the plain canvas");
        check_eq(stats.glyphs, plain_stats.glyphs, "background: same glyphs");
        check_eq(stats.background_pixels, help_screen.non_black,
                 "background: count survives the text");
        check(over != plain, "background: the picture changed");
        check(stats.contrast_min >= 0 && stats.contrast_max <= 255,
              "background: contrast within a luma range");
        check(stats.contrast_max > stats.contrast_min,
              "background: text has a luma range");

        int outside_changed = 0;
        for (int y = 0; y < gm::kPagerCanvasHeight; ++y) {
            for (int x = 0; x < gm::kPagerCanvasWidth; ++x) {
                if (x < gm::kPagerPageX + page_width && y < gm::kPagerPageY + page_height) {
                    continue;
                }
                const std::size_t at =
                    static_cast<std::size_t>(y) * gm::kPagerCanvasWidth + x;
                if (over[at] != help_screen.pixels[at]) {
                    ++outside_changed;
                }
            }
        }
        check_eq(outside_changed, 0, "background: untouched outside the page box");

        int changed_inside = 0;
        for (int y = 0; y < page_height; ++y) {
            for (int x = 0; x < page_width; ++x) {
                const std::size_t at =
                    static_cast<std::size_t>(y) * gm::kPagerCanvasWidth + x;
                if (over[at] != help_screen.pixels[at]) {
                    ++changed_inside;
                }
            }
        }
        check(changed_inside > 0, "background: text is visible on the page");
        check(changed_inside < page_width * page_height,
              "background: text does not cover the page");

        std::vector<std::uint32_t> again;
        gm::PageRenderStats again_stats;
        check(gm::render_page(pager, game, again, width, height, options, &again_stats,
                              &error),
              "background: second render");
        check(again == over, "background: two renders are byte-identical");
        check_eq(again_stats.ink_pixels, stats.ink_pixels,
                 "background: second ink");

        std::cout << "  help.sme page 1 over HELPSCRN: ink=" << stats.ink_pixels
                  << " background=" << stats.background_pixels
                  << " contrast=" << stats.contrast_min << ".." << stats.contrast_max
                  << " changed-inside=" << changed_inside << "\n";
    }

    // Another screen of the same archive is selectable by name and decoded the
    // same way (`LOADPAGE` is the prelevel background, docs/UI.md §7.1).
    {
        const DecodedScreen loading = decode_screen(game, "LOADPAGE");
        check(loading.ok, "background: LOADPAGE decodes");
        if (loading.ok) {
            bool ok = false;
            const gm::SmlDocument empty = parse_or_empty("<d></d>", ok);
            gm::SmlPager blank(lookup);
            check(blank.open(empty, &error), "background: LOADPAGE pager");
            gm::PageRenderOptions options;
            options.background = true;
            options.background_asset = "LOADPAGE";
            std::vector<std::uint32_t> pixels;
            int w = 0;
            int h = 0;
            gm::PageRenderStats stats;
            check(gm::render_page(blank, game, pixels, w, h, options, &stats, &error),
                  "background: LOADPAGE render");
            check(pixels == loading.pixels, "background: LOADPAGE byte for byte");
            check_eq(stats.background_pixels, loading.non_black,
                     "background: LOADPAGE pixels");
        }
    }

    // Unknown asset and the provider overload: named failures, not a black
    // frame that pretends to be a background.
    {
        gm::PageRenderOptions options;
        options.background = true;
        options.background_asset = "NOSUCHBG";
        std::vector<std::uint32_t> pixels;
        int w = 0;
        int h = 0;
        gm::PageRenderStats stats;
        check(!gm::render_page(pager, game, pixels, w, h, options, &stats, &error),
              "background: unknown asset refused");
        check(error.find("not in") != std::string::npos,
              "background: unknown asset names the archive");
        check_eq(static_cast<long long>(pixels.size()), 320ll * 200ll,
                 "background: failed render resets the buffer");

        options.background_asset = gm::kPagerBackgroundAsset;
        check(!gm::render_page(pager, lookup, pixels, w, h, options, &stats, &error),
              "background: provider overload refused");
        check(error.find("needs a game directory") != std::string::npos,
              "background: provider overload names the reason");
    }

    // Fade: level 0 is a black frame, 11 (identity) and 15 (clamped) are the
    // plain render, 5 darkens monotonically without moving a text pixel.
    {
        const auto render_fade = [&](int level, std::vector<std::uint32_t>& out,
                                     gm::PageRenderStats& stats) {
            gm::PageRenderOptions options;
            options.fade = true;
            options.fade_level = level;
            int w = 0;
            int h = 0;
            return gm::render_page(pager, game, out, w, h, options, &stats, &error);
        };

        std::vector<std::uint32_t> black;
        gm::PageRenderStats black_stats;
        check(render_fade(0, black, black_stats), "fade 0: render");
        int non_black = 0;
        for (const std::uint32_t value : black) {
            if ((value >> 8u) != 0u) {
                ++non_black;
            }
        }
        check_eq(non_black, 0, "fade 0: every pixel is black");
        check_eq(black_stats.ink_pixels, plain_stats.ink_pixels,
                 "fade 0: text pixels still counted");

        std::vector<std::uint32_t> identity;
        gm::PageRenderStats identity_stats;
        check(render_fade(gm::kPagerFadeMax, identity, identity_stats), "fade 11: render");
        check(identity == plain, "fade 11: byte-identical to no fade");
        std::vector<std::uint32_t> clamped;
        gm::PageRenderStats clamped_stats;
        check(render_fade(15, clamped, clamped_stats), "fade 15: render");
        check(clamped == plain, "fade 15: clamped to identity");

        std::vector<std::uint32_t> middle;
        gm::PageRenderStats middle_stats;
        check(render_fade(5, middle, middle_stats), "fade 5: render");
        check(middle != plain, "fade 5: changes the picture");
        check_eq(middle_stats.ink_pixels, plain_stats.ink_pixels,
                 "fade 5: text pixels unchanged");
        bool darker = true;
        bool strictly_darker = false;
        for (std::size_t i = 0; i < middle.size(); ++i) {
            for (int shift : {24, 16, 8}) {
                const std::uint32_t a = (middle[i] >> shift) & 0xFFu;
                const std::uint32_t b = (plain[i] >> shift) & 0xFFu;
                if (a > b) {
                    darker = false;
                }
                if (a < b) {
                    strictly_darker = true;
                }
            }
        }
        check(darker, "fade 5: no channel gets brighter");
        check(strictly_darker, "fade 5: text/background darken");

        std::vector<std::uint32_t> middle_again;
        gm::PageRenderStats middle_again_stats;
        check(render_fade(5, middle_again, middle_again_stats), "fade 5: second render");
        check(middle_again == middle, "fade 5: deterministic");

        const auto max_channel = [](const std::vector<std::uint32_t>& pixels) {
            std::uint32_t top = 0;
            for (const std::uint32_t value : pixels) {
                top = std::max(top, (value >> 24u) & 0xFFu);
                top = std::max(top, (value >> 16u) & 0xFFu);
                top = std::max(top, (value >> 8u) & 0xFFu);
            }
            return top;
        };
        std::cout << "  fade: 0 = black, 5 = darkened (brightest channel "
                  << max_channel(plain) << "->" << max_channel(middle)
                  << "), 11/15 = identity\n";
    }
}

// M8.5 slice 5: the exact fade tables of `0x682f90` in the palettised path.
//
// `render_page` keeps the palette index of every painted pixel, maps it through
// the dumped table **before** the palette lookup and only then writes
// `0xRRGGBBAA`.  The reference above rebuilds the same index canvas
// independently, so "fade after RGB" or "fade a colour back to an index" cannot
// pass.  Pinned here (docs/PAGER_RENDER.md §2.6):
//
//  * level 11 (the identity of `0x483290(dst, 11/11, 1)`) is byte-identical to
//    the unfaded frame: 0 differing pixels out of 64000;
//  * level 0 is *not* the all-black model frame: the table leaves the slots
//    outside the palette ramps alone, so the number of black pixels is smaller
//    and the difference from the model is counted;
//  * levels 12..15 are four different frames (the model clamps them to 11, i.e.
//    to the unfaded picture) - the gain of the exact path;
//  * a missing table/dump and an unreadable dump are refusals with text, and
//    `fade_lut` wins over the model when both flags are set;
//  * the whole frame is also compared with the engine's single-palette
//    (`FRONTEND/GAMEPAL`) reading of the same tables, and the number of
//    pixels where the per-source palettes of the renderer differ is printed.
void test_fade_lut(const fs::path& game, const gm::FontLookup& lookup) {
    const std::string tag = "fade LUT: ";
    constexpr int kCanvas = gm::kPagerCanvasWidth * gm::kPagerCanvasHeight;
    const fs::path dump = find_hud_lut_dump();
    std::error_code ec;
    if (dump.empty() || !fs::is_regular_file(dump, ec) || ec) {
        std::cout << "  fade LUT: SKIP real dump (not found; set GM_HUD_LUT or run "
                     "tools/win/dump_lut.lua) path="
                  << dump.string() << "\n";
        return;
    }

    gm::HudLut lut;
    std::string error;
    if (!gm::load_hud_lut(dump, lut, &error)) {
        check(false, tag + "dump: " + error);
        return;
    }

    const auto bytes = read_file(game / "SML" / "help.sme");
    auto doc = gm::parse_sml(bytes, error);
    if (!doc) {
        check(false, tag + "help.sme parses: " + error);
        return;
    }
    gm::SmlPager pager(lookup);
    if (!pager.open(*doc, &error)) {
        check(false, tag + "help.sme opens: " + error);
        return;
    }
    pager.show_page(0);

    const DecodedScreen screen = decode_screen(game, "HELPSCRN");
    check(screen.ok, tag + "HELPSCRN decodes");
    if (!screen.ok) {
        return;
    }
    std::vector<const std::vector<std::uint8_t>*> palettes;
    const std::vector<ReferencePixel> reference =
        reference_indices(pager, lookup, screen, palettes);
    std::printf("  fade LUT: dump=%s, %zu palettes (1 screen + %zu font atlases)\n",
                dump.string().c_str(), palettes.size(), palettes.size() - 1);

    const auto render = [&](const gm::PageRenderOptions& options,
                            std::vector<std::uint32_t>& out,
                            gm::PageRenderStats& stats) {
        int width = 0;
        int height = 0;
        std::string render_error;
        const bool ok =
            gm::render_page(pager, game, out, width, height, options, &stats,
                            &render_error);
        check(ok, tag + "render: " + render_error);
        if (ok) {
            check_eq(width, gm::kPagerCanvasWidth, tag + "canvas width");
            check_eq(height, gm::kPagerCanvasHeight, tag + "canvas height");
            check_eq(static_cast<long long>(out.size()), kCanvas,
                     tag + "canvas pixels");
        }
        return ok;
    };
    const auto render_lut = [&](int level, std::vector<std::uint32_t>& out,
                                gm::PageRenderStats& stats) {
        gm::PageRenderOptions options;
        options.background = true;
        options.fade_lut = true;
        options.fade_lut_level = level;
        options.fade_lut_table = &lut;
        return render(options, out, stats);
    };

    gm::PageRenderOptions plain_options;
    plain_options.background = true;
    std::vector<std::uint32_t> plain;
    gm::PageRenderStats plain_stats;
    if (!render(plain_options, plain, plain_stats)) {
        return;
    }
    check_eq(plain_stats.background_pixels, screen.non_black,
             tag + "plain frame carries HELPSCRN");

    // 1. Level 11 is the identity: 0 differing pixels.
    std::vector<std::uint32_t> exact11;
    gm::PageRenderStats stats11;
    if (!render_lut(gm::kHudLutIdentityLevel, exact11, stats11)) {
        return;
    }
    const int identity_diffs = count_diff(exact11, plain);
    check_eq(identity_diffs, 0, tag + "level 11 is byte-identical to no fade");
    check_eq(count_diff(exact11, reference_frame(reference, palettes, lut,
                                                 gm::kHudLutIdentityLevel)),
             0, tag + "level 11 equals the index reference");
    check_eq(stats11.ink_pixels, plain_stats.ink_pixels, tag + "level 11 ink unchanged");

    // The dump path (the CLI would use it) must paint the same bytes as a
    // pre-loaded table.
    {
        gm::PageRenderOptions options;
        options.background = true;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_dump = dump.string();
        std::vector<std::uint32_t> loaded;
        gm::PageRenderStats stats;
        check(render(options, loaded, stats), tag + "dump path render");
        check_eq(count_diff(loaded, exact11), 0,
                 tag + "dump path equals a pre-loaded table");
    }

    // 2. Level 0: the table keeps the out-of-ramp slots, the model blackens all.
    std::vector<std::uint32_t> exact0;
    gm::PageRenderStats stats0;
    check(render_lut(0, exact0, stats0), tag + "level 0 render");
    std::vector<std::uint32_t> model0;
    gm::PageRenderStats model0_stats;
    {
        gm::PageRenderOptions options;
        options.background = true;
        options.fade = true;
        options.fade_level = 0;
        check(render(options, model0, model0_stats), tag + "level 0 model render");
    }
    const std::vector<std::uint32_t> expected0 =
        reference_frame(reference, palettes, lut, 0);
    check_eq(count_diff(exact0, expected0), 0,
             tag + "level 0 equals the index reference");
    const int black_lut0 = count_black(exact0);
    const int black_model0 = count_black(model0);
    check_eq(black_lut0, count_black(expected0),
             tag + "level 0 black pixels match the table");
    check_eq(black_model0, kCanvas, tag + "model level 0 blackens every pixel");
    const int diff0_model = count_diff(exact0, model0);
    check(diff0_model > 0, tag + "level 0 differs from the model");
    std::vector<bool> survivors(256, false);
    int survivor_indices = 0;
    for (std::size_t at = 0; at < reference.size(); ++at) {
        if (reference[at].palette < 0 || (expected0[at] >> 8u) == 0u) {
            continue;
        }
        if (!survivors[reference[at].index]) {
            survivors[reference[at].index] = true;
            ++survivor_indices;
        }
    }
    std::printf("  fade LUT level  0: black %d/%d (model %d), %d px differ from the "
                "model, %d source indices left non-black\n",
                black_lut0, kCanvas, black_model0, diff0_model, survivor_indices);

    // 3. Levels 12..15: four different frames, while the model clamps to 11.
    std::vector<std::vector<std::uint32_t>> exact(4);
    std::vector<gm::PageRenderStats> exact_stats(4);
    for (int i = 0; i < 4; ++i) {
        const int level = gm::kHudLutIdentityLevel + 1 + i;
        check(render_lut(level, exact[static_cast<std::size_t>(i)],
                         exact_stats[static_cast<std::size_t>(i)]),
              tag + "level " + std::to_string(level) + " render");
        check_eq(count_diff(exact[static_cast<std::size_t>(i)],
                            reference_frame(reference, palettes, lut, level)),
                 0, tag + "level " + std::to_string(level) +
                        " equals the index reference");
        check(count_diff(exact[static_cast<std::size_t>(i)], plain) > 0,
              tag + "level " + std::to_string(level) + " changes the frame");
        check_eq(exact_stats[static_cast<std::size_t>(i)].ink_pixels,
                 plain_stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const int pair12_13 = count_diff(exact[0], exact[1]);
    const int pair13_14 = count_diff(exact[1], exact[2]);
    const int pair14_15 = count_diff(exact[2], exact[3]);
    const int to11_12 = count_diff(exact[0], exact11);
    const int to11_13 = count_diff(exact[1], exact11);
    const int to11_14 = count_diff(exact[2], exact11);
    const int to11_15 = count_diff(exact[3], exact11);
    check(pair12_13 > 0, tag + "levels 12 and 13 differ");
    check(pair13_14 > 0, tag + "levels 13 and 14 differ");
    check(pair14_15 > 0, tag + "levels 14 and 15 differ");
    check(to11_12 > 0 && to11_13 > 0 && to11_14 > 0 && to11_15 > 0,
          tag + "each of 12..15 differs from the identity");
    std::printf("  fade LUT level 12..15: adjacent diffs 12/13 %d, 13/14 %d, 14/15 %d; "
                "vs level 11: %d/%d/%d/%d px\n",
                pair12_13, pair13_14, pair14_15, to11_12, to11_13, to11_14,
                to11_15);

    std::vector<std::uint32_t> model12;
    gm::PageRenderStats model12_stats;
    {
        gm::PageRenderOptions options;
        options.background = true;
        options.fade = true;
        options.fade_level = 12;
        check(render(options, model12, model12_stats), tag + "level 12 model render");
    }
    check_eq(count_diff(model12, plain), 0,
             tag + "the model clamps level 12 to the identity");
    check(exact[0] != model12,
          tag + "level 12 differs from the clamped model");

    // 4. The provider overload has no game directory: the exact path still works
    // (the table is caller-supplied) and level 11 is the identity there too.
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> plain_black;
        gm::PageRenderStats plain_black_stats;
        check(gm::render_page(pager, lookup, plain_black, width, height,
                              &plain_black_stats, &error),
              tag + "provider overload plain render");
        gm::PageRenderOptions options;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        std::vector<std::uint32_t> exact_black;
        gm::PageRenderStats exact_black_stats;
        check(gm::render_page(pager, lookup, exact_black, width, height, options,
                              &exact_black_stats, &error),
              tag + "provider overload LUT render");
        check_eq(count_diff(exact_black, plain_black), 0,
                 tag + "provider overload: level 11 is identity on the black canvas");
    }

    // 5. Negatives: no table and no dump, an unreadable dump, and the
    // precedence of `fade_lut` over the model.
    {
        gm::PageRenderOptions options;
        options.background = true;
        options.fade_lut = true;
        std::vector<std::uint32_t> pixels;
        int width = 0;
        int height = 0;
        gm::PageRenderStats stats;
        std::string refusal;
        check(!gm::render_page(pager, game, pixels, width, height, options, &stats,
                              &refusal),
              tag + "no table and no dump refused");
        check(refusal.find("fade LUT") != std::string::npos,
              tag + "refusal names the missing table");
        check_eq(static_cast<long long>(pixels.size()), kCanvas,
                 tag + "failed render resets the buffer");
    }
    {
        gm::PageRenderOptions options;
        options.background = true;
        options.fade_lut = true;
        options.fade_lut_dump =
            (fs::temp_directory_path() / "gm_pager_render_no_lut.bin").string();
        std::vector<std::uint32_t> pixels;
        int width = 0;
        int height = 0;
        gm::PageRenderStats stats;
        std::string refusal;
        check(!gm::render_page(pager, game, pixels, width, height, options, &stats,
                              &refusal),
              tag + "missing dump refused");
        check(refusal.find("hud_lut") != std::string::npos,
              tag + "missing dump carries the hud_lut text");
    }
    {
        gm::PageRenderOptions options;
        options.background = true;
        options.fade = true;
        options.fade_level = 0;  // the model would blacken the frame
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        std::vector<std::uint32_t> pixels;
        gm::PageRenderStats stats;
        check(render(options, pixels, stats), tag + "both flags render");
        check_eq(count_diff(pixels, plain), 0,
                 tag + "fade_lut wins over the model");
    }

    // 6. The engine reads the whole 8-bpp buffer through one palette
    // (`FRONTEND/GAMEPAL`); the renderer keeps the source palette of each
    // pixel.  The per-level number of pixels where that choice is visible is
    // measured, not assumed.
    {
        std::string palette_error;
        const auto installed = gm::installed_palette(game, palette_error);
        if (installed && installed->size() == gm::kHudLutPaletteBytes) {
            std::vector<int> single(gm::kHudLutLevels, 0);
            for (int level = 0; level < gm::kHudLutLevels; ++level) {
                single[static_cast<std::size_t>(level)] = count_diff(
                    reference_frame(reference, palettes, lut, level),
                    reference_frame_single(reference, *installed, lut, level));
            }
            check_eq(single[gm::kHudLutIdentityLevel], 0,
                     tag + "level 11 equals the single-palette GAMEPAL reference");
            std::string row;
            for (int level = 0; level < gm::kHudLutLevels; ++level) {
                row += " " + std::to_string(level) + ":" +
                       std::to_string(single[static_cast<std::size_t>(level)]);
            }
            std::printf("  fade LUT single-palette GAMEPAL (per level, px):%s\n",
                        row.c_str());
        } else {
            std::cout << "  fade LUT: SKIP GAMEPAL comparison (" << palette_error
                      << ")\n";
        }
    }

    // 7. A mapped index outside the pixel's palette is a refusal with text
    // (the dump writes 0..255, so a shipped 256-entry palette cannot reach it;
    // a shortened atlas palette can: pick a level and a glyph index the page
    // really paints whose table entry leaves the palette).
    {
        int probe_level = -1;
        std::uint8_t probe_index = 0;
        for (int level = 0; level < gm::kHudLutLevels && probe_level < 0; ++level) {
            for (const ReferencePixel& pixel : reference) {
                if (pixel.palette <= 0) {
                    continue;  // palette 0 is the screen, not a glyph atlas
                }
                if (pixel.index < 64 && gm::map(lut, level, pixel.index) >= 64) {
                    probe_level = level;
                    probe_index = pixel.index;
                    break;
                }
            }
        }
        check(probe_level >= 0, tag + "a painted glyph index leaves a 64-entry palette");
        if (probe_level >= 0) {
            constexpr std::size_t kShortEntries = 64;
            std::map<int, gm::FontAtlas> truncated;
            for (int number = 0; number < 16; ++number) {
                const gm::FontAtlas* atlas = lookup(number);
                if (atlas != nullptr) {
                    truncated[number] = *atlas;
                    truncated[number].palette.resize(kShortEntries * 4u);
                }
            }
            const gm::FontLookup short_lookup =
                [&truncated](int number) -> const gm::FontAtlas* {
                const auto it = truncated.find(number);
                return it == truncated.end() ? nullptr : &it->second;
            };
            gm::PageRenderOptions options;
            options.fade_lut = true;
            options.fade_lut_level = probe_level;
            options.fade_lut_table = &lut;
            std::vector<std::uint32_t> pixels;
            int width = 0;
            int height = 0;
            gm::PageRenderStats stats;
            std::string refusal;
            check(!gm::render_page(pager, short_lookup, pixels, width, height,
                                  options, &stats, &refusal),
                  tag + "out-of-palette mapped index refused");
            check(refusal.find("outside the 64-entry palette") != std::string::npos,
                  tag + "refusal names the palette size");
            check(refusal.find(std::to_string(probe_index)) != std::string::npos &&
                      refusal.find(std::to_string(
                          gm::map(lut, probe_level, probe_index))) !=
                          std::string::npos,
                  tag + "refusal names the index and the mapped index");
            std::printf("  fade LUT out-of-palette: level %d index %d -> %d refused "
                        "with: %s\n",
                        probe_level, static_cast<int>(probe_index),
                        static_cast<int>(gm::map(lut, probe_level, probe_index)),
                        refusal.c_str());
            check(gm::render_page(pager, lookup, pixels, width, height, options,
                                  &stats, &error),
                  tag + "the same level is fine with the full palette");
        }
    }

    std::cout << "  fade LUT: level 11 = no fade (0/" << kCanvas
              << " px), 12..15 differ; exact tables from " << dump.filename().string()
              << "\n";
}

// The page guard of the CLI (`--page 99`): the pager refuses an out-of-range
// page and the renderer keeps painting the page the cursor is on.
void test_page_bounds(const fs::path& game, const gm::FontLookup& lookup) {
    std::string error;
    const auto bytes = read_file(game / "SML" / "CREDITS.SML");
    auto doc = gm::parse_sml(bytes, error);
    if (!doc) {
        std::cerr << "FAIL: page bounds: parse\n";
        ++failures;
        return;
    }
    gm::SmlPager pager(lookup);
    check(pager.open(*doc, &error), "page bounds: open");
    const int pages = pager.page_count();

    // `--page 99` is 1-based in the CLI: page 98 in the 0-based cursor.
    check(!pager.show_page(98), "page bounds: --page 99 refused");
    check_eq(pager.page(), 0, "page bounds: page kept after the refusal");
    check(!pager.show_page(-1), "page bounds: negative page refused");
    check(!pager.show_page(pages), "page bounds: one past the end refused");
    check_eq(pager.page(), 0, "page bounds: still on page 1");

    std::vector<std::uint32_t> pixels;
    int width = 0;
    int height = 0;
    gm::PageRenderStats stats;
    check(gm::render_page(pager, lookup, pixels, width, height, &stats, &error),
          "page bounds: render of the kept page");
    std::cout << "  page bounds: --page 99 refused (" << pages
              << " pages), rendered page 1, ink=" << stats.ink_pixels << "\n";
}

} // namespace


/// A glyph drawn upside down keeps the ink count, so `ink_pixels` cannot catch
/// it: compare the *row profile* of one glyph against its atlas bitmap
/// (row 0 is the top row, `0x47faf0` / `gm/hud.py::draw_text`).
void test_glyph_orientation(const gm::FontLookup& lookup) {
    const std::string tag = "glyph orientation: ";
    bool ok = false;
    const gm::SmlDocument doc = parse_or_empty("<d>L</d>", ok);
    if (!ok) {
        check(false, tag + "parse");
        return;
    }
    gm::SmlPager pager(lookup);
    std::string error;
    if (!pager.open(doc, &error)) {
        check(false, tag + "open: " + error);
        return;
    }
    std::vector<std::uint32_t> pixels;
    int width = 0;
    int height = 0;
    gm::PageRenderStats stats;
    if (!gm::render_page(pager, lookup, pixels, width, height, &stats, &error)) {
        check(false, tag + "render: " + error);
        return;
    }
    if (pager.lines().empty()) {
        check(false, tag + "one line");
        return;
    }
    const gm::LayoutLine& line = pager.lines().front();
    const gm::FontAtlas* atlas = lookup(line.font);
    if (atlas == nullptr) {
        check(false, tag + "atlas for font " + std::to_string(line.font));
        return;
    }
    const gm::FontGlyph* glyph = atlas->glyph('L');
    if (glyph == nullptr || glyph->width <= 0 || glyph->pixels.empty()) {
        check(false, tag + "glyph L");
        return;
    }
    int mismatched_rows = 0;
    int expected_ink = 0;
    for (int row = 0; row < atlas->cell_height; ++row) {
        int want = 0;
        for (int column = 0; column < glyph->width; ++column) {
            if (glyph->pixels[static_cast<std::size_t>(row) * glyph->width + column] != 0) {
                ++want;
            }
        }
        int got = 0;
        for (int column = 0; column < glyph->width; ++column) {
            const int x = line.x + column;
            const int y = line.y + row;
            if (x < 0 || x >= width || y < 0 || y >= height) {
                continue;
            }
            if (pixels[static_cast<std::size_t>(y) * width + x] != gm::kPagerBackground) {
                ++got;
            }
        }
        expected_ink += want;
        if (want != got) {
            ++mismatched_rows;
        }
    }
    check(expected_ink > 0, tag + "the glyph has ink");
    check_eq(mismatched_rows, 0,
             tag + "per-row profile equals the atlas (row 0 = top, not bottom)");
}

int main() {
    const char* env = std::getenv("GUNMETAL_DIR");
    const fs::path game = env != nullptr ? fs::path(env) : fs::path("/mnt/d/games/Gunmetal 3dfx");
    std::error_code ec;
    if (!fs::exists(game / "FRONTEND" / "BMP.IDX", ec) ||
        !fs::exists(game / "SML" / "help.sme", ec) ||
        !fs::exists(game / "SML" / "CREDITS.SML", ec)) {
        std::cout << "pager_render: game not installed, corpus skipped\n";
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {1, 2, 3, 4, 5, 7, 8}) {
        auto atlas = gm::load_font(game, number, error);
        if (!atlas) {
            std::cerr << "FAIL: font " << number << ": " << error << "\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }
    const gm::FontLookup lookup = make_lookup(fonts);

    test_closed_pager(lookup);
    test_open_errors(fonts);
    test_glyph_orientation(lookup);

    // The two documents of the slice: the help screen and the credits.  The
    // ink numbers are the slice-3 baseline (docs/PAGER_RENDER.md §4) and must
    // not move when the background/fade layers are added.
    test_corpus_file(game, lookup, "help.sme", {2534, 3905, 2226});
    test_corpus_file(game, lookup, "CREDITS.SML",
                     {2640, 2558, 2817, 3104, 2719});
    test_background_and_fade(game, lookup);
    test_fade_lut(game, lookup);
    test_page_bounds(game, lookup);

    if (failures != 0) {
        std::cerr << "pager_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "pager_render: 320x200 RGBA of the current page (atlas glyphs, "
                 "HELPSCRN background, fade LUT, deterministic, boundaries); OK\n";
    return 0;
}
