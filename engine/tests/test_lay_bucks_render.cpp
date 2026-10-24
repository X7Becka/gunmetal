// One frame of screen 20, `LAY DOWN THE BUCKS` (`M8.8` slice 14,
// `ui/lay_bucks_render`, docs/LAY_BUCKS_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object and the screen has no cursor and no value rows.  The test
// pins the geometry of docs/UI_SCREENS.md §2 and the listing of `0x48a830`:
//
//   * model: `rows` 1, step 0, four text runs — the title `LAY DOWN THE BUCKS`
//     (font 5, centred in the 196-px band, y 14) and the three notices
//     `THIS FEATURE IS NOT` / `AVAILABLE IN THE` / `SHAREWARE VERSION` (font 0,
//     x 42, y 27/36/45), all with `item = -1`, so no highlight and no font 4;
//   * the shipped install: 1 row, 0 arrows, no missing glyph, 320x200 RGBA,
//     every pixel opaque; the title band and the three notice bands are
//     compared byte for byte with an independent blit and the bounding boxes are
//     printed; ink = the four text bands;
//   * no money: `options.cash` 0/4/11/99 give byte-identical frames (the
//     handler never reads `0x4bc6f8`), the diffs and the labels are printed;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `FONTS4`, one with `FONTS4` but no `PLATE`, a `PLATE` of the
//     wrong size, an unknown background entry, `fade_lut` without a table and a
//     dump, and a missing dump file — all `lay_bucks_render:` texts with a
//     cleared buffer; a `FONTS4`+`PLATE` archive renders; two renders are
//     deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/LAY_BUCKS_RENDER.md §4 pins them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "ui/lay_bucks_render.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(long long got, long long want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got \"" << got << "\", want \"" << want
                  << "\")\n";
    }
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

/// One frame (or one refusal) of the renderer.
struct Frame {
    bool ok = false;
    std::string error;
    std::vector<std::uint32_t> rgba;
    int width = 0;
    int height = 0;
    gm::LayBucksRenderStats stats;
};

Frame run(const fs::path& game, const gm::LayBucksRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_lay_bucks(game, options, frame.rgba, frame.width, frame.height,
                                    &frame.stats, &frame.error);
    return frame;
}

long long differing_pixels(const std::vector<std::uint32_t>& a,
                           const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) {
        return -1;
    }
    long long count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++count;
        }
    }
    return count;
}

bool is_black(std::uint32_t pixel) { return pixel == gm::kLayBucksBackground; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

bool has_colour(const Frame& frame, int y0, int y1, std::uint32_t colour, int x0 = 0) {
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = std::max(0, x0); x < frame.width; ++x) {
            if (at(frame, x, y) == colour) {
                return true;
            }
        }
    }
    return false;
}

struct Box {
    int x0 = -1;
    int y0 = -1;
    int x1 = -1;
    int y1 = -1;
    int count = 0;

    bool empty() const { return count == 0; }
    int width() const { return empty() ? 0 : x1 - x0 + 1; }
    int height() const { return empty() ? 0 : y1 - y0 + 1; }
};

std::string box_text(const Box& box) {
    if (box.empty()) {
        return "empty";
    }
    return "(" + std::to_string(box.x0) + "," + std::to_string(box.y0) + ")-(" +
           std::to_string(box.x1) + "," + std::to_string(box.y1) + ") " +
           std::to_string(box.width()) + "x" + std::to_string(box.height()) + ", " +
           std::to_string(box.count) + " px";
}

/// Bounding box of the non-black pixels of the rows `y0..y1`.
Box ink_box(const Frame& frame, int y0, int y1) {
    Box box;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (is_black(at(frame, x, y))) {
                continue;
            }
            ++box.count;
            if (box.x0 < 0 || x < box.x0) {
                box.x0 = x;
            }
            if (box.y0 < 0 || y < box.y0) {
                box.y0 = y;
            }
            if (x > box.x1) {
                box.x1 = x;
            }
            if (y > box.y1) {
                box.y1 = y;
            }
        }
    }
    return box;
}

