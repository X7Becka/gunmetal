// One frame of screen 13, `START GAME` (`M8.7` slice 5,
// `ui/start_game_render`, docs/START_GAME_RENDER.md).
//
// `ui/save_menu` is the state (20 slots, their titles, the highlight); this
// module paints screen 13 into the 320x200 RGBA canvas with the optional `PLATE`
// band and the `FONTS4` faces 5 (title), 0 (rows) and 4 (cursor).  The test pins
// the geometry of docs/UI_SCREENS.md §3.6 from the handler `0x48aa60` and from
// pixels, not from the code:
//
//   * the shipped install in save mode: `rows == 8` (two entries + six slots),
//     `first_slot == 0`, `slot_rows == 6`, 320x200 RGBA, no missing glyph and
//     every pixel opaque;
//   * `START GAME` (font 5) centred at y 12, `New Single Player Game` at
//     (35, 23), `New Multiplayer Game` at (35, 33) — the negative row -33 of
//     §3.6 is the absolute y 33, `ESC TO CANCEL` at (35, 110); every one of the
//     ten text bands is compared byte for byte with an independent blit;
//   * the six slot rows at (39, 44 + 11*i) show the table entry of slots
//     `first_slot + i` (`A SAVED GAME`, `2`, then `::: empty slot :::`), and
//     each has the `FILESLOT` 124x10 bar of `0x48ab90` at (35, 43 + 11*i) under
//     the name: the whole 196x10 strip of every row is compared with an
//     independent "bar + glyphs" composite, the bars are identical across rows
//     and windows, `(--)fileslot` removes exactly the six 124x10 rectangles
//     (counts and bboxes printed) and leaves `ink` unchanged; a renamed
//     `FILESLOT` entry is a refusal;
//   * the window: `first_slot` 0 -> slots 0..5, 5 -> 5..10, 12 -> 12..17;
//     slot 19 is never visible, out-of-range requests clamp to 0..13; the
//     highlight is font 4 only while the selected slot is inside the window;
//   * the scroll arrows: the `MENUARRW` 16x8 sprite is decoded here from the raw
//     BMP bytes (file order) and compared pixel by pixel — at `first_slot` 0
//     only the down arrow at (160,123)-(173,127), at 5 and 12 both (up at
//     (160,3)-(173,7)); 41 pixels each;
//   * names: a temporary copy of the shipped `saved0.sbt` with the 12 title
//     bytes overwritten by spaces differs from the real frame only in slot row
//     0, by exactly the glyph ink of `"A SAVED GAME"`;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently,
//     the 38324 pixels outside it stay black, the text ink is unchanged and
//     `menu_panel_asset(Screen::StartGame) == "PLATE"`;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a
//     `BMP.IDX` without `FONTS5` (the title face is loaded first), a missing
//     background entry — all texts with a cleared buffer and zeroed stats; a
//     closed model still paints six `::: empty slot :::` rows; determinism of
//     two renders.
//   * exact fade (`fade_lut`, slice 8, §6 of the doc): level 11 is
//     byte-identical to the unfaded frame (0/64000), level 0 has a printed black
//     count and differs, 12..15 are four different frames while the canvas
//     outside the PLATE band stays black; no table and no dump is a
//     `start_game_render:` text.  The table is built in memory from GAMEPAL, no
//     dump is read.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/START_GAME_RENDER.md §4 pins them.

#include <algorithm>
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

#include "res/bigfile.h"
#include "res/bmp.h"
#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "ui/menu_render.h"
#include "ui/save_menu.h"
#include "ui/start_game_render.h"

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
    gm::StartGameRenderStats stats;
};

Frame run(const fs::path& game, const gm::SaveMenu& menu,
          const gm::StartGameRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_start_game(game, menu, options, frame.rgba, frame.width,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kStartGameBackground; }

/// `#000` with any alpha: the faded palette entries of level 0 are black too,
/// but their alpha is the forced 255 and their RGB is what counts here.
bool is_rgb_black(std::uint32_t pixel) { return (pixel >> 8u) == 0u; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Bounding box of the non-black pixels of a band (`y` rows only).
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

/// Compare the rows `y..y+height-1` of the band `0..width` of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kStartGamePanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kStartGameBackground : want;
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

int item_y(int item) { return gm::kStartGameItemFirstY + gm::kStartGameItemStep * item; }
int slot_y(int row) {
    return gm::kStartGameSlotFirstY + gm::kStartGameSlotStep * row;
}
int band_y(int row) {
    return gm::kStartGameFileslotFirstY + gm::kStartGameSlotStep * row;
}

/// One `FRONTEND/BMP.IDX` sprite as the original blitters see it: the index
/// bytes in *file* order (`0x47efe0` reads `width*height` bytes at `0x47f100`
/// without a flip) plus the palette of the same entry.
struct SpriteAsset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rows;
    std::vector<std::uint32_t> palette;
};

SpriteAsset load_sprite_asset(const fs::path& game, const std::string& name);
SpriteAsset load_arrow_asset(const fs::path& game);
SpriteAsset load_fileslot_asset(const fs::path& game);

/// Is (`x`, `y`) inside the `FILESLOT` bar of slot row `row`?  The bar is the
/// whole 124x10 image at (35, 43 + 11*row) (`0x688707 + i*0x86c`).
bool in_fileslot_band(int x, int y) {
    for (int row = 0; row < gm::kStartGameVisible; ++row) {
        const int by = band_y(row);
        if (x >= gm::kStartGameFileslotX &&
            x < gm::kStartGameFileslotX + gm::kStartGameFileslotWidth && y >= by &&
            y < by + gm::kStartGameFileslotHeight) {
            return true;
        }
    }
    return false;
}

/// Number of glyph pixels of one run (index != 0, inside the 196 px band):
/// the `ink_pixels` contribution of that run, independent of what lies below.
long long glyph_ink(const gm::FontAtlas& atlas, std::string_view text, int x) {
    const std::vector<std::uint32_t> band =
        compose_band(atlas, text, x, gm::kStartGamePanelWidth, atlas.cell_height);
    long long ink = 0;
    for (const std::uint32_t pixel : band) {
        ink += pixel == 0 ? 0 : 1;
    }
    return ink;
}

/// Expected colours of the 196 x 10 strip of slot row `row` on the black
/// canvas: the `FILESLOT` bar at (35, 43 + 11*row) (`0x48ab90`: the file rows
/// forward, so screen row r is the *file* row r) with the glyphs of `text` at
/// (39, 44 + 11*row) on top.  0 marks the black canvas.  With `bar == false`
/// the same strip without the band (the `fileslot = false` frame).
std::vector<std::uint32_t> compose_slot_strip(const SpriteAsset& bar,
                                              const gm::FontAtlas& face,
                                              std::string_view text, int /*row*/,
                                              bool band) {
    const int width = gm::kStartGamePanelWidth;
    const int height = gm::kStartGameFileslotHeight;
    std::vector<std::uint32_t> strip(static_cast<std::size_t>(width) *
                                         static_cast<std::size_t>(height),
                                     0u);
    if (band) {
        for (int r = 0; r < gm::kStartGameFileslotHeight; ++r) {
            // `0x47efe0` stores the BMP rows as they are and `0x484cc0` copies
            // them forward: screen row r is the entry's *file* row r.
            for (int c = 0; c < gm::kStartGameFileslotWidth; ++c) {
                const std::uint8_t index =
                    bar.rows[static_cast<std::size_t>(r) *
                                 static_cast<std::size_t>(bar.width) +
                             static_cast<std::size_t>(c)];
                strip[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(gm::kStartGameFileslotX + c)] =
                    bar.palette[index];
            }
        }
    }
    const int text_x = gm::kStartGameSlotX;
    const int text_row = gm::kStartGameSlotFirstY - gm::kStartGameFileslotFirstY;
    int pen = text_x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        const gm::FontGlyph* glyph = face.glyph(ch);
        if (glyph == nullptr || glyph->width <= 0) {
            pen += face.advance_of(ch);
            continue;
        }
        for (int r = 0; r < face.cell_height; ++r) {
            for (int column = 0; column < glyph->width; ++column) {
                const std::uint8_t index =
                    glyph->pixels[static_cast<std::size_t>(r) * glyph->width + column];
                if (index == 0) {
                    continue;
                }
                const int px = pen + column;
                if (px < 0 || px >= width || text_row + r >= height) {
                    continue;
                }
                const std::size_t base = static_cast<std::size_t>(index) * 4u;
                strip[static_cast<std::size_t>(text_row + r) *
                          static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(px)] =
                    (static_cast<std::uint32_t>(face.palette[base + 0]) << 24) |
                    (static_cast<std::uint32_t>(face.palette[base + 1]) << 16) |
                    (static_cast<std::uint32_t>(face.palette[base + 2]) << 8) | 0xFFu;
            }
        }
        pen += face.advance_of(ch);
    }
    return strip;
}

