// One frame of screen 17, `SELECT LEVEL FOR NEW GAME` (`M8.4` slice 3,
// `ui/level_select_render`, docs/LEVEL_SELECT_RENDER.md).
//
// `ui/level_select` is the state (36 rows, cursor, unlock flags); this module
// paints it into the 320x200 RGBA canvas with the `PLATE` band and the three
// `FONTS4` faces.  The test pins the proven geometry of `0x48a3c0` from
// pixels, not from the code:
//
//   * counter 1 (7 of the 36 levels unlocked): `rows == 10` (the list is
//     always the whole corpus, every visible row is painted), 320x200 RGBA,
//     no missing glyph, the title centred at y 14, row 0 at (14, 27),
//     `ESC TO CANCEL` at (38, 111) — every band is compared byte for byte with
//     an independent blit built here from `load_font`'s atlases;
//   * the faces: the title uses font 5 (0x9b0f0f + 0x6b473b shadows), a row
//     font 0 (0x130f0f) and the cursor font 4 (0xefefef);
//   * counter 25 (36 rows): `rows == 10`, the scroll rule of
//     `level_select_scroll` (`0x48a3f1`..`0x48a416`) is pinned as numbers and
//     the visible window is verified band by band: selection 19 -> window
//     14..23 (`level15`..`level24`), cursor on band 5; selection 24 -> window
//     19..28; selection 35 -> window 26..35, cursor on band 9;
//   * the cursor changes the picture (number of differing pixels) and only
//     the glyph colours of its row;
//   * the scroll arrows: the `MENUARRW` 16x8 sprite is decoded here from the
//     raw BMP bytes (file order, palette of the entry) and compared pixel by
//     pixel with the frames — at scroll 0 only the down arrow at
//     (160,123)-(173,127), in the middle both (up at (160,3)-(173,7)), at the
//     bottom window (scroll 26) only the up arrow; 41 pixels each, 0 outside
//     the rows except the arrow;
//   * `title=false` / `cancel=false` remove exactly their own pixels (the
//     differing pixels all lie in the title/ESC band and equal its ink);
//   * names (`0x48a4c0`): a synthetic lowercase `LVLNAMES.SLU` in a temp game
//     directory (FRONTEND symlinked, read-only) turns `level1` into
//     `level1 - Title 01` and grows the ink; without the file the frame is the
//     stem frame; the shipped uppercase file matches 0/36 stems because the
//     compare is byte-exact;
//   * `PLATE` as the background: 25676 panel pixels out of the 196x131 band
//     are decoded here independently, the other 38324 stay black, the ink is
//     unchanged and the panel asset of screen 17 is the same `PLATE` that
//     `ui/menu_render` uses;
//   * negatives: an empty list, an empty game path, a game without
//     `FRONTEND/BMP.IDX`, a missing background entry -- all texts with a
//     cleared buffer and zeroed stats; determinism of two renders;
//   * exact fade (`fade_lut`, slice 8): the table is built from `GAMEPAL` in
//     memory (`build_hud_lut`, no file); level 11 is byte-identical to the
//     unfaded `PLATE` frame, level 0 blackens it, 12..15 are four different
//     frames, and the dump (when present) gives the same bytes.
//
// The installation is only read; the panel/synthetic halves skip cleanly when
// it is absent (the empty-list negative always runs).  Numbers are printed and
// docs/LEVEL_SELECT_RENDER.md §4 pins them.

#include <algorithm>
#include <cctype>
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
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "game/campaign_state.h"
#include "render/hud_lut.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "render/text_layout.h"
#include "ui/frontend_menu.h"
#include "ui/level_select.h"
#include "ui/level_select_render.h"
#include "ui/menu_render.h"

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
    gm::LevelSelectRenderStats stats;
};

Frame run(const fs::path& game, const gm::LevelSelect& select,
          const gm::LevelSelectRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_level_select(game, select, options, frame.rgba, frame.width,
                                       frame.height, &frame.stats, &frame.error);
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kLevelSelectBackground; }

/// Pixels of the opaque black canvas: with the `PLATE` background they are the
/// 38 324 pixels outside the band plus the black pixels of the panel itself.
int count_black(const Frame& frame) {
    int black = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        black += is_black(pixel) ? 1 : 0;
    }
    return black;
}

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Bounding box of the non-black pixels of the rows `y0..y1` (inclusive).
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

/// The two `MENUARRW` rectangles of the plate copy (`0x68681f`/`0x68c33b`:
/// column 159, rows 2 and 121, 16x8 each).  Nothing else of the screen writes
/// there: the last row band ends at y 106 and `ESC` at 110.
bool in_arrow_band(int x, int y) {
    const bool column = x >= gm::kLevelSelectArrowX &&
                        x < gm::kLevelSelectArrowX + gm::kLevelSelectArrowWidth;
    const bool up = y >= gm::kLevelSelectArrowUpY &&
                    y < gm::kLevelSelectArrowUpY + gm::kLevelSelectArrowHeight;
    const bool down = y >= gm::kLevelSelectArrowDownY &&
                      y < gm::kLevelSelectArrowDownY + gm::kLevelSelectArrowHeight;
    return column && (up || down);
}

/// Independent blit of one text run into a `width` x `height` band: glyph row
/// `r` at `y + r` (`0x47faf0`, `gm/hud.py::draw_text`), palette index 0
/// transparent, the atlas palette forced opaque.  0 marks an untouched pixel.
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

/// Compare the rows `y..y+height-1` of the band `0..width` of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kLevelSelectPanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    int wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kLevelSelectBackground : want;
            if (got != expected) {
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

std::set<std::uint32_t> ink_colours(const Frame& frame, int y0, int y1) {
    std::set<std::uint32_t> colours;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t pixel = at(frame, x, y);
            if (!is_black(pixel)) {
                colours.insert(pixel);
            }
        }
    }
    return colours;
}

std::string join_colours(const std::set<std::uint32_t>& colours, std::size_t max = 4) {
    std::string out;
    std::size_t shown = 0;
    for (const std::uint32_t colour : colours) {
        if (shown != 0) {
            out += ",";
        }
        char buffer[11];
        std::snprintf(buffer, sizeof(buffer), "%08x", colour);
        out += buffer;
        if (++shown >= max) {
            out += "...";
            break;
        }
    }
    return out;
}