/// Independent blit of one text run into a `width` x `height` band: glyph row
/// `r` at `y + r` (`0x47faf0`), palette index 0 transparent, the atlas palette
/// forced opaque.  0 marks an untouched pixel.
std::vector<std::uint32_t> compose_band(const gm::FontAtlas& atlas, std::string_view text,
                                        int x, int width, int height) {
    std::vector<std::uint32_t> band(static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height),
                                    0u);
    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        const gm::FontGlyph* glyph = atlas.glyph(ch);
        if (glyph == nullptr || glyph->width <= 0) {
            pen += atlas.advance_of(ch);
            continue;
        }
        for (int row = 0; row < atlas.cell_height && row < height; ++row) {
            for (int column = 0; column < glyph->width; ++column) {
                const std::uint8_t index =
                    glyph->pixels[static_cast<std::size_t>(row) * glyph->width + column];
                if (index == 0) {
                    continue;
                }
                const int px = pen + column;
                if (px < 0 || px >= width) {
                    continue;
                }
                const std::size_t base = index * 4u;
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(px)] =
                    (static_cast<std::uint32_t>(atlas.palette[base + 0]) << 24) |
                    (static_cast<std::uint32_t>(atlas.palette[base + 1]) << 16) |
                    (static_cast<std::uint32_t>(atlas.palette[base + 2]) << 8) | 0xFFu;
            }
        }
        pen += atlas.advance_of(ch);
    }
    return band;
}

int pen_width(const gm::FontAtlas& face, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += face.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

long long band_ink(const std::vector<std::uint32_t>& band) {
    long long ink = 0;
    for (const std::uint32_t pixel : band) {
        ink += pixel == 0 ? 0 : 1;
    }
    return ink;
}

/// Compare the rows `y..y+cell_height-1` of the 196-px band of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kLayBucksPanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    long long wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t expected = want == 0 ? gm::kLayBucksBackground : want;
            if (at(frame, column, y + row) != expected) {
                if (wrong == 0) {
                    first_x = column;
                }
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, what + ": band matches the independent blit (first x " +
                           std::to_string(first_x) + ")");
}

