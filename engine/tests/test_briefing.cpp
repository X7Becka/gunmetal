// Mission briefings: `SML/brief/<level>.SME` -> level name, pages, lines, text
// (`ui/briefing`, docs/BRIEFING.md).
//
// The module is the pager plus an index: `SmlPager` decodes the `.sme` and cuts
// it into 267x167 pages, this layer names the files and flattens every page.
// The test therefore pins three independent things:
//
//   * the corpus shape of `briefing_levels` (25 stems, sorted, unique) and the
//     page/line/glyph numbers of every briefing;
//   * the text against `gm/sml.py` as an oracle: `describe()["body"]` is the
//     decoded document with all tags removed and placeholders expanded, so
//     FNV-1a64 of the whitespace-stripped `plain_text()` must match the value
//     computed by Python (only the numbers are pasted here, the test never runs
//     Python).  Whitespace is dropped because the layout can break inside a
//     word (`level9`: "cra" + "te") and the break is not part of the document;
//   * `glyphs` against `PageRenderStats::glyphs` of `ui/pager_render` on the
//     same page (`missing_glyphs == 0`), i.e. the count is really characters
//     with an atlas glyph, not a guess.
//
// Negatives are always exercised in a temporary directory (no game files
// needed): a missing installation, `SML/brief` absent, a non-`.sme` name, a
// path instead of a level, an empty name, a missing/empty/undecodable file and
// a directory that pretends to be `level97.sme`.  Every refusal must return
// false, clear the output and fill a `"briefing: ..."` text.
//
// The installation is only read.  When it is missing the corpus half prints a
// skip line and the process succeeds, like the other corpus tests.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/hash.h"
#include "ui/briefing.h"
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

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got \"" << got << "\", want \"" << want
                  << "\")\n";
    }
}

void check_hash(std::uint64_t got, std::uint64_t want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got 0x" << std::hex << got << ", want 0x"
                  << want << std::dec << ")\n";
    }
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

