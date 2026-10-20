// One SML screen: parse -> layout -> pages/lines -> RGB.
//
// Coverage:
//
//   * synthetic documents for the tag set the layout exposes (``<r>``, ``<p>``,
//     ``<c>``, ``<f>``, ``<x>``, ``<k>``, ``<s>``, ``<M>``, ``<I>``), one and
//     two pages, wrapping, empty screens, selection and scrolling;
//   * the shipped ``help.sme``, ``prelevel/level21.sme``,
//     ``design/weapon20.sme`` and ``brief/level10.sme``: page/line counts (the
//     same numbers as docs/TEXT_LAYOUT.md) and FNV-1a64 RGB goldens;
//   * ``<I>`` pixels checked against ``res/bmp`` (independent of the blitter);
//   * negatives: broken markup, missing file, empty input, empty screen, page
//     out of range, too small/wrong-sized target, short palette, unsafe and
//     missing image names, a font the engine never initialises.
//
// The text layout, glyph drawing and indexed blitter have their own proven
// tests (docs/TEXT_LAYOUT.md, docs/SPRITE.md); this suite pins how Screen wires
// them together.  The game is only read; without it the corpus part is skipped.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "res/bmp.h"
#include "ui/screen.h"

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

std::uint64_t fnv1a64(std::span<const std::uint8_t> data) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string format_hash(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::optional<gm::Screen> load_markup(const fs::path& game, const std::string& markup,
                                      std::string& error) {
    return gm::Screen::load(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(markup.data()),
                                      markup.size()),
        game, error);
}

struct Rendered {
    gm::RenderResult target;
    std::uint64_t hash = 0;
};

bool render_page(gm::Screen& screen, int page, Rendered& out, std::string& error) {
    out.target.width = screen.page_width();
    out.target.height = screen.page_height();
    out.target.rgb.assign(static_cast<std::size_t>(out.target.width) *
                              static_cast<std::size_t>(out.target.height) * 3u,
                          0);
    if (!screen.render(out.target, {}, page, error)) {
        return false;
    }
    out.hash = fnv1a64(out.target.rgb);
    return true;
}

std::string line_text(const gm::Screen& screen, std::size_t index) {
    return index < screen.lines().size() ? screen.lines()[index].text : std::string{};
}

// Every <I> texel must land at its slot with its own BMP palette (scale 1 in
// the shipped corpus).  This checks the blitter+asset path against res/bmp,
// independently of the FNV goldens.
void verify_images(gm::Screen& screen, int page, const gm::RenderResult& target,
                   const std::string& tag) {
    int slots = 0;
    long long mismatches = 0;
    for (const gm::ScreenImageSlot& slot : screen.image_slots()) {
        if (slot.page != page) {
            continue;
        }
        ++slots;
        const auto it = screen.image_assets().find([&slot] {
            std::string key = slot.name;
            for (char& c : key) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return key;
        }());
        check(it != screen.image_assets().end(), tag + ": image asset for " + slot.name);
        if (it == screen.image_assets().end()) {
            continue;
        }
        const gm::ScreenImageAsset& asset = it->second;
        check_eq(slot.scale, 1, tag + ": shipped image scale is 1");
        for (int dy = 0; dy < slot.height * slot.scale; ++dy) {
            const int ty = slot.y + dy;
            if (ty < 0 || ty >= target.height || ty >= screen.page_height()) {
                continue;
            }
            const int sy = dy / slot.scale;
            if (sy >= asset.height) {
                continue;
            }
            for (int dx = 0; dx < slot.width * slot.scale; ++dx) {
                const int tx = slot.x + dx;
                if (tx < 0 || tx >= target.width || tx >= screen.page_width()) {
                    continue;
                }
                const int sx = dx / slot.scale;
                if (sx >= asset.width) {
                    continue;
                }
                const std::uint8_t index =
                    asset.pixels[static_cast<std::size_t>(sy) * asset.width + sx];
                const std::size_t at =
                    (static_cast<std::size_t>(ty) * target.width + tx) * 3u;
                const std::size_t pal = static_cast<std::size_t>(index) * 4u;
                if (target.rgb[at] != asset.palette[pal] ||
                    target.rgb[at + 1] != asset.palette[pal + 1] ||
                    target.rgb[at + 2] != asset.palette[pal + 2]) {
                    ++mismatches;
                }
            }
        }
    }
    check(slots > 0, tag + ": at least one <I> on the page");
    check_eq(mismatches, 0, tag + ": every image texel matches its BMP");
}