/// The dumped tables: `GM_HUD_LUT`, else `~/.cache/gunmetal-re/hud_lut.bin`
/// (the same lookup as `test_pager_render`/`test_hud_lut`).  The dump is only
/// an optional cross-check of the built table, so a missing file skips one
/// block instead of failing.
fs::path find_hud_lut_dump() {
    if (const char* env = std::getenv("GM_HUD_LUT"); env != nullptr && *env != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".cache" / "gunmetal-re" / "hud_lut.bin";
    }
    return {};
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

int count_lines(std::string_view text) {
    int lines = 0;
    bool content = false;
    for (const char ch : text) {
        if (ch == '\n') {
            lines += content ? 1 : 0;
            content = false;
            continue;
        }
        if (ch != '\r' && ch != ' ' && ch != '\t') {
            content = true;
        }
    }
    if (content) {
        ++lines;
    }
    return lines;
}

/// `LVLNAMES.SLU` of the install (case-insensitive name), like the renderer.
std::string installed_names(const fs::path& game) {
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(game, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string name = entry.path().filename().string();
        for (char& ch : name) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (name == "LVLNAMES.SLU") {
            return read_file(entry.path());
        }
    }
    return {};
}

// --- the scroll rule (0x48a3f1..0x48a416) ----------------------------------
void test_scroll() {
    check_eq(gm::level_select_scroll(0, 0), 0, "scroll: empty list");
    check_eq(gm::level_select_scroll(-3, 5), 0, "scroll: negative count");
    for (int selected = 0; selected < 7; ++selected) {
        check_eq(gm::level_select_scroll(7, selected), 0,
                 "scroll: 7 rows, selected " + std::to_string(selected));
    }
    for (int selected = 0; selected < 10; ++selected) {
        check_eq(gm::level_select_scroll(10, selected), 0,
                 "scroll: 10 rows, selected " + std::to_string(selected));
    }
    check_eq(gm::level_select_scroll(11, 5), 0, "scroll: 11 rows, selected 5");
    check_eq(gm::level_select_scroll(11, 6), 1, "scroll: 11 rows, selected 6");
    check_eq(gm::level_select_scroll(11, 10), 1, "scroll: 11 rows, selected 10");
    check_eq(gm::level_select_scroll(36, 0), 0, "scroll: 36 rows, selected 0");
    check_eq(gm::level_select_scroll(36, 4), 0, "scroll: 36 rows, selected 4");
    check_eq(gm::level_select_scroll(36, 5), 0, "scroll: 36 rows, selected 5");
    check_eq(gm::level_select_scroll(36, 6), 1, "scroll: 36 rows, selected 6");
    check_eq(gm::level_select_scroll(36, 19), 14, "scroll: 36 rows, selected 19");
    check_eq(gm::level_select_scroll(36, 24), 19, "scroll: 36 rows, selected 24");
    check_eq(gm::level_select_scroll(36, 35), 26, "scroll: 36 rows, selected 35");
    check_eq(gm::level_select_scroll(36, -3), 0, "scroll: negative selection");
    check_eq(gm::level_select_scroll(36, 99), 26, "scroll: selection clamps to the list");

    std::cout << "level_select_render scroll: 7 rows -> 0; 36 rows selected 0/5/6/19/24/35 -> "
              << gm::level_select_scroll(36, 0) << "/" << gm::level_select_scroll(36, 5) << "/"
              << gm::level_select_scroll(36, 6) << "/" << gm::level_select_scroll(36, 19)
              << "/" << gm::level_select_scroll(36, 24) << "/"
              << gm::level_select_scroll(36, 35) << "\n";
}

// --- the display text (0x48a4c0) ------------------------------------------
void test_names(const fs::path& game) {
    const std::string text = "LEVEL1 Testing Range\r\nLEVEL10 Another\r\nLEVEL2 Power Station\r\n";
    check_eq(gm::level_select_name("level1", ""), "level1", "name: no file -> stem");
    check_eq(gm::level_select_name("", text), "", "name: empty stem");
    // The original copies up to '\n', so the CR of a CRLF file stays.
    check_eq(gm::level_select_name("LEVEL1", text), "LEVEL1 - Testing Range\r",
             "name: byte-exact match keeps the CR");
    check_eq(gm::level_select_name("level1", text), "level1",
             "name: lowercase stem does not match the uppercase line");
    check_eq(gm::level_select_name("LEVEL2", text), "LEVEL2 - Power Station\r",
             "name: second line");
    check_eq(gm::level_select_name("LEVEL10", text), "LEVEL10 - Another\r",
             "name: the longer stem wins its own line");
    check_eq(gm::level_select_name("LEVEL1x", text), "LEVEL1x", "name: unknown stem");
    // A space is required after the stem (`0x48a515`).
    check_eq(gm::level_select_name("LEVEL1", "LEVEL1X no\r\n"), "LEVEL1",
             "name: a prefix without the space does not match");
    const std::string longer = gm::level_select_name("ABCD", "ABCD 0123456789012345678901234567890\n");
    check_eq(static_cast<long long>(longer.size()), gm::kLevelSelectNameMaxChars,
             "name: 30 characters at most");
    check_eq(longer, "ABCD - 01234567890123456789012", "name: the 30-char prefix");

    const std::string file = installed_names(game);
    check(!file.empty(), "names: LVLNAMES.SLU found in the install");
    check_eq(count_lines(file), 30, "names: 30 lines");
    int matched = 0;
    for (int i = 1; i <= 24; ++i) {
        const std::string stem = "level" + std::to_string(i);
        matched += gm::level_select_name(stem, file) != stem ? 1 : 0;
    }
    check_eq(matched, 0, "names: 0 lowercase stems match the uppercase file");
    check_eq(gm::level_select_name("LEVEL1", file), "LEVEL1 - Testing Range\r",
             "names: uppercase stem finds its title");
    check_eq(gm::level_select_name("TEMPLE", file), "TEMPLE",
             "names: a stem without a line");
    std::cout << "level_select_render names: lines=" << count_lines(file) << " matched="
              << matched << "/24 (lowercase stems vs uppercase file, byte compare), LEVEL1=\""
              << "LEVEL1 - Testing Range<CR>\"\n";
}

// --- counter 1: 7 unlocked of 36 rows, 10 visible -------------------------
void test_counter1(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    gm::CampaignState campaign;
    campaign.load(1);
    gm::LevelSelect select;
    select.build(campaign, nullptr);
    check_eq(static_cast<long long>(select.levels().size()), 36,
             "counter 1: the list is the whole 36-level corpus");
    int unlocked = 0;
    for (const gm::LevelEntry& entry : select.levels()) {
        unlocked += entry.unlocked ? 1 : 0;
    }
    check_eq(unlocked, 7, "counter 1: 7 levels unlocked");

    const gm::LevelSelectRenderOptions options;
    const Frame frame = run(game, select, options);
    check(frame.ok, "counter 1: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check(frame.error.empty(), "counter 1: no error on success");
    check_eq(frame.width, 320, "counter 1: width");
    check_eq(frame.height, 200, "counter 1: height");
    check_eq(static_cast<long long>(frame.rgba.size()), 320 * 200, "counter 1: buffer size");
    check_eq(frame.stats.rows, 10, "counter 1: stats.rows");
    check_eq(frame.stats.selected_row, 0, "counter 1: stats.selected_row");
    check(frame.stats.ink_pixels > 0, "counter 1: ink > 0");
    check_eq(frame.stats.missing_glyphs, 0, "counter 1: no missing glyphs");
    int opaque = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque += (pixel & 0xFFu) == 0xFFu ? 1 : 0;
    }
    check_eq(opaque, 64000, "counter 1: every pixel is opaque");

    const gm::FontAtlas& title_font = fonts.at(gm::kLevelSelectTitleFont);
    const gm::FontAtlas& row_font = fonts.at(gm::kLevelSelectRowFont);
    const gm::FontAtlas& selected_font = fonts.at(gm::kLevelSelectSelectedFont);

    // Title: centred in the 196 px band at y 14.
    const int title_x =
        (gm::kLevelSelectPanelWidth - title_font.measure(gm::kLevelSelectTitle)) / 2;
    const Box title = ink_box(frame, gm::kLevelSelectTitleY,
                              gm::kLevelSelectTitleY + title_font.cell_height - 1);
    check_eq(title.x0, title_x, "counter 1: title x");
    // The last advance carries the +1 tracking, so the ink is one pixel
    // narrower than the measured width.
    check_eq(title.width(), title_font.measure(gm::kLevelSelectTitle) - 1,
             "counter 1: title width");
    check_eq(title.y0, gm::kLevelSelectTitleY, "counter 1: title y");
    check_band(frame, title_font, gm::kLevelSelectTitle, title_x, gm::kLevelSelectTitleY,
               "counter 1: title");

    // Row 0 is the cursor: the stem `level1` at (14, 27) in font 4.
    const Box first = ink_box(frame, gm::kLevelSelectFirstRowY,
                              gm::kLevelSelectFirstRowY + row_font.cell_height - 1);
    check_eq(first.x0, gm::kLevelSelectRowX, "counter 1: row 0 x");
    check_eq(first.width(), row_font.measure("level1") - 1, "counter 1: row 0 width");
    check_eq(first.y0, gm::kLevelSelectFirstRowY, "counter 1: row 0 y");
    check_band(frame, selected_font, "level1", gm::kLevelSelectRowX,
               gm::kLevelSelectFirstRowY, "counter 1: row 0");

    // ESC TO CANCEL at (38, 111), font 0, not centred.
    const Box cancel = ink_box(frame, gm::kLevelSelectCancelY,
                               gm::kLevelSelectCancelY + row_font.cell_height - 1);
    check_eq(cancel.x0, gm::kLevelSelectCancelX, "counter 1: ESC x");
    check_eq(cancel.width(), row_font.measure(gm::kLevelSelectCancel) - 1,
             "counter 1: ESC width");
    check_eq(cancel.y0, gm::kLevelSelectCancelY, "counter 1: ESC y");
    check_band(frame, row_font, gm::kLevelSelectCancel, gm::kLevelSelectCancelX,
               gm::kLevelSelectCancelY, "counter 1: ESC");

    // The ink is exactly the ink of the eleven bands (title + 10 rows + ESC).
    long long band_ink = title.count + cancel.count;
    for (int i = 0; i < 10; ++i) {
        const int y = gm::kLevelSelectFirstRowY + gm::kLevelSelectRowStep * i;
        band_ink += ink_box(frame, y, y + gm::kLevelSelectRowStep - 1).count;
    }
    check_eq(frame.stats.ink_pixels, band_ink, "counter 1: ink is the sum of the bands");

    // The three faces: font 5 dark red title, font 0 dark grey row, font 4
    // white cursor (colours of `FONTS4`, docs/MENU_RENDER.md §3).
    const std::set<std::uint32_t> title_colours =
        ink_colours(frame, gm::kLevelSelectTitleY,
                    gm::kLevelSelectTitleY + title_font.cell_height - 1);
    check(title_colours.count(0x9B0F0FFFu) != 0, "counter 1: title uses the font-5 red");
    check(title_colours.count(0x130F0FFFu) == 0, "counter 1: title is not font 0");
    const std::set<std::uint32_t> row_colours =
        ink_colours(frame, gm::kLevelSelectFirstRowY,
                    gm::kLevelSelectFirstRowY + row_font.cell_height - 1);
    check(row_colours.count(0xEFEFEFFFu) != 0, "counter 1: the cursor row uses font 4 (white)");
    check(row_colours.count(0x130F0FFFu) == 0, "counter 1: the cursor row is not font 0");

    // Determinism.
    const Frame again = run(game, select, options);
    check(again.ok, "counter 1: second render");
    check_eq(differing_pixels(frame.rgba, again.rgba), 0, "counter 1: deterministic");

    // The unlock state is a model of `ui/level_select`; the original screen
    // has no locks, so it paints no marker: counter 1 and counter 25 give the
    // same first 10 rows and the same picture.
    gm::CampaignState all;
    all.load(25);
    gm::LevelSelect everything;
    everything.build(all, nullptr);
    const Frame all_frame = run(game, everything, options);
    check(all_frame.ok, "counter 1: the counter-25 frame renders");
    if (all_frame.ok) {
        check_eq(all_frame.stats.rows, 10, "counter 1: the counter-25 frame has 10 rows");
        check_eq(differing_pixels(frame.rgba, all_frame.rgba), 0,
                 "counter 1: the unlock state does not change the picture");
    }

    std::cout << "level_select_render counter=1: unlocked=" << unlocked
              << "/36, 320x200 rows=" << frame.stats.rows << " selected=0 ink="
              << frame.stats.ink_pixels << " missing=0; title=(" << title.x0 << ","
              << title.y0 << ") w=" << title.width() << " first=(" << first.x0 << ","
              << first.y0 << ") w=" << first.width() << " esc=(" << cancel.x0 << ","
              << cancel.y0 << ") w=" << cancel.width() << "; colours title=["
              << join_colours(title_colours) << "] row=[" << join_colours(row_colours)
              << "]\n";
}

// --- PLATE as the background ----------------------------------------------
void test_plate(const fs::path& game, const gm::LevelSelect& select) {
    const Frame black = run(game, select, gm::LevelSelectRenderOptions{});
    if (!black.ok) {
        check(false, "plate: the black frame renders");
        return;
    }
    gm::LevelSelectRenderOptions options;
    options.background = std::string(gm::kLevelSelectPanelName);
    const Frame frame = run(game, select, options);
    check(frame.ok, "plate: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels, black.stats.ink_pixels,
             "plate: the text ink does not change");
    check_eq(frame.stats.rows, black.stats.rows, "plate: rows do not change");

    // Independent decode of the panel.
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    check(archive.has_value(), "plate: BMP.IDX opens: " + error);
    if (!archive) {
        return;
    }
    const gm::BigEntry* plate_entry = nullptr;
    for (const gm::BigEntry& entry : archive->entries()) {
        if (entry.name == "PLATE") {
            plate_entry = &entry;
            break;
        }
    }
    check(plate_entry != nullptr, "plate: PLATE is in BMP.IDX");
    if (plate_entry == nullptr) {
        return;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*plate_entry, error);
    check(bytes.has_value(), "plate: PLATE reads: " + error);
    if (!bytes) {
        return;
    }
    std::optional<gm::BmpImage> image = gm::parse_bmp(*bytes, error);
    check(image.has_value(), "plate: PLATE parses: " + error);
    if (!image) {
        return;
    }
    check_eq(image->width, gm::kLevelSelectPanelWidth, "plate: 196 wide");
    check_eq(image->height, gm::kLevelSelectPanelHeight, "plate: 131 high");

    int panel_pixels = 0;
    int ink_pixels = 0;
    int wrong = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t got = at(frame, x, y);
            if (x >= image->width || y >= image->height) {
                ++panel_pixels;
                wrong += is_black(got) ? 0 : 1;
                continue;
            }
            if (!is_black(at(black, x, y))) {
                ++ink_pixels;
                wrong += got == at(black, x, y) ? 0 : 1;
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(image->width) +
                static_cast<std::size_t>(x);
            const std::uint8_t palette_index = image->pixels[index];
            const std::size_t base = static_cast<std::size_t>(palette_index) * 4u;
            const std::uint32_t want =
                (static_cast<std::uint32_t>(image->palette[base + 0]) << 24) |
                (static_cast<std::uint32_t>(image->palette[base + 1]) << 16) |
                (static_cast<std::uint32_t>(image->palette[base + 2]) << 8) | 0xFFu;
            ++ink_pixels;
            wrong += got == want ? 0 : 1;
        }
    }
    check_eq(wrong, 0, "plate: every pixel is the panel or the unchanged text");
    check_eq(panel_pixels, 320 * 200 - 196 * 131, "plate: pixels outside the band stay black");
    check_eq(ink_pixels, 196 * 131, "plate: the band is covered");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::LevelSelect)),
             std::string(gm::kLevelSelectPanelName), "plate: screen 17's panel is PLATE");
    std::cout << "level_select_render plate: 196x131=" << 196 * 131
              << " panel, outside=" << 320 * 200 - 196 * 131 << " black, ink="
              << frame.stats.ink_pixels << " (unchanged), mismatches=0\n";
}