/// Compare the 196 x 10 strip of slot row `row` (rows 43 + 11*row .. 52 +
/// 11*row) with the independent bar + glyph composite; black canvas elsewhere.
void check_slot_row(const Frame& frame, const SpriteAsset& bar, const gm::FontAtlas& face,
                    std::string_view text, int row, bool band, const std::string& what) {
    const int width = gm::kStartGamePanelWidth;
    const int height = gm::kStartGameFileslotHeight;
    const std::vector<std::uint32_t> strip = compose_slot_strip(bar, face, text, row, band);
    int wrong = 0;
    int first_x = -1;
    int first_y = -1;
    for (int r = 0; r < height; ++r) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t want =
                strip[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, band_y(row) + r);
            const std::uint32_t expected = want == 0 ? gm::kStartGameBackground : want;
            if (got != expected) {
                if (wrong == 0) {
                    first_x = column;
                    first_y = band_y(row) + r;
                }
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, what + ": 124x10 bar + glyphs match the independent composite "
                       "(first "
                       + std::to_string(first_x) + "," + std::to_string(first_y) + ")");
}

/// The two `MENUARRW` rectangles of the plate copy (`0x68681f`/`0x68c33b`:
/// column 159, rows 2 and 121, 16x8 each).  No text of this screen reaches them
/// (the title ends at y 19, the last slot band at y 106, `ESC` at y 117).
bool in_arrow_band(int x, int y) {
    const bool column = x >= gm::kStartGameArrowX &&
                        x < gm::kStartGameArrowX + gm::kStartGameArrowWidth;
    const bool up = y >= gm::kStartGameArrowUpY &&
                    y < gm::kStartGameArrowUpY + gm::kStartGameArrowHeight;
    const bool down = y >= gm::kStartGameArrowDownY &&
                      y < gm::kStartGameArrowDownY + gm::kStartGameArrowHeight;
    return column && (up || down);
}

/// Number of differing pixels of *a*/*b* outside the ten text bands, the `ESC`
/// band and the two arrow rectangles.
int differences_outside_bands(const Frame& a, const Frame& b) {
    int outside = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (at(a, x, y) == at(b, x, y)) {
                continue;
            }
            bool inside = y >= gm::kStartGameTitleY && y < gm::kStartGameTitleY + 8;
            inside = inside || (y >= item_y(0) && y < item_y(1) + 8);
            inside = inside || (y >= gm::kStartGameCancelY &&
                                y < gm::kStartGameCancelY + 8);
            for (int row = 0; row < gm::kStartGameVisible && !inside; ++row) {
                inside = y >= slot_y(row) && y < slot_y(row) + 8;
            }
            if (!inside && !in_arrow_band(x, y)) {
                ++outside;
            }
        }
    }
    return outside;
}

/// Temporary root: the approved `/tmp/opencode` tree when present, else the
/// platform temp directory.  Only test scratch data is written there.
fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_start_game_render";
    }
    return fs::temp_directory_path() / "gm_start_game_render";
}

bool read_bytes(const fs::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
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

/// Sum of the atlas advances of a run: what `0x485dd0` centres with
/// (`0x47fa50`); `FontAtlas::measure` adds the trailing tracking pixel.
int pen_width(const gm::FontAtlas& face, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += face.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// Mark the canvas pixels covered by a glyph run: 1 where a glyph pixel lands
/// (independent of the colours), so the caller knows what is painted over the
/// plate and over the `FILESLOT` bars.
void mark_glyphs(std::vector<std::uint8_t>& mask, int width, const gm::FontAtlas& face,
                 std::string_view text, int x, int y) {
    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        const gm::FontGlyph* glyph = face.glyph(ch);
        if (glyph == nullptr || glyph->width <= 0) {
            pen += face.advance_of(ch);
            continue;
        }
        for (int r = 0; r < face.cell_height; ++r) {
            for (int column = 0; column < glyph->width; ++column) {
                const std::uint8_t index =
                    glyph->pixels[static_cast<std::size_t>(r) * glyph->width + column];
                if (index == 0) {
                    continue;
                }
                const int px = pen + column;
                const int py = y + r;
                if (px < 0 || px >= width) {
                    continue;
                }
                const std::size_t at_index =
                    static_cast<std::size_t>(py) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(px);
                if (at_index < mask.size()) {
                    mask[at_index] = 1;
                }
            }
        }
        pen += face.advance_of(ch);
    }
}

/// Mask of every glyph pixel of the frame at `first_slot` (title, two entries,
/// six slot rows of the window, `ESC`).
std::vector<std::uint8_t> text_mask(const gm::SaveMenu& menu, int first_slot,
                                    const std::map<int, gm::FontAtlas>& fonts) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(gm::kStartGameCanvasWidth) *
                                       static_cast<std::size_t>(gm::kStartGameCanvasHeight),
                                   0);
    const int width = gm::kStartGameCanvasWidth;
    const gm::FontAtlas& title = fonts.at(gm::kStartGameTitleFont);
    const gm::FontAtlas& row = fonts.at(gm::kStartGameRowFont);
    const gm::FontAtlas& cursor = fonts.at(gm::kStartGameSelectedFont);
    const int title_width = pen_width(title, gm::kStartGameTitle);
    mark_glyphs(mask, width, title, gm::kStartGameTitle,
                (gm::kStartGamePanelWidth - title_width) / 2, gm::kStartGameTitleY);
    const std::string_view items[gm::kStartGameItems] = {gm::kStartGameSinglePlayer,
                                                         gm::kStartGameMultiplayer};
    for (int item = 0; item < gm::kStartGameItems; ++item) {
        mark_glyphs(mask, width, row, items[item], gm::kStartGameItemX, item_y(item));
    }
    for (int line = 0; line < gm::kStartGameVisible; ++line) {
        const int index = first_slot + line;
        const gm::SaveSlotView& view = menu.at(index);
        const std::string_view text =
            view.occupied ? std::string_view(view.name) : gm::SaveMenu::kEmptyLabel;
        const gm::FontAtlas& face = index == menu.selected() ? cursor : row;
        mark_glyphs(mask, width, face, text, gm::kStartGameSlotX, slot_y(line));
    }
    mark_glyphs(mask, width, row, gm::kStartGameCancel, gm::kStartGameCancelX,
                gm::kStartGameCancelY);
    return mask;
}

/// Colours of the pixels the mask marks: what a run actually painted, without
/// the panel or the `FILESLOT` greys underneath.
std::set<std::uint32_t> glyph_colours(const Frame& frame,
                                      const std::vector<std::uint8_t>& mask) {
    std::set<std::uint32_t> colours;
    for (std::size_t i = 0; i < frame.rgba.size() && i < mask.size(); ++i) {
        if (mask[i] != 0) {
            colours.insert(frame.rgba[i]);
        }
    }
    return colours;
}

/// `glyph_colours` of one run (used to pin "which face is which").
std::set<std::uint32_t> run_colours(const Frame& frame, const gm::FontAtlas& face,
                                    std::string_view text, int x, int y) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(gm::kStartGameCanvasWidth) *
                                       static_cast<std::size_t>(gm::kStartGameCanvasHeight),
                                   0);
    mark_glyphs(mask, gm::kStartGameCanvasWidth, face, text, x, y);
    return glyph_colours(frame, mask);
}

