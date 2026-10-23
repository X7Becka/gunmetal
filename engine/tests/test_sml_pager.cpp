// Runtime pagination of the SML pager (state 2, `0x487b69`): the cursor on top
// of `render/text_layout`.
//
// The pager does not paginate by itself: `text_layout` cuts the document into
// 267x167 pages exactly like `0x497880`/`0x498600`.  These tests therefore
// compare every pager number with an independent `layout_text` call on the same
// document and pin the cursor invariants:
//
//   * page counts, per-page line counts and images match `text_layout`;
//   * forward paging succeeds exactly `page_count - 1` times, then false and the
//     page does not move; backward is symmetric;
//   * `first_page`/`last_page`/`show_page` change the page only when it moves;
//   * the line cursor walks all visual lines, follows the page and stops at both
//     ends without a change;
//   * negatives: before `open()`, an empty document, `<d></d>`, a broken file,
//     a missing font and a pager without a font source.
//
// The installation is only read.  When it is missing the corpus cases print a
// skip line and the process succeeds, like the other corpus tests.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "render/text_layout.h"
#include "res/sml.h"
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

// The fonts a shipped document can select (docs/TEXT_LAYOUT.md §2.3/§2.4):
// 1 and 2 are the body faces (FONTS4 via slots 0/5), 3 the display face.
gm::FontLookup make_lookup(const std::map<int, gm::FontAtlas>& fonts) {
    return [&fonts](int number) -> const gm::FontAtlas* {
        const auto it = fonts.find(number);
        return it == fonts.end() ? nullptr : &it->second;
    };
}

void test_closed_pager() {
    gm::SmlPager pager;
    const std::string tag = "closed pager: ";
    check_eq(pager.page_count(), 0, tag + "page count");
    check_eq(pager.page(), 0, tag + "page");
    check_eq(pager.line_count(), 0, tag + "line count");
    check_eq(pager.line(), 0, tag + "line");
    check_eq(pager.status(), std::string("0/0"), tag + "status");
    check(pager.lines().empty(), tag + "no lines");
    check(pager.images().empty(), tag + "no images");
    check(pager.current_page() == nullptr, tag + "no current page");
    check(!pager.opened(), tag + "not opened");

    check(!pager.next_page(), tag + "next before open");
    check(!pager.prev_page(), tag + "prev before open");
    check(!pager.first_page(), tag + "first before open");
    check(!pager.last_page(), tag + "last before open");
    check(!pager.show_page(0), tag + "show before open");
    check(!pager.scroll_next_line(), tag + "scroll down before open");
    check(!pager.scroll_prev_line(), tag + "scroll up before open");
    check_eq(pager.page(), 0, tag + "page kept");
    check_eq(pager.status(), std::string("0/0"), tag + "status kept");
}

void test_open_errors(const std::map<int, gm::FontAtlas>& fonts) {
    const gm::FontLookup lookup = make_lookup(fonts);

    // 1. Empty document: no text at all.
    {
        gm::SmlPager pager(lookup);
        gm::SmlDocument empty;
        std::string error;
        check(!pager.open(empty, &error), "empty document: open false");
        check(error.find("empty document") != std::string::npos,
              "empty document: error text");
        check(!pager.error().empty(), "empty document: error() set");
        check_eq(pager.page_count(), 0, "empty document: 0 pages");
        check_eq(pager.status(), std::string("0/0"), "empty document: status 0/0");
        check(!pager.next_page(), "empty document: next false");
    }

    // 2. No font source and no installation: a clean, named error.
    {
        gm::SmlPager pager;
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d>AB</d>", ok);
        if (ok) {
            std::string error;
            check(!pager.open(doc, &error), "no font source: open false");
            check(error.find("no font source") != std::string::npos,
                  "no font source: error text");
            check_eq(pager.page_count(), 0, "no font source: 0 pages");
        }
    }

    // 3. A real non-SML file is the broken document.
    {
        gm::SmlPager pager(lookup);
        std::string error;
        check(!pager.open(fs::path(GM_PAGER_TEST_SOURCE), &error),
              "broken document: open false");
        check(error.find("sml_pager:") != std::string::npos,
              "broken document: error names sml_pager");
        check_eq(pager.page_count(), 0, "broken document: 0 pages");
        check_eq(pager.status(), std::string("0/0"), "broken document: status 0/0");
    }

    // 4. A font the engine never initialises (font 6) is a layout error.
    {
        gm::SmlPager pager(lookup);
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d><f s=\"6\">AB</f></d>", ok);
        if (ok) {
            std::string error;
            check(!pager.open(doc, &error), "font 6: open false");
            check(error.find("no atlas for font 6") != std::string::npos,
                  "font 6: error text");
            check_eq(pager.page_count(), 0, "font 6: 0 pages");
        }
    }

    // 5. Valid markup without text: one page, no lines, status "page 1/1".
    {
        gm::SmlPager pager(lookup);
        bool ok = false;
        const gm::SmlDocument doc = parse_or_empty("<d></d>", ok);
        if (ok) {
            std::string error;
            check(pager.open(doc, &error), "empty body: open true");
            check(error.empty(), "empty body: no error");
            check(pager.opened(), "empty body: opened");
            check_eq(pager.page_count(), 1, "empty body: one page");
            check_eq(pager.line_count(), 0, "empty body: no lines");
            check(pager.lines().empty(), "empty body: lines() empty");
            check_eq(pager.status(), std::string("page 1/1"),
                     "empty body: status page 1/1");
            check(!pager.next_page(), "empty body: next false");
            check(!pager.prev_page(), "empty body: prev false");
            check(!pager.scroll_next_line(), "empty body: scroll down false");
            check(!pager.scroll_prev_line(), "empty body: scroll up false");
            check_eq(pager.page(), 0, "empty body: page kept");
        }
    }
}