// --- counter 25: 10 visible rows of 36 ------------------------------------
void test_window(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    gm::CampaignState campaign;
    campaign.load(25);
    gm::LevelSelect select;
    select.build(campaign, nullptr);
    const std::vector<gm::LevelEntry>& levels = select.levels();
    check_eq(static_cast<long long>(levels.size()), 36, "counter 25: 36 rows");
    const std::string names = installed_names(game);
    const gm::FontAtlas& row_font = fonts.at(gm::kLevelSelectRowFont);
    const gm::FontAtlas& selected_font = fonts.at(gm::kLevelSelectSelectedFont);

    const Frame first = run(game, select, gm::LevelSelectRenderOptions{});
    check(first.ok, "counter 25: renders: " + first.error);
    if (!first.ok) {
        return;
    }
    check_eq(first.stats.rows, 10, "counter 25: 10 visible rows");
    check_eq(first.stats.selected_row, 0, "counter 25: cursor on 0");
    for (int i = 0; i < 10; ++i) {
        const std::string text = gm::level_select_name(levels[static_cast<std::size_t>(i)].name,
                                                       names);
        check_band(first, i == 0 ? selected_font : row_font, text, gm::kLevelSelectRowX,
                   gm::kLevelSelectFirstRowY + gm::kLevelSelectRowStep * i,
                   "counter 25: window row " + std::to_string(i));
    }

    // Selection 19 (level20): the window starts at 14 (`level15`..`level24`),
    // the cursor sits on visible row 5.
    check(select.select(levels[19].name), "counter 25: select level20");
    check_eq(select.selected(), 19, "counter 25: cursor 19");
    const int scroll19 = gm::level_select_scroll(36, 19);
    check_eq(scroll19, 14, "counter 25: scroll for 19");
    const Frame second = run(game, select, gm::LevelSelectRenderOptions{});
    check(second.ok, "counter 25: frame at 19: " + second.error);
    if (!second.ok) {
        return;
    }
    check_eq(second.stats.rows, 10, "counter 25: rows at 19");
    check_eq(second.stats.selected_row, 19, "counter 25: selected_row at 19");
    check_eq(levels[14].name, "level15", "counter 25: first visible is level15");
    check_eq(levels[23].name, "level24", "counter 25: last visible is level24");
    for (int i = 0; i < 10; ++i) {
        const int index = scroll19 + i;
        const std::string text =
            gm::level_select_name(levels[static_cast<std::size_t>(index)].name, names);
        check_band(second, index == 19 ? selected_font : row_font, text,
                   gm::kLevelSelectRowX, gm::kLevelSelectFirstRowY + gm::kLevelSelectRowStep * i,
                   "counter 25: window row " + std::to_string(i) + " at 19");
    }
    const long long moved = differing_pixels(first.rgba, second.rgba);
    check(moved > 0, "counter 25: the window moved");
    // Only the level bands and the scroll arrows may differ (the title and ESC
    // are the same; the up arrow appears on the scroll-14 frame).
    int outside = 0;
    for (int y = 0; y < second.height; ++y) {
        for (int x = 0; x < second.width; ++x) {
            if (at(first, x, y) == at(second, x, y)) {
                continue;
            }
            const bool level_band =
                y >= gm::kLevelSelectFirstRowY &&
                y < gm::kLevelSelectFirstRowY +
                        gm::kLevelSelectRowStep * gm::LevelSelect::kVisibleRows;
            if (!level_band && !in_arrow_band(x, y)) {
                ++outside;
            }
        }
    }
    check_eq(outside, 0, "counter 25: only the level bands and arrows differ");

    // Selection 24 -> window 19..28, cursor on row 5; 35 -> window 26..35,
    // cursor on row 9.
    check(select.select(levels[24].name), "counter 25: select level25");
    const Frame third = run(game, select, gm::LevelSelectRenderOptions{});
    check(third.ok, "counter 25: frame at 24");
    check_eq(gm::level_select_scroll(36, 24), 19, "counter 25: scroll for 24");
    check_eq(third.stats.selected_row, 24, "counter 25: selected_row at 24");
    if (third.ok) {
        check_band(third, selected_font,
                   gm::level_select_name(levels[24].name, names), gm::kLevelSelectRowX,
                   gm::kLevelSelectFirstRowY + gm::kLevelSelectRowStep * 5,
                   "counter 25: cursor band at 24");
    }
    check(select.select(levels[35].name), "counter 25: select dm4");
    const Frame fourth = run(game, select, gm::LevelSelectRenderOptions{});
    check(fourth.ok, "counter 25: frame at 35");
    check_eq(gm::level_select_scroll(36, 35), 26, "counter 25: scroll for 35");
    check_eq(fourth.stats.selected_row, 35, "counter 25: selected_row at 35");
    if (fourth.ok) {
        check_band(fourth, selected_font,
                   gm::level_select_name(levels[35].name, names), gm::kLevelSelectRowX,
                   gm::kLevelSelectFirstRowY + gm::kLevelSelectRowStep * 9,
                   "counter 25: cursor band at 35");
    }

    std::cout << "level_select_render counter=25: rows=10 selected 19 -> window 14..23 ("
              << levels[14].name << ".." << levels[23].name << ") cursor band 5; "
              << "selected 24 -> window 19..28; selected 35 -> window 26..35 cursor band 9; "
              << "differing pixels 0->19: " << moved << "\n";
}