// --- the shipped frame: two entries + six slot bands -------------------------
void test_install_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                        const SpriteAsset& bar) {
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    check_eq(static_cast<long long>(menu.slots().size()), 20, "install: 20 slot views");
    check(menu.at(0).occupied, "install: saved0 is occupied");
    check_eq(menu.at(0).name, "A SAVED GAME", "install: saved0 title");
    check(menu.at(1).occupied, "install: saved1 is occupied");
    check_eq(menu.at(1).name, "2", "install: saved1 title");
    check_eq(std::string(menu.at(2).occupied ? menu.at(2).name
                                             : std::string(gm::SaveMenu::kEmptyLabel)),
             "::: empty slot :::", "install: slot 2 is the empty label (0x4bc3b0)");

    const Frame frame = run(game, menu, gm::StartGameRenderOptions{});
    check(frame.ok, "install: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check(frame.error.empty(), "install: no error on success");
    check_eq(frame.width, 320, "install: width");
    check_eq(frame.height, 200, "install: height");
    check_eq(static_cast<long long>(frame.rgba.size()), 320 * 200, "install: buffer size");
    check_eq(frame.stats.rows, gm::kStartGameRows, "install: stats.rows (0x67f10c = 8)");
    check_eq(frame.stats.first_slot, 0, "install: stats.first_slot");
    check_eq(frame.stats.slot_rows, gm::kStartGameVisible, "install: stats.slot_rows");
    check_eq(frame.stats.fileslot_bands, gm::kStartGameVisible,
             "install: stats.fileslot_bands (one bar per visible slot)");
    check(frame.stats.ink_pixels > 0, "install: ink > 0");
    check_eq(frame.stats.missing_glyphs, 0, "install: no missing glyphs");
    check_eq(frame.stats.arrows, 1, "install: only the down arrow at first_slot 0");
    int opaque = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque += (pixel & 0xFFu) == 0xFFu ? 1 : 0;
    }
    check_eq(opaque, 64000, "install: every pixel is opaque");

    const gm::FontAtlas& title_font = fonts.at(gm::kStartGameTitleFont);
    const gm::FontAtlas& row_font = fonts.at(gm::kStartGameRowFont);
    const gm::FontAtlas& selected_font = fonts.at(gm::kStartGameSelectedFont);

    // The title is centred in the 196 px band at y = 12 (0x485de2..0x485dfa).
    const int title_width = title_font.measure(gm::kStartGameTitle);
    const int title_x = (gm::kStartGamePanelWidth - title_width) / 2;
    const Box title = ink_box(frame, gm::kStartGameTitleY,
                              gm::kStartGameTitleY + title_font.cell_height - 1);
    check_eq(title.x0, title_x, "install: title x (centred)");
    check_eq(title.y0, gm::kStartGameTitleY, "install: title y");
    check_eq(title.width(), title_width - 1, "install: title width (measure - 1)");
    check_band(frame, title_font, gm::kStartGameTitle, title_x, gm::kStartGameTitleY,
               "install: title");

    // The two entries at (35, 23) and (35, 33): item 1 comes from row -33.
    const std::string_view items[gm::kStartGameItems] = {gm::kStartGameSinglePlayer,
                                                         gm::kStartGameMultiplayer};
    Box item_boxes[gm::kStartGameItems];
    for (int item = 0; item < gm::kStartGameItems; ++item) {
        const int y = item_y(item);
        check_band(frame, row_font, items[item], gm::kStartGameItemX, y,
                   "install: item " + std::to_string(item));
        const Box box = ink_box(frame, y, y + row_font.cell_height - 1);
        check_eq(box.x0, gm::kStartGameItemX,
                 "install: item " + std::to_string(item) + " x");
        check_eq(box.y0, y, "install: item " + std::to_string(item) + " y");
        check(box.x1 < gm::kStartGamePanelWidth,
              "install: item " + std::to_string(item) + " fits the 196 px band");
        check_eq(box.x1, gm::kStartGameItemX + row_font.measure(items[item]) - 2,
                 "install: item " + std::to_string(item) + " width (measure - 1)");
        item_boxes[item] = box;
    }

    // The six slot rows at (39, 44 + 11*i): the cursor slot uses font 4, and the
    // `FILESLOT` 124x10 bar of `0x48ab90` sits at (35, 43 + 11*i) under the text.
    check(bar.ok, "install: FILESLOT decodes from FRONTEND/BMP.IDX");
    check_eq(bar.width, gm::kStartGameFileslotWidth, "install: FILESLOT is 124 wide");
    check_eq(bar.height, gm::kStartGameFileslotHeight, "install: FILESLOT is 10 high");
    Box band_boxes[gm::kStartGameVisible];
    int occupied_in_window = 0;
    for (int row = 0; row < gm::kStartGameVisible; ++row) {
        const int index = row;
        const gm::SaveSlotView& view = menu.at(index);
        occupied_in_window += view.occupied ? 1 : 0;
        const std::string_view text =
            view.occupied ? std::string_view(view.name) : gm::SaveMenu::kEmptyLabel;
        const gm::FontAtlas& face = index == menu.selected() ? selected_font : row_font;
        check_slot_row(frame, bar, face, text, row, true,
                       "install: slot row " + std::to_string(row) + " (slot " +
                           std::to_string(index) + ")");
        // The whole 124x10 bar is non-black (its darkest index is 4 = #333), so
        // the strip box is the bar rectangle itself: (35, 43 + 11*row)-(158,
        // 52 + 11*row), 1240 pixels.
        const Box box = ink_box(frame, band_y(row),
                                band_y(row) + gm::kStartGameFileslotHeight - 1);
        band_boxes[row] = box;
        check_eq(box.x0, gm::kStartGameFileslotX,
                 "install: slot row " + std::to_string(row) + " bar x0");
        check_eq(box.y0, band_y(row),
                 "install: slot row " + std::to_string(row) + " bar y0");
        check_eq(box.x1, gm::kStartGameFileslotX + gm::kStartGameFileslotWidth - 1,
                 "install: slot row " + std::to_string(row) + " bar x1");
        check_eq(box.y1, band_y(row) + gm::kStartGameFileslotHeight - 1,
                 "install: slot row " + std::to_string(row) + " bar y1");
        check_eq(box.count, gm::kStartGameFileslotWidth * gm::kStartGameFileslotHeight,
                 "install: slot row " + std::to_string(row) + " bar is 1240 pixels");
    }
    check_eq(occupied_in_window, 2, "install: saved0/saved1 occupy the window");

    // ESC TO CANCEL at (35, 110), font 0 — screen 14 has it at (38, 101).
    const Box cancel = ink_box(frame, gm::kStartGameCancelY,
                               gm::kStartGameCancelY + row_font.cell_height - 1);
    check_eq(cancel.x0, gm::kStartGameCancelX, "install: ESC x");
    check_eq(cancel.y0, gm::kStartGameCancelY, "install: ESC y");
    check_eq(cancel.width(), row_font.measure(gm::kStartGameCancel) - 1,
             "install: ESC width");
    check_band(frame, row_font, gm::kStartGameCancel, gm::kStartGameCancelX,
               gm::kStartGameCancelY, "install: ESC");

    // The ink is exactly the ink of the ten text runs (title, two entries, six
    // slots, ESC); the arrows and the `FILESLOT` bars are not glyph ink.
    long long band_ink = glyph_ink(title_font, gm::kStartGameTitle, title_x) +
                         glyph_ink(row_font, gm::kStartGameCancel, gm::kStartGameCancelX);
    for (int item = 0; item < gm::kStartGameItems; ++item) {
        band_ink += glyph_ink(row_font, items[item], gm::kStartGameItemX);
    }
    for (int row = 0; row < gm::kStartGameVisible; ++row) {
        const int index = row;
        const gm::SaveSlotView& view = menu.at(index);
        const std::string_view text =
            view.occupied ? std::string_view(view.name) : gm::SaveMenu::kEmptyLabel;
        const gm::FontAtlas& face = index == menu.selected() ? selected_font : row_font;
        band_ink += glyph_ink(face, text, gm::kStartGameSlotX);
    }
    check_eq(frame.stats.ink_pixels, band_ink, "install: ink is the sum of the bands");

    // Faces: font 5 title dark red, font 0 rows dark grey, font 4 cursor white.
    // The colours are taken from the glyph mask, so the `FILESLOT` greys under
    // a row cannot leak into the set.
    const std::set<std::uint32_t> title_colours = run_colours(
        frame, title_font, gm::kStartGameTitle, title_x, gm::kStartGameTitleY);
    check(title_colours.count(0x9B0F0FFFu) != 0, "install: the title uses font 5");
    const std::set<std::uint32_t> cursor_colours =
        run_colours(frame, selected_font, menu.at(0).name, gm::kStartGameSlotX, slot_y(0));
    check(cursor_colours.count(0xEFEFEFFFu) != 0, "install: slot 0 uses font 4 (white)");
    check(cursor_colours.count(0x130F0FFFu) == 0, "install: slot 0 is not font 0");
    const std::set<std::uint32_t> row_colours =
        run_colours(frame, row_font, items[0], gm::kStartGameItemX, item_y(0));
    check(row_colours.count(0x130F0FFFu) != 0, "install: item 0 uses font 0 (dark grey)");
    check(row_colours.count(0xEFEFEFFFu) == 0, "install: item 0 is not font 4");

    // Determinism.
    const Frame again = run(game, menu, gm::StartGameRenderOptions{});
    check(again.ok, "install: second render");
    check_eq(differing_pixels(frame.rgba, again.rgba), 0, "install: deterministic");

    std::cout << "start_game_render install: 20 slots rows=" << frame.stats.rows
              << " first_slot=" << frame.stats.first_slot
              << " slot_rows=" << frame.stats.slot_rows
              << " fileslot_bands=" << frame.stats.fileslot_bands
              << " ink=" << frame.stats.ink_pixels
              << " arrows=" << frame.stats.arrows
              << " missing=" << frame.stats.missing_glyphs << "; title=(" << title.x0 << ","
              << title.y0 << ") w=" << title.width() << "; item0=(" << item_boxes[0].x0 << ","
              << item_boxes[0].y0 << ") w=" << item_boxes[0].width() << "; item1=("
              << item_boxes[1].x0 << "," << item_boxes[1].y0 << ") w="
              << item_boxes[1].width() << "; esc=(" << cancel.x0
              << "," << cancel.y0 << ") w=" << cancel.width() << "; bar0=("
              << band_boxes[0].x0 << "," << band_boxes[0].y0 << ")-(" << band_boxes[0].x1
              << "," << band_boxes[0].y1 << ") " << band_boxes[0].count
              << " px, step 11; colours title=[" << join_colours(title_colours)
              << "] cursor=[" << join_colours(cursor_colours) << "] row=["
              << join_colours(row_colours) << "]\n";
}