void check_refused(const fs::path& game, const gm::LayBucksRenderOptions& options,
                   const std::string& what, std::string_view needle) {
    std::vector<std::uint32_t> rgba(7, 0xDEADBEEFu);
    int width = 11;
    int height = 13;
    gm::LayBucksRenderStats stats;
    stats.rows = 99;
    std::string error;
    const bool ok = gm::render_lay_bucks(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "lay_bucks_render: "),
          what + ": error is prefixed (got \"" + error + "\")");
    check(contains(error, needle), what + ": error names \"" + std::string(needle) +
                                       "\" (got \"" + error + "\")");
    check_eq(static_cast<long long>(rgba.size()), 0, what + ": buffer cleared");
    check_eq(width, 0, what + ": width 0");
    check_eq(height, 0, what + ": height 0");
    check_eq(stats.rows, 0, what + ": stats zeroed");
    check_eq(stats.ink_pixels, 0, what + ": stats ink zeroed");
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";
    check_eq(gm::kLayBucksCanvasWidth, 320, tag + "canvas width 320");
    check_eq(gm::kLayBucksCanvasHeight, 200, tag + "canvas height 200");
    check_eq(gm::kLayBucksPanelWidth, 196, tag + "panel width 196");
    check_eq(gm::kLayBucksPanelHeight, 131, tag + "panel height 131");
    check_eq(gm::kLayBucksRows, 1, tag + "rows 1 (0x67f10c, 0x48a830)");
    check_eq(gm::kLayBucksItemStep, 0, tag + "step 0 (0x662d58, 0x48a83a)");
    check_eq(gm::kLayBucksLineCount, 4, tag + "four 0x485dd0 calls");
    check_eq(gm::kLayBucksTitleFont, 5, tag + "title font 5");
    check_eq(gm::kLayBucksRowFont, 0, tag + "body font 0");

    check_eq(gm::kLayBucksTitleY, 14, tag + "title y 14 (push 0xe)");
    check_eq(gm::kLayBucksTitleX, 38, tag + "title x argument 38 (ignored)");
    check_eq(gm::kLayBucksNoticeX, 42, tag + "notice x 42 (push 0x2a)");
    check_eq(gm::kLayBucksNotice1Y, 27, tag + "notice 1 y 27 (push 0x1b)");
    check_eq(gm::kLayBucksNotice2Y, 36, tag + "notice 2 y 36 (push 0x24)");
    check_eq(gm::kLayBucksNotice3Y, 45, tag + "notice 3 y 45 (push 0x2d)");

    check_eq(std::string(gm::kLayBucksTitle), "LAY DOWN THE BUCKS", tag + "title text");
    check_eq(std::string(gm::kLayBucksNotice1), "THIS FEATURE IS NOT", tag + "notice 1 text");
    check_eq(std::string(gm::kLayBucksNotice2), "AVAILABLE IN THE", tag + "notice 2 text");
    check_eq(std::string(gm::kLayBucksNotice3), "SHAREWARE VERSION", tag + "notice 3 text");

    check_eq(gm::lay_bucks_line_y(0), 14, tag + "line 0 y");
    check_eq(gm::lay_bucks_line_y(1), 27, tag + "line 1 y");
    check_eq(gm::lay_bucks_line_y(2), 36, tag + "line 2 y");
    check_eq(gm::lay_bucks_line_y(3), 45, tag + "line 3 y");
    check_eq(gm::lay_bucks_line_y(-1), 14, tag + "out-of-range line -> title y");
    check_eq(gm::lay_bucks_line_y(9), 14, tag + "out-of-range line -> title y (high)");
    check_eq(gm::lay_bucks_line_font(0), 5, tag + "line 0 font");
    check_eq(gm::lay_bucks_line_font(2), 0, tag + "line 2 font");
    check_eq(gm::lay_bucks_line_font(7), 5, tag + "out-of-range line -> title font");
    check_eq(std::string(gm::lay_bucks_line_text(3)), "SHAREWARE VERSION",
             tag + "line 3 text");
    check_eq(std::string(gm::lay_bucks_line_text(99)), "LAY DOWN THE BUCKS",
             tag + "out-of-range line -> title text");

    gm::LayBucksRenderOptions defaults;
    check_eq(defaults.cash, 4, tag + "default cash index 4 (accepted, ignored)");
    check(!defaults.fade_lut, tag + "fade off by default");
    check_eq(defaults.fade_lut_level, gm::kHudLutIdentityLevel, tag + "fade level 11");

    gm::LayBucksRenderStats zero;
    check_eq(zero.rows, 0, tag + "stats default rows 0");
    check_eq(zero.ink_pixels, 0, tag + "stats default ink 0");
    check_eq(zero.missing_glyphs, 0, tag + "stats default missing 0");
    check_eq(zero.arrows, 0, tag + "stats default arrows 0");
}