// --- the scroll arrows (0x48a418..0x48a44d, MENUARRW) ---------------------
/// `MENUARRW` as the original blitter sees it: the 16x8 index bytes in *file*
/// order (`0x47efe0` reads `width*height` bytes at `0x47f100` without a flip)
/// plus the palette of the same entry.
struct ArrowAsset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rows;
    std::vector<std::uint32_t> palette;
};

ArrowAsset load_arrow_asset(const fs::path& game) {
    ArrowAsset asset;
    std::string error;
    std::optional<gm::BigFile> archive = gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return asset;
    }
    const gm::BigEntry* entry = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (candidate.name == "MENUARRW") {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return asset;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return asset;
    }
    std::optional<gm::BmpImage> image = gm::parse_bmp(*bytes, error);
    if (!image || image->bpp != 8 || image->palette.size() < 4) {
        return asset;
    }
    const std::size_t image_size = static_cast<std::size_t>(image->width) *
                                   static_cast<std::size_t>(image->height);
    if (bytes->size() < 54 || image_size == 0) {
        return asset;
    }
    const std::size_t offset = static_cast<std::size_t>((*bytes)[10]) |
                               (static_cast<std::size_t>((*bytes)[11]) << 8) |
                               (static_cast<std::size_t>((*bytes)[12]) << 16) |
                               (static_cast<std::size_t>((*bytes)[13]) << 24);
    if (offset + image_size > bytes->size()) {
        return asset;
    }
    asset.width = image->width;
    asset.height = image->height;
    asset.rows.assign(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                      bytes->begin() + static_cast<std::ptrdiff_t>(offset + image_size));
    asset.palette.resize(image->palette.size() / 4u);
    for (std::size_t i = 0; i < asset.palette.size(); ++i) {
        const std::size_t base = i * 4u;
        asset.palette[i] = (static_cast<std::uint32_t>(image->palette[base + 0]) << 24) |
                           (static_cast<std::uint32_t>(image->palette[base + 1]) << 16) |
                           (static_cast<std::uint32_t>(image->palette[base + 2]) << 8) | 0xFFu;
    }
    asset.ok = true;
    return asset;
}