// --- the window: first_slot 0, 5, 12 and the clamps --------------------------
void test_window(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                 const SpriteAsset& bar) {
    const gm::FontAtlas& row_font = fonts.at(gm::kStartGameRowFont);
    const gm::FontAtlas& selected_font = fonts.at(gm::kStartGameSelectedFont);

    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    check_eq(menu.selected(), 0, "window: the shipped install selects slot 0");

    const auto frame_for = [&](int first, const std::string& tag) {
        gm::StartGameRenderOptions options;
        options.first_slot = first;
        const Frame frame = run(game, menu, options);
        check(frame.ok, tag + ": renders: " + frame.error);
        if (!frame.ok) {
            return frame;
        }
        check_eq(frame.stats.slot_rows, gm::kStartGameVisible, tag + ": six slot rows");
        check_eq(frame.stats.rows, gm::kStartGameRows, tag + ": rows 8");
        check_eq(frame.stats.fileslot_bands, gm::kStartGameVisible,
                 tag + ": six FILESLOT bars");
        for (int row = 0; row < gm::kStartGameVisible; ++row) {
            const int index = frame.stats.first_slot + row;
            const gm::SaveSlotView& view = menu.at(index);
            const std::string_view text =
                view.occupied ? std::string_view(view.name) : gm::SaveMenu::kEmptyLabel;
            const bool highlighted = index == menu.selected();
            const gm::FontAtlas& face = highlighted ? selected_font : row_font;
            check_slot_row(frame, bar, face, text, row, true,
                           tag + ": slot row " + std::to_string(row) + " (slot " +
                               std::to_string(index) + ")");
        }
        return frame;
    };

    const Frame zero = frame_for(0, "window 0");
    check_eq(zero.stats.first_slot, 0, "window 0: first_slot 0 (slots 0..5)");
    check_eq(zero.stats.arrows, 1, "window 0: only the down arrow");
    const Frame five = frame_for(5, "window 5");
    check_eq(five.stats.first_slot, 5, "window 5: first_slot 5 (slots 5..10)");
    check_eq(five.stats.arrows, 2, "window 5: both arrows");
    const Frame twelve = frame_for(12, "window 12");
    check_eq(twelve.stats.first_slot, 12, "window 12: first_slot 12 (slots 12..17)");
    check_eq(twelve.stats.arrows, 2, "window 12: both arrows (12 + 6 < 20)");

    // Slot 19 is never visible; the window clamps to 0..13.
    const Frame clamped = frame_for(99, "window 99");
    check_eq(clamped.stats.first_slot, gm::kStartGameMaxFirst,
             "window 99: clamped to 13 (slots 13..18, slot 19 never visible)");
    const Frame under = frame_for(-5, "window -5");
    check_eq(under.stats.first_slot, 0, "window -5: clamped to 0");

    // The cursor is only painted inside the window (model: the original
    // compares item id with 0x68ccf4 inside the six slot rows).
    const std::set<std::uint32_t> five_colours =
        ink_colours(five, slot_y(0), slot_y(5) + row_font.cell_height - 1);
    check(five_colours.count(0xEFEFEFFFu) == 0,
          "window 5: no white cursor, slot 0 is outside the window");

    const long long moved = differing_pixels(zero.rgba, five.rgba);
    check(moved > 0, "window: moving the window changes the picture");
    check_eq(differences_outside_bands(zero, five), 0,
             "window 0 -> 5: outside the slot bands only the up arrow changes");

    std::cout << "start_game_render window: first_slot 0 -> slots 0..5; 5 -> slots 5..10; "
                 "12 -> slots 12..17; 99 -> 13 (slot 19 never visible); differing 0->5: "
              << moved << " pixels, arrows " << zero.stats.arrows << "/" << five.stats.arrows
              << "/" << twelve.stats.arrows << "\n";
}