// Absolute page index of a visual line, from the reference layout.
int page_of_line(const gm::LayoutResult& layout, int line) {
    int first = 0;
    for (std::size_t page = 0; page < layout.pages.size(); ++page) {
        const int count = static_cast<int>(layout.pages[page].lines.size());
        if (line >= first && line < first + count) {
            return static_cast<int>(page);
        }
        first += count;
    }
    return -1;
}

int total_lines(const gm::LayoutResult& layout) {
    int sum = 0;
    for (const gm::LayoutPage& page : layout.pages) {
        sum += static_cast<int>(page.lines.size());
    }
    return sum;
}

std::string per_page(const gm::LayoutResult& layout) {
    std::string out;
    for (std::size_t page = 0; page < layout.pages.size(); ++page) {
        if (page != 0) {
            out.push_back('/');
        }
        out += std::to_string(layout.pages[page].lines.size());
    }
    return out;
}

// One corpus document: every pager number against an independent layout.
void check_corpus_file(const fs::path& game, const gm::FontLookup& lookup,
                       const std::string& name, int& pages_total, int& lines_total) {
    const std::string tag = name + ": ";
    const fs::path path = game / "SML" / name;
    const auto bytes = read_file(path);
    std::string error;
    auto doc = gm::parse_sml(bytes, error);
    check(doc.has_value(), tag + "parses");
    if (!doc) {
        std::cerr << "FAIL: " << tag << error << "\n";
        return;
    }

    // Reference pagination: the same `layout_text` call the pager makes.
    const auto reference = gm::layout_text(*doc, lookup, gm::kDefaultFont,
                                           gm::LayoutOptions{}, &error);
    check(error.empty(), tag + "reference layout has no error");
    const int reference_pages = static_cast<int>(reference.pages.size());
    const int reference_lines = total_lines(reference);

    gm::SmlPager pager(lookup);
    check(pager.open(*doc, &error), tag + "open");
    if (!pager.opened()) {
        std::cerr << "FAIL: " << tag << error << "\n";
        return;
    }
    check(error.empty(), tag + "open has no error");
    check_eq(pager.page_count(), reference_pages, tag + "page count matches layout");
    check_eq(pager.page_width(), 267, tag + "page width");
    check_eq(pager.page_height(), 167, tag + "page height");
    check_eq(pager.line_count(), reference_lines, tag + "line count matches layout");
    check_eq(pager.status(),
             std::string("page 1/") + std::to_string(reference_pages),
             tag + "initial status");

    // Per-page lines/images plus the "inside the 267x167 box" invariant.
    int seen_lines = 0;
    int pages_without_lines = 0;
    for (int page = 0; page < reference_pages; ++page) {
        if (page == 0) {
            check(!pager.first_page(), tag + "first_page on page 0 is a no-op");
        } else {
            check(pager.next_page(), tag + "next_page to " + std::to_string(page));
        }
        check_eq(pager.page(), page, tag + "page " + std::to_string(page) + " cursor");
        const gm::LayoutPage& ref_page = reference.pages[static_cast<std::size_t>(page)];
        check_eq(static_cast<long long>(pager.lines().size()),
                 static_cast<long long>(ref_page.lines.size()),
                 tag + "page " + std::to_string(page) + " lines match layout");
        check_eq(static_cast<long long>(pager.images().size()),
                 static_cast<long long>(ref_page.images.size()),
                 tag + "page " + std::to_string(page) + " images match layout");
        if (pager.lines().empty()) {
            ++pages_without_lines;
        }
        check_eq(pager.line(),
                 static_cast<long long>(seen_lines),
                 tag + "page " + std::to_string(page) + " line cursor at page start");
        seen_lines += static_cast<int>(ref_page.lines.size());
        for (const gm::LayoutLine& line : pager.lines()) {
            check(line.x >= 0 && line.x + line.width <= 267,
                  tag + "page " + std::to_string(page) + " line inside width");
            check(line.y >= 0 && line.y + line.height <= 167,
                  tag + "page " + std::to_string(page) + " line inside height");
        }
        for (const gm::LayoutImage& image : pager.images()) {
            check(image.x >= 0 && image.x + image.width <= 267,
                  tag + "page " + std::to_string(page) + " image inside width");
        }
    }
    check_eq(seen_lines, reference_lines, tag + "sum of page lines");
    check_eq(pages_without_lines, 0, tag + "every page of a text document has lines");
    check_eq(pager.page(), reference_pages - 1, tag + "on the last page");
    check_eq(pager.status(),
             std::string("page ") + std::to_string(reference_pages) + "/" +
                 std::to_string(reference_pages),
             tag + "last status");

    // Forward paging is exactly page_count - 1 successes, then false.
    {
        check(pager.first_page() == (reference_pages > 1),
              tag + "first_page from the last page");
        if (reference_pages <= 1) {
            check(!pager.first_page(), tag + "single page: first is a no-op");
        }
        check_eq(pager.page(), 0, tag + "back on page 0");
        int successes = 0;
        while (pager.next_page()) {
            ++successes;
            check(successes < reference_pages, tag + "next_page did not overrun");
        }
        check_eq(successes, reference_pages - 1, tag + "forward successes");
        check_eq(pager.page(), reference_pages - 1, tag + "forward lands on the last");
        const int kept = pager.page();
        const int kept_line = pager.line();
        check(!pager.next_page(), tag + "next_page on the last is false");
        check_eq(pager.page(), kept, tag + "next_page on the last keeps page");
        check_eq(pager.line(), kept_line, tag + "next_page on the last keeps line");
    }

    // Backward is symmetric.
    {
        int successes = 0;
        while (pager.prev_page()) {
            ++successes;
            check(successes < reference_pages, tag + "prev_page did not overrun");
        }
        check_eq(successes, reference_pages - 1, tag + "backward successes");
        check_eq(pager.page(), 0, tag + "backward lands on page 0");
        check_eq(pager.line(), 0, tag + "backward lands on line 0");
        check(!pager.prev_page(), tag + "prev_page on the first is false");
        check_eq(pager.page(), 0, tag + "prev_page on the first keeps page");
    }

    // first_page/last_page/show_page: true only when the page moved.
    {
        check(pager.last_page() == (reference_pages > 1), tag + "last_page");
        check_eq(pager.page(), reference_pages - 1, tag + "last_page lands last");
        check(!pager.last_page(), tag + "last_page again is a no-op");
        check(!pager.show_page(reference_pages), tag + "show_page out of range");
        check(!pager.show_page(-1), tag + "show_page negative");
        check_eq(pager.page(), reference_pages - 1, tag + "rejected page keeps cursor");
        if (reference_pages > 1) {
            check(pager.show_page(1), tag + "show_page(1)");
            check_eq(pager.page(), 1, tag + "show_page(1) lands");
            check(!pager.show_page(1), tag + "show_page to the current is a no-op");
        }
        check(pager.first_page() == (reference_pages > 1), tag + "first_page back");
        check_eq(pager.page(), 0, tag + "first_page lands page 0");
    }

    // Line cursor: walk every visual line, follow the pages, stop at both ends.
    {
        check_eq(pager.line(), 0, tag + "line cursor starts at 0");
        int walked = 0;
        for (int line = 0; line < reference_lines; ++line) {
            check_eq(pager.line(), line, tag + "line cursor " + std::to_string(line));
            const int want_page = page_of_line(reference, line);
            check_eq(pager.page(), want_page,
                     tag + "line " + std::to_string(line) + " follows its page");
            const bool moved = pager.scroll_next_line();
            if (line + 1 < reference_lines) {
                check(moved, tag + "scroll_next_line " + std::to_string(line));
                ++walked;
            } else {
                check(!moved, tag + "scroll_next_line at the end is false");
            }
        }
        check_eq(walked, reference_lines - 1, tag + "line down successes");
        check_eq(pager.line(), reference_lines - 1, tag + "line cursor at the end");
        check_eq(pager.page(), page_of_line(reference, reference_lines - 1),
                 tag + "end line on its page");
        const int kept_page = pager.page();
        check(!pager.scroll_next_line(), tag + "scroll down at the end keeps state");
        check_eq(pager.line(), reference_lines - 1, tag + "end line kept");
        check_eq(pager.page(), kept_page, tag + "end page kept");

        walked = 0;
        while (pager.scroll_prev_line()) {
            ++walked;
            check_eq(pager.line(), reference_lines - 1 - walked,
                     tag + "line cursor on the way back");
        }
        check_eq(walked, reference_lines - 1, tag + "line up successes");
        check_eq(pager.line(), 0, tag + "line cursor back at 0");
        check_eq(pager.page(), 0, tag + "page back at 0");
        check(!pager.scroll_prev_line(), tag + "scroll up at the start is false");
    }

    // reset() closes the pager; the same document opens again.
    {
        pager.reset();
        check(!pager.opened(), tag + "reset closes");
        check_eq(pager.page_count(), 0, tag + "reset drops the pages");
        check_eq(pager.status(), std::string("0/0"), tag + "reset status");
        check(!pager.next_page(), tag + "reset next false");
        check(pager.open(*doc, &error), tag + "reopen");
        check_eq(pager.page_count(), reference_pages, tag + "reopen page count");
    }

    pages_total += reference_pages;
    lines_total += reference_lines;
    std::cout << "  " << name << ": " << reference_pages << " pages, lines "
              << per_page(reference) << " (" << reference_lines << "), status "
              << pager.status() << "\n";
}