/// Compare the 16x8 rectangle at (`x`, `y`) with the sprite: `0x485cc0` reads
/// the stored rows forward, `0x485d00` the same bytes backwards (180 degrees);
/// index 0 leaves the black canvas alone.  Returns the number of drawn pixels.
int check_arrow_rect(const Frame& frame, const ArrowAsset& asset, bool up, int x, int y,
                     const std::string& what) {
    int wrong = 0;
    int drawn = 0;
    for (int row = 0; row < asset.height; ++row) {
        for (int column = 0; column < asset.width; ++column) {
            const int source_row = up ? asset.height - 1 - row : row;
            const int source_column = up ? asset.width - 1 - column : column;
            const std::uint8_t index =
                asset.rows[static_cast<std::size_t>(source_row) *
                               static_cast<std::size_t>(asset.width) +
                           static_cast<std::size_t>(source_column)];
            std::uint32_t want = gm::kLevelSelectBackground;
            if (index != 0) {
                want = asset.palette[index];
                ++drawn;
            }
            if (at(frame, x + column, y + row) != want) {
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, what + ": rect matches the independent sprite decode");
    return drawn;
}

/// Non-black pixels of the whole canvas.
long long canvas_ink(const Frame& frame) {
    long long count = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        count += is_black(pixel) ? 0 : 1;
    }
    return count;
}

/// Number of differing pixels outside the ten level bands.
int differences_outside_rows(const Frame& a, const Frame& b) {
    int outside = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (at(a, x, y) == at(b, x, y)) {
                continue;
            }
            const bool rows = y >= gm::kLevelSelectFirstRowY &&
                              y < gm::kLevelSelectFirstRowY +
                                      gm::kLevelSelectRowStep * gm::LevelSelect::kVisibleRows;
            if (!rows) {
                ++outside;
            }
        }
    }
    return outside;
}

void test_arrows(const fs::path& game) {
    const ArrowAsset asset = load_arrow_asset(game);
    check(asset.ok, "arrows: MENUARRW decodes from FRONTEND/BMP.IDX");
    if (!asset.ok) {
        return;
    }
    check_eq(asset.width, gm::kLevelSelectArrowWidth, "arrows: MENUARRW is 16 wide");
    check_eq(asset.height, gm::kLevelSelectArrowHeight, "arrows: MENUARRW is 8 high");
    int sprite_pixels = 0;
    for (const std::uint8_t index : asset.rows) {
        sprite_pixels += index != 0 ? 1 : 0;
    }
    check_eq(sprite_pixels, 41, "arrows: 41 non-zero sprite bytes");

    gm::CampaignState campaign;
    campaign.load(25);
    gm::LevelSelect select;
    select.build(campaign, nullptr);

    // Cursor 0: scroll 0, so only the down arrow (`0x48a438`: 0 + 10 < 36).
    const Frame zero = run(game, select, gm::LevelSelectRenderOptions{});
    check(zero.ok, "arrows at scroll 0: renders: " + zero.error);
    if (!zero.ok) {
        return;
    }
    check_eq(zero.stats.arrows, 1, "arrows at scroll 0: one arrow");
    const Box up_zero = ink_box(zero, gm::kLevelSelectArrowUpY,
                                gm::kLevelSelectArrowUpY + gm::kLevelSelectArrowHeight - 1);
    check(up_zero.empty(), "arrows at scroll 0: the up arrow is absent");
    const Box down_zero = ink_box(zero, gm::kLevelSelectArrowDownY,
                                 gm::kLevelSelectArrowDownY + gm::kLevelSelectArrowHeight - 1);
    check_eq(down_zero.count, 41, "arrows at scroll 0: 41 down-arrow pixels");
    check_eq(down_zero.x0, 160, "arrows: down arrow bbox x0");
    check_eq(down_zero.y0, 123, "arrows: down arrow bbox y0");
    check_eq(down_zero.x1, 173, "arrows: down arrow bbox x1");
    check_eq(down_zero.y1, 127, "arrows: down arrow bbox y1");
    check_eq(check_arrow_rect(zero, asset, false, gm::kLevelSelectArrowX,
                              gm::kLevelSelectArrowDownY, "arrows at scroll 0: down"),
             41, "arrows at scroll 0: the down rect has 41 pixels");

    // Cursor 19: scroll 14, both arrows (`14 > 0`, `14 + 10 < 36`).
    check(select.select(select.levels()[19].name), "arrows: select level20");
    const Frame middle = run(game, select, gm::LevelSelectRenderOptions{});
    check(middle.ok, "arrows in the middle: renders: " + middle.error);
    if (!middle.ok) {
        return;
    }
    check_eq(middle.stats.arrows, 2, "arrows in the middle: two arrows");
    const Box up_middle = ink_box(middle, gm::kLevelSelectArrowUpY,
                                  gm::kLevelSelectArrowUpY + gm::kLevelSelectArrowHeight - 1);
    check_eq(up_middle.count, 41, "arrows in the middle: 41 up-arrow pixels");
    check_eq(up_middle.x0, 160, "arrows: up arrow bbox x0");
    check_eq(up_middle.y0, 3, "arrows: up arrow bbox y0");
    check_eq(up_middle.x1, 173, "arrows: up arrow bbox x1");
    check_eq(up_middle.y1, 7, "arrows: up arrow bbox y1");
    check_eq(check_arrow_rect(middle, asset, true, gm::kLevelSelectArrowX,
                              gm::kLevelSelectArrowUpY, "arrows in the middle: up"),
             41, "arrows in the middle: the up rect has 41 pixels");
    check_eq(check_arrow_rect(middle, asset, false, gm::kLevelSelectArrowX,
                              gm::kLevelSelectArrowDownY, "arrows in the middle: down"),
             41, "arrows in the middle: the down rect has 41 pixels");
    check_eq(differences_outside_rows(zero, middle), 41,
             "arrows: the up arrow is the only change outside the rows");

    // Cursor 35: scroll 26, `26 + 10 == 36` -> the down arrow is gone.
    check(select.select(select.levels()[35].name), "arrows: select dm4");
    const Frame bottom = run(game, select, gm::LevelSelectRenderOptions{});
    check(bottom.ok, "arrows at the bottom: renders: " + bottom.error);
    if (!bottom.ok) {
        return;
    }
    check_eq(bottom.stats.arrows, 1, "arrows at the bottom: one arrow");
    check(ink_box(bottom, gm::kLevelSelectArrowDownY,
                  gm::kLevelSelectArrowDownY + gm::kLevelSelectArrowHeight - 1)
              .empty(),
          "arrows at the bottom: the down arrow is absent");
    check_eq(ink_box(bottom, gm::kLevelSelectArrowUpY,
                     gm::kLevelSelectArrowUpY + gm::kLevelSelectArrowHeight - 1)
                 .count,
             41, "arrows at the bottom: 41 up-arrow pixels");
    check_eq(check_arrow_rect(bottom, asset, true, gm::kLevelSelectArrowX,
                              gm::kLevelSelectArrowUpY, "arrows at the bottom: up"),
             41, "arrows at the bottom: the up rect has 41 pixels");

    std::cout << "level_select_render arrows: MENUARRW 16x8, 41 px per arrow; scroll 0 -> "
                 "down only bbox ("
              << down_zero.x0 << "," << down_zero.y0 << ")-(" << down_zero.x1 << ","
              << down_zero.y1 << "), ink " << canvas_ink(zero) << "; scroll 14 -> both, up "
                 "bbox ("
              << up_middle.x0 << "," << up_middle.y0 << ")-(" << up_middle.x1 << ","
              << up_middle.y1 << "), ink " << canvas_ink(middle) << ", differing outside "
                 "rows "
              << differences_outside_rows(zero, middle) << "; scroll 26 -> up only, ink "
              << canvas_ink(bottom) << "\n";
}