// --- shipped install: geometry, ink, cash ------------------------------------
void test_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const Frame frame = run(game, gm::LayBucksRenderOptions{});
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kLayBucksCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kLayBucksCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()),
             static_cast<long long>(gm::kLayBucksCanvasWidth) * gm::kLayBucksCanvasHeight,
             tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kLayBucksRows, tag + "stats rows 1");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW call)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "no missing glyph");
    check(frame.stats.ink_pixels > 0, tag + "frame has ink");

    long long opaque = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque += (pixel & 0xFFu) == 0xFFu ? 1 : 0;
    }
    check_eq(opaque, 64000, tag + "every pixel opaque");

    const auto& title = fonts.at(gm::kLayBucksTitleFont);
    const auto& body = fonts.at(gm::kLayBucksRowFont);

    // Title: centred over the 196-px band, y 14 (`item = -1` -> row is absolute).
    const int title_width = pen_width(title, gm::kLayBucksTitle);
    const int title_x = std::max(0, (gm::kLayBucksPanelWidth - title_width) / 2);
    check(title_x + title_width <= gm::kLayBucksPanelWidth,
          tag + "centred title fits the 196-px band (" + std::to_string(title_x) + "+" +
              std::to_string(title_width) + ")");
    check_band(frame, title, gm::kLayBucksTitle, title_x, gm::kLayBucksTitleY,
               tag + "title");
    // The three notices are left-aligned at x 42.
    check_band(frame, body, gm::kLayBucksNotice1, gm::kLayBucksNoticeX,
               gm::kLayBucksNotice1Y, tag + "notice 1");
    check_band(frame, body, gm::kLayBucksNotice2, gm::kLayBucksNoticeX,
               gm::kLayBucksNotice2Y, tag + "notice 2");
    check_band(frame, body, gm::kLayBucksNotice3, gm::kLayBucksNoticeX,
               gm::kLayBucksNotice3Y, tag + "notice 3");

    // Each band holds only its own rows: nothing bleeds above y 14 or below 45.
    const Box all = ink_box(frame, 0, frame.height - 1);
    check(!all.empty() && all.y0 == gm::kLayBucksTitleY,
          tag + "top-most ink row is the title row 14 (got " +
              std::to_string(all.y0) + ")");
    check(!all.empty() && all.y1 <= gm::kLayBucksNotice3Y + body.cell_height - 1,
          tag + "bottom-most ink belongs to the last notice (got " +
              std::to_string(all.y1) + ")");

    std::vector<Box> boxes;
    const std::array<int, gm::kLayBucksLineCount> first_rows = {
        gm::kLayBucksTitleY, gm::kLayBucksNotice1Y, gm::kLayBucksNotice2Y,
        gm::kLayBucksNotice3Y};
    long long band_ink_sum = 0;
    for (int line = 0; line < gm::kLayBucksLineCount; ++line) {
        const int y = first_rows[static_cast<std::size_t>(line)];
        const int next = line + 1 < gm::kLayBucksLineCount
                             ? first_rows[static_cast<std::size_t>(line + 1)] - 1
                             : y + body.cell_height - 1;
        const Box box = ink_box(frame, y, next);
        boxes.push_back(box);
        check(!box.empty(), tag + "line " + std::to_string(line) + " has ink");
        const int font = gm::lay_bucks_line_font(line);
        const auto& atlas = fonts.at(font);
        const std::string_view text = gm::lay_bucks_line_text(line);
        const int x = line == 0 ? title_x : gm::kLayBucksNoticeX;
        band_ink_sum += band_ink(
            compose_band(atlas, text, x, gm::kLayBucksPanelWidth, frame.height));
        check_eq(box.count, band_ink(
                                compose_band(atlas, text, x, gm::kLayBucksPanelWidth,
                                             frame.height)),
                 tag + "line " + std::to_string(line) + " ink matches the model");
    }
    check_eq(frame.stats.ink_pixels, band_ink_sum,
             tag + "stats ink = the four text bands");
    check_eq(differing_pixels(frame.rgba, frame.rgba), 0, tag + "a frame equals itself");

    // Colour of the faces (docs/MENU_RENDER.md §3): font 5 dark red,
    // font 0 dark grey.
    const std::uint32_t title_colour = 0x9B0F0FFFu; // (155,15,15)
    const std::uint32_t body_colour = 0x130F0FFFu;  // (19,15,15)
    check(has_colour(frame, gm::kLayBucksTitleY,
                     gm::kLayBucksTitleY + title.cell_height - 1, title_colour),
          tag + "title is font 5 dark red (155,15,15)");
    check(has_colour(frame, gm::kLayBucksNotice1Y,
                     gm::kLayBucksNotice3Y + body.cell_height - 1, body_colour),
          tag + "notices are font 0 dark grey (19,15,15)");
    check(!has_colour(frame, 0, frame.height - 1, 0xEFEFEFFFu),
          tag + "no font 4 white (the highlight branch is dead: item = -1)");

    std::printf("  frame: 320x200, rows=%d arrows=%d ink=%d missing=%d; title "
                "%d px wide at x=%d y=%d; lines: %s | %s | %s | %s\n",
                frame.stats.rows, frame.stats.arrows, frame.stats.ink_pixels,
                frame.stats.missing_glyphs, title_width, title_x, gm::kLayBucksTitleY,
                box_text(boxes[0]).c_str(), box_text(boxes[1]).c_str(),
                box_text(boxes[2]).c_str(), box_text(boxes[3]).c_str());

    // No money on this screen: the cash index of screen 27 changes nothing.
    for (const int cash : {0, 4, 11, 99, -7}) {
        gm::LayBucksRenderOptions options;
        options.cash = cash;
        const Frame other = run(game, options);
        check(other.ok, tag + "cash " + std::to_string(cash) + ": " + other.error);
        if (!other.ok) {
            continue;
        }
        check_eq(differing_pixels(frame.rgba, other.rgba), 0,
                 tag + "cash " + std::to_string(cash) + " does not change the frame");
        check_eq(other.stats.ink_pixels, frame.stats.ink_pixels,
                 tag + "cash " + std::to_string(cash) + " ink unchanged");
    }
    std::printf("  cash 0/4/11/99/-7: 0/64000 px differ (0x48a830 reads no 0x4bc6f8)\n");
}