/// Every non-whitespace byte of the line texts: the oracle shape of
/// `gm/sml.py:describe()["body"]` with all whitespace removed.  Spaces are dropped
/// because a line/page break can fall inside a word (`level9`: "cra" + "te"),
/// which the layout does not mark; dropping them keeps the character sequence
/// comparable without weakening it (any dropped/added letter still differs).
std::string compact_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::uint64_t hash_text(std::string_view text) {
    return gm::fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

/// Visual line `index` of the whole briefing, in page order.
std::string briefing_line(const gm::Briefing& briefing, int index) {
    int seen = 0;
    for (const gm::BriefingPage& page : briefing.pages) {
        for (const std::string& line : page.text) {
            if (seen == index) {
                return line;
            }
            ++seen;
        }
    }
    return {};
}

// One corpus row: numbers from this implementation, `body` from gm/sml.py.
struct LevelPin {
    const char* level;
    int pages;
    int lines;
    int glyphs;
    std::uint64_t body;  // FNV-1a64 of describe()["body"]
};

const LevelPin kLevels[] = {
    {"level0a", 5, 37, 601, 0x1babe20f4260875eull},
    {"level1", 2, 20, 910, 0x45239da6047fcad3ull},
    {"level11", 2, 30, 1443, 0xbaab87e3e31cf305ull},
    {"level12", 3, 47, 2224, 0xb9754706891c4d3full},
    {"level13", 1, 12, 474, 0x4fdbada1eb159347ull},
    {"level14", 2, 21, 970, 0x4c4c31b0cdb8e3c4ull},
    {"level15", 2, 29, 1423, 0xd46a942a3f039717ull},
    {"level16", 3, 36, 1752, 0x3fb9f68a173210fbull},
    {"level18", 2, 21, 989, 0x9fcd393a5dfab809ull},
    {"level19", 1, 9, 343, 0x00e9765d187124d3ull},
    {"level2", 2, 17, 781, 0x17034ab8a39a5dedull},
    {"level20", 2, 31, 1509, 0x3e3581fb35943c5bull},
    {"level3", 2, 23, 1042, 0xb898b085c2aff17cull},
    {"level4", 3, 37, 1643, 0x86ba1fa7739bf8ebull},
    {"level6", 2, 29, 1379, 0x61f8a40ad0156629ull},
    {"level24", 2, 24, 1169, 0x695c4f8d343b22e4ull},
    {"level10", 3, 35, 1665, 0xa902c546459b6a85ull},
    {"level17", 2, 31, 1471, 0x81ef726a7fe65012ull},
    {"level21", 2, 30, 1487, 0x1a1fac56ae9b2356ull},
    {"level22", 5, 74, 3699, 0x33c00e1abf0a8920ull},
    {"level23", 2, 26, 1180, 0x402b2972229e95a2ull},
    {"level5", 2, 28, 1307, 0xae3da1fc7527429eull},
    {"level7", 2, 21, 999, 0x0941e12abe68d283ull},
    {"level8", 2, 31, 1562, 0x98823206d5029854ull},
    {"level9", 2, 28, 1262, 0xa4673e8cb261368bull},
};

const LevelPin* find_pin(const std::string& level) {
    for (const LevelPin& pin : kLevels) {
        if (level == pin.level) {
            return &pin;
        }
    }
    return nullptr;
}

// Detailed rows: per-page lines/glyphs plus the first two visual lines.
struct PagePin {
    int lines;
    int glyphs;
};

struct DetailPin {
    const char* level;
    const char* first;
    const char* second;
    std::vector<PagePin> pages;
};

const DetailPin kDetails[] = {
    {"level0a", "CONGRATULATIONS!", "You have been named",
     {{10, 180}, {6, 87}, {6, 91}, {4, 48}, {11, 195}}},
    {"level1", "dmills=niacdispatch",
     "Morning, Soldier. Lieutenant Mills here, Dispatch. Welcome",
     {{15, 719}, {5, 191}}},
    {"level15", "dmills=niacdispatch",
     "Howdy, Soldier! Lieutenant Mills here. Nataka brass",
     {{16, 768}, {13, 655}}},
};

// Corpus sums, pinned after the first run (see docs/BRIEFING.md §4).
constexpr int kTotalPages = 58;
constexpr int kTotalLines = 727;
constexpr int kTotalGlyphs = 33284;
constexpr int kMaxPages = 5;
constexpr const char* kMaxLevel = "level0a";

bool write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

/// Every refusal: false, `"briefing: ..."` text, cleared output, no crash.
void check_refused(const fs::path& game, const std::string& level,
                   const std::string& what, std::string_view expected) {
    gm::Briefing briefing;
    briefing.pages.resize(1);  // must be cleared by the call
    briefing.source = "stale";
    briefing.level = "stale";
    std::string error;
    const bool ok = gm::load_briefing(game, level, briefing, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "briefing:"), what + ": error prefix (got \"" + error + "\")");
    check(error.find(expected) != std::string::npos,
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check_eq(briefing.page_count(), 0, what + ": page count cleared");
    check_eq(briefing.line_count(), 0, what + ": line count cleared");
    check(briefing.plain_text().empty(), what + ": text cleared");
    check(briefing.source.empty(), what + ": source cleared");
}

void test_synthetic_negatives() {
    const fs::path root = fs::temp_directory_path() / "gm_briefing_negatives";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path game = root / "game";
    const fs::path brief = game / "SML" / "brief";
    fs::create_directories(brief, ec);
    check(!ec, "synthetic: temp tree created");
    if (ec) {
        return;
    }
    fs::create_directories(root / "game-without-brief", ec);

    // A small well-formed corpus for the enumeration half: `.sme` in mixed
    // case, one `.sml` (ignored), a directory named `dir.sme` (ignored).
    check(write_file(brief / "Alpha.SME", "<d><r>ALPHA</d>"), "synthetic: alpha");
    check(write_file(brief / "beta.sme", "<d><r>BETA</d>"), "synthetic: beta");
    check(write_file(brief / "gamma.sml", "<d><r>GAMMA</d>"), "synthetic: gamma");
    fs::create_directories(brief / "dir.sme", ec);

    const auto levels = gm::briefing_levels(game);
    check_eq(static_cast<long long>(levels.size()), 2, "synthetic: two .sme stems");
    if (levels.size() == 2) {
        check_eq(levels[0], std::string("alpha"), "synthetic: first stem lowercased");
        check_eq(levels[1], std::string("beta"), "synthetic: second stem");
    }
    check_eq(static_cast<long long>(gm::briefing_levels(root / "no-game").size()), 0,
             "synthetic: missing game -> no levels");
    check_eq(static_cast<long long>(gm::briefing_levels(fs::path{}).size()), 0,
             "synthetic: empty game -> no levels");

    // `<d><r>ALPHA</d>` is a valid document, but the pager needs font
    // atlases; without `FRONTEND/BMP.IDX` the failure is the pager's text,
    // not a crash.  This is the "game directory exists, fonts do not" case.
    {
        gm::Briefing briefing;
        std::string error;
        check(!gm::load_briefing(game, "alpha", briefing, &error),
              "synthetic: no atlases refused");
        check(starts_with(error, "briefing:"), "synthetic: no atlases error prefix");
        check_eq(briefing.page_count(), 0, "synthetic: no atlases cleared");
    }

    // Undecodable bytes: no `<d>` wrapper at all.
    check(write_file(brief / "level99.sme", "this is not sml markup"),
          "synthetic: broken file");
    check_refused(game, "level99", "synthetic: broken bytes", "sml: no tags");

    // An empty file is refused before parsing.
    check(write_file(brief / "level98.sme", ""), "synthetic: empty file");
    check_refused(game, "level98", "synthetic: empty file", "empty file");

    // A directory called `level97.sme` is not a readable briefing.
    fs::create_directories(brief / "level97.sme", ec);
    check_refused(game, "level97", "synthetic: directory instead of a file",
                  "file not found");

    // Name validation happens before the scan.
    check_refused(game, "", "synthetic: empty level", "empty level");
    check_refused(game, "   ", "synthetic: blank level", "empty level");
    check_refused(game, "level1.sml", "synthetic: .sml level", "not a .sme");
    check_refused(game, "SML/brief/level1.sme", "synthetic: level is a path", "not a path");
    check_refused(game, "..", "synthetic: dotdot level", "not a path");
    check_refused(game, "no-such-level", "synthetic: missing level", "file not found");

    // Not a directory at all.
    check(write_file(root / "not-a-directory.txt", "x"), "synthetic: plain file");
    check_refused(root / "not-a-directory.txt", "level1", "synthetic: game is a file",
                  "game directory not found");
    check_refused(root / "no-game", "level1", "synthetic: missing game",
                  "game directory not found");
    check_refused(root / "game-without-brief", "level1", "synthetic: no SML/brief",
                  "no SML/brief");

    fs::remove_all(root, ec);
}

void test_negatives(const fs::path& game) {
    // Real installation, invalid requests.  `level0` is the interesting one:
    // mission 0 ships as `LEVEL0A.SME`, there is no `LEVEL0.SME`.
    check_refused(game, "level0", "real: level0 (ships as level0a)", "file not found");
    check_refused(game, "help", "real: a non-briefing screen", "file not found");
    check_refused(game, "level1.sml", "real: .sml is not read", "not a .sme");
    check_refused(game, "SML/brief/LEVEL1.SME", "real: full path rejected", "not a path");
    check_refused(fs::path{}, "level1", "real: empty game path",
                  "game directory not found");
    check_refused(game / "SML" / "help.sme", "level1", "real: game is a file",
                  "game directory not found");
}

void test_corpus(const fs::path& game) {
    const auto levels = gm::briefing_levels(game);
    std::cout << "briefing levels: " << levels.size() << " (";
    if (!levels.empty()) {
        std::cout << levels.front() << " .. " << levels.back();
    }
    std::cout << ")\n";
    check_eq(static_cast<long long>(levels.size()),
             static_cast<long long>(sizeof(kLevels) / sizeof(kLevels[0])),
             "corpus: level count");
    check(!levels.empty() && levels.front() == "level0a",
          "corpus: first level is level0a (LEVEL0A.SME)");
    check(!levels.empty() && levels.back() == "level9", "corpus: last level is level9");
    for (std::size_t i = 1; i < levels.size(); ++i) {
        check(levels[i - 1] < levels[i], "corpus: levels sorted and unique");
    }
    check(gm::briefing_levels(game) == levels, "corpus: level list deterministic");

    int total_pages = 0;
    int total_lines = 0;
    int total_glyphs = 0;
    int max_pages = 0;
    std::string max_level;
    for (const LevelPin& pin : kLevels) {
        const std::string tag = std::string("corpus ") + pin.level + ": ";
        gm::Briefing briefing;
        std::string error;
        const bool ok = gm::load_briefing(game, pin.level, briefing, &error);
        check(ok, tag + "loads");
        if (!ok) {
            std::cerr << "FAIL: " << tag << error << "\n";
            continue;
        }
        check(error.empty(), tag + "no error on success");
        check_eq(briefing.level, std::string(pin.level), tag + "level name");
        check(!briefing.source.empty(), tag + "source set");
        check_eq(briefing.page_count(), pin.pages, tag + "page count");
        check_eq(briefing.line_count(), pin.lines, tag + "line count");

        int glyphs = 0;
        int lines = 0;
        for (int page = 0; page < briefing.page_count(); ++page) {
            const gm::BriefingPage& body = briefing.pages[static_cast<std::size_t>(page)];
            check_eq(body.index, page, tag + "page index " + std::to_string(page));
            check_eq(body.lines, static_cast<int>(body.text.size()),
                     tag + "page " + std::to_string(page) + " text size");
            check(body.lines > 0, tag + "page " + std::to_string(page) + " has lines");
            glyphs += body.glyphs;
            lines += body.lines;
            for (const std::string& line : body.text) {
                check(!line.empty(), tag + "page " + std::to_string(page) +
                                         " has no empty visual line");
            }
        }
        check_eq(glyphs, pin.glyphs, tag + "glyph sum");
        check_eq(lines, pin.lines, tag + "page line sum");
        check_hash(hash_text(compact_text(briefing.plain_text())), pin.body,
                   tag + "text matches gm/sml.py describe()[\"body\"] without spaces");
        check(!briefing.plain_text().empty(), tag + "plain text non-empty");

        // The text is latin-1/ASCII in the shipment: no high bytes, so UTF-8
        // would be a superset and the bytes can be compared directly.
        bool ascii = true;
        for (const char c : briefing.plain_text()) {
            if (static_cast<unsigned char>(c) >= 0x80) {
                ascii = false;
            }
        }
        check(ascii, tag + "text is ASCII (decode is latin-1, no utf-8 guess)");

        // Loading the same level twice is deterministic.
        gm::Briefing again;
        std::string second_error;
        check(gm::load_briefing(game, pin.level, again, &second_error), tag + "reloads");
        check_eq(again.page_count(), briefing.page_count(), tag + "reload page count");
        check_eq(again.line_count(), briefing.line_count(), tag + "reload line count");
        check_eq(again.plain_text(), briefing.plain_text(), tag + "reload text");
        check_eq(again.source, briefing.source, tag + "reload source");

        total_pages += pin.pages;
        total_lines += pin.lines;
        total_glyphs += pin.glyphs;
        if (pin.pages > max_pages) {
            max_pages = pin.pages;
            max_level = pin.level;
        }
        std::cout << "  " << pin.level << ": " << pin.pages << " pages, " << pin.lines
                  << " lines, " << pin.glyphs << " glyphs, body 0x" << std::hex
                  << pin.body << std::dec << "\n";
    }

    // Every file that exists is covered by the pin table.
    for (const std::string& level : levels) {
        check(find_pin(level) != nullptr, "corpus: " + level + " pinned");
    }
    check_eq(total_pages, kTotalPages, "corpus: total pages");
    check_eq(total_lines, kTotalLines, "corpus: total lines");
    check_eq(total_glyphs, kTotalGlyphs, "corpus: total glyphs");
    check_eq(max_pages, kMaxPages, "corpus: maximum pages");
    check_eq(max_level, std::string(kMaxLevel), "corpus: level with the most pages");
    std::cout << "briefing corpus: " << levels.size() << " levels, " << total_pages
              << " pages, " << total_lines << " lines, " << total_glyphs
              << " glyphs; most pages: " << max_level << " (" << max_pages << ")\n";
}

void test_details(const fs::path& game) {
    for (const DetailPin& detail : kDetails) {
        const std::string tag = std::string("detail ") + detail.level + ": ";
        gm::Briefing briefing;
        std::string error;
        if (!gm::load_briefing(game, detail.level, briefing, &error)) {
            check(false, tag + "loads (" + error + ")");
            continue;
        }
        std::cout << "  " << detail.level << ": " << briefing.page_count() << " pages\n";
        for (int page = 0; page < briefing.page_count(); ++page) {
            const gm::BriefingPage& body = briefing.pages[static_cast<std::size_t>(page)];
            std::cout << "    page " << page + 1 << "/" << briefing.page_count()
                      << ": " << body.lines << " lines, " << body.glyphs << " glyphs"
                      << "\n";
            for (int line = 0; line < body.lines && line < 2; ++line) {
                std::cout << "      " << line + 1 << ": \""
                          << body.text[static_cast<std::size_t>(line)] << "\"\n";
            }
        }

        check_eq(briefing_line(briefing, 0), std::string(detail.first),
                 tag + "first line");
        check_eq(briefing_line(briefing, 1), std::string(detail.second),
                 tag + "second line");
        if (!detail.pages.empty()) {
            check_eq(static_cast<long long>(briefing.page_count()),
                     static_cast<long long>(detail.pages.size()),
                     tag + "pinned page count");
            for (std::size_t page = 0; page < detail.pages.size() &&
                                       page < briefing.pages.size();
                 ++page) {
                check_eq(briefing.pages[page].lines, detail.pages[page].lines,
                         tag + "page " + std::to_string(page + 1) + " lines");
                check_eq(briefing.pages[page].glyphs, detail.pages[page].glyphs,
                         tag + "page " + std::to_string(page + 1) + " glyphs");
            }
        }

        // `glyphs` is what pager_render finds in the atlases, not a guess:
        // text_layout produces only glyphs the shipped faces have.
        gm::SmlPager pager(game);
        check(pager.open(briefing.source, &error), tag + "pager opens the source");
        if (!pager.opened()) {
            check(false, tag + "pager error: " + error);
            continue;
        }
        check_eq(pager.page_count(), briefing.page_count(),
                 tag + "pager page count matches briefing");
        std::vector<std::uint32_t> rgba;
        int width = 0;
        int height = 0;
        for (int page = 0; page < briefing.page_count(); ++page) {
            if (page > 0) {
                check(pager.show_page(page), tag + "show_page " + std::to_string(page));
            }
            gm::PageRenderStats stats;
            std::string render_error;
            check(gm::render_page(pager, game, rgba, width, height, &stats, &render_error),
                  tag + "render page " + std::to_string(page + 1) + ": " + render_error);
            const gm::BriefingPage& body =
                briefing.pages[static_cast<std::size_t>(page)];
            check_eq(stats.lines, body.lines,
                     tag + "page " + std::to_string(page + 1) + " render lines");
            check_eq(stats.glyphs, body.glyphs,
                     tag + "page " + std::to_string(page + 1) + " render glyphs");
            check_eq(stats.missing_glyphs, 0,
                     tag + "page " + std::to_string(page + 1) + " no missing glyphs");
        }
    }
}

void test_levels_shape(const fs::path& game) {
    // Spelling tolerance of the level argument: the same briefing is reachable
    // as `LEVEL1`, `level1`, `Level15.SmE` and with surrounding whitespace.
    const auto levels = gm::briefing_levels(game);
    check(!levels.empty(), "levels: non-empty");
    if (levels.empty()) {
        return;
    }
    gm::Briefing upper;
    std::string error;
    check(gm::load_briefing(game, "LEVEL1", upper, &error),
          "levels: LEVEL1 found case-insensitively");
    check_eq(upper.level, std::string("level1"), "levels: LEVEL1 normalised");
    check(starts_with(upper.source, game.string()),
          "levels: source is under the game directory");
    gm::Briefing suffixed;
    check(gm::load_briefing(game, "Level15.SmE", suffixed, &error),
          "levels: Level15.SmE accepted");
    check_eq(suffixed.level, std::string("level15"), "levels: suffix stripped");
    check(suffixed.page_count() > 0, "levels: suffix load has pages");
    check(gm::load_briefing(game, "  level1  ", upper, &error),
          "levels: surrounding whitespace tolerated");
}

} // namespace

int main() {
    test_synthetic_negatives();

    const char* env = std::getenv("GUNMETAL_DIR");
    const fs::path game = env != nullptr ? fs::path(env) : fs::path("/mnt/d/games/Gunmetal 3dfx");
    std::error_code ec;
    if (!fs::is_directory(game / "SML" / "brief", ec) ||
        !fs::exists(game / "FRONTEND" / "BMP.IDX", ec)) {
        std::cout << "briefing: game not installed, corpus skipped\n";
        return failures == 0 ? 0 : 1;
    }

    test_negatives(game);
    test_levels_shape(game);
    test_corpus(game);
    test_details(game);

    if (failures != 0) {
        std::cerr << "briefing: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "briefing: SML/brief index, pager pages, text against gm/sml.py "
                 "and glyphs against pager_render; OK\n";
    return 0;
}