// --- the scroll arrows (0x48aa7c..0x48aab3, MENUARRW) -----------------------
SpriteAsset load_sprite_asset(const fs::path& game, const std::string& name) {
    SpriteAsset asset;
    std::string error;
    std::optional<gm::BigFile> archive = gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return asset;
    }
    const gm::BigEntry* entry = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (candidate.name == name) {
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

SpriteAsset load_arrow_asset(const fs::path& game) {
    return load_sprite_asset(game, "MENUARRW");
}

SpriteAsset load_fileslot_asset(const fs::path& game) {
    return load_sprite_asset(game, std::string(gm::kStartGameFileslotName));
}

/// Compare the 16x8 rectangle at (`x`, `y`) with the sprite: `0x485cc0` reads
/// the stored rows forward, `0x485d00` the same bytes backwards (180 degrees);
/// index 0 leaves the black canvas alone.  Returns the number of drawn pixels.
int check_arrow_rect(const Frame& frame, const SpriteAsset& asset, bool up, int x, int y,
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
            std::uint32_t want = gm::kStartGameBackground;
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

void test_arrows(const fs::path& game) {
    const SpriteAsset asset = load_arrow_asset(game);
    check(asset.ok, "arrows: MENUARRW decodes from FRONTEND/BMP.IDX");
    if (!asset.ok) {
        return;
    }
    check_eq(asset.width, gm::kStartGameArrowWidth, "arrows: MENUARRW is 16 wide");
    check_eq(asset.height, gm::kStartGameArrowHeight, "arrows: MENUARRW is 8 high");
    int sprite_pixels = 0;
    for (const std::uint8_t index : asset.rows) {
        sprite_pixels += index != 0 ? 1 : 0;
    }
    check_eq(sprite_pixels, 41, "arrows: 41 non-zero sprite bytes");

    // No text band reaches the two arrow rectangles.
    check(gm::kStartGameTitleY + 8 <= gm::kStartGameArrowUpY ||
              gm::kStartGameTitleY >= gm::kStartGameArrowUpY + gm::kStartGameArrowHeight,
          "arrows: the title stays out of the up-arrow rect");
    for (int row = 0; row < gm::kStartGameVisible; ++row) {
        check(slot_y(row) + 8 <= gm::kStartGameArrowDownY ||
                  slot_y(row) >= gm::kStartGameArrowUpY + gm::kStartGameArrowHeight,
              "arrows: slot row " + std::to_string(row) + " stays out of the arrow rects");
    }
    check(gm::kStartGameCancelY + 8 <= gm::kStartGameArrowDownY,
          "arrows: ESC stays above the down arrow");

    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);

    // first_slot 0: only the down arrow (`0x48aa9f`: 0 + 6 < 20).
    const Frame zero = run(game, menu, gm::StartGameRenderOptions{});
    check(zero.ok, "arrows at first_slot 0: renders: " + zero.error);
    if (!zero.ok) {
        return;
    }
    check_eq(zero.stats.arrows, 1, "arrows at first_slot 0: one arrow");
    check(ink_box(zero, gm::kStartGameArrowUpY,
                  gm::kStartGameArrowUpY + gm::kStartGameArrowHeight - 1)
              .empty(),
          "arrows at first_slot 0: the up arrow is absent");
    const Box down_zero = ink_box(zero, gm::kStartGameArrowDownY,
                                  gm::kStartGameArrowDownY + gm::kStartGameArrowHeight - 1);
    check_eq(down_zero.count, 41, "arrows at first_slot 0: 41 down-arrow pixels");
    check_eq(down_zero.x0, 160, "arrows: down arrow bbox x0");
    check_eq(down_zero.y0, 123, "arrows: down arrow bbox y0");
    check_eq(down_zero.x1, 173, "arrows: down arrow bbox x1");
    check_eq(down_zero.y1, 127, "arrows: down arrow bbox y1");
    check_eq(check_arrow_rect(zero, asset, false, gm::kStartGameArrowX,
                              gm::kStartGameArrowDownY, "arrows at first_slot 0: down"),
             41, "arrows at first_slot 0: the down rect has 41 pixels");

    // first_slot 5: both arrows (`5 > 0`, `5 + 6 < 20`).
    gm::StartGameRenderOptions middle_options;
    middle_options.first_slot = 5;
    const Frame middle = run(game, menu, middle_options);
    check(middle.ok, "arrows at first_slot 5: renders: " + middle.error);
    if (!middle.ok) {
        return;
    }
    check_eq(middle.stats.arrows, 2, "arrows at first_slot 5: two arrows");
    const Box up_middle = ink_box(middle, gm::kStartGameArrowUpY,
                                  gm::kStartGameArrowUpY + gm::kStartGameArrowHeight - 1);
    check_eq(up_middle.count, 41, "arrows at first_slot 5: 41 up-arrow pixels");
    check_eq(up_middle.x0, 160, "arrows: up arrow bbox x0");
    check_eq(up_middle.y0, 3, "arrows: up arrow bbox y0");
    check_eq(up_middle.x1, 173, "arrows: up arrow bbox x1");
    check_eq(up_middle.y1, 7, "arrows: up arrow bbox y1");
    check_eq(check_arrow_rect(middle, asset, true, gm::kStartGameArrowX,
                              gm::kStartGameArrowUpY, "arrows at first_slot 5: up"),
             41, "arrows at first_slot 5: the up rect has 41 pixels");
    check_eq(check_arrow_rect(middle, asset, false, gm::kStartGameArrowX,
                              gm::kStartGameArrowDownY, "arrows at first_slot 5: down"),
             41, "arrows at first_slot 5: the down rect has 41 pixels");

    // first_slot 12: `12 + 6 == 18 < 20`, so the down arrow is still painted.
    gm::StartGameRenderOptions last_options;
    last_options.first_slot = 12;
    const Frame last = run(game, menu, last_options);
    check(last.ok, "arrows at first_slot 12: renders: " + last.error);
    if (!last.ok) {
        return;
    }
    check_eq(last.stats.arrows, 2, "arrows at first_slot 12: both arrows (12 + 6 < 20)");
    check_eq(ink_box(last, gm::kStartGameArrowUpY,
                     gm::kStartGameArrowUpY + gm::kStartGameArrowHeight - 1)
                 .count,
             41, "arrows at first_slot 12: 41 up-arrow pixels");
    check_eq(ink_box(last, gm::kStartGameArrowDownY,
                     gm::kStartGameArrowDownY + gm::kStartGameArrowHeight - 1)
                 .count,
             41, "arrows at first_slot 12: 41 down-arrow pixels");

    std::cout << "start_game_render arrows: MENUARRW 16x8, 41 px per arrow; first_slot 0 -> "
                 "down only bbox ("
              << down_zero.x0 << "," << down_zero.y0 << ")-(" << down_zero.x1 << ","
              << down_zero.y1 << "), ink " << canvas_ink(zero) << "; first_slot 5 -> both, up "
                 "bbox ("
              << up_middle.x0 << "," << up_middle.y0 << ")-(" << up_middle.x1 << ","
              << up_middle.y1 << "), ink " << canvas_ink(middle) << "; first_slot 12 -> both, "
                 "ink "
              << canvas_ink(last) << "\n";
}

// --- the highlight -----------------------------------------------------------
void test_highlight(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    const Frame first = run(game, menu, gm::StartGameRenderOptions{});
    menu.move(1);
    check_eq(menu.selected(), 1, "highlight: selection 1");
    const Frame second = run(game, menu, gm::StartGameRenderOptions{});
    check(first.ok && second.ok, "highlight: both frames render");
    if (!first.ok || !second.ok) {
        return;
    }
    check_eq(second.stats.first_slot, 0, "highlight: window does not move at 1");
    const long long moved = differing_pixels(first.rgba, second.rgba);
    check(moved > 0, "highlight: the cursor changes the picture");
    int outside = 0;
    for (int y = 0; y < first.height; ++y) {
        for (int x = 0; x < first.width; ++x) {
            if (at(first, x, y) == at(second, x, y)) {
                continue;
            }
            const bool rows = y >= slot_y(0) && y < slot_y(1) + 8;
            if (!rows) {
                ++outside;
            }
        }
    }
    check_eq(outside, 0, "highlight: the cursor touches only slot rows 0 and 1");
    const gm::FontAtlas& selected_font = fonts.at(gm::kStartGameSelectedFont);
    check(ink_colours(second, slot_y(1), slot_y(1) + selected_font.cell_height - 1)
                  .count(0xEFEFEFFFu) != 0,
          "highlight: slot 1 is white");
    check(ink_colours(second, slot_y(0), slot_y(0) + selected_font.cell_height - 1)
                  .count(0x130F0FFFu) != 0,
          "highlight: slot 0 returns to font 0");

    std::cout << "start_game_render highlight: selection 0->1 changes " << moved
              << " pixels (slot rows 0/1 only), slot 1 becomes font 4 white\n";
}

// --- real slot names visible in the pixels ----------------------------------
void test_names(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SAVEGAME", ec);
    check(!ec, "names: temp SAVEGAME created: " + ec.message());
    if (ec) {
        return;
    }

    for (const char* name : {"saved0.sbt", "saved1.sbt"}) {
        std::vector<std::uint8_t> bytes;
        check(read_bytes(game / "SAVEGAME" / name, bytes), std::string("names: read ") + name);
        check(bytes.size() > 12, std::string("names: ") + name + " has a header");
        if (bytes.size() <= 12) {
            fs::remove_all(root, ec);
            return;
        }
        if (std::string(name) == "saved0.sbt") {
            // Replace the 12 title bytes by spaces; the header stays valid.
            for (std::size_t i = 0; i < 12; ++i) {
                bytes[i] = ' ';
            }
        }
        check(write_bytes(root / "SAVEGAME" / name, bytes),
              std::string("names: write ") + name);
    }

    gm::SaveMenu real;
    real.open(gm::SaveMenuMode::save, game);
    gm::SaveMenu blank;
    blank.open(gm::SaveMenuMode::save, root);
    check(blank.at(0).occupied, "names: the patched saved0 still parses");
    check_eq(blank.at(0).name, "            ", "names: patched title is 12 spaces");
    check_eq(blank.at(1).name, "2", "names: saved1 is unchanged");

    const Frame real_frame = run(game, real, gm::StartGameRenderOptions{});
    const Frame blank_frame = run(game, blank, gm::StartGameRenderOptions{});
    check(real_frame.ok && blank_frame.ok, "names: both frames render");
    if (!real_frame.ok || !blank_frame.ok) {
        fs::remove_all(root, ec);
        return;
    }
    const long long diff = differing_pixels(real_frame.rgba, blank_frame.rgba);
    check(diff > 0, "names: the title is visible in the pixels");
    int outside = 0;
    for (int y = 0; y < real_frame.height; ++y) {
        for (int x = 0; x < real_frame.width; ++x) {
            if (at(real_frame, x, y) == at(blank_frame, x, y)) {
                continue;
            }
            if (y < slot_y(0) || y >= slot_y(0) + 8) {
                ++outside;
            }
        }
    }
    check_eq(outside, 0, "names: only slot row 0 differs");

    // The difference is exactly the glyph ink of "A SAVED GAME" in font 4.
    const gm::FontAtlas& selected_font = fonts.at(gm::kStartGameSelectedFont);
    const std::vector<std::uint32_t> band = compose_band(
        selected_font, "A SAVED GAME", gm::kStartGameSlotX, gm::kStartGamePanelWidth,
        selected_font.cell_height);
    long long glyph_ink = 0;
    for (const std::uint32_t pixel : band) {
        glyph_ink += pixel == 0 ? 0 : 1;
    }
    check_eq(diff, glyph_ink, "names: the frame differs by the glyph ink of the title");

    std::cout << "start_game_render names: patched saved0 title -> " << diff
              << " differing pixels, all in slot row 0 (y 44..51), equals the glyph ink of "
                 "\"A SAVED GAME\" ("
              << glyph_ink << ")\n";
    fs::remove_all(root, ec);
}

// --- PLATE as the background -------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                const SpriteAsset& bar) {
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    const Frame black = run(game, menu, gm::StartGameRenderOptions{});
    check(black.ok, "plate: the black frame renders");
    if (!black.ok) {
        return;
    }
    gm::StartGameRenderOptions options;
    options.background = std::string(gm::kStartGamePanelName);
    const Frame frame = run(game, menu, options);
    check(frame.ok, "plate: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels, black.stats.ink_pixels,
             "plate: the text ink does not change");
    check_eq(frame.stats.rows, black.stats.rows, "plate: rows do not change");
    check_eq(frame.stats.slot_rows, black.stats.slot_rows, "plate: slot_rows do not change");
    check_eq(frame.stats.arrows, black.stats.arrows, "plate: arrows do not change");
    check_eq(frame.stats.fileslot_bands, black.stats.fileslot_bands,
             "plate: fileslot_bands do not change");

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
    check_eq(image->width, gm::kStartGamePanelWidth, "plate: 196 wide");
    check_eq(image->height, gm::kStartGamePanelHeight, "plate: 131 high");

    int panel_pixels = 0;
    int text_pixels = 0;  // counted to compare with the bar, read via (void)
    int bar_pixels = 0;
    int wrong = 0;
    (void)text_pixels;
    const std::vector<std::uint8_t> mask =
        text_mask(menu, black.stats.first_slot, fonts);
    const auto band_at = [&](int x, int y) -> std::uint32_t {
        for (int row = 0; row < gm::kStartGameVisible; ++row) {
            const int by = band_y(row);
            const int bx = gm::kStartGameFileslotX;
            if (x < bx || x >= bx + gm::kStartGameFileslotWidth || y < by ||
                y >= by + gm::kStartGameFileslotHeight) {
                continue;
            }
            const int r = y - by;
            const int c = x - bx;
            const std::uint8_t index =
                bar.rows[static_cast<std::size_t>(r) *
                             static_cast<std::size_t>(bar.width) +
                         static_cast<std::size_t>(c)];
            return bar.palette[index];
        }
        return 0u;
    };
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t got = at(frame, x, y);
            if (x >= image->width || y >= image->height) {
                ++panel_pixels;
                wrong += is_black(got) ? 0 : 1;
                continue;
            }
            const std::size_t pixel = static_cast<std::size_t>(y) *
                                          static_cast<std::size_t>(frame.width) +
                                      static_cast<std::size_t>(x);
            if (mask[pixel] != 0) {
                // Glyphs are painted identically in both frames, over the panel
                // and over a bar alike.
                ++text_pixels;
                wrong += got == at(black, x, y) ? 0 : 1;
                continue;
            }
            if (in_fileslot_band(x, y)) {
                // The 124x10 `FILESLOT` bar of `0x48ab90` overrides the panel.
                ++bar_pixels;
                wrong += got == band_at(x, y) ? 0 : 1;
                continue;
            }
            if (!is_black(at(black, x, y))) {
                // The `MENUARRW` rectangles are painted identically in both
                // frames; absent arrows leave the panel alone.
                ++text_pixels;
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
            wrong += got == want ? 0 : 1;
        }
    }
    check_eq(wrong, 0, "plate: every pixel is the panel, a FILESLOT bar or the unchanged text");
    check_eq(panel_pixels, 320 * 200 - 196 * 131, "plate: pixels outside the band stay black");
    long long glyph_in_bars = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) *
                                          static_cast<std::size_t>(frame.width) +
                                      static_cast<std::size_t>(x);
            glyph_in_bars += (mask[pixel] != 0 && in_fileslot_band(x, y)) ? 1 : 0;
        }
    }
    check_eq(static_cast<long long>(bar_pixels) + glyph_in_bars,
             6LL * gm::kStartGameFileslotWidth * gm::kStartGameFileslotHeight,
             "plate: the six 124x10 bars cover 7440 pixels of the panel");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::StartGame)),
             std::string(gm::kStartGamePanelName), "plate: screen 13's panel is PLATE");
    const long long changed = differing_pixels(black.rgba, frame.rgba);
    check(changed > 0, "plate: the background changes pixels");
    std::cout << "start_game_render plate: 196x131=" << 196 * 131 << " panel, outside="
              << 320 * 200 - 196 * 131 << " black, ink=" << frame.stats.ink_pixels
              << " (unchanged), fileslot bars=6/7440 px, differing pixels vs black="
              << changed << ", mismatches=0\n";
}

