// SML text layout: font atlases, metrics, wrapping, centring and pagination.
//
// Goldens come from two independent readers of the shipped data:
//
//   * the font table (atlas name, cell grid, advance/pixel FNV-1a64) matches
//     ``gm/bigfile.py`` + ``gm/bmp.py`` extraction run with the arguments of
//     the ``0x47fe30`` calls (see docs/TEXT_LAYOUT.md);
//   * the layout goldens (pages, per-page line counts, page starts/ends,
//     used height, bounding box, run FNV) were cross-checked against a Python
//     transcription of ``0x497880``/``0x497c20`` on the same files.
//
// The game install is only read.  When it is missing the corpus cases print a
// skip line and the process succeeds, like the other corpus tests.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/text_layout.h"
#include "res/sml.h"

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

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got \"" << got << "\", want \"" << want << "\")\n";
    }
}

std::uint64_t fnv1a64(std::string_view data) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string format_hash(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

struct FontGolden {
    int number;
    int source;
    const char* atlas;
    int width;
    int height;
    int cell_width;
    int cell_height;
    int first_char;
    int last_char;
    int advance_sum;
    const char* advance_hash;
    const char* pixel_hash;
    std::size_t pixel_bytes;
};

// Generated from the C++ loader and independently reproduced by the Python
// reader at the identical pixel level (advance list and per-glyph bytes).
constexpr FontGolden kFontGoldens[] = {
    {0, 0, "FONTS4", 512, 168, 8, 8, 32, 127, 437, "c573b531d6b66c14",
     "54d0bcf61ca22c27", 2896},
    {1, 0, "FONTS4", 512, 168, 8, 8, 32, 127, 437, "c573b531d6b66c14",
     "54d0bcf61ca22c27", 2896},
    {2, 5, "FONTS4", 512, 168, 8, 8, 32, 127, 437, "c573b531d6b66c14",
     "2219a69c4069c5e0", 2896},
    {3, 3, "FONTS2", 1024, 80, 16, 16, 32, 95, 421, "7172793b19f9b78f",
     "0ff1928f77e8e044", 5731},
    {4, 4, "FONTS4", 512, 168, 8, 8, 32, 127, 435, "da255b58205bff9a",
     "e365cf98d2ea34bd", 2880},
    {5, 5, "FONTS4", 512, 168, 8, 8, 32, 127, 437, "c573b531d6b66c14",
     "2219a69c4069c5e0", 2896},
    {7, 7, "FONTS3", 520, 56, 8, 8, 32, 95, 308, "c5db161d6e2d4cf1",
     "01c6c5632f4b526e", 2048},
    {8, 8, "FONTS4", 512, 168, 8, 8, 32, 95, 302, "d83af31edff063f9",
     "330404b98c15197f", 2008},
    {9, 9, "NUMFONTS", 180, 48, 8, 10, 48, 57, 90, "fc1e228b7cbd5893",
     "593e0afdb6809044", 820},
    {10, 10, "NUMFONTS", 180, 48, 8, 10, 48, 57, 90, "fc1e228b7cbd5893",
     "636f2cdc525be6f6", 820},
    {11, 11, "NUMFONTS", 180, 48, 8, 8, 48, 57, 90, "fc1e228b7cbd5893",
     "286d8e8977cfa3cc", 660},
    {12, 12, "NUMFONTS", 180, 48, 8, 8, 48, 57, 90, "fc1e228b7cbd5893",
     "ee6bfb3703b0fcb3", 660},
};

std::string advance_blob(const gm::FontAtlas& atlas) {
    std::string blob;
    for (const int advance : atlas.advance) {
        blob += std::to_string(advance);
        blob.push_back(',');
    }
    return blob;
}

std::string pixel_blob(const gm::FontAtlas& atlas) {
    std::string blob;
    for (const gm::FontGlyph& glyph : atlas.glyphs) {
        blob += std::to_string(glyph.width);
        blob.push_back(':');
        blob.append(reinterpret_cast<const char*>(glyph.pixels.data()), glyph.pixels.size());
    }
    return blob;
}

std::string run_blob(const gm::LayoutResult& layout) {
    std::string blob;
    for (const gm::LayoutRun& run : layout.runs) {
        blob += std::to_string(run.x) + "," + std::to_string(run.y) + "," +
                std::to_string(run.width) + "," + std::to_string(run.font) + ":" + run.text +
                "\n";
    }
    return blob;
}

void test_font_table() {
    check(gm::font_line_height(3) == 18, "line height of font 3 is 18");
    check(gm::font_line_height(2) == 10, "line height of font 2 is 10");
    check(gm::font_line_height(0) == 10, "line height of font 0 is 10");
    check(gm::font_line_height(3, 12, 20) == 20, "big line height override");
    check(gm::font_line_height(1, 12, 20) == 12, "small line height override");

    check(gm::logical_font_number(1) == 0, "logical font 1 renders with slot 0");
    check(gm::logical_font_number(2) == 5, "logical font 2 renders with slot 5");
    check(gm::logical_font_number(3) == 3, "logical font 3 renders with slot 3");
    check(gm::logical_font_number(0) == 0, "logical font 0 unchanged");
    check(gm::logical_font_number(12) == 12, "logical font 12 unchanged");

    check(gm::font_spec(0) != nullptr && gm::font_spec(0)->atlas == std::string("FONTS4"),
          "font 0 -> FONTS4");
    check(gm::font_spec(1) != nullptr && gm::font_spec(1)->atlas == std::string("FONTS4"),
          "logical font 1 -> FONTS4 (via slot 0)");
    check(gm::font_spec(2) != nullptr && gm::font_spec(2)->atlas == std::string("FONTS4"),
          "logical font 2 -> FONTS4 (via slot 5)");
    check(gm::font_spec(3) != nullptr && gm::font_spec(3)->atlas == std::string("FONTS2"),
          "font 3 -> FONTS2");
    check(gm::font_spec(7) != nullptr && gm::font_spec(7)->atlas == std::string("FONTS3"),
          "font 7 -> FONTS3");
    check(gm::font_spec(9) != nullptr && gm::font_spec(9)->atlas == std::string("NUMFONTS"),
          "font 9 -> NUMFONTS");
    check(gm::font_spec(6) == nullptr, "font 6 has no slot");
    check(gm::font_spec(13) == nullptr, "font 13 has no slot");
    check(gm::font_spec(30) == nullptr, "font 30 has no slot");
    check(gm::font_table_spec(2) != nullptr &&
              gm::font_table_spec(2)->atlas == std::string("FONTS1"),
          "raw table slot 2 is FONTS1");
    check(gm::font_table_spec(1) != nullptr &&
              gm::font_table_spec(1)->atlas == std::string("FONTS1"),
          "raw table slot 1 is FONTS1");
    check(gm::font_table_spec(6) == nullptr, "raw table slot 6 absent");
}

bool load_font_set(const fs::path& game, std::map<int, gm::FontAtlas>& fonts,
                   std::string& error) {
    for (const FontGolden& golden : kFontGoldens) {
        auto atlas = gm::load_font(game, golden.number, error);
        if (!atlas) {
            return false;
        }
        fonts[golden.number] = std::move(*atlas);
    }
    return true;
}

void test_fonts(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    for (const FontGolden& golden : kFontGoldens) {
        const auto it = fonts.find(golden.number);
        check(it != fonts.end(), "font " + std::to_string(golden.number) + " loaded");
        if (it == fonts.end()) {
            continue;
        }
        const gm::FontAtlas& atlas = it->second;
        const std::string tag = "font " + std::to_string(golden.number) + ": ";
        check_eq(atlas.number, golden.number, tag + "number");
        check_eq(atlas.source, golden.source, tag + "source slot");
        check_eq(atlas.atlas, golden.atlas, tag + "atlas name");
        check_eq(atlas.width, golden.width, tag + "bitmap width");
        check_eq(atlas.height, golden.height, tag + "bitmap height");
        check_eq(atlas.cell_width, golden.cell_width, tag + "cell width");
        check_eq(atlas.cell_height, golden.cell_height, tag + "cell height");
        check_eq(atlas.first_char, golden.first_char, tag + "first char");
        check_eq(atlas.last_char, golden.last_char, tag + "last char");
        check(atlas.has_glyphs(), tag + "glyphs present");
        check_eq(static_cast<long long>(atlas.glyphs.size()),
                 golden.last_char - golden.first_char + 1, tag + "glyph count");
        int sum = 0;
        for (const int advance : atlas.advance) {
            sum += advance;
        }
        check_eq(sum, golden.advance_sum, tag + "advance sum");
        check_eq(format_hash(fnv1a64(advance_blob(atlas))), golden.advance_hash,
                 tag + "advance hash");
        const std::string pixels = pixel_blob(atlas);
        check_eq(format_hash(fnv1a64(pixels)), golden.pixel_hash, tag + "pixel hash");
        check_eq(static_cast<long long>(pixels.size()), static_cast<long long>(golden.pixel_bytes),
                 tag + "pixel bytes");
    }

    // Spot checks that read like the corpus.
    const gm::FontAtlas& body = fonts.at(1);
    check_eq(body.advance_of('A'), 6, "font 1 advance of A");
    check_eq(body.advance_of(' '), 4, "font 1 advance of space");
    check_eq(body.advance_of('2'), 6, "font 1 advance of 2");
    check_eq(body.advance_of(0x7f), 1, "font 1 last glyph advance (no ink -> +1)");
    check_eq(body.advance_of(0x80), 0, "font 1 skips bytes outside the table");
    check_eq(body.measure("GUNMETAL HELP"), 76, "font 1 width of GUNMETAL HELP");
    check_eq(fonts.at(2).measure("GUNMETAL HELP"), 76,
             "font 2 has the same metrics as font 1");
    check_eq(fonts.at(3).measure("MINE LAYER"), 89, "font 3 width of MINE LAYER");
    check_eq(fonts.at(3).advance_of('A'), 9, "font 3 advance of A");
    check_eq(fonts.at(3).advance_of(' '), 8, "font 3 advance of space");
    check_eq(fonts.at(9).advance_of('0'), 9, "NUMFONTS digit advance");
    check_eq(fonts.at(9).advance_of('A'), 0, "NUMFONTS has no letters");
}

void test_font_errors(const fs::path& game) {
    std::string error;

    auto missing = gm::load_font(game, 6, error);
    check(!missing.has_value(), "font 6 rejected");
    check(error.find("no atlas") != std::string::npos, "font 6 error mentions the atlas");

    error.clear();
    auto out_of_range = gm::load_font(game, 31, error);
    check(!out_of_range.has_value(), "font 31 rejected");
    check(error.find("outside 0..30") != std::string::npos, "font 31 range error");

    error.clear();
    auto negative = gm::load_font(game, -1, error);
    check(!negative.has_value(), "font -1 rejected");

    error.clear();
    auto no_game = gm::load_font(game / "definitely-not-a-game", 1, error);
    check(!no_game.has_value(), "missing FRONTEND/BMP.IDX rejected");
    check(error.find("BMP.IDX") != std::string::npos, "missing archive error names BMP.IDX");

    const std::string garbage = "not a bmp at all";
    error.clear();
    auto bad = gm::load_font_bmp(1,
                                 std::span<const std::uint8_t>(
                                     reinterpret_cast<const std::uint8_t*>(garbage.data()),
                                     garbage.size()),
                                 error);
    check(!bad.has_value(), "garbage BMP rejected");
    check(error.find("not a BMP") != std::string::npos, "garbage BMP error");
}

gm::SmlDocument parse_or_empty(const std::string& markup, bool& ok) {
    std::string error;
    auto doc = gm::parse_sml(std::span<const std::uint8_t>(
                                 reinterpret_cast<const std::uint8_t*>(markup.data()),
                                 markup.size()),
                             gm::SmlSource::Plain, error);
    ok = doc.has_value();
    if (!ok) {
        std::cerr << "FAIL: cannot parse synthetic document: " << error << "\n";
        ++failures;
        gm::SmlDocument fallback;
        return fallback;
    }
    return *doc;
}

gm::FontLookup make_lookup(const std::map<int, gm::FontAtlas>& fonts) {
    return [&fonts](int number) -> const gm::FontAtlas* {
        const auto it = fonts.find(number);
        return it == fonts.end() ? nullptr : &it->second;
    };
}

void test_synthetic(const std::map<int, gm::FontAtlas>& fonts) {
    const gm::FontLookup lookup = make_lookup(fonts);
    gm::LayoutOptions options;
    options.width = 30;
    options.height = 100;
    std::string error;

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>AAA BBB</d>", ok);
        if (ok) {
            const auto layout = gm::layout_text(doc, lookup, 1, options, &error);
            check(error.empty(), "synthetic wrap: no error");
            check_eq(static_cast<long long>(layout.pages.size()), 1, "synthetic wrap: pages");
            check_eq(static_cast<long long>(layout.pages.at(0).lines.size()), 2,
                     "synthetic wrap: lines");
            const auto& lines = layout.pages.at(0).lines;
            check_eq(lines.at(0).x, 0, "synthetic wrap: line 0 x");
            check_eq(lines.at(0).y, 0, "synthetic wrap: line 0 y");
            check_eq(lines.at(0).width, 22, "synthetic wrap: line 0 width");
            check_eq(lines.at(1).y, 10, "synthetic wrap: line 1 y");
            check_eq(lines.at(1).width, 18, "synthetic wrap: line 1 width");
            check_eq(layout.runs.size(), 2u, "synthetic wrap: runs");
            check_eq(layout.runs.at(0).text, std::string("AAA "), "synthetic wrap: first run");
            check_eq(layout.runs.at(1).text, std::string("BBB"), "synthetic wrap: second run");
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d><c>AB</c></d>", ok);
        if (ok) {
            const auto layout = gm::layout_text(doc, lookup, 1, options, &error);
            check(error.empty(), "synthetic centre: no error");
            check_eq(layout.runs.size(), 1u, "synthetic centre: runs");
            if (!layout.runs.empty()) {
                check_eq(layout.runs.at(0).x, 9, "synthetic centre: x = (30-12)/2");
                check_eq(layout.runs.at(0).width, 12, "synthetic centre: width");
                check_eq(layout.runs.at(0).y, 0, "synthetic centre: y");
            }
            const auto bounds = gm::text_bounds(layout);
            check_eq(bounds.width, 21, "synthetic centre: bbox width");
            check_eq(bounds.height, 10, "synthetic centre: bbox height");
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>A<r>B<p>C</d>", ok);
        if (ok) {
            const auto layout = gm::layout_text(doc, lookup, 1, options, &error);
            check(error.empty(), "synthetic r/p: no error");
            check_eq(layout.runs.size(), 3u, "synthetic r/p: runs");
            if (layout.runs.size() == 3) {
                check_eq(layout.runs.at(0).y, 0, "synthetic r/p: A y");
                check_eq(layout.runs.at(1).y, 10, "synthetic r/p: B y after <r>");
                check_eq(layout.runs.at(2).y, 25, "synthetic r/p: C y after <p> (+5 then +10)");
            }
            check_eq(gm::text_bounds(layout).height, 35, "synthetic r/p: bbox height");
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc =
            parse_or_empty("<d><f s=\"3\">AB<r>CD</f>E</d>", ok);
        if (ok) {
            gm::LayoutOptions wide;
            wide.width = 267;
            wide.height = 167;
            const auto layout = gm::layout_text(doc, lookup, 1, wide, &error);
            check(error.empty(), "synthetic font switch: no error");
            check_eq(layout.runs.size(), 3u, "synthetic font switch: runs");
            if (layout.runs.size() == 3) {
                check_eq(layout.runs.at(0).font, 3, "synthetic font switch: first run font");
                check_eq(layout.runs.at(0).width, 18, "synthetic font switch: AB = 9+9");
                check_eq(layout.runs.at(1).y, 18, "synthetic font switch: <r> uses 18 px");
                check_eq(layout.runs.at(2).font, 1, "synthetic font switch: </f> restores 1");
                check_eq(layout.runs.at(2).x, 18, "synthetic font switch: E follows CD");
            }
            check_eq(layout.pages.at(0).lines.size(), 2u,
                     "synthetic font switch: two visual lines");
            check_eq(layout.pages.at(0).lines.at(0).height, 18,
                     "synthetic font switch: first line height 18");
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>A<s>B<k>C</d>", ok);
        if (ok) {
            gm::LayoutOptions wide = options;
            wide.width = 100;
            const auto layout = gm::layout_text(doc, lookup, 1, wide, &error);
            check(error.empty(), "synthetic placeholders: no error");
            check_eq(layout.runs.size(), 5u, "synthetic placeholders: runs");
            const int soldier = fonts.at(1).measure("Soldier");
            if (layout.runs.size() == 5) {
                check_eq(layout.runs.at(1).text, std::string(" "), "<s> inserts a space");
                check_eq(layout.runs.at(1).width, 4, "<s> space width");
                check_eq(layout.runs.at(2).text, std::string("B"), "text after <s>");
                check_eq(layout.runs.at(3).text, std::string("Soldier"), "<k> inserts Soldier");
                check_eq(layout.runs.at(3).width, soldier, "<k> width");
                check_eq(layout.runs.at(4).x, 6 + 4 + 6 + soldier, "<k> advances the pen");
            }
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>ABCDEF</d>", ok);
        if (ok) {
            gm::LayoutOptions narrow;
            narrow.width = 10;
            narrow.height = 100;
            const auto layout = gm::layout_text(doc, lookup, 1, narrow, &error);
            check(error.empty(), "forced break: no error");
            check_eq(static_cast<long long>(layout.pages.at(0).lines.size()), 6,
                     "forced break: one glyph per line");
            const std::string letters = "ABCDEF";
            for (std::size_t i = 0; i < layout.pages.at(0).lines.size(); ++i) {
                check_eq(layout.pages.at(0).lines.at(i).width,
                         fonts.at(1).advance_of(letters.at(i)), "forced break: glyph width");
            }
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>AB CD</d>", ok);
        if (ok) {
            gm::LayoutOptions nowrap;
            nowrap.width = 10;
            nowrap.height = 100;
            nowrap.wrap = false;
            const auto layout = gm::layout_text(doc, lookup, 1, nowrap, &error);
            check(error.empty(), "wrap=false: no error");
            check_eq(layout.runs.size(), 1u, "wrap=false: one run");
            check_eq(layout.runs.at(0).width, 28, "wrap=false: overflow kept on one line");
        }
    }
}

void test_layout_errors(const std::map<int, gm::FontAtlas>& fonts) {
    const gm::FontLookup lookup = make_lookup(fonts);
    std::string error;

    {
        gm::SmlDocument empty;
        const auto layout = gm::layout_text(empty, fonts.at(1), {}, &error);
        check(!error.empty(), "empty document: error");
        check(layout.pages.empty(), "empty document: no pages");
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>AB</d>", ok);
        if (ok) {
            gm::LayoutOptions bad;
            bad.width = -5;
            const auto layout = gm::layout_text(doc, fonts.at(1), bad, &error);
            check(error.find("non-positive") != std::string::npos, "negative width: error");
            check(layout.pages.empty(), "negative width: no pages");

            bad = gm::LayoutOptions{};
            bad.height = 0;
            error.clear();
            const auto zero = gm::layout_text(doc, fonts.at(1), bad, &error);
            check(error.find("non-positive") != std::string::npos, "zero height: error");
            check(zero.pages.empty(), "zero height: no pages");

            error.clear();
            gm::LayoutOptions wrap_only;
            wrap_only.line_height = 0;
            const auto bad_lh = gm::layout_text(doc, lookup, 1, wrap_only, &error);
            check(error.find("line height") != std::string::npos, "zero line height: error");
            check(bad_lh.pages.empty(), "zero line height: no pages");

            error.clear();
            auto no_glyphs = fonts.at(1);
            no_glyphs.glyphs.clear();
            const auto metrics = gm::layout_text(doc, no_glyphs, {}, &error);
            check(error.find("no glyphs") != std::string::npos, "metrics-only font: error");
            check(metrics.pages.empty(), "metrics-only font: no pages");
        }
    }

    {
        // 20000 characters: must terminate and report the page limit.
        std::string markup = "<d>";
        for (int i = 0; i < 10000; ++i) {
            markup += "AB ";
        }
        markup += "</d>";
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty(markup, ok);
        if (ok) {
            gm::LayoutOptions limited;
            limited.max_pages = 8;
            error.clear();
            const auto layout = gm::layout_text(doc, lookup, 1, limited, &error);
            check(layout.truncated, "huge text: truncated flag");
            check(error.find("page limit") != std::string::npos,
                  "huge text: clean page-limit error");
            check(layout.pages.size() <= 8, "huge text: page cap respected");
            check(!layout.runs.empty(), "huge text: still produced runs");
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d></d>", ok);
        if (ok) {
            error.clear();
            const auto layout = gm::layout_text(doc, fonts.at(1), {}, &error);
            check(error.empty(), "empty body: no error");
            check_eq(static_cast<long long>(layout.pages.size()), 1, "empty body: one page");
            check_eq(layout.pages.at(0).lines.size(), 0u, "empty body: no lines");
        }
    }

    {
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d><f s=\"6\">AB</d>", ok);
        if (ok) {
            error.clear();
            const auto layout = gm::layout_text(doc, lookup, 1, {}, &error);
            check(error.find("no atlas for font 6") != std::string::npos,
                  "font switch to an unknown font: clean error");
        }
    }
}

struct LayoutGolden {
    const char* file;
    int pages;
    const char* lines;      // per page, comma separated
    const char* starts;     // per page, comma separated
    const char* ends;
    const char* used;
    int bbox_width;
    int bbox_height;
    int runs;
    const char* hash;
    bool approximate;  // float model for <I> still a hypothesis
};

constexpr LayoutGolden kLayoutGoldens[] = {
    {"help.sme", 3, "15,15,12", "0,340,770", "340,770,0", "150,150,140", 249, 150, 42,
     "841bdf60491f3930", false},
    {"prelevel/level21.sme", 1, "13", "0", "0", "135", 267, 135, 13,
     "bb84eb2a8d32c27e", false},
    {"design/weapon20.sme", 1, "2", "0", "0", "40", 162, 40, 2,
     "6b7d2c6f2de68941", false},
    {"prelevel/level24.sme", 1, "13", "0", "0", "145", 267, 145, 13,
     "ba055dc487dd2cf6", false},
    {"design/weapon17.sme", 1, "5", "0", "0", "70", 261, 70, 5,
     "3aec1bffd6195cf9", false},
    // <I> floats: the metrics/positions are pinned, but the image rectangle
    // itself is the documented approximation of 0x498012.
    {"brief/level10.sme", 3, "16,16,3", "0,825,1642", "825,1642,0", "165,160,30", 267, 165, 41,
     "e573f2c65e343f65", true},
};

std::vector<int> parse_ints(const std::string& text) {
    std::vector<int> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string piece =
            text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!piece.empty()) {
            out.push_back(std::atoi(piece.c_str()));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

void test_golden(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const gm::FontLookup lookup = make_lookup(fonts);
    gm::LayoutOptions options;
    const fs::path sml = game / "SML";

    for (const LayoutGolden& golden : kLayoutGoldens) {
        const fs::path path = sml / golden.file;
        const auto bytes = read_file(path);
        std::string error;
        auto doc = gm::parse_sml(bytes, error);
        check(doc.has_value(), std::string(golden.file) + ": parses");
        if (!doc) {
            std::cerr << "FAIL: " << golden.file << ": " << error << "\n";
            continue;
        }
        const auto layout = gm::layout_text(*doc, lookup, 1, options, &error);
        const std::string tag = std::string(golden.file) + ": ";
        check(error.empty(), tag + "no layout error");
        check_eq(static_cast<long long>(layout.pages.size()), golden.pages, tag + "pages");
        if (static_cast<int>(layout.pages.size()) != golden.pages) {
            continue;
        }
        const auto lines = parse_ints(golden.lines);
        const auto starts = parse_ints(golden.starts);
        const auto ends = parse_ints(golden.ends);
        const auto used = parse_ints(golden.used);
        for (std::size_t page = 0; page < layout.pages.size(); ++page) {
            const auto& pg = layout.pages[page];
            const std::string ptag = tag + "page " + std::to_string(page) + " ";
            check_eq(static_cast<long long>(pg.index), static_cast<long long>(page),
                     ptag + "index");
            check_eq(static_cast<long long>(pg.lines.size()), lines.at(page), ptag + "lines");
            check_eq(static_cast<long long>(pg.start), starts.at(page), ptag + "start");
            check_eq(static_cast<long long>(pg.end), ends.at(page), ptag + "end");
            check_eq(pg.used_height, used.at(page), ptag + "used height");
            for (const auto& line : pg.lines) {
                check(line.y + line.height <= options.height,
                      ptag + "line inside the page (" + std::to_string(line.y) + ")");
                check(line.x >= 0 && line.x + line.width <= options.width,
                      ptag + "line inside the page width");
            }
        }
        const auto bounds = gm::text_bounds(layout);
        check_eq(bounds.width, golden.bbox_width, tag + "bbox width");
        check_eq(bounds.height, golden.bbox_height, tag + "bbox height");
        check_eq(static_cast<long long>(layout.runs.size()), golden.runs, tag + "runs");
        check_eq(format_hash(fnv1a64(run_blob(layout))), golden.hash, tag + "run hash");

        // Every run must be inside its page (except the documented <k> corner
        // that we now retry on the next page, so this is exact).
        for (const auto& run : layout.runs) {
            check(run.x >= 0 && run.y >= 0, tag + "run origin non-negative");
            check(run.x + run.width <= options.width, tag + "run inside page width");
        }
    }

    // Font 3 page: the display face makes <p> spacing 9 instead of 5.
    {
        const auto bytes = read_file(sml / "design/weapon20.sme");
        std::string error;
        auto doc = gm::parse_sml(bytes, error);
        if (doc) {
            const auto layout = gm::layout_text(*doc, lookup, 1, options, &error);
            check_eq(layout.runs.size(), 2u, "weapon20: two runs");
            if (layout.runs.size() == 2) {
                check_eq(layout.runs.at(0).font, 3, "weapon20: title uses font 3");
                check_eq(layout.runs.at(0).y, 15, "weapon20: first <p> adds half a line");
                check_eq(layout.runs.at(1).y, 30,
                         "weapon20: </f> restores font 1 before the second <p>");
            }
        }
    }
}

// Every shipped SML document must lay out without errors or page overflows.
// The totals pin the whole corpus under the model documented in
// docs/TEXT_LAYOUT.md (fonts 1/2/3, 267x167).
void test_corpus_sweep(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const gm::FontLookup lookup = make_lookup(fonts);
    gm::LayoutOptions options;
    std::size_t files = 0;
    std::size_t parse_failures = 0;
    std::size_t pages = 0;
    std::size_t lines = 0;
    for (const auto& entry : fs::recursive_directory_iterator(game / "SML")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext != ".sme" && ext != ".sml") {
            continue;
        }
        std::string error;
        auto doc = gm::parse_sml(read_file(entry.path()), error);
        if (!doc) {
            ++parse_failures;
            check(entry.path().filename() == "dm4.sml",
                  "sweep: only dm4.sml may fail to parse (" + entry.path().filename().string() +
                      ": " + error + ")");
            continue;
        }
        const auto layout = gm::layout_text(*doc, lookup, 1, options, &error);
        const std::string tag = "sweep " + entry.path().filename().string() + ": ";
        check(error.empty(), tag + "no layout error");
        check(!layout.truncated, tag + "not truncated");
        check(!layout.pages.empty(), tag + "has a page");
        ++files;
        pages += layout.pages.size();
        for (const auto& page : layout.pages) {
            lines += page.lines.size();
            check(page.used_height <= options.height, tag + "page height");
            for (const auto& line : page.lines) {
                check(line.x >= 0 && line.x + line.width <= options.width, tag + "line width");
                check(line.y >= 0 && line.y + line.height <= options.height, tag + "line height");
            }
            for (const auto& run : layout.runs) {
                check(run.x + run.width <= options.width, tag + "run width");
            }
        }
    }
    check_eq(static_cast<long long>(files), 299, "sweep: 299 parseable files");
    check_eq(static_cast<long long>(parse_failures), 1, "sweep: dm4.sml is the only failure");
    check_eq(static_cast<long long>(pages), 372, "sweep: total pages");
    check_eq(static_cast<long long>(lines), 3365, "sweep: total lines");
}

void test_draw(const std::map<int, gm::FontAtlas>& fonts) {
    bool ok = false;
    const gm::SmlDocument doc = parse_or_empty("<d>AB</d>", ok);
    if (!ok) {
        return;
    }
    gm::LayoutOptions options;
    options.width = 100;
    options.height = 40;
    std::string error;
    const auto layout = gm::layout_text(doc, fonts.at(1), options, &error);
    check(error.empty(), "draw: layout ok");

    gm::RenderResult target;
    target.width = 32;
    target.height = 16;
    target.rgb.assign(32u * 16u * 3u, 0);
    check(gm::draw_text(target, layout, fonts.at(1), 1, 1, error), "draw: succeeds");
    check(error.empty(), "draw: no error");

    int changed = 0;
    for (const std::uint8_t byte : target.rgb) {
        if (byte != 0) {
            ++changed;
        }
    }
    check(changed > 0, "draw: wrote pixels");
    // Nothing outside the two 8x8 cells at (1,1) may change.
    for (int y = 0; y < target.height; ++y) {
        for (int x = 0; x < target.width; ++x) {
            const bool inside = x >= 1 && x < 1 + 12 && y >= 1 && y < 9;
            if (inside) {
                continue;
            }
            const std::size_t at = (static_cast<std::size_t>(y) * target.width + x) * 3u;
            check(target.rgb[at] == 0 && target.rgb[at + 1] == 0 && target.rgb[at + 2] == 0,
                  "draw: pixel outside the runs untouched");
        }
    }

    error.clear();
    auto metrics_only = fonts.at(1);
    metrics_only.glyphs.clear();
    check(!gm::draw_text(target, layout, metrics_only, 1, 1, error),
          "draw: metrics-only atlas rejected");
    check(error.find("metrics-only") != std::string::npos, "draw: metrics-only error");

    error.clear();
    gm::RenderResult short_target;
    short_target.width = 10;
    short_target.height = 10;
    short_target.rgb.assign(3, 0);
    check(!gm::draw_text(short_target, layout, fonts.at(1), 0, 0, error),
          "draw: bad target rejected");
    check(error.find("expected") != std::string::npos, "draw: target size error");

    // A run whose font has no atlas must fail cleanly.
    error.clear();
    const gm::FontLookup empty_lookup = [](int) -> const gm::FontAtlas* { return nullptr; };
    check(!gm::draw_text(target, layout, empty_lookup, 0, 0, error),
          "draw: missing run atlas rejected");
    check(error.find("no atlas") != std::string::npos, "draw: missing atlas error");
}

} // namespace

int main() {
    test_font_table();

    const char* env = std::getenv("GUNMETAL_DIR");
    const fs::path game = env != nullptr ? fs::path(env) : fs::path("/mnt/d/games/Gunmetal 3dfx");
    std::error_code ec;
    if (!fs::exists(game / "FRONTEND" / "BMP.IDX", ec) ||
        !fs::exists(game / "SML" / "help.sme", ec)) {
        std::cout << "text_layout: game not installed, skipped\n";
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    if (!load_font_set(game, fonts, error)) {
        std::cerr << "FAIL: font set: " << error << "\n";
        return 1;
    }

    test_fonts(game, fonts);
    test_font_errors(game);
    test_synthetic(fonts);
    test_layout_errors(fonts);
    test_golden(game, fonts);
    test_corpus_sweep(game, fonts);
    test_draw(fonts);

    if (failures != 0) {
        std::cerr << "text_layout: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "text_layout: 12 font atlases, 6 golden layouts (help/level21/weapon20/"
                 "level24/weapon17/level10), 299 corpus layouts (372 pages), "
                 "wrap/centre/pagination/errors; OK\n";
    return 0;
}