// --------------------------------------------------------------- synthetic --

void test_synthetic(const fs::path& game) {
    std::string error;

    // One page, two rows.
    {
        auto screen = load_markup(game, "<d>AB<r>CD</d>", error);
        check(screen.has_value(), "one page: loads");
        if (screen) {
            check(screen->layout(100, 100), "one page: layout");
            check(error.empty(), "one page: no error");
            check_eq(static_cast<long long>(screen->pages()), 1, "one page: pages");
            check_eq(static_cast<long long>(screen->lines().size()), 2, "one page: lines");
            check_eq(line_text(*screen, 0), "AB", "one page: first line text");
            check_eq(line_text(*screen, 1), "CD", "one page: second line text");
            check_eq(screen->lines().at(1).y, 10, "one page: second line y");
            check_eq(screen->lines().at(0).font, 1, "one page: font 1");
            check_eq(screen->state().pages, 1, "one page: state pages");
            check_eq(screen->state().lines, 2, "one page: state lines");
            check(screen->state().image_geometry_hypothesis,
                  "one page: image hypothesis flagged");

            Rendered rendered;
            check(render_page(*screen, 0, rendered, error), "one page: render");
            check(rendered.hash != 0, "one page: render wrote pixels");

            check_eq(static_cast<long long>(screen->state().rendered_page), 0,
                     "one page: state rendered page");
            check_eq(format_hash(screen->state().rgb_hash), format_hash(rendered.hash),
                     "one page: state rgb hash");
        }
    }

    // Two pages: the same source with a 25 px page breaks before CCC.
    {
        auto screen = load_markup(game, "<d>AAA<r>BBB<r>CCC<r>DDD</d>", error);
        check(screen.has_value(), "two pages: loads");
        if (screen) {
            check(screen->layout(100, 25), "two pages: layout");
            check_eq(static_cast<long long>(screen->pages()), 2, "two pages: pages");
            check_eq(static_cast<long long>(screen->lines().size()), 4, "two pages: lines");
            check_eq(static_cast<long long>(screen->layout_result().pages.at(0).lines.size()), 2,
                     "two pages: page 0 lines");
            check_eq(static_cast<long long>(screen->layout_result().pages.at(1).lines.size()), 2,
                     "two pages: page 1 lines");
            check_eq(line_text(*screen, 0), "AAA", "two pages: line 0 text");
            check_eq(line_text(*screen, 2), "CCC", "two pages: line 2 text");
            check_eq(screen->lines().at(2).page, 1, "two pages: line 2 page");
            check_eq(screen->lines().at(2).index, 0, "two pages: line 2 local index");
        }
    }

    // Wrapping: the golden of test_text_layout (30 px -> 22 + 18).
    {
        auto screen = load_markup(game, "<d>AAA BBB</d>", error);
        if (screen) {
            check(screen->layout(30, 100), "wrap: layout");
            check_eq(static_cast<long long>(screen->lines().size()), 2, "wrap: lines");
            check_eq(screen->lines().at(0).width, 22, "wrap: line 0 width");
            check_eq(screen->lines().at(1).width, 18, "wrap: line 1 width");
            check_eq(line_text(*screen, 0), "AAA ", "wrap: line 0 text");
            check_eq(line_text(*screen, 1), "BBB", "wrap: line 1 text");
        } else {
            check(false, "wrap: loads");
        }
    }

    // Paragraph spacing: half a line (5 px) then a full line.
    {
        auto screen = load_markup(game, "<d>A<p>B</d>", error);
        if (screen) {
            check(screen->layout(100, 100), "paragraph: layout");
            check_eq(static_cast<long long>(screen->lines().size()), 2, "paragraph: lines");
            check_eq(screen->lines().at(1).y, 15, "paragraph: +5 then +10");
            check_eq(line_text(*screen, 1), "B", "paragraph: text");
        } else {
            check(false, "paragraph: loads");
        }
    }

    // Centring: <c>AB</c> in 30 px starts at (30-12)/2 = 9.
    {
        auto screen = load_markup(game, "<d><c>AB</c></d>", error);
        if (screen) {
            check(screen->layout(30, 100), "centre: layout");
            check_eq(static_cast<long long>(screen->lines().size()), 1, "centre: lines");
            check_eq(screen->lines().at(0).x, 9, "centre: x");
            check_eq(line_text(*screen, 0), "AB", "centre: text");
        } else {
            check(false, "centre: loads");
        }
    }

    // Font switch inside the line: heights and restoration.
    {
        auto screen = load_markup(game, "<d><f s=\"3\">AB<r>CD</f>E</d>", error);
        if (screen) {
            check(screen->layout(267, 167), "font switch: layout");
            check_eq(static_cast<long long>(screen->lines().size()), 2, "font switch: lines");
            check_eq(screen->lines().at(0).height, 18, "font switch: line 0 height");
            check_eq(screen->lines().at(0).font, 3, "font switch: line 0 font");
            check_eq(line_text(*screen, 1), "CDE", "font switch: line 1 text");
            check_eq(screen->lines().at(1).font, 3, "font switch: line 1 font (first run)");
            bool has_font1 = false;
            for (const gm::LayoutRun& run : screen->layout_result().runs) {
                if (run.text == "E") {
                    has_font1 = run.font == 1;
                }
            }
            check(has_font1, "font switch: </f> restores font 1");
        } else {
            check(false, "font switch: loads");
        }
    }

    // <s>/<k>/<x>/<M>: placeholders advance the pen, <x> jumps, <M> is a no-op.
    {
        auto screen = load_markup(
            game, "<d>A<s>B<r><k><x v=\"50\">C<M B=\"1\" S=\"70\">D</d>", error);
        if (screen) {
            check(screen->layout(267, 167), "placeholders: layout");
            check_eq(static_cast<long long>(screen->lines().size()), 2,
                     "placeholders: lines");
            check_eq(line_text(*screen, 0), "A B", "placeholders: <s> inserts a space");
            check_eq(line_text(*screen, 1), "SoldierCD", "placeholders: <k> and text");
            int c_x = -1;
            for (const gm::LayoutRun& run : screen->layout_result().runs) {
                if (run.text == "C") {
                    c_x = run.x;
                }
            }
            check_eq(c_x, 133, "placeholders: <x v=50> lands at 133/267");
        } else {
            check(false, "placeholders: loads");
        }
    }

    // <I>: an image-only page pins the blit rectangle, a following row pins the
    // left-float indent (column_start = space + image width).
    {
        auto screen = load_markup(
            game, "<d><I f=\"drilhed\" x=\"64\" y=\"64\" a=\"l\"></d>", error);
        check(screen.has_value(), "<I>: image-only loads");
        if (screen) {
            check(screen->layout(267, 167), "<I>: image-only layout");
            check_eq(static_cast<long long>(screen->state().images), 1, "<I>: one slot");
            check_eq(static_cast<long long>(screen->lines().size()), 0,
                     "<I>: image does not create a line");
            check_eq(static_cast<long long>(screen->image_slots().size()), 1,
                     "<I>: slot list");
            if (!screen->image_slots().empty()) {
                const gm::ScreenImageSlot& slot = screen->image_slots().at(0);
                check_eq(slot.x, 0, "<I>: left slot x (float anchor)");
                check_eq(slot.y, 0, "<I>: slot y (current row)");
                check_eq(slot.width, 64, "<I>: slot width");
                check_eq(slot.height, 64, "<I>: slot height");
                check_eq(slot.scale, 1, "<I>: default scale");
                check(slot.name == "drilhed", "<I>: name");
            }
            Rendered rendered;
            check(render_page(*screen, 0, rendered, error), "<I>: render");
            verify_images(*screen, 0, rendered.target, "<I>");
        }
    }

    {
        auto screen = load_markup(
            game, "<d><I f=\"drilhed\" x=\"64\" y=\"64\" a=\"l\"><r>Text after image</d>",
            error);
        check(screen.has_value(), "<I> text: loads");
        if (screen) {
            check(screen->layout(267, 167), "<I> text: layout");
            check_eq(static_cast<long long>(screen->lines().size()), 1,
                     "<I> text: one text line");
            check_eq(screen->lines().at(0).x, 68, "<I> text: after the left float");
            check_eq(static_cast<long long>(screen->lines().at(0).y), 10,
                     "<I> text: next row");
            check_eq(line_text(*screen, 0), "Text after image", "<I> text: text");
        } else {
            check(false, "<I> text: loads");
        }
    }

    {
        auto screen = load_markup(
            game, "<d><I f=\"drilhed\" x=\"64\" y=\"64\" a=\"r\"></d>", error);
        if (screen) {
            check(screen->layout(267, 167), "<I> right: layout");
            if (!screen->image_slots().empty()) {
                check_eq(screen->image_slots().at(0).x, 203,
                         "<I> right: 267 - 64 at the right edge");
            }
        } else {
            check(false, "<I> right: loads");
        }
    }

    {
        // Optional integer zoom and a caller-supplied 256-colour palette.
        auto screen = load_markup(game, "<d><I f=\"drilhed\" s=\"2\"></d>", error);
        if (screen) {
            check(screen->layout(267, 167), "<I> scale: layout");
            check_eq(static_cast<long long>(screen->image_slots().size()), 1,
                     "<I> scale: slot");
            if (!screen->image_slots().empty()) {
                check_eq(screen->image_slots().at(0).scale, 2, "<I> scale: s=2");
                check_eq(screen->image_slots().at(0).width, 64,
                         "<I> scale: layout slot stays 64");
            }
            std::vector<std::uint8_t> palette(gm::kScreenPaletteBytes, 0);
            for (int i = 0; i < 256; ++i) {
                palette[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(i);
                palette[static_cast<std::size_t>(i) * 4 + 1] =
                    static_cast<std::uint8_t>(255 - i);
                palette[static_cast<std::size_t>(i) * 4 + 2] =
                    static_cast<std::uint8_t>((i * 3) & 0xff);
            }
            gm::RenderResult target;
            target.width = 267;
            target.height = 167;
            target.rgb.assign(267u * 167u * 3u, 0);
            check(screen->render(target, palette, 0, error), "<I> scale: render");
            const auto asset = screen->image_assets().find("drilhed");
            check(asset != screen->image_assets().end(), "<I> scale: asset");
            if (asset != screen->image_assets().end()) {
                const int sx = 5;
                const int sy = 7;
                const std::uint8_t index =
                    asset->second.pixels[static_cast<std::size_t>(sy) * 64 + sx];
                const std::size_t at = (static_cast<std::size_t>(sy * 2) * 267 + sx * 2) * 3u;
                const std::size_t pal = static_cast<std::size_t>(index) * 4;
                check(target.rgb[at] == palette[pal] &&
                          target.rgb[at + 1] == palette[pal + 1] &&
                          target.rgb[at + 2] == palette[pal + 2],
                      "<I> scale: 2x2 block samples texel (5,7) with the override palette");
            }
        } else {
            check(false, "<I> scale: loads");
        }
    }

    // Selection and scrolling over two pages.
    {
        auto screen = load_markup(game, "<d>AAA<r>BBB<r>CCC<r>DDD</d>", error);
        if (screen) {
            check(screen->layout(100, 25), "selection: layout");
            check_eq(static_cast<long long>(screen->lines().size()), 4, "selection: lines");
            check(!screen->has_selection(), "selection: none initially");
            check(screen->select(2), "selection: select line 2");
            check_eq(static_cast<long long>(screen->state().page), 1,
                     "selection: page follows the line");
            check_eq(static_cast<long long>(screen->state().selected), 2,
                     "selection: selected index");
            check(screen->has_selection(), "selection: active");

            Rendered page1;
            check(render_page(*screen, 1, page1, error), "selection: render page 1");
            const gm::ScreenLine& line = screen->lines().at(2);
            const auto pixel = [&](int x, int y) {
                const std::size_t at = (static_cast<std::size_t>(y) * page1.target.width + x) * 3u;
                return std::array<int, 3>{page1.target.rgb[at], page1.target.rgb[at + 1],
                                          page1.target.rgb[at + 2]};
            };
            check(pixel(line.x, std::max(0, line.y - 1)) ==
                      std::array<int, 3>{gm::kScreenSelectionColor[0],
                                         gm::kScreenSelectionColor[1],
                                         gm::kScreenSelectionColor[2]},
                  "selection: top frame pixel");
            check_eq(static_cast<long long>(screen->state().drawn_lines), 1,
                     "selection: one frame drawn");

            Rendered page0;
            check(render_page(*screen, 0, page0, error), "selection: render page 0");
            const int probe_x = std::min(page0.target.width - 1, 18);
            const std::size_t probe =
                (0 * static_cast<std::size_t>(page0.target.width) + probe_x) * 3u;
            check(!(page0.target.rgb[probe] == 255 && page0.target.rgb[probe + 1] == 255 &&
                    page0.target.rgb[probe + 2] == 0),
                  "selection: no frame on the other page");

            check(screen->scroll(-1), "selection: scroll up");
            check_eq(static_cast<long long>(screen->state().selected), 1,
                     "selection: scroll up index");
            check_eq(static_cast<long long>(screen->state().page), 0,
                     "selection: scroll up page");
            check(screen->scroll(10), "selection: scroll clamps down");
            check_eq(static_cast<long long>(screen->state().selected), 3,
                     "selection: clamped to the last line");
            check_eq(static_cast<long long>(screen->state().page), 1,
                     "selection: clamped page");
            check(screen->scroll(-99), "selection: scroll clamps up");
            check_eq(static_cast<long long>(screen->state().selected), 0,
                     "selection: clamped to the first line");
            check(!screen->select(4), "selection: out of range rejected");
            check(screen->state().error.find("out of range") != std::string::npos,
                  "selection: out of range error");

            check(screen->show_page(1), "selection: show page 1");
            check_eq(static_cast<long long>(screen->state().page), 1,
                     "selection: show page state");
            check(!screen->show_page(2), "selection: show page out of range");
        } else {
            check(false, "selection: loads");
        }
    }

    // Empty screen: one empty page, blank render, navigation fails cleanly.
    {
        auto screen = load_markup(game, "<d></d>", error);
        check(screen.has_value(), "empty: loads");
        if (screen) {
            check(screen->layout(267, 167), "empty: layout");
            check_eq(static_cast<long long>(screen->pages()), 1, "empty: one page");
            check_eq(static_cast<long long>(screen->lines().size()), 0, "empty: no lines");
            check_eq(static_cast<long long>(screen->state().images), 0,
                     "empty: no images");
            Rendered rendered;
            check(render_page(*screen, 0, rendered, error), "empty: blank render");
            bool zero = true;
            for (const std::uint8_t byte : rendered.target.rgb) {
                zero = zero && byte == 0;
            }
            check(zero, "empty: render leaves the target blank");
            check(!screen->select(0), "empty: select fails");
            check(screen->state().error.find("out of range") != std::string::npos,
                  "empty: select error");
            check(screen->scroll(0), "empty: scroll(0) is a no-op");
            check(!screen->scroll(1), "empty: scroll fails");
            check(!screen->show_page(1), "empty: page 1 rejected");
        }
    }

    // Re-layout resets navigation.
    {
        auto screen = load_markup(game, "<d>AAA<r>BBB<r>CCC<r>DDD</d>", error);
        if (screen) {
            check(screen->layout(100, 25), "relayout: first layout");
            check(screen->select(2), "relayout: select");
            check(screen->layout(100, 25), "relayout: second layout");
            check(!screen->has_selection(), "relayout: selection reset");
            check_eq(static_cast<long long>(screen->state().page), 0, "relayout: page reset");
        } else {
            check(false, "relayout: loads");
        }
    }

    // A larger target: the page is drawn at the origin, outside it stays clean.
    {
        auto screen = load_markup(game, "<d>AB<r>CD</d>", error);
        if (screen) {
            check(screen->layout(267, 167), "large target: layout");
            gm::RenderResult large;
            large.width = 320;
            large.height = 200;
            large.rgb.assign(320u * 200u * 3u, 0);
            check(screen->render(large, {}, 0, error), "large target: render");
            bool outside = true;
            for (int y = 0; y < large.height; ++y) {
                for (int x = 0; x < large.width; ++x) {
                    if (x < 267 && y < 167) {
                        continue;
                    }
                    const std::size_t at = (static_cast<std::size_t>(y) * large.width + x) * 3u;
                    outside = outside && large.rgb[at] == 0 && large.rgb[at + 1] == 0 &&
                              large.rgb[at + 2] == 0;
                }
            }
            check(outside, "large target: pixels outside the page untouched");
            Rendered page;
            check(render_page(*screen, 0, page, error), "large target: page reference");
            bool same = true;
            for (int y = 0; y < 167; ++y) {
                for (int x = 0; x < 267; ++x) {
                    const std::size_t a = (static_cast<std::size_t>(y) * 320 + x) * 3u;
                    const std::size_t b = (static_cast<std::size_t>(y) * 267 + x) * 3u;
                    same = same && large.rgb[a] == page.target.rgb[b] &&
                           large.rgb[a + 1] == page.target.rgb[b + 1] &&
                           large.rgb[a + 2] == page.target.rgb[b + 2];
                }
            }
            check(same, "large target: page bytes match the small target");
        } else {
            check(false, "large target: loads");
        }
    }
}

// --------------------------------------------------------------- negatives --

void test_negatives(const fs::path& game) {
    std::string error;

    {
        const std::string broken = "<d>AB";
        auto screen = gm::Screen::load(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(broken.data()), broken.size()),
            game, error);
        check(!screen.has_value(), "negative: broken markup rejected");
        check(!error.empty() && error.find("screen:") == 0,
              "negative: broken markup error is prefixed");
    }
    {
        auto screen = gm::Screen::load(std::span<const std::uint8_t>{}, game, error);
        check(!screen.has_value(), "negative: empty input rejected");
        check(!error.empty(), "negative: empty input error");
    }
    {
        auto screen = gm::Screen::load(game / "SML" / "no-such-screen.sme", game, error);
        check(!screen.has_value(), "negative: missing screen file rejected");
        check(error.find("not found") != std::string::npos,
              "negative: missing screen error");
    }

    auto screen = load_markup(game, "<d>AB<r>CD</d>", error);
    check(screen.has_value(), "negative: fixture loads");
    if (!screen) {
        return;
    }

    {
        gm::RenderResult target;
        target.width = 267;
        target.height = 167;
        target.rgb.assign(267u * 167u * 3u, 0xAB);
        error.clear();
        check(!screen->render(target, {}, 0, error),
              "negative: render before layout rejected");
        check(error.find("layout()") != std::string::npos,
              "negative: render before layout error");
        check(!screen->select(0), "negative: select before layout rejected");
    }

    check(screen->layout(267, 167), "negative: fixture layout");

    {
        gm::RenderResult target;
        target.width = 267;
        target.height = 167;
        target.rgb.assign(267u * 167u * 3u, 0xAB);
        error.clear();
        check(!screen->render(target, {}, 5, error), "negative: page out of range");
        check(error.find("page") != std::string::npos, "negative: page error");
        check(target.rgb.front() == 0xAB && target.rgb.back() == 0xAB,
              "negative: page failure leaves the target untouched");
    }
    {
        gm::RenderResult target;
        target.width = 50;
        target.height = 50;
        target.rgb.assign(50u * 50u * 3u, 0xAB);
        error.clear();
        check(!screen->render(target, {}, 0, error), "negative: target too small");
        check(error.find("smaller than the page") != std::string::npos,
              "negative: target size error");
        check(target.rgb.front() == 0xAB, "negative: small target untouched");
    }
    {
        gm::RenderResult target;
        target.width = 267;
        target.height = 167;
        target.rgb.assign(10, 0);
        error.clear();
        check(!screen->render(target, {}, 0, error), "negative: wrong RGB size");
        check(error.find("expected") != std::string::npos, "negative: RGB size error");
    }
    {
        gm::RenderResult target;
        target.width = 267;
        target.height = 167;
        target.rgb.assign(267u * 167u * 3u, 0);
        const std::vector<std::uint8_t> short_palette(64, 0);
        error.clear();
        check(!screen->render(target, short_palette, 0, error),
              "negative: short palette rejected");
        check(error.find("palette") != std::string::npos, "negative: palette error");
    }
    {
        auto unsafe = load_markup(game, "<d><I f=\"../evil.bmp\" a=\"l\"></d>", error);
        check(unsafe.has_value(), "negative: unsafe image markup parses");
        if (unsafe) {
            check(!unsafe->layout(267, 167), "negative: unsafe image name rejected");
            check(unsafe->state().error.find("unsafe") != std::string::npos,
                  "negative: unsafe image error");
        }
    }
    {
        auto missing = load_markup(game, "<d><I f=\"no-such-image\" a=\"l\"></d>", error);
        check(missing.has_value(), "negative: missing image markup parses");
        if (missing) {
            check(!missing->layout(267, 167), "negative: missing image rejected");
            check(missing->state().error.find("not found") != std::string::npos,
                  "negative: missing image error");
        }
    }
    {
        auto font = load_markup(game, "<d><f s=\"6\">AB</d>", error);
        check(font.has_value(), "negative: unknown-font markup parses");
        if (font) {
            check(!font->layout(267, 167), "negative: font 6 rejected");
            check(font->state().error.find("no atlas for font 6") != std::string::npos,
                  "negative: font 6 error");
        }
    }
    {
        // A failed re-layout must drop the previous pages, not keep them usable.
        check(screen->layout(267, 167), "negative: valid layout before failure");
        check(!screen->layout(0, 167), "negative: zero page width rejected");
        check(!screen->state().laid_out, "negative: failed layout clears the flag");
        check_eq(static_cast<long long>(screen->pages()), 0,
                 "negative: failed layout clears pages");
        gm::RenderResult target;
        target.width = 267;
        target.height = 167;
        target.rgb.assign(267u * 167u * 3u, 0);
        error.clear();
        check(!screen->render(target, {}, 0, error),
              "negative: render after a failed layout rejected");
    }
}