// --- the FILESLOT bar (0x48ab90, slice 9) -----------------------------------
// `0x48ab21` calls `0x48ab90(dest = 0x688707 + i*0x86c, 0xc4, name)`; inside,// ten rows (`mov ebx,0xa` at `0x48abab`) of `0x1f` dwords (31 dwords = 124
// bytes, `push 0x1f` at `0x48abb0`) are copied forward from the `FILESLOT`
// buffer `0x661298` (source stride `0x7c` = 124), so the whole 124x10 image
// lands at (35, 43 + 11*i) of the 196x131 plate copy (`0x86c = 11*196`) under
// the row text.  Pinned here:
//   * the asset itself: 124x10, 2318 bytes (54 header + 1024 palette + 1240
//     pixels), 14 indices, no index 0 (nothing transparent, the copy is a plain
//     `rep movs`);
//   * `fileslot = false` differs from the default frame exactly on the six
//     124x10 rectangles (per-bar count and bbox printed), `ink` unchanged;
//   * orientation: screen row r is the entry's *file* row r (`0x47efe0` freads
//     the pixel bytes verbatim), i.e. `parse_bmp` row `height-1-r`;
//   * `PLATE` + bars: the bars override the panel, the text and arrows are
//     unchanged;
//   * a `BMP.IDX` with `FILESLOT` renamed is a `start_game_render:` refusal,
//     while `fileslot = false` paints the same frame as the real install (the
//     documented escape for a partial install).
void check_refused(const fs::path& game, const gm::SaveMenu& menu,
                   const gm::StartGameRenderOptions& options, const std::string& what,
                   std::string_view expected);

bool archive_rename_entry(const fs::path& game, const fs::path& dir, std::string_view from,
                          std::string_view to, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir / "FRONTEND", ec);
    if (ec) {
        error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    for (const char* name : {"BMP.IDX", "BMP.DAT"}) {
        fs::copy_file(game / "FRONTEND" / name, dir / "FRONTEND" / name,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = std::string("cannot copy ") + name + ": " + ec.message();
            return false;
        }
    }
    std::vector<std::uint8_t> idx;
    if (!read_bytes(dir / "FRONTEND" / "BMP.IDX", idx)) {
        error = "cannot read the index copy";
        return false;
    }
    if (idx.size() < 12) {
        error = "index copy is too short";
        return false;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(idx[8]) |
                                (static_cast<std::uint32_t>(idx[9]) << 8) |
                                (static_cast<std::uint32_t>(idx[10]) << 16) |
                                (static_cast<std::uint32_t>(idx[11]) << 24);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t base = 12 + static_cast<std::size_t>(i) * 12;
        if (base + 8 > idx.size()) {
            break;
        }
        std::string name;
        for (std::size_t k = 0; k < 8 && idx[base + k] != 0; ++k) {
            name.push_back(static_cast<char>(idx[base + k]));
        }
        if (name != std::string(from)) {
            continue;
        }
        for (std::size_t k = 0; k < 8; ++k) {
            idx[base + k] = k < to.size() ? static_cast<std::uint8_t>(to[k]) : 0;
        }
        return write_bytes(dir / "FRONTEND" / "BMP.IDX", idx);
    }
    error = "entry " + std::string(from) + " is not in the index copy";
    return false;
}