// --- PLATE -------------------------------------------------------------------
struct Decoded {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> palette;
    std::vector<std::uint8_t> indices;

    std::uint32_t colour(int x, int y) const {
        const std::uint8_t index =
            indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)];
        const std::size_t base = static_cast<std::size_t>(index) * 4u;
        return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
               (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
               (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
    }
};

std::optional<Decoded> decode_installed(const fs::path& game, const std::string& name) {
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return std::nullopt;
    }
    const gm::BigEntry* entry = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (candidate.name == name) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return std::nullopt;
    }
    std::optional<gm::BmpImage> image = gm::parse_bmp(*bytes, error);
    if (!image) {
        return std::nullopt;
    }
    Decoded decoded;
    decoded.width = image->width;
    decoded.height = image->height;
    decoded.palette.resize(image->palette.size());
    for (std::size_t i = 0; i + 3 < image->palette.size(); i += 4) {
        decoded.palette[i + 0] = image->palette[i + 0];
        decoded.palette[i + 1] = image->palette[i + 1];
        decoded.palette[i + 2] = image->palette[i + 2];
        decoded.palette[i + 3] = 0xFFu;
    }
    decoded.indices = image->pixels;
    return decoded;
}

void test_plate(const fs::path& game) {
    const std::string tag = "PLATE: ";
    const std::optional<Decoded> plate = decode_installed(game, "PLATE");
    check(plate.has_value() && plate->width == gm::kLayBucksPanelWidth &&
              plate->height == gm::kLayBucksPanelHeight,
          tag + "installed PLATE decodes to 196x131");
    if (!plate) {
        return;
    }
    const Frame black_frame = run(game, gm::LayBucksRenderOptions{});
    gm::LayBucksRenderOptions options;
    options.background = std::string(gm::kLayBucksPanelName);
    const Frame plate_frame = run(game, options);
    check(black_frame.ok && plate_frame.ok, tag + "both frames render");
    if (!black_frame.ok || !plate_frame.ok) {
        return;
    }
    long long wrong = 0;
    for (int y = 0; y < plate->height; ++y) {
        for (int x = 0; x < plate->width; ++x) {
            if (!is_black(at(black_frame, x, y))) {
                continue; // glyph pixels sit on top of the panel
            }
            const std::uint32_t want = plate->colour(x, y);
            const std::uint32_t got = at(plate_frame, x, y);
            wrong += got == want ? 0 : 1;
        }
    }
    check_eq(wrong, 0, tag + "PLATE pixels match the independent decode under the ink");
    long long outside = 0;
    for (int y = 0; y < plate_frame.height; ++y) {
        for (int x = 0; x < plate_frame.width; ++x) {
            if (x < plate->width && y < plate->height) {
                continue;
            }
            outside += is_black(at(plate_frame, x, y)) ? 0 : 1;
        }
    }
    check_eq(outside, 0, tag + "everything outside the 196x131 band stays black");
    check_eq(plate_frame.stats.ink_pixels, black_frame.stats.ink_pixels,
             tag + "text ink unchanged by the backdrop");
    check(!is_black(at(plate_frame, 0, 0)) || plate->colour(0, 0) == gm::kLayBucksBackground,
          tag + "frame is not the black canvas");
    std::printf("  PLATE: 196x131 decoded, black vs PLATE diff=%lld px, text ink %d "
                "unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kLayBucksCanvasWidth) *
                                  gm::kLayBucksCanvasHeight;

    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  fade LUT: SKIP (no 1024-byte GAMEPAL: " << palette_error << ")\n";
        return;
    }
    gm::HudLut lut;
    std::string error;
    if (!gm::build_hud_lut(*palette, lut, &error)) {
        check(false, tag + "build_hud_lut: " + error);
        return;
    }
    std::printf("  fade LUT: 16x256 built in memory from FRONTEND/GAMEPAL, no dump\n");

    const auto make = [&](bool fade, int level) {
        gm::LayBucksRenderOptions options;
        options.background = std::string(gm::kLayBucksPanelName);
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, options);
    };
    const auto outside_panel = [](const Frame& frame) {
        long long count = 0;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                if (x < gm::kLayBucksPanelWidth && y < gm::kLayBucksPanelHeight) {
                    continue;
                }
                count += is_black(at(frame, x, y)) ? 0 : 1;
            }
        }
        return count;
    };

    const Frame plain = make(false, gm::kHudLutIdentityLevel);
    check(plain.ok, tag + "unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    check_eq(static_cast<long long>(plain.rgba.size()), kCanvas, tag + "canvas 64000");

    const Frame identity = make(true, gm::kHudLutIdentityLevel);
    check(identity.ok, tag + "level 11 frame: " + identity.error);
    if (!identity.ok) {
        return;
    }
    check_eq(differing_pixels(identity.rgba, plain.rgba), 0,
             tag + "level 11 is byte-identical to no fade");
    check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels, tag + "level 11 ink unchanged");
    check_eq(outside_panel(identity), 0, tag + "level 11 leaves the canvas black");

    std::vector<Frame> exact(4);
    for (int i = 0; i < 4; ++i) {
        const int level = gm::kHudLutIdentityLevel + 1 + i;
        exact[static_cast<std::size_t>(i)] = make(true, level);
        check(exact[static_cast<std::size_t>(i)].ok,
              tag + "level " + std::to_string(level) + ": " +
                  exact[static_cast<std::size_t>(i)].error);
        if (!exact[static_cast<std::size_t>(i)].ok) {
            return;
        }
        check(differing_pixels(exact[static_cast<std::size_t>(i)].rgba, plain.rgba) > 0,
              tag + "level " + std::to_string(level) + " changes the frame");
        check_eq(exact[static_cast<std::size_t>(i)].stats.ink_pixels, plain.stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const long long pair12_13 = differing_pixels(exact[0].rgba, exact[1].rgba);
    const long long pair13_14 = differing_pixels(exact[1].rgba, exact[2].rgba);
    const long long pair14_15 = differing_pixels(exact[2].rgba, exact[3].rgba);
    check(pair12_13 > 0 && pair13_14 > 0 && pair14_15 > 0,
          tag + "levels 12..15 are pairwise different");
    std::printf("  fade: level 11 vs no fade: 0/64000 px differ; adjacent 12..15 "
                "diffs %lld/%lld/%lld; vs 11: %lld/%lld/%lld/%lld\n",
                pair12_13, pair13_14, pair14_15,
                differing_pixels(exact[0].rgba, identity.rgba),
                differing_pixels(exact[1].rgba, identity.rgba),
                differing_pixels(exact[2].rgba, identity.rgba),
                differing_pixels(exact[3].rgba, identity.rgba));

    // Determinism of the faded frame.
    const Frame again = make(true, 13);
    check(again.ok && differing_pixels(again.rgba, exact[1].rgba) == 0,
          tag + "level 13 is deterministic");
}

// --- synthetic archives ------------------------------------------------------
fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_lay_bucks_render";
    }
    return fs::temp_directory_path() / "gm_lay_bucks_render";
}

