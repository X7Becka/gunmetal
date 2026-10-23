// Page rendering of the SML pager (`M8.1` slice 3, docs/PAGER_RENDER.md).
//
// The pager already knows the lines of the current page; this module paints
// them with the font atlases of `FRONTEND/BMP.IDX` into the 320x200 RGBA canvas
// of the front end (`0x6d8fc0`).  The tests pin:
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
//     provider and a provider without the page's font fail with a text.
//
// The installation is only read and the runs skip cleanly when it is absent.

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

#include "render/text_layout.h"
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

// One corpus document: every page rendered and cross-checked.
void test_corpus_file(const fs::path& game, const gm::FontLookup& lookup,
                      const std::string& name) {
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

    // The two documents of the slice: the help screen and the credits.
    test_corpus_file(game, lookup, "help.sme");
    test_corpus_file(game, lookup, "CREDITS.SML");
    test_page_bounds(game, lookup);

    if (failures != 0) {
        std::cerr << "pager_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "pager_render: 320x200 RGBA of the current page (atlas glyphs, "
                 "deterministic, boundaries); OK\n";
    return 0;
}