void test_fileslot(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                   const SpriteAsset& bar) {
    const std::string tag = "fileslot: ";
    check(bar.ok, tag + "FILESLOT decodes from FRONTEND/BMP.IDX");
    if (!bar.ok) {
        return;
    }
    constexpr long long kBarPixels =
        static_cast<long long>(gm::kStartGameFileslotWidth) * gm::kStartGameFileslotHeight;
    check_eq(bar.width, gm::kStartGameFileslotWidth, tag + "FILESLOT is 124 wide");
    check_eq(bar.height, gm::kStartGameFileslotHeight, tag + "FILESLOT is 10 high");
    check_eq(static_cast<long long>(bar.rows.size()), kBarPixels,
             tag + "1240 pixel bytes");
    std::set<int> indices;
    int painted = 0;
    for (const std::uint8_t index : bar.rows) {
        indices.insert(index);
        painted += index != 0 ? 1 : 0;
    }
    check_eq(painted, kBarPixels, tag + "every byte is copied (1240/1240 non-zero)");
    check_eq(static_cast<long long>(indices.size()), 14, tag + "14 palette indices");
    check(indices.count(0) == 0, tag + "index 0 is not used (the copy is opaque anyway)");
    check_eq(static_cast<long long>(bar.palette.size()), 256, tag + "256-entry palette");

    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    const Frame with = run(game, menu, gm::StartGameRenderOptions{});
    gm::StartGameRenderOptions no_bar_options;
    no_bar_options.fileslot = false;
    const Frame without = run(game, menu, no_bar_options);
    check(with.ok, tag + "default frame renders: " + with.error);
    check(without.ok, tag + "(--)fileslot frame renders: " + without.error);
    if (!with.ok || !without.ok) {
        return;
    }
    check_eq(with.stats.fileslot_bands, gm::kStartGameVisible,
             tag + "default: six bars");
    check_eq(without.stats.fileslot_bands, 0, tag + "(--)fileslot: no bars");
    check_eq(with.stats.ink_pixels, without.stats.ink_pixels,
             tag + "the bar pixels are not glyph ink");
    check_eq(with.stats.slot_rows, without.stats.slot_rows, tag + "slot_rows unchanged");
    check_eq(with.stats.rows, without.stats.rows, tag + "rows unchanged");
    check_eq(with.stats.arrows, without.stats.arrows, tag + "arrows unchanged");

    const std::vector<std::uint8_t> mask = text_mask(menu, with.stats.first_slot, fonts);
    long long total = 0;
    long long outside = 0;
    std::string bar_report;
    for (int row = 0; row < gm::kStartGameVisible; ++row) {
        const int y0 = band_y(row);
        const int x0 = gm::kStartGameFileslotX;
        long long diff = 0;
        long long glyphs = 0;
        int bx0 = -1;
        int by0 = -1;
        int bx1 = -1;
        int by1 = -1;
        for (int y = y0; y < y0 + gm::kStartGameFileslotHeight; ++y) {
            for (int x = x0; x < x0 + gm::kStartGameFileslotWidth; ++x) {
                if (at(with, x, y) == at(without, x, y)) {
                    continue;
                }
                ++diff;
                if (bx0 < 0 || x < bx0) {
                    bx0 = x;
                }
                if (by0 < 0 || y < by0) {
                    by0 = y;
                }
                if (x > bx1) {
                    bx1 = x;
                }
                if (y > by1) {
                    by1 = y;
                }
            }
        }
        for (int y = y0; y < y0 + gm::kStartGameFileslotHeight; ++y) {
            for (int x = x0; x < x0 + gm::kStartGameFileslotWidth; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(with.width) +
                                          static_cast<std::size_t>(x);
                glyphs += mask[pixel] != 0 ? 1 : 0;
            }
        }
        check_eq(diff, kBarPixels - glyphs,
                 tag + "bar " + std::to_string(row) + ": 1240 px minus the text over it");
        check_eq(bx0, gm::kStartGameFileslotX,
                 tag + "bar " + std::to_string(row) + ": bbox x0");
        check_eq(by0, y0, tag + "bar " + std::to_string(row) + ": bbox y0");
        check_eq(bx1, gm::kStartGameFileslotX + gm::kStartGameFileslotWidth - 1,
                 tag + "bar " + std::to_string(row) + ": bbox x1");
        check_eq(by1, y0 + gm::kStartGameFileslotHeight - 1,
                 tag + "bar " + std::to_string(row) + ": bbox y1");
        total += diff;
        if (!bar_report.empty()) {
            bar_report += ", ";
        }
        bar_report += "row " + std::to_string(row) + " y" + std::to_string(y0) + ": " +
                      std::to_string(diff) + " diff, bbox (" + std::to_string(bx0) + "," +
                      std::to_string(by0) + ")-(" + std::to_string(bx1) + "," +
                      std::to_string(by1) + "), glyphs " + std::to_string(glyphs);

        // The no-bar frame is the text on the black canvas, and the bar frame
        // of every row is identical (only the names move with the window).
        const gm::SaveSlotView& view = menu.at(row);
        const std::string_view text =
            view.occupied ? std::string_view(view.name) : gm::SaveMenu::kEmptyLabel;
        const gm::FontAtlas& face = row == menu.selected() ? fonts.at(gm::kStartGameSelectedFont)
                                                          : fonts.at(gm::kStartGameRowFont);
        check_slot_row(without, bar, face, text, row, false,
                       tag + "(--)fileslot row " + std::to_string(row));
    }
    check_eq(differing_pixels(with.rgba, without.rgba), total,
             tag + "all differences are the six bars");
    for (int y = 0; y < with.height; ++y) {
        for (int x = 0; x < with.width; ++x) {
            if (at(with, x, y) != at(without, x, y) && !in_fileslot_band(x, y)) {
                ++outside;
            }
        }
    }
    check_eq(outside, 0, tag + "no difference outside the six bar rectangles");

    // Orientation: the screen row r is the entry's file row r (`0x47efe0`
    // freads the BMP rows verbatim, `0x484cc0` copies them forward), so the
    // first screen row of the bar is the *last* `parse_bmp` row of the image.
    check_eq(static_cast<long long>(at(with, gm::kStartGameFileslotX, band_y(0))),
             static_cast<long long>(bar.palette[bar.rows[0]]),
             tag + "bar top-left is file row 0 (flipV(parse_bmp))");
    check_eq(static_cast<long long>(at(with, gm::kStartGameFileslotX +
                                               gm::kStartGameFileslotWidth - 1,
                                        band_y(0))),
             static_cast<long long>(
                 bar.palette[bar.rows[gm::kStartGameFileslotWidth - 1]]),
             tag + "bar top-right is file row 0");
    check_eq(static_cast<long long>(at(with, gm::kStartGameFileslotX,
                                        band_y(0) + gm::kStartGameFileslotHeight - 1)),
             static_cast<long long>(bar.palette[bar.rows[static_cast<std::size_t>(
                 gm::kStartGameFileslotHeight - 1) * gm::kStartGameFileslotWidth]]),
             tag + "bar bottom-left is file row 9");

    // PLATE + bars: the bars are painted over the panel, the text is unchanged.
    gm::StartGameRenderOptions plate_bar;
    plate_bar.background = std::string(gm::kStartGamePanelName);
    const Frame plate_with = run(game, menu, plate_bar);
    gm::StartGameRenderOptions plate_without = plate_bar;
    plate_without.fileslot = false;
    const Frame plate_plain = run(game, menu, plate_without);
    check(plate_with.ok && plate_plain.ok, tag + "PLATE frames render");
    if (plate_with.ok && plate_plain.ok) {
        check_eq(plate_with.stats.ink_pixels, plate_plain.stats.ink_pixels,
                 tag + "PLATE: text ink unchanged by the bars");
        check_eq(plate_with.stats.fileslot_bands, gm::kStartGameVisible,
                 tag + "PLATE: six bars");
        long long plate_diff = 0;
        long long plate_outside = 0;
        for (int y = 0; y < plate_with.height; ++y) {
            for (int x = 0; x < plate_with.width; ++x) {
                if (at(plate_with, x, y) == at(plate_plain, x, y)) {
                    continue;
                }
                ++plate_diff;
                plate_outside += in_fileslot_band(x, y) ? 0 : 1;
            }
        }
        check(plate_diff > 0, tag + "PLATE: the bars change the panel");
        check_eq(plate_outside, 0, tag + "PLATE: nothing outside the bars changes");
        check_eq(static_cast<long long>(at(plate_with, gm::kStartGameFileslotX, band_y(0))),
                 static_cast<long long>(bar.palette[bar.rows[0]]),
                 tag + "PLATE: the bar wins over the panel");
        std::printf("  start_game_render fileslot PLATE: bars change %lld panel pixels "
                    "(vs the black-canvas bar diff %lld; some bar pixels equal the "
                    "panel colour)\n", plate_diff, total);
    }

    // A renamed FILESLOT entry is a text refusal; `fileslot = false` still
    // paints the same install (the documented escape for a partial install).
    const fs::path root = temp_root() / "no-fileslot";
    std::error_code ec;
    fs::remove_all(root, ec);
    std::string patch_error;
    const bool patched = archive_rename_entry(game, root, gm::kStartGameFileslotName,
                                              "FILESLOX", patch_error);
    check(patched, tag + "archive copy with FILESLOT renamed: " + patch_error);
    if (patched) {
        check_refused(root, menu, gm::StartGameRenderOptions{}, "no FILESLOT", "FILESLOT");
        const Frame patched_plain = run(root, menu, no_bar_options);
        check(patched_plain.ok, tag + "(--)fileslot on the patched archive: " +
                                     patched_plain.error);
        if (patched_plain.ok) {
            check_eq(differing_pixels(patched_plain.rgba, without.rgba), 0,
                     tag + "(--)fileslot is byte-identical without the asset");
        }
    }
    fs::remove_all(root, ec);

    std::printf("  start_game_render fileslot: FILESLOT 124x10, 2318 bytes, 1240 px, "
                "14 indices, no index 0; (--)fileslot total %lld: %s\n",
                total, bar_report.c_str());
}

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::SaveMenu& menu,
                   const gm::StartGameRenderOptions& options, const std::string& what,
                   std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::StartGameRenderStats stats;
    stats.rows = 7;
    stats.first_slot = 7;
    stats.slot_rows = 7;
    stats.fileslot_bands = 7;
    stats.ink_pixels = 7;
    stats.arrows = 7;
    stats.missing_glyphs = 7;
    std::string error;
    const bool ok =
        gm::render_start_game(game, menu, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "start_game_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error + "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.first_slot, 0, what + ": first_slot cleared");
    check_eq(stats.slot_rows, 0, what + ": slot_rows cleared");
    check_eq(stats.fileslot_bands, 0, what + ": fileslot_bands cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
}

void test_negatives(const fs::path& game) {
    gm::SaveMenu menu; // 20 empty views, no open() — a legal state to paint
    check_eq(static_cast<long long>(menu.slots().size()), 20,
             "negatives: the model always exposes 20 rows (the 0-row guard is a contract)");

    check_refused(fs::path{}, menu, gm::StartGameRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }

    // No FRONTEND/BMP.IDX: the atlases cannot be built.
    check_refused(root / "game-without-bmp", menu, gm::StartGameRenderOptions{},
                  "no BMP.IDX", "BMP.IDX");

    // A valid BIGFILE index whose only entry is FONTS1: all three faces of this
    // screen come from the `FONTS4` atlas (fonts 0/4/5), so that entry is the
    // missing one — the title face 5 is loaded first.
    const fs::path fontless = root / "game-without-font4" / "FRONTEND";
    fs::create_directories(fontless, ec);
    check(!ec, "negatives: fontless tree created");
    if (ec) {
        fs::remove_all(root, ec);
        return;
    }
    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                     static_cast<std::uint8_t>(0x1A)};
    put32(idx, 1);
    std::string name = "FONTS1";
    name.resize(8, '\0');
    idx.insert(idx.end(), name.begin(), name.end());
    put32(idx, 0);
    check(write_bytes(fontless / "BMP.IDX", idx), "negatives: synthetic BMP.IDX written");
    check(write_bytes(fontless / "BMP.DAT", {}), "negatives: synthetic BMP.DAT written");
    check_refused(root / "game-without-font4", menu, gm::StartGameRenderOptions{},
                  "missing FONTS4 (the face atlas)", "FONTS4");

    fs::remove_all(root, ec);

    if (game.empty()) {
        return;
    }

    // A background name that is not in the archive is a text, not black.
    gm::StartGameRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, menu, bad_background, "unknown background", "NOSUCH");

    // The same menu, but with a real game directory, paints two entries and six
    // empty slots: a closed model is a legitimate frame.
    const Frame frame = run(game, menu, gm::StartGameRenderOptions{});
    check(frame.ok, "negatives: a closed menu still renders: " + frame.error);
    if (frame.ok) {
        check_eq(frame.stats.rows, gm::kStartGameRows, "negatives: rows 8");
        check_eq(frame.stats.slot_rows, gm::kStartGameVisible, "negatives: six slot rows");
        check_eq(frame.stats.fileslot_bands, gm::kStartGameVisible,
                 "negatives: six FILESLOT bars");
        check_eq(frame.stats.first_slot, 0, "negatives: first_slot 0");
        for (int row = 0; row < gm::kStartGameVisible; ++row) {
            check_eq(std::string(menu.at(row).occupied ? menu.at(row).name
                                                       : std::string(gm::SaveMenu::kEmptyLabel)),
                     std::string(gm::SaveMenu::kEmptyLabel),
                     "negatives: row " + std::to_string(row) + " is the empty label");
        }
        std::cout << "start_game_render negatives: closed menu rows=" << frame.stats.rows
                  << " slot_rows=" << frame.stats.slot_rows
                  << " ink=" << frame.stats.ink_pixels << "\n";
    }
}