bool write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

/// A `BIGFILE` index with `names` packed back to back at offsets 0, size, ...;
/// `dat` gets the bytes in the same order.
bool write_archive(const fs::path& dir, const std::vector<std::string>& names,
                   const std::vector<std::vector<std::uint8_t>>& payloads) {
    std::error_code ec;
    fs::create_directories(dir / "FRONTEND", ec);
    if (ec || names.size() != payloads.size()) {
        return false;
    }
    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                     static_cast<std::uint8_t>(0x1A)};
    put32(idx, static_cast<std::uint32_t>(names.size()));
    std::vector<std::uint8_t> dat;
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string name = names[i];
        name.resize(8, '\0');
        idx.insert(idx.end(), name.begin(), name.end());
        put32(idx, static_cast<std::uint32_t>(dat.size()));
        dat.insert(dat.end(), payloads[i].begin(), payloads[i].end());
    }
    return write_bytes(dir / "FRONTEND" / "BMP.IDX", idx) &&
           write_bytes(dir / "FRONTEND" / "BMP.DAT", dat);
}

/// Raw bytes of one entry of the installed `FRONTEND/BMP.IDX`.
std::optional<std::vector<std::uint8_t>> installed_entry(const fs::path& game,
                                                         const std::string& name) {
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return std::nullopt;
    }
    for (const gm::BigEntry& entry : archive->entries()) {
        if (entry.name == name) {
            return archive->read(entry, error);
        }
    }
    return std::nullopt;
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::LayBucksRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::LayBucksRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> check_bytes =
        installed_entry(game, "CHECK");
    check(fonts4.has_value() && plate.has_value() && check_bytes.has_value(),
          "negatives: FONTS4/PLATE/CHECK bytes available");
    if (!fonts4 || !plate || !check_bytes) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. An index with FONTS4 only: the faces load, PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4"}, {*fonts4}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-plate", gm::LayBucksRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. PLATE only: FONTS4 (faces 5/0) is missing.
    check(write_archive(root / "game-without-font4", {"PLATE"}, {*plate}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::LayBucksRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 3. A PLATE of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-plate", {"FONTS4", "PLATE"},
                        {*fonts4, *check_bytes}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::LayBucksRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 4. FONTS4 + PLATE is the whole screen: it must render.
    check(write_archive(root / "game-minimal", {"FONTS4", "PLATE"}, {*fonts4, *plate}),
          "negatives: minimal archive written");
    {
        const Frame minimal = run(root / "game-minimal", gm::LayBucksRenderOptions{});
        check(minimal.ok, "negatives: minimal archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, 1, "negatives: minimal archive rows 1");
            check_eq(minimal.stats.arrows, 0, "negatives: minimal archive arrows 0");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  minimal archive (FONTS4+PLATE): ink=%d missing=%d rows=%d\n",
                        minimal.stats.ink_pixels, minimal.stats.missing_glyphs,
                        minimal.stats.rows);
        }
    }
    fs::remove_all(root, ec);

    // 5. A background name that is not in the archive is a text, not black.
    gm::LayBucksRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 6. The exact fade needs a table or a dump.
    gm::LayBucksRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::LayBucksRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 7. The shipped install renders the API default and is deterministic.
    const Frame first = run(game, gm::LayBucksRenderOptions{});
    const Frame second = run(game, gm::LayBucksRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  negatives: rows=%d ink=%d arrows=%d missing=%d\n", first.stats.rows,
                    first.stats.ink_pixels, first.stats.arrows,
                    first.stats.missing_glyphs);
    }
}

} // namespace

int main() {
    const char* env = std::getenv("GUNMETAL_DIR");
    std::vector<fs::path> roots;
    if (env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    fs::path game;
    std::error_code ec;
    for (const fs::path& root : roots) {
        if (fs::exists(root / "FRONTEND" / "BMP.IDX", ec)) {
            game = root;
            break;
        }
    }

    test_model();
    if (game.empty()) {
        std::cout << "lay_bucks_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kLayBucksTitleFont, gm::kLayBucksRowFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "lay_bucks_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_frame(game, fonts);
    test_plate(game);
    test_fade_lut(game);
    test_negatives(game);
    if (failures != 0) {
        std::cerr << "lay_bucks_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "lay_bucks_render: ok (" << checks << " checks)\n";
    return 0;
}