// --- title/cancel switches and the cursor highlight -----------------------
void test_switches(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    gm::CampaignState campaign;
    campaign.load(25);
    gm::LevelSelect select;
    select.build(campaign, nullptr);
    const gm::FontAtlas& row_font = fonts.at(gm::kLevelSelectRowFont);
    const gm::FontAtlas& title_font = fonts.at(gm::kLevelSelectTitleFont);

    const Frame full = run(game, select, gm::LevelSelectRenderOptions{});
    gm::LevelSelectRenderOptions no_title;
    no_title.title = false;
    const Frame without_title = run(game, select, no_title);
    gm::LevelSelectRenderOptions no_cancel;
    no_cancel.cancel = false;
    const Frame without_cancel = run(game, select, no_cancel);
    gm::LevelSelectRenderOptions bare;
    bare.title = false;
    bare.cancel = false;
    const Frame bare_frame = run(game, select, bare);
    check(full.ok && without_title.ok && without_cancel.ok && bare_frame.ok,
          "switches: all four frames render");
    if (!full.ok || !without_title.ok || !without_cancel.ok || !bare_frame.ok) {
        return;
    }

    const Box title_band = ink_box(full, gm::kLevelSelectTitleY,
                                  gm::kLevelSelectTitleY + title_font.cell_height - 1);
    const Box cancel_band = ink_box(full, gm::kLevelSelectCancelY,
                                    gm::kLevelSelectCancelY + row_font.cell_height - 1);
    check_eq(full.stats.ink_pixels - without_title.stats.ink_pixels, title_band.count,
             "switches: title=false removes the title ink");
    check_eq(full.stats.ink_pixels - without_cancel.stats.ink_pixels, cancel_band.count,
             "switches: cancel=false removes the ESC ink");
    check_eq(bare_frame.stats.ink_pixels,
             full.stats.ink_pixels - title_band.count - cancel_band.count,
             "switches: both switches remove exactly their ink");
    check(ink_box(without_title, gm::kLevelSelectTitleY,
                  gm::kLevelSelectTitleY + title_font.cell_height - 1)
              .empty(),
          "switches: no title pixels when title=false");
    check(ink_box(without_cancel, gm::kLevelSelectCancelY,
                  gm::kLevelSelectCancelY + row_font.cell_height - 1)
              .empty(),
          "switches: no ESC pixels when cancel=false");
    // The remaining pixels are identical (the switch touches nothing else).
    int outside_title = 0;
    for (int y = 0; y < full.height; ++y) {
        for (int x = 0; x < full.width; ++x) {
            if (at(full, x, y) == at(without_title, x, y)) {
                continue;
            }
            const bool band = y >= gm::kLevelSelectTitleY &&
                              y < gm::kLevelSelectTitleY + title_font.cell_height;
            if (!band) {
                ++outside_title;
            }
        }
    }
    check_eq(outside_title, 0, "switches: title=false touches only the title band");

    // Cursor: selection 0 -> 1 changes the glyph colours of exactly the two
    // rows involved (font 0 dark grey <-> font 4 white).
    check(select.select("level2"), "switches: select level2");
    const Frame second = run(game, select, gm::LevelSelectRenderOptions{});
    check(second.ok, "switches: cursor frame renders");
    if (!second.ok) {
        return;
    }
    const long long moved = differing_pixels(full.rgba, second.rgba);
    check(moved > 0, "switches: the cursor changes the picture");
    int outside_rows = 0;
    for (int y = 0; y < full.height; ++y) {
        for (int x = 0; x < full.width; ++x) {
            if (at(full, x, y) == at(second, x, y)) {
                continue;
            }
            const bool rows = y >= gm::kLevelSelectFirstRowY &&
                              y < gm::kLevelSelectFirstRowY + 2 * gm::kLevelSelectRowStep;
            if (!rows) {
                ++outside_rows;
            }
        }
    }
    check_eq(outside_rows, 0, "switches: the cursor touches only rows 0 and 1");
    const std::set<std::uint32_t> selected_colours =
        ink_colours(second, gm::kLevelSelectFirstRowY + gm::kLevelSelectRowStep,
                    gm::kLevelSelectFirstRowY + 2 * gm::kLevelSelectRowStep - 1);
    check(selected_colours.count(0xEFEFEFFFu) != 0, "switches: the cursor row is white");

    std::cout << "level_select_render switches: ink=" << full.stats.ink_pixels
              << " title=" << title_band.count << " esc=" << cancel_band.count
              << " bare=" << bare_frame.stats.ink_pixels << "; cursor move 0->1: "
              << moved << " differing pixels (rows 0/1 only)\n";
}