void test_corpus(const fs::path& game, const gm::FontLookup& lookup) {
    // Different page counts and both markup forms: one page (prelevel,
    // design), three pages (help, briefing with <I>), the credits and the
    // longest briefing of the shipment.
    const char* files[] = {
        "prelevel/level21.sme",
        "design/weapon20.sme",
        "help.sme",
        "brief/level10.sme",
        "CREDITS.SML",
        "brief/level22.sme",
    };
    int pages = 0;
    int lines = 0;
    for (const char* file : files) {
        check_corpus_file(game, lookup, file, pages, lines);
    }
    check(pages > static_cast<int>(sizeof(files) / sizeof(files[0])),
          "corpus: more than one page per document on average");
    std::cout << "sml_pager corpus: " << (sizeof(files) / sizeof(files[0]))
              << " documents, " << pages << " pages, " << lines << " lines\n";
}

// The installation-backed constructor reads FRONTEND/BMP.IDX itself and
// resolves `HELP.SME` case-insensitively under `<game>/SML`.
void test_game_constructor(const fs::path& game, const gm::FontLookup& lookup) {
    gm::SmlPager pager(game);
    std::string error;

    check(pager.open("help.sme", &error), "game pager: open help.sme");
    if (pager.opened()) {
        const auto bytes = read_file(game / "SML" / "help.sme");
        auto doc = gm::parse_sml(bytes, error);
        check(doc.has_value(), "game pager: help.sme parses");
        if (doc) {
            const auto reference = gm::layout_text(*doc, lookup, gm::kDefaultFont,
                                                   gm::LayoutOptions{}, &error);
            check_eq(pager.page_count(),
                     static_cast<long long>(reference.pages.size()),
                     "game pager: page count matches layout");
        }
        std::cout << "  game constructor: SML/help.sme -> " << pager.status() << "\n";
        check(pager.open("SML/CREDITS.SML", &error),
              "game pager: open SML/CREDITS.SML");
        check(pager.opened(), "game pager: CREDITS opened");
        std::cout << "  game constructor: SML/CREDITS.SML -> " << pager.status() << "\n";

        // Atlas cache is per *document*: level21 uses the default font only,
        // weapon20 selects <F S="3">, so the second open must load font 3 even
        // though the first document did not ask for it.
        check(pager.open("prelevel/level21.sme", &error),
              "game pager: open prelevel/level21.sme");
        check(pager.open("design/weapon20.sme", &error),
              "game pager: font 3 loads for the next document");
        check(pager.opened(), "game pager: weapon20 opened");
        check_eq(pager.page_count(), 1, "game pager: weapon20 one page");
        std::cout << "  game constructor: design/weapon20.sme -> " << pager.status()
                  << " (font 3 loaded after a font-1 document)\n";
    }

    check(!pager.open("no-such-screen.sml", &error),
          "game pager: missing file rejected");
    check(error.find("file not found") != std::string::npos,
          "game pager: missing file error");
}

} // namespace

int main() {
    test_closed_pager();

    const char* env = std::getenv("GUNMETAL_DIR");
    const fs::path game = env != nullptr ? fs::path(env) : fs::path("/mnt/d/games/Gunmetal 3dfx");
    std::error_code ec;
    if (!fs::exists(game / "FRONTEND" / "BMP.IDX", ec) ||
        !fs::exists(game / "SML" / "help.sme", ec)) {
        std::cout << "sml_pager: game not installed, corpus skipped\n";
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

    test_open_errors(fonts);
    test_corpus(game, lookup);
    test_game_constructor(game, lookup);

    if (failures != 0) {
        std::cerr << "sml_pager: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "sml_pager: cursor over text_layout pages (267x167), line cursor, "
                 "boundary invariants and negatives; OK\n";
    return 0;
}