// ------------------------------------------------------------------ golden --

struct ScreenGolden {
    const char* file;
    int pages;
    int lines;
    const char* per_page;  // page line counts, comma separated
    const char* page0;     // FNV-1a64 of page 0 at 267x167
    const char* all;       // FNV-1a64 over the concatenated pages
};

// Counts are cross-checked with docs/TEXT_LAYOUT.md (the layout golden table);
// the RGB hashes pin this runtime (components are proven in their own docs).
constexpr ScreenGolden kGoldens[] = {
    {"help.sme", 3, 42, "15,15,12", "e19f3ef44f57c6af", "f8cdde79f171e28c"},
    {"prelevel/level21.sme", 1, 13, "13", "44b4c00f86496903", "44b4c00f86496903"},
    {"design/weapon20.sme", 1, 2, "2", "674f46c7bd980c4d", "674f46c7bd980c4d"},
    {"brief/level10.sme", 3, 35, "16,16,3", "daa9fcc5f7fd2c2a", "a77b19b6abb9c2e1"},
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

void test_golden(const fs::path& game) {
    std::string error;
    for (const ScreenGolden& golden : kGoldens) {
        const std::string tag = std::string(golden.file) + ": ";
        auto screen = gm::Screen::load(golden.file, game, error);
        check(screen.has_value(), tag + "loads (case-insensitive relative path)");
        if (!screen) {
            std::cerr << "FAIL: " << tag << error << "\n";
            continue;
        }
        check(screen->layout(), tag + "layout at the default 267x167");
        if (!screen->state().laid_out) {
            std::cerr << "FAIL: " << tag << screen->state().error << "\n";
            continue;
        }
        check_eq(static_cast<long long>(screen->pages()), golden.pages, tag + "pages");
        check_eq(static_cast<long long>(screen->lines().size()), golden.lines,
                 tag + "lines");
        check_eq(static_cast<long long>(screen->state().pages), golden.pages,
                 tag + "state pages");

        std::size_t counted = 0;
        const std::vector<int> per_page = parse_ints(golden.per_page);
        check_eq(static_cast<long long>(per_page.size()),
                 static_cast<long long>(screen->layout_result().pages.size()),
                 tag + "per-page golden length");
        for (std::size_t page = 0; page < screen->layout_result().pages.size(); ++page) {
            const gm::LayoutPage& layout_page = screen->layout_result().pages[page];
            counted += layout_page.lines.size();
            if (page < per_page.size()) {
                check_eq(static_cast<long long>(layout_page.lines.size()), per_page[page],
                         tag + "page " + std::to_string(page) + " lines");
            }
        }
        check_eq(static_cast<long long>(counted), golden.lines, tag + "page line sum");

        Rendered page;
        check(render_page(*screen, 0, page, error), tag + "render page 0");
        check_eq(format_hash(page.hash), golden.page0, tag + "page 0 RGB FNV");

        std::uint64_t all = 1469598103934665603ull;
        std::size_t page_with_images = 0;
        for (int index = 0; index < golden.pages; ++index) {
            Rendered rendered;
            check(render_page(*screen, index, rendered, error),
                  tag + "render page " + std::to_string(index));
            for (const std::uint8_t byte : rendered.target.rgb) {
                all ^= byte;
                all *= 1099511628211ull;
            }
            bool has_images = false;
            for (const gm::ScreenImageSlot& slot : screen->image_slots()) {
                if (slot.page != index) {
                    continue;
                }
                has_images = true;
                // The rectangle therefore always lies inside the page; exact
                // texels are pinned by the synthetic image-only case above.
                check(slot.x >= 0 && slot.y >= 0, tag + "image origin non-negative");
                check(slot.x + slot.width <= screen->page_width(),
                      tag + "image inside page width");
                check(slot.y + slot.height <= screen->page_height(),
                      tag + "image inside page height");
                check(!screen->image_assets().empty(), tag + "image asset decoded");
            }
            if (has_images) {
                ++page_with_images;
            }
        }
        check_eq(format_hash(all), golden.all, tag + "all-pages RGB FNV");
        if (std::string_view(golden.file) == "brief/level10.sme") {
            check_eq(static_cast<long long>(screen->image_slots().size()), 2,
                     tag + "two <I> slots");
            check(page_with_images > 0, tag + "<I> pages exercised");
        }
    }
}

} // namespace

int main() {
    const char* env = std::getenv("GUNMETAL_DIR");
    const fs::path game = env != nullptr ? fs::path(env) : fs::path("/mnt/d/games/Gunmetal 3dfx");
    std::error_code ec;
    if (!fs::exists(game / "FRONTEND" / "BMP.IDX", ec) ||
        !fs::exists(game / "SML" / "help.sme", ec)) {
        std::cout << "screen: game not installed, skipped\n";
        return 0;
    }

    test_synthetic(game);
    test_negatives(game);
    test_golden(game);

    if (failures != 0) {
        std::cerr << "screen: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "screen: synthetic (pages/wrap/centre/fonts/<I>/selection), "
                 "negatives and 4 shipped goldens; OK\n";
    return 0;
}