// --- exact fade of `0x682f90` at the palette-index level -------------------
//
// The renderer paints palette indices from three sources (`PLATE`, `MENUARRW`,
// `FONTS4` atlases); this block checks the wiring, not a new geometry:
// the table is **built** from the installed `GAMEPAL` (no dump needed), level
// 11 must reproduce the unfaded frame byte for byte, level 0 must blacken it,
// and 12..15 must be four different frames (the RGBA model of `pager_render`
// would alias all four to 11).  The dump, when present, is a second path to the
// same bytes.
void check_refused(const fs::path& game, const gm::LevelSelect& select,
                   const gm::LevelSelectRenderOptions& options, const std::string& what,
                   std::string_view expected);

void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    gm::CampaignState campaign;
    campaign.load(1);
    gm::LevelSelect select;
    select.build(campaign, nullptr);
    check_eq(static_cast<long long>(select.levels().size()), 36,
             tag + "counter 1 state (36 rows)");

    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    check(palette.has_value(), tag + "GAMEPAL reads: " + palette_error);
    if (!palette) {
        return;
    }
    check_eq(static_cast<long long>(palette->size()),
             static_cast<long long>(gm::kHudLutPaletteBytes),
             tag + "GAMEPAL is 256 RGBA quads");
    if (palette->size() != gm::kHudLutPaletteBytes) {
        return;
    }
    gm::HudLut lut;
    std::string error;
    check(gm::build_hud_lut(*palette, lut, &error), tag + "table built from GAMEPAL: " + error);

    const auto run_lut = [&](int level, Frame& frame) {
        gm::LevelSelectRenderOptions options;
        options.background = std::string(gm::kLevelSelectPanelName);
        options.fade_lut = true;
        options.fade_lut_level = level;
        options.fade_lut_table = &lut;
        frame = run(game, select, options);
        check(frame.ok, tag + "level " + std::to_string(level) + " renders: " + frame.error);
        return frame.ok;
    };

    gm::LevelSelectRenderOptions plain_options;
    plain_options.background = std::string(gm::kLevelSelectPanelName);
    const Frame plain = run(game, select, plain_options);
    check(plain.ok, tag + "unfaded PLATE frame renders: " + plain.error);
    if (!plain.ok) {
        return;
    }

    // 1. Level 11 is the identity table: 0 differing pixels.
    Frame exact11;
    if (!run_lut(gm::kHudLutIdentityLevel, exact11)) {
        return;
    }
    const long long identity_diffs = differing_pixels(exact11.rgba, plain.rgba);
    check_eq(identity_diffs, 0, tag + "level 11 is byte-identical to no fade");
    check_eq(exact11.stats.ink_pixels, plain.stats.ink_pixels, tag + "level 11 ink unchanged");
    check_eq(exact11.stats.rows, plain.stats.rows, tag + "level 11 rows unchanged");
    check_eq(exact11.stats.arrows, plain.stats.arrows, tag + "level 11 arrows unchanged");

    // 2. Level 0 blackens the frame and differs from the unfaded one.
    Frame exact0;
    if (!run_lut(0, exact0)) {
        return;
    }
    const int black_plain = count_black(plain);
    const int black0 = count_black(exact0);
    const long long diff0 = differing_pixels(exact0.rgba, plain.rgba);
    check(diff0 > 0, tag + "level 0 differs from the unfaded frame");
    check(black0 > black_plain, tag + "level 0 adds black pixels");
    // The 20 skip slots of `0x483250` are copied unchanged; on this palette 19
    // of them are not black, so an exact level-0 frame is 63 981 black, not
    // 64 000 (the RGBA model would blacken everything).
    check_eq(64000 - black0, 19, tag + "level 0 keeps the 19 non-black skip slots");

    // 3. Levels 12..15: four different frames, none of them the identity.
    std::vector<Frame> exact(4);
    for (int i = 0; i < 4; ++i) {
        if (!run_lut(gm::kHudLutIdentityLevel + 1 + i, exact[static_cast<std::size_t>(i)])) {
            return;
        }
    }
    const long long pair12_13 = differing_pixels(exact[0].rgba, exact[1].rgba);
    const long long pair13_14 = differing_pixels(exact[1].rgba, exact[2].rgba);
    const long long pair14_15 = differing_pixels(exact[2].rgba, exact[3].rgba);
    const long long pair12_14 = differing_pixels(exact[0].rgba, exact[2].rgba);
    const long long pair12_15 = differing_pixels(exact[0].rgba, exact[3].rgba);
    const long long pair13_15 = differing_pixels(exact[1].rgba, exact[3].rgba);
    const long long to11_12 = differing_pixels(exact[0].rgba, exact11.rgba);
    const long long to11_13 = differing_pixels(exact[1].rgba, exact11.rgba);
    const long long to11_14 = differing_pixels(exact[2].rgba, exact11.rgba);
    const long long to11_15 = differing_pixels(exact[3].rgba, exact11.rgba);
    check(pair12_13 > 0, tag + "levels 12 and 13 differ");
    check(pair13_14 > 0, tag + "levels 13 and 14 differ");
    check(pair14_15 > 0, tag + "levels 14 and 15 differ");
    check(pair12_14 > 0 && pair12_15 > 0 && pair13_15 > 0,
          tag + "levels 12..15 are four pairwise different frames");
    check(to11_12 > 0 && to11_13 > 0 && to11_14 > 0 && to11_15 > 0,
          tag + "each of 12..15 differs from the identity");

    // 4. The dump is a second source of the same table; it must paint the same
    // bytes as the table built in memory (the dump is optional).
    const fs::path dump = find_hud_lut_dump();
    std::error_code ec;
    if (!dump.empty() && fs::is_regular_file(dump, ec) && !ec) {
        gm::LevelSelectRenderOptions options = plain_options;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_dump = dump.string();
        const Frame loaded = run(game, select, options);
        check(loaded.ok, tag + "dump path renders: " + loaded.error);
        if (loaded.ok) {
            check_eq(differing_pixels(loaded.rgba, exact11.rgba), 0,
                     tag + "dump path equals the table built in memory");
        }
        std::printf("  fade LUT: dump=%s\n", dump.string().c_str());
    } else {
        std::cout << "  fade LUT: SKIP real dump (not found; set GM_HUD_LUT or run "
                     "tools/win/dump_lut.lua) path="
                  << dump.string() << "\n";
    }

    // 5. Refusals: no table and no dump, and a dump that is not there.
    gm::LevelSelectRenderOptions none = plain_options;
    none.fade_lut = true;
    check_refused(game, select, none, "fade without a table or a dump", "fade LUT");
    gm::LevelSelectRenderOptions missing = plain_options;
    missing.fade_lut = true;
    missing.fade_lut_dump = (fs::temp_directory_path() /
                             "gm_level_select_render_no_lut.bin").string();
    check_refused(game, select, missing, "fade with a missing dump", "hud_lut");

    std::printf("  fade LUT: level 11 vs unfaded: %lld differing pixels; "
                "level 0: %d black pixels (unfaded %d), %lld differing pixels\n",
                identity_diffs, black0, black_plain, diff0);
    std::printf("  fade LUT level 12..15: adjacent diffs 12/13 %lld, 13/14 %lld, "
                "14/15 %lld; vs level 11: %lld/%lld/%lld/%lld px\n",
                pair12_13, pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);
}