// M8.7 slice 8: the exact fade tables of `0x682f90` in the palettised path
// (docs/START_GAME_RENDER.md §6, the same layer as `ui/pager_render.cpp`).
//
// The palette index of every painted pixel (PLATE, MENUARRW, glyphs) goes
// through `render/hud_lut.h::map` **before** the palette lookup, so the table
// sees the index the engine sees, not a colour.  The table is built in memory
// from the installed `FRONTEND/GAMEPAL` by `build_hud_lut` - no dump is read.
// Pinned here:
//
//   * level 11 (the identity of `0x483290(dst, 11/11, 1)`) is byte-identical to
//     the unfaded frame: 0 differing pixels out of 64000;
//   * level 0 is *not* an all-black frame: the table leaves the slots outside
//     the palette ramps alone, so the black count is printed and the frame
//     differs from the unfaded one;
//   * levels 12..15 are four different frames (the model path of the pager
//     clamps them to 11; this screen has no model path at all);
//   * the canvas outside the 196x131 PLATE has no palette index and stays black
//     at every level;
//   * no table and no dump is a `start_game_render:` text, an unreadable dump
//     too, and a pre-loaded table wins over the dump; level 99 clamps to 15.
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas =
        static_cast<long long>(gm::kStartGameCanvasWidth) * gm::kStartGameCanvasHeight;

    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  fade LUT: SKIP (no 1024-byte GAMEPAL: " << palette_error
                  << ")\n";
        return;
    }
    gm::HudLut lut;
    std::string error;
    if (!gm::build_hud_lut(*palette, lut, &error)) {
        check(false, tag + "build_hud_lut: " + error);
        return;
    }
    check(error.empty(), tag + "build_hud_lut leaves no error");
    std::printf("  fade LUT: 16x256 built in memory from FRONTEND/GAMEPAL, no dump\n");

    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    const auto make = [&](bool fade, int level) {
        gm::StartGameRenderOptions options;
        options.background = std::string(gm::kStartGamePanelName);
        options.first_slot = 0;
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, menu, options);
    };
    const auto black_pixels = [](const Frame& frame) {
        long long count = 0;
        for (const std::uint32_t pixel : frame.rgba) {
            count += is_rgb_black(pixel) ? 1 : 0;
        }
        return count;
    };
    const auto outside_panel = [](const Frame& frame) {
        long long count = 0;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                if (x < gm::kStartGamePanelWidth && y < gm::kStartGamePanelHeight) {
                    continue;
                }
                if (!is_black(at(frame, x, y))) {
                    ++count;
                }
            }
        }
        return count;
    };

    const Frame plain = make(false, gm::kHudLutIdentityLevel);
    check(plain.ok, tag + "unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    check_eq(plain.stats.first_slot, 0, tag + "unfaded first_slot 0");
    check_eq(static_cast<long long>(plain.rgba.size()), kCanvas,
             tag + "unfaded canvas 64000");

    // 1. Level 11 is the identity: 0 differing pixels.
    const Frame identity = make(true, gm::kHudLutIdentityLevel);
    check(identity.ok, tag + "level 11 frame: " + identity.error);
    if (!identity.ok) {
        return;
    }
    const long long identity_diffs = differing_pixels(identity.rgba, plain.rgba);
    check_eq(identity_diffs, 0, tag + "level 11 is byte-identical to no fade");
    check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels,
             tag + "level 11 ink unchanged");
    check_eq(identity.stats.arrows, plain.stats.arrows, tag + "level 11 arrows unchanged");
    check_eq(outside_panel(identity), 0, tag + "level 11 leaves the canvas black");

    // 2. Level 0: counted black pixels and a different frame.
    const Frame level0 = make(true, 0);
    check(level0.ok, tag + "level 0 frame: " + level0.error);
    if (!level0.ok) {
        return;
    }
    const long long black0 = black_pixels(level0);
    const long long black_plain = black_pixels(plain);
    const long long diff0 = differing_pixels(level0.rgba, plain.rgba);
    check(black0 > 0, tag + "level 0 still has black pixels to count");
    check(diff0 > 0, tag + "level 0 differs from the unfaded frame");
    check_eq(outside_panel(level0), 0, tag + "level 0 leaves the canvas black");
    check_eq(level0.stats.ink_pixels, plain.stats.ink_pixels,
             tag + "level 0 ink unchanged");

    // 3. Levels 12..15: four different frames, all different from level 11.
    std::vector<Frame> exact(4);
    for (int i = 0; i < 4; ++i) {
        const int level = gm::kHudLutIdentityLevel + 1 + i;
        exact[static_cast<std::size_t>(i)] = make(true, level);
        check(exact[static_cast<std::size_t>(i)].ok,
              tag + "level " + std::to_string(level) + " frame: " +
                  exact[static_cast<std::size_t>(i)].error);
        if (!exact[static_cast<std::size_t>(i)].ok) {
            return;
        }
        check(differing_pixels(exact[static_cast<std::size_t>(i)].rgba, plain.rgba) > 0,
              tag + "level " + std::to_string(level) + " changes the frame");
        check_eq(outside_panel(exact[static_cast<std::size_t>(i)]), 0,
                 tag + "level " + std::to_string(level) + " leaves the canvas black");
        check_eq(exact[static_cast<std::size_t>(i)].stats.ink_pixels,
                 plain.stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const long long pair12_13 = differing_pixels(exact[0].rgba, exact[1].rgba);
    const long long pair13_14 = differing_pixels(exact[1].rgba, exact[2].rgba);
    const long long pair14_15 = differing_pixels(exact[2].rgba, exact[3].rgba);
    check(pair12_13 > 0, tag + "levels 12 and 13 differ");
    check(pair13_14 > 0, tag + "levels 13 and 14 differ");
    check(pair14_15 > 0, tag + "levels 14 and 15 differ");
    const long long to11_12 = differing_pixels(exact[0].rgba, identity.rgba);
    const long long to11_13 = differing_pixels(exact[1].rgba, identity.rgba);
    const long long to11_14 = differing_pixels(exact[2].rgba, identity.rgba);
    const long long to11_15 = differing_pixels(exact[3].rgba, identity.rgba);
    check(to11_12 > 0 && to11_13 > 0 && to11_14 > 0 && to11_15 > 0,
          tag + "each of 12..15 differs from the identity");

    // Level 99 is clamped to 15 by `map`, not aliased elsewhere.
    const Frame clamp99 = make(true, 99);
    check(clamp99.ok, tag + "level 99 frame: " + clamp99.error);
    if (clamp99.ok) {
        check_eq(differing_pixels(clamp99.rgba, exact[3].rgba), 0,
                 tag + "level 99 clamps to 15");
    }

    std::printf("  start_game_render fade: level 11 diffs %lld/%lld; level 0 black "
                "%lld (plain %lld), diffs %lld; adjacent 12/13 %lld, 13/14 %lld, "
                "14/15 %lld; vs 11 %lld/%lld/%lld/%lld\n",
                identity_diffs, kCanvas, black0, black_plain, diff0, pair12_13,
                pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);

    // 4. Negatives: no table and no dump, an unreadable dump, and the table
    // winning over the dump (the dump path is the CLI's option).
    {
        gm::StartGameRenderOptions options;
        options.background = std::string(gm::kStartGamePanelName);
        options.fade_lut = true;
        const Frame refusal = run(game, menu, options);
        check(!refusal.ok, tag + "no table and no dump refused");
        check(starts_with(refusal.error, "start_game_render: ") &&
                  contains(refusal.error, "fade LUT needs"),
              tag + "refusal names the missing table: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "refusal clears the buffer");
        check_eq(refusal.width, 0, tag + "refusal zeroes the width");
        check_eq(refusal.height, 0, tag + "refusal zeroes the height");
        check_eq(refusal.stats.ink_pixels, 0, tag + "refusal zeroes the stats");
    }
    {
        gm::StartGameRenderOptions options;
        options.background = std::string(gm::kStartGamePanelName);
        options.fade_lut = true;
        options.fade_lut_dump =
            (temp_root() / "no-lut.bin").string();
        const Frame refusal = run(game, menu, options);
        check(!refusal.ok, tag + "missing dump refused");
        check(starts_with(refusal.error, "start_game_render: ") &&
                  contains(refusal.error, "hud_lut:"),
              tag + "missing dump carries the hud_lut text: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "missing dump clears the buffer");
    }
    {
        gm::StartGameRenderOptions options;
        options.background = std::string(gm::kStartGamePanelName);
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        options.fade_lut_dump =
            (temp_root() / "no-lut.bin").string();
        const Frame both = run(game, menu, options);
        check(both.ok, tag + "a table wins over a missing dump: " + both.error);
        if (both.ok) {
            check_eq(differing_pixels(both.rgba, plain.rgba), 0,
                     tag + "table path equals the unfaded level 11 frame");
        }
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
        if (fs::exists(root / "FRONTEND" / "BMP.IDX", ec) &&
            fs::exists(root / "SAVEGAME" / "saved0.sbt", ec)) {
            game = root;
            break;
        }
    }
    if (game.empty()) {
        std::cout << "start_game_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kStartGameTitleFont, gm::kStartGameRowFont,
                             gm::kStartGameSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "start_game_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }
    const SpriteAsset bar = load_fileslot_asset(game);

    test_install_frame(game, fonts, bar);
    test_window(game, fonts, bar);
    test_arrows(game);
    test_highlight(game, fonts);
    test_names(game, fonts);
    test_fileslot(game, fonts, bar);
    test_plate(game, fonts, bar);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "start_game_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "start_game_render: ok (" << checks
              << " checks) screen 13 -> 320x200 RGBA (title y 12, entries at y 23/33 x 35, "
                 "6 slots at y 44 step 11 x 39, FILESLOT 124x10 bars at (35, 43 + 11*i), "
                 "cursor font 4, ESC 35/110, arrows, names, PLATE, negatives)\n";
    return 0;
}