// --- refusals and the synthetic name file ---------------------------------
void check_refused(const fs::path& game, const gm::LevelSelect& select,
                   const gm::LevelSelectRenderOptions& options, const std::string& what,
                   std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::LevelSelectRenderStats stats;
    stats.rows = 7;
    stats.selected_row = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_level_select(game, select, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "level_select_render:"), what + ": error prefix (got \"" +
                                                               error + "\")");
    check(contains(error, expected), what + ": error mentions \"" + std::string(expected) +
                                         "\" (got \"" + error + "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.selected_row, 0, what + ": selected_row cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

void test_negatives(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    // An empty list needs no installation: `build()` was never called.
    gm::LevelSelect empty;
    check_refused(game, empty, gm::LevelSelectRenderOptions{}, "empty list", "no levels");

    gm::CampaignState campaign;
    campaign.load(1);
    gm::LevelSelect select;
    select.build(campaign, nullptr);
    check_refused(fs::path{}, select, gm::LevelSelectRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = fs::temp_directory_path() / "gm_level_select_render_negatives";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }

    // No FRONTEND/BMP.IDX: the atlases cannot be built.
    check_refused(root / "game-without-bmp", select, gm::LevelSelectRenderOptions{},
                  "no BMP.IDX", "BMP.IDX");

    // A background name that is not in the archive is a text, not black.
    gm::LevelSelectRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, select, bad_background, "unknown background", "NOSUCH");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    // A game directory whose FRONTEND is the shipped archive (symlink) but
    // without LVLNAMES.SLU: the stems, no error.
    const fs::path no_names = root / "game-without-names";
    fs::create_directories(no_names, ec);
    fs::create_directory_symlink(game / "FRONTEND", no_names / "FRONTEND", ec);
    check(!ec, "negatives: FRONTEND symlinked: " + ec.message());
    if (ec) {
        fs::remove_all(root, ec);
        return;
    }
    const Frame without_names = run(no_names, select, gm::LevelSelectRenderOptions{});
    check(without_names.ok, "negatives: no LVLNAMES.SLU still renders: " +
                                without_names.error);
    const Frame real = run(game, select, gm::LevelSelectRenderOptions{});
    check(real.ok, "negatives: the install frame renders");
    if (without_names.ok && real.ok) {
        const std::string names = installed_names(game);
        int matched = 0;
        for (const gm::LevelEntry& entry : select.levels()) {
            matched += gm::level_select_name(entry.name, names) != entry.name ? 1 : 0;
        }
        if (matched == 0) {
            check_eq(differing_pixels(without_names.rgba, real.rgba), 0,
                     "negatives: the install falls back to the stems too");
        }
        check_eq(without_names.stats.ink_pixels, real.stats.ink_pixels,
                 "negatives: same ink without the name file");
        std::cout << "level_select_render negatives: no LVLNAMES.SLU -> stems, ink="
                  << without_names.stats.ink_pixels << " (install matched "
                  << matched << "/" << select.levels().size() << " stems)\n";
    }

    // A synthetic lowercase LVLNAMES.SLU (LF, in a temp directory) makes the
    // titles appear: every visible row is compared band by band.
    const fs::path lower_names = root / "game-lower-names";
    fs::create_directories(lower_names, ec);
    fs::create_directory_symlink(game / "FRONTEND", lower_names / "FRONTEND", ec);
    check(!ec, "negatives: second FRONTEND symlinked: " + ec.message());
    if (ec) {
        fs::remove_all(root, ec);
        return;
    }
    gm::CampaignState all;
    all.load(25);
    gm::LevelSelect full;
    full.build(all, nullptr);
    std::string table;
    for (std::size_t i = 0; i < full.levels().size(); ++i) {
        char line[64];
        std::snprintf(line, sizeof(line), "%s Title %02zu\n",
                      full.levels()[i].name.c_str(), i);
        table += line;
    }
    check(write_file(lower_names / "LVLNAMES.SLU", table),
          "negatives: synthetic LVLNAMES.SLU written");
    const Frame titled = run(lower_names, full, gm::LevelSelectRenderOptions{});
    check(titled.ok, "negatives: the synthetic names render: " + titled.error);
    if (titled.ok) {
        check_eq(titled.stats.rows, 10, "negatives: 10 rows with names");
        check_eq(titled.stats.missing_glyphs, 0, "negatives: no missing glyphs in the names");
        check(titled.stats.ink_pixels > real.stats.ink_pixels,
              "negatives: the titles add ink");
        const std::string names = read_file(lower_names / "LVLNAMES.SLU");
        for (int i = 0; i < 10; ++i) {
            const std::string text = gm::level_select_name(
                full.levels()[static_cast<std::size_t>(i)].name, names);
            check(text.find(" - Title ") != std::string::npos,
                  "negatives: row " + std::to_string(i) + " carries its title");
            check_band(titled, i == 0 ? fonts.at(gm::kLevelSelectSelectedFont)
                                      : fonts.at(gm::kLevelSelectRowFont),
                       text, gm::kLevelSelectRowX,
                       gm::kLevelSelectFirstRowY + gm::kLevelSelectRowStep * i,
                       "negatives: titled row " + std::to_string(i));
        }
        std::cout << "level_select_render negatives: synthetic lowercase LVLNAMES.SLU -> "
                  << "ink " << titled.stats.ink_pixels << " vs stems " << real.stats.ink_pixels
                  << " (+" << titled.stats.ink_pixels - real.stats.ink_pixels << ")\n";
    }

    fs::remove_all(root, ec);
}

} // namespace

int main() {
    test_scroll();

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
    if (game.empty()) {
        std::cout << "level_select_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{}, {});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kLevelSelectTitleFont, gm::kLevelSelectRowFont,
                             gm::kLevelSelectSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "level_select_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_names(game);
    test_counter1(game, fonts);

    gm::CampaignState seven;
    seven.load(1);
    gm::LevelSelect seven_rows;
    seven_rows.build(seven, nullptr);
    test_plate(game, seven_rows);

    test_fade_lut(game);
    test_window(game, fonts);
    test_arrows(game);
    test_switches(game, fonts);
    test_negatives(game, fonts);

    if (failures != 0) {
        std::cerr << "level_select_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "level_select_render: ok (" << checks
              << " checks) screen 17 -> 320x200 RGBA (PLATE band, title/rows/ESC, cursor, "
                 "names, scroll, arrows, negatives)\n";
    return 0;
}
