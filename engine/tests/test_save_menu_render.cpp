// One frame of screen 14, `SAVE GAME` (`M8.7` slice 3,
// `ui/save_menu_render`, docs/SAVE_MENU_RENDER.md).
//
// `ui/save_menu` is the state (20 slots, 6-row window, highlight); this module
// paints it into the 320x200 RGBA canvas with the optional `PLATE` band and the
// `FONTS4` faces 0 (rows) and 4 (cursor).  The test pins the geometry of
// docs/UI_SCREENS.md §3.6 from pixels, not from the code:
//
//   * the shipped install in save mode: 20 slots, `rows == 6`, `scroll == 0`,
//     `selected_row == 0`, `slot_rows == 2` (saved0/saved1 occupy the window),
//     320x200 RGBA, no missing glyph and every pixel opaque;
//   * row 0 `"A SAVED GAME"` at (42, 28) and every one of the six bands
//     (y = 28 + 12*i) is compared byte for byte with an independent blit built
//     here from `load_font`'s atlases; the empty rows show `::: empty slot :::`
//     (`0x4bc3b0`); `ESC TO CANCEL` at (38, 101) — not 111 as on screen 17;
//   * the `FILESLOT` 124x10 bar (slice 10) at (38, 27 + 12*i) under each row:
//     the asset is decoded here from `FRONTEND/BMP.IDX` (file row order) and
//     every 196x10 strip is compared with the independent bar + glyph
//     composite; `(--)fileslot` removes exactly the six rectangles, `ink`
//     unchanged; `PLATE` + bars and the renamed-asset refusal;
//   * the faces: the cursor row uses font 4 (`0xefefef`), the other rows font 0
//     (`0x130f0f`);
//   * the window: selection 5 -> scroll 0 (slots 0..5), selection 8 -> scroll 3
//     (slots 3..8), a long `move` stops at the last reachable slot 18 ->
//     scroll 13 (slots 13..18) — slot 19 is never visible; every window is
//     verified band by band and the differing-pixel counts are printed;
//   * the highlight is visible: selection 0 -> 1 changes pixels inside the two
//     row bands only;
//   * the scroll arrows: the `MENUARRW` 16x8 sprite is decoded here from the
//     raw BMP bytes (file order) and compared pixel by pixel — at scroll 0
//     only the down arrow at (160,123)-(173,127), at scroll 3 both (up at
//     (160,3)-(173,7)), and at the last window 13 the down arrow is still
//     painted (13 + 6 < 20 even though slot 19 is unreachable); 41 pixels
//     each;
//   * names: a temporary copy of the shipped `saved0.sbt` with the 12 title
//     bytes overwritten by spaces differs from the real frame only in row 0,
//     by exactly the glyph ink of `"A SAVED GAME"`;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently,
//     the 38324 pixels outside it stay black, the text ink is unchanged and
//     `menu_panel_asset(Screen::SaveGame) == "PLATE"`;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a
//     `BMP.IDX` without `FONTS4`, a missing background entry — all texts with a
//     cleared buffer and zeroed stats; determinism of two renders;
//   * exact fade (`fade_lut`, slice 8): the table is built from `GAMEPAL` in
//     memory (`build_hud_lut`, no file); slot 0 with `PLATE` is byte-identical
//     at level 11, blackened at level 0, and levels 12..15 are four different
//     frames; the dump (when present) gives the same bytes.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/SAVE_MENU_RENDER.md §4 pins them.

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

#include "render/hud_lut.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "render/text_layout.h"
#include "ui/menu_render.h"
#include "ui/save_menu.h"
#include "ui/save_menu_render.h"

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
    gm::SaveMenuRenderStats stats;
};

Frame run(const fs::path& game, const gm::SaveMenu& menu,
          const gm::SaveMenuRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_save_menu(game, menu, options, frame.rgba, frame.width,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kSaveMenuBackground; }

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

/// The `FILESLOT` band of slot row `row`: `0x687aca + i*0x930` inside the plate
/// copy `0x6865f8`, i.e. (38, 27 + 12*row) — one pixel above the text row and
/// four left of its x 42.
int bar_y(int row) { return gm::kSaveMenuFileslotFirstY + gm::kSaveMenuRowStep * row; }

/// Is (`x`, `y`) inside the `FILESLOT` bar of slot row `row`?
bool in_fileslot_band(int x, int y) {
    for (int row = 0; row < gm::kSaveMenuRows; ++row) {
        const int by = bar_y(row);
        if (x >= gm::kSaveMenuFileslotX &&
            x < gm::kSaveMenuFileslotX + gm::kSaveMenuFileslotWidth && y >= by &&
            y < by + gm::kSaveMenuFileslotHeight) {
            return true;
        }
    }
    return false;
}

/// One `FRONTEND/BMP.IDX` sprite in the *file* order the original loader
/// `0x47efe0` sees (`fread` of `width*height` bytes, no flip) plus its palette.
struct BandAsset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::size_t bytes = 0;
    std::vector<std::uint8_t> rows;
    std::vector<std::uint32_t> palette;
};

BandAsset load_band_asset(const fs::path& game, std::string_view name) {
    BandAsset asset;
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
    asset.bytes = bytes->size();
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

/// Expected colours of the 196 x 10 strip of slot row `row` on the black
/// canvas: the `FILESLOT` bar at (38, 27 + 12*row) (`0x48ab90`: the file rows
/// forward, so screen row r is the *file* row r) with the glyphs of `text` at
/// (42, 28 + 12*row) on top.  0 marks the black canvas.  With `band == false`
/// the same strip without the bar (the `fileslot = false` frame).
std::vector<std::uint32_t> compose_slot_strip(const BandAsset& bar,
                                              const gm::FontAtlas& face,
                                              std::string_view text, int /*row*/,
                                              bool band) {
    const int width = gm::kSaveMenuPanelWidth;
    const int height = gm::kSaveMenuFileslotHeight;
    std::vector<std::uint32_t> strip(static_cast<std::size_t>(width) *
                                         static_cast<std::size_t>(height),
                                     0u);
    if (band) {
        for (int r = 0; r < gm::kSaveMenuFileslotHeight; ++r) {
            // `0x47efe0` stores the BMP rows as they are and `0x484cc0` copies
            // them forward: screen row r is the entry's *file* row r.
            for (int c = 0; c < gm::kSaveMenuFileslotWidth; ++c) {
                const std::uint8_t index =
                    bar.rows[static_cast<std::size_t>(r) *
                                 static_cast<std::size_t>(bar.width) +
                             static_cast<std::size_t>(c)];
                strip[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(gm::kSaveMenuFileslotX + c)] =
                    bar.palette[index];
            }
        }
    }
    const int text_x = gm::kSaveMenuRowX;
    const int text_row = gm::kSaveMenuFirstRowY - gm::kSaveMenuFileslotFirstY;
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

/// Compare the 196 x 10 strip of slot row `row` (rows 27 + 12*row .. 36 +
/// 12*row) with the independent bar + glyph composite; black canvas elsewhere.
void check_slot_row(const Frame& frame, const BandAsset& bar, const gm::FontAtlas& face,
                    std::string_view text, int row, bool band, const std::string& what) {
    const int width = gm::kSaveMenuPanelWidth;
    const int height = gm::kSaveMenuFileslotHeight;
    const std::vector<std::uint32_t> strip = compose_slot_strip(bar, face, text, row, band);
    int wrong = 0;
    int first_x = -1;
    int first_y = -1;
    for (int r = 0; r < height; ++r) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t want =
                strip[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, bar_y(row) + r);
            const std::uint32_t expected = want == 0 ? gm::kSaveMenuBackground : want;
            if (got != expected) {
                if (wrong == 0) {
                    first_x = column;
                    first_y = bar_y(row) + r;
                }
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, what + ": 124x10 bar + glyphs match the independent composite "
                       "(first " +
                           std::to_string(first_x) + "," + std::to_string(first_y) + ")");
}

/// The glyph pixels of one run as they paint: `Box` in local coordinates
/// (`y0`/`y1` are cell rows 0..cell_height-1), `count` = glyph ink.
Box glyph_box(const gm::FontAtlas& face, std::string_view text, int x) {
    Box box;
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
                ++box.count;
                if (box.x0 < 0 || px < box.x0) {
                    box.x0 = px;
                }
                if (box.y0 < 0 || r < box.y0) {
                    box.y0 = r;
                }
                if (px > box.x1) {
                    box.x1 = px;
                }
                if (r > box.y1) {
                    box.y1 = r;
                }
            }
        }
        pen += face.advance_of(ch);
    }
    return box;
}

/// Colours of the pixels a run actually painted, read from the frame at the
/// glyph positions (so the bar or the panel underneath cannot leak in).
std::set<std::uint32_t> frame_glyph_colours(const Frame& frame, const gm::FontAtlas& face,
                                            std::string_view text, int x, int y) {
    std::set<std::uint32_t> colours;
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
                if (index != 0) {
                    colours.insert(at(frame, pen + column, y + r));
                }
            }
        }
        pen += face.advance_of(ch);
    }
    return colours;
}

/// Compare the rows `y..y+height-1` of the band `0..width` of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kSaveMenuPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kSaveMenuBackground : want;
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

[[maybe_unused]] std::set<std::uint32_t> ink_colours(const Frame& frame, int y0,
                                                    int y1) {
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

int row_y(int row) { return gm::kSaveMenuFirstRowY + gm::kSaveMenuRowStep * row; }

/// The two `MENUARRW` rectangles of the plate copy (`0x68681f`/`0x68c33b`:
/// column 159, rows 2 and 121, 16x8 each).  The six row bands end at y 95 and
/// `ESC` at 109, so nothing else of the screen writes there.
bool in_arrow_band(int x, int y) {
    const bool column = x >= gm::kSaveMenuArrowX &&
                        x < gm::kSaveMenuArrowX + gm::kSaveMenuArrowWidth;
    const bool up = y >= gm::kSaveMenuArrowUpY &&
                    y < gm::kSaveMenuArrowUpY + gm::kSaveMenuArrowHeight;
    const bool down = y >= gm::kSaveMenuArrowDownY &&
                      y < gm::kSaveMenuArrowDownY + gm::kSaveMenuArrowHeight;
    return column && (up || down);
}

/// Number of differing pixels of *a*/*b* outside the six row bands and `ESC`
/// (the arrow rectangles are *not* excluded here).
int differences_outside_text(const Frame& a, const Frame& b) {
    int outside = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (at(a, x, y) == at(b, x, y)) {
                continue;
            }
            bool inside = y >= gm::kSaveMenuCancelY && y < gm::kSaveMenuCancelY + 8;
            for (int row = 0; row < gm::kSaveMenuRows && !inside; ++row) {
                inside = y >= row_y(row) && y < row_y(row) + 8;
            }
            for (int row = 0; row < gm::kSaveMenuRows && !inside; ++row) {
                inside = y >= bar_y(row) && y < bar_y(row) + gm::kSaveMenuFileslotHeight;
            }
            if (!inside) {
                ++outside;
            }
        }
    }
    return outside;
}

/// Number of differing pixels of *a*/*b* that lie outside the six row bands,
/// the `ESC` band and the two scroll-arrow rectangles.
int differences_outside_bands(const Frame& a, const Frame& b) {
    int outside = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (at(a, x, y) == at(b, x, y)) {
                continue;
            }
            bool inside = y >= gm::kSaveMenuCancelY &&
                          y < gm::kSaveMenuCancelY + 8;
            for (int row = 0; row < gm::kSaveMenuRows && !inside; ++row) {
                inside = y >= row_y(row) && y < row_y(row) + 8;
            }
            for (int row = 0; row < gm::kSaveMenuRows && !inside; ++row) {
                inside = y >= bar_y(row) && y < bar_y(row) + gm::kSaveMenuFileslotHeight;
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
        return approved / "gm_save_menu_render";
    }
    return fs::temp_directory_path() / "gm_save_menu_render";
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

// --- the shipped frame: 20 slots, window 0..5 --------------------------------
void test_install_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                        const BandAsset& bar) {
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    check_eq(static_cast<long long>(menu.slots().size()), 20, "install: 20 slot views");
    check(menu.at(0).occupied, "install: saved0 is occupied");
    check_eq(menu.at(0).name, "A SAVED GAME", "install: saved0 title");
    check(menu.at(1).occupied, "install: saved1 is occupied");
    check_eq(menu.at(1).name, "2", "install: saved1 title");
    check_eq(std::string(menu.label(0)), "A SAVED GAME", "install: row 0 label");
    check_eq(std::string(menu.label(2)), "::: empty slot :::",
             "install: empty row label (0x4bc3b0)");

    int occupied_in_window = 0;
    for (int i = 0; i < gm::kSaveMenuRows; ++i) {
        occupied_in_window += menu.at(i).occupied ? 1 : 0;
    }

    const Frame frame = run(game, menu, gm::SaveMenuRenderOptions{});
    check(frame.ok, "install: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check(frame.error.empty(), "install: no error on success");
    check_eq(frame.width, 320, "install: width");
    check_eq(frame.height, 200, "install: height");
    check_eq(static_cast<long long>(frame.rgba.size()), 320 * 200, "install: buffer size");
    check_eq(frame.stats.rows, gm::kSaveMenuRows, "install: stats.rows");
    check_eq(frame.stats.scroll, 0, "install: stats.scroll");
    check_eq(frame.stats.selected_row, 0, "install: stats.selected_row");
    check_eq(frame.stats.slot_rows, occupied_in_window, "install: stats.slot_rows");
    check_eq(frame.stats.fileslot_bands, gm::kSaveMenuRows,
             "install: stats.fileslot_bands (one bar per row)");
    check(frame.stats.ink_pixels > 0, "install: ink > 0");
    check_eq(frame.stats.missing_glyphs, 0, "install: no missing glyphs");
    int opaque = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque += (pixel & 0xFFu) == 0xFFu ? 1 : 0;
    }
    check_eq(opaque, 64000, "install: every pixel is opaque");

    const gm::FontAtlas& row_font = fonts.at(gm::kSaveMenuRowFont);
    const gm::FontAtlas& selected_font = fonts.at(gm::kSaveMenuSelectedFont);

    // Row 0: the `FILESLOT` bar (38,27)-(161,36) with the cursor name on it.
    const Box first = ink_box(frame, bar_y(0), bar_y(0) + gm::kSaveMenuFileslotHeight - 1);
    check_eq(first.x0, gm::kSaveMenuFileslotX, "install: bar 0 x0");
    check_eq(first.y0, gm::kSaveMenuFileslotFirstY, "install: bar 0 y0");
    check_eq(first.x1, gm::kSaveMenuFileslotX + gm::kSaveMenuFileslotWidth - 1,
             "install: bar 0 x1");
    check_eq(first.y1, bar_y(0) + gm::kSaveMenuFileslotHeight - 1, "install: bar 0 y1");
    check_eq(first.count, gm::kSaveMenuFileslotWidth * gm::kSaveMenuFileslotHeight,
             "install: bar 0 covers 124x10 non-black pixels");

    // The text inside the bar: `measure - 1` wide (last advance carries +1 of
    // tracking), starting at x 42, one pixel below the bar top.
    const Box first_text = glyph_box(selected_font, menu.label(0), gm::kSaveMenuRowX);
    check_eq(first_text.x0, gm::kSaveMenuRowX, "install: row 0 text x");
    check_eq(first_text.width(), selected_font.measure(menu.label(0)) - 1,
             "install: row 0 text width (measure - 1 tracking)");
    check_eq(first_text.y0, 0, "install: row 0 glyphs start at the cell top");
    check_eq(gm::kSaveMenuFirstRowY - gm::kSaveMenuFileslotFirstY, 1,
             "install: the text row is one pixel below the bar top");
    check_eq(gm::kSaveMenuRowX - gm::kSaveMenuFileslotX, 4,
             "install: the text starts four pixels right of the bar");
    check_slot_row(frame, bar, selected_font, menu.label(0), 0, true, "install: row 0");

    // Every band, including the four `::: empty slot :::` rows.
    for (int row = 0; row < gm::kSaveMenuRows; ++row) {
        const gm::FontAtlas& face = row == 0 ? selected_font : row_font;
        check_slot_row(frame, bar, face, menu.label(row), row, true,
                       "install: row " + std::to_string(row));
        const Box box = ink_box(frame, bar_y(row),
                                bar_y(row) + gm::kSaveMenuFileslotHeight - 1);
        check_eq(box.x0, gm::kSaveMenuFileslotX,
                 "install: row " + std::to_string(row) + " bar starts at x 38");
        check_eq(box.y0, bar_y(row),
                 "install: row " + std::to_string(row) + " bar starts at its y");
    }

    // ESC TO CANCEL at (38, 101), font 0 — screen 17 has it at 111.
    const Box cancel =
        ink_box(frame, gm::kSaveMenuCancelY,
                gm::kSaveMenuCancelY + row_font.cell_height - 1);
    check_eq(cancel.x0, gm::kSaveMenuCancelX, "install: ESC x");
    check_eq(cancel.y0, gm::kSaveMenuCancelY, "install: ESC y");
    check_eq(cancel.width(), row_font.measure(gm::kSaveMenuCancel) - 1,
             "install: ESC width");
    check_band(frame, row_font, gm::kSaveMenuCancel, gm::kSaveMenuCancelX,
               gm::kSaveMenuCancelY, "install: ESC");

    // `ink` stays the glyph ink of the seven text runs; the bar pixels are not
    // counted (they are not glyphs).
    long long band_ink = cancel.count;
    for (int row = 0; row < gm::kSaveMenuRows; ++row) {
        const gm::FontAtlas& face = row == 0 ? selected_font : row_font;
        band_ink += glyph_box(face, menu.label(row), gm::kSaveMenuRowX).count;
    }
    check_eq(frame.stats.ink_pixels, band_ink, "install: ink is the sum of the glyph runs");

    // Faces: font 4 white cursor, font 0 dark grey rows (read at the glyphs, so
    // the bar greys underneath do not count).
    const std::set<std::uint32_t> cursor_colours = frame_glyph_colours(
        frame, selected_font, menu.label(0), gm::kSaveMenuRowX, row_y(0));
    check(cursor_colours.count(0xEFEFEFFFu) != 0, "install: the cursor row uses font 4 (white)");
    check(cursor_colours.count(0x130F0FFFu) == 0, "install: the cursor row is not font 0");
    const std::set<std::uint32_t> row_colours = frame_glyph_colours(
        frame, row_font, menu.label(1), gm::kSaveMenuRowX, row_y(1));
    check(row_colours.count(0x130F0FFFu) != 0, "install: row 1 uses font 0 (dark grey)");
    check(row_colours.count(0xEFEFEFFFu) == 0, "install: row 1 is not font 4");

    // Determinism.
    const Frame again = run(game, menu, gm::SaveMenuRenderOptions{});
    check(again.ok, "install: second render");
    check_eq(differing_pixels(frame.rgba, again.rgba), 0, "install: deterministic");

    std::cout << "save_menu_render install: 20 slots rows=" << frame.stats.rows
              << " scroll=" << frame.stats.scroll << " selected=" << frame.stats.selected_row
              << " slot_rows=" << frame.stats.slot_rows
              << " fileslot_bands=" << frame.stats.fileslot_bands
              << " ink=" << frame.stats.ink_pixels
              << " missing=" << frame.stats.missing_glyphs << "; bar0=(" << first.x0 << ","
              << first.y0 << ")-(" << first.x1 << "," << first.y1 << ") "
              << first.count << " px; first=(" << first_text.x0 << ","
              << gm::kSaveMenuFirstRowY << ") w=" << first_text.width() << "; esc=("
              << cancel.x0 << "," << cancel.y0 << ") w=" << cancel.width()
              << "; colours cursor=[" << join_colours(cursor_colours) << "] row=["
              << join_colours(row_colours) << "]\n";
}

// --- the window: selections 5, 8 and the last reachable 18 -------------------
void test_window(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                 const BandAsset& bar) {
    const gm::FontAtlas& row_font = fonts.at(gm::kSaveMenuRowFont);
    const gm::FontAtlas& selected_font = fonts.at(gm::kSaveMenuSelectedFont);

    const auto frame_and_bands = [&](gm::SaveMenu& menu, const std::string& tag) {
        const Frame frame = run(game, menu, gm::SaveMenuRenderOptions{});
        check(frame.ok, tag + ": renders: " + frame.error);
        if (!frame.ok) {
            return frame;
        }
        check_eq(frame.stats.rows, gm::kSaveMenuRows, tag + ": six rows");
        check_eq(frame.stats.fileslot_bands, gm::kSaveMenuRows, tag + ": six bars");
        check_eq(frame.stats.scroll, menu.visible_first(), tag + ": scroll is visible_first");
        check_eq(frame.stats.selected_row, menu.selected(), tag + ": selected_row");
        for (int row = 0; row < gm::kSaveMenuRows; ++row) {
            const int index = menu.visible_first() + row;
            const gm::FontAtlas& face =
                index == menu.selected() ? selected_font : row_font;
            check_slot_row(frame, bar, face, menu.label(row), row, true,
                           tag + ": row " + std::to_string(row) + " (slot " +
                               std::to_string(index) + ")");
        }
        return frame;
    };

    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    check(menu.at(0).selectable, "window: save mode makes slot 0 selectable");

    menu.move(5);
    check_eq(menu.selected(), 5, "window: selection 5");
    const Frame five = frame_and_bands(menu, "window 5");
    check_eq(five.stats.scroll, 0, "window 5: scroll 0 (slots 0..5)");
    check_eq(five.stats.selected_row, 5, "window 5: cursor on slot 5");

    menu.move(3);
    check_eq(menu.selected(), 8, "window: selection 8");
    const Frame eight = frame_and_bands(menu, "window 8");
    check_eq(eight.stats.scroll, 3, "window 8: scroll 3 (slots 3..8)");
    check_eq(eight.stats.selected_row, 8, "window 8: cursor on slot 8");
    check_eq(std::string(menu.label(5)), std::string(menu.at(8).occupied
                                                        ? menu.at(8).name
                                                        : gm::SaveMenu::kEmptyLabel),
             "window 8: last band is slot 8");
    const long long moved = differing_pixels(five.rgba, eight.rgba);
    check(moved > 0, "window: moving the window changes the picture");
    check_eq(differences_outside_bands(five, eight), 0,
             "window: only the row bands changed");

    // Slot 19 is unreachable; a long move clamps on 18 and the window on 13.
    menu.move(100);
    check_eq(menu.selected(), 18, "window: a long move stops at slot 18");
    const Frame last = frame_and_bands(menu, "window 18");
    check_eq(last.stats.scroll, 13, "window 18: scroll 13 (slots 13..18)");
    check_eq(last.stats.selected_row, 18, "window 18: cursor on slot 18");
    check_eq(last.stats.slot_rows, 0, "window 18: no occupied slot in 13..18");
    const Box cursor_band = ink_box(last, bar_y(5),
                                    bar_y(5) + gm::kSaveMenuFileslotHeight - 1);
    check_eq(cursor_band.x0, gm::kSaveMenuFileslotX, "window 18: last bar starts at x 38");
    check_eq(cursor_band.y0, bar_y(5), "window 18: last bar starts at its y");
    check(frame_glyph_colours(last, selected_font, menu.label(5), gm::kSaveMenuRowX,
                              row_y(5))
                  .count(0xEFEFEFFFu) != 0,
          "window 18: the cursor band is font 4");

    std::cout << "save_menu_render window: selected 5 -> scroll 0 (slots 0..5); 8 -> scroll 3 "
                 "(slots 3..8); move(+100) -> selected 18 (slot 19 unreachable), scroll 13 "
                 "(slots 13..18); differing 5->8: "
              << moved << "\n";
}

// --- the scroll arrows (0x48abf1..0x48ac23, MENUARRW) ---------------------
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
            std::uint32_t want = gm::kSaveMenuBackground;
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
    const ArrowAsset asset = load_arrow_asset(game);
    check(asset.ok, "arrows: MENUARRW decodes from FRONTEND/BMP.IDX");
    if (!asset.ok) {
        return;
    }
    check_eq(asset.width, gm::kSaveMenuArrowWidth, "arrows: MENUARRW is 16 wide");
    check_eq(asset.height, gm::kSaveMenuArrowHeight, "arrows: MENUARRW is 8 high");
    int sprite_pixels = 0;
    for (const std::uint8_t index : asset.rows) {
        sprite_pixels += index != 0 ? 1 : 0;
    }
    check_eq(sprite_pixels, 41, "arrows: 41 non-zero sprite bytes");

    // The six row bands and `ESC` cannot reach the two arrow rectangles.
    for (int row = 0; row < gm::kSaveMenuRows; ++row) {
        check(row_y(row) + 8 <= gm::kSaveMenuArrowDownY ||
                  row_y(row) >= gm::kSaveMenuArrowUpY + gm::kSaveMenuArrowHeight,
              "arrows: row " + std::to_string(row) + " stays out of the arrow rects");
    }
    check(gm::kSaveMenuCancelY + 8 <= gm::kSaveMenuArrowDownY,
          "arrows: ESC stays above the down arrow");

    // Slot 0: scroll 0, so only the down arrow (`0x48ac0f`: 0 + 6 < 20).
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    const Frame zero = run(game, menu, gm::SaveMenuRenderOptions{});
    check(zero.ok, "arrows at scroll 0: renders: " + zero.error);
    if (!zero.ok) {
        return;
    }
    check_eq(zero.stats.rows, gm::kSaveMenuRows, "arrows at scroll 0: six rows");
    check_eq(zero.stats.scroll, 0, "arrows at scroll 0: scroll 0");
    check_eq(zero.stats.arrows, 1, "arrows at scroll 0: one arrow");
    check(ink_box(zero, gm::kSaveMenuArrowUpY,
                  gm::kSaveMenuArrowUpY + gm::kSaveMenuArrowHeight - 1)
              .empty(),
          "arrows at scroll 0: the up arrow is absent");
    const Box down_zero = ink_box(zero, gm::kSaveMenuArrowDownY,
                                 gm::kSaveMenuArrowDownY + gm::kSaveMenuArrowHeight - 1);
    check_eq(down_zero.count, 41, "arrows at scroll 0: 41 down-arrow pixels");
    check_eq(down_zero.x0, 160, "arrows: down arrow bbox x0");
    check_eq(down_zero.y0, 123, "arrows: down arrow bbox y0");
    check_eq(down_zero.x1, 173, "arrows: down arrow bbox x1");
    check_eq(down_zero.y1, 127, "arrows: down arrow bbox y1");
    check_eq(check_arrow_rect(zero, asset, false, gm::kSaveMenuArrowX,
                              gm::kSaveMenuArrowDownY, "arrows at scroll 0: down"),
             41, "arrows at scroll 0: the down rect has 41 pixels");

    // Slot 8: scroll 3, both arrows (`3 > 0`, `3 + 6 < 20`).
    menu.move(8);
    check_eq(menu.selected(), 8, "arrows: selection 8");
    const Frame middle = run(game, menu, gm::SaveMenuRenderOptions{});
    check(middle.ok, "arrows in the middle: renders: " + middle.error);
    if (!middle.ok) {
        return;
    }
    check_eq(middle.stats.rows, gm::kSaveMenuRows, "arrows in the middle: six rows");
    check_eq(middle.stats.scroll, 3, "arrows in the middle: scroll 3");
    check_eq(middle.stats.arrows, 2, "arrows in the middle: two arrows");
    const Box up_middle = ink_box(middle, gm::kSaveMenuArrowUpY,
                                  gm::kSaveMenuArrowUpY + gm::kSaveMenuArrowHeight - 1);
    check_eq(up_middle.count, 41, "arrows in the middle: 41 up-arrow pixels");
    check_eq(up_middle.x0, 160, "arrows: up arrow bbox x0");
    check_eq(up_middle.y0, 3, "arrows: up arrow bbox y0");
    check_eq(up_middle.x1, 173, "arrows: up arrow bbox x1");
    check_eq(up_middle.y1, 7, "arrows: up arrow bbox y1");
    check_eq(check_arrow_rect(middle, asset, true, gm::kSaveMenuArrowX,
                              gm::kSaveMenuArrowUpY, "arrows in the middle: up"),
             41, "arrows in the middle: the up rect has 41 pixels");
    check_eq(check_arrow_rect(middle, asset, false, gm::kSaveMenuArrowX,
                              gm::kSaveMenuArrowDownY, "arrows in the middle: down"),
             41, "arrows in the middle: the down rect has 41 pixels");
    check_eq(differences_outside_text(zero, middle), 41,
             "arrows: the up arrow is the only change outside the text bands");

    // Slot 18: scroll 13, the last window.  `13 + 6 == 19 < 20`, so the
    // original still paints the down arrow even though slot 19 is unreachable.
    menu.move(100);
    check_eq(menu.selected(), 18, "arrows: selection 18");
    const Frame last = run(game, menu, gm::SaveMenuRenderOptions{});
    check(last.ok, "arrows at the bottom: renders: " + last.error);
    if (!last.ok) {
        return;
    }
    check_eq(last.stats.rows, gm::kSaveMenuRows, "arrows at the bottom: six rows");
    check_eq(last.stats.scroll, 13, "arrows at the bottom: scroll 13");
    check_eq(last.stats.arrows, 2, "arrows at the bottom: both arrows (13 + 6 < 20)");
    check_eq(ink_box(last, gm::kSaveMenuArrowUpY,
                     gm::kSaveMenuArrowUpY + gm::kSaveMenuArrowHeight - 1)
                 .count,
             41, "arrows at the bottom: 41 up-arrow pixels");
    check_eq(ink_box(last, gm::kSaveMenuArrowDownY,
                     gm::kSaveMenuArrowDownY + gm::kSaveMenuArrowHeight - 1)
                 .count,
             41, "arrows at the bottom: 41 down-arrow pixels");

    std::cout << "save_menu_render arrows: MENUARRW 16x8, 41 px per arrow; scroll 0 -> down "
                 "only bbox ("
              << down_zero.x0 << "," << down_zero.y0 << ")-(" << down_zero.x1 << ","
              << down_zero.y1 << "), ink " << canvas_ink(zero) << "; scroll 3 -> both, up "
                 "bbox ("
              << up_middle.x0 << "," << up_middle.y0 << ")-(" << up_middle.x1 << ","
              << up_middle.y1 << "), ink " << canvas_ink(middle) << ", differing outside "
                 "bands "
              << differences_outside_text(zero, middle) << "; scroll 13 -> both (13 + 6 < 20), ink "
              << canvas_ink(last) << "\n";
}

// --- the highlight -----------------------------------------------------------
void test_highlight(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    const Frame first = run(game, menu, gm::SaveMenuRenderOptions{});
    menu.move(1);
    check_eq(menu.selected(), 1, "highlight: selection 1");
    const Frame second = run(game, menu, gm::SaveMenuRenderOptions{});
    check(first.ok && second.ok, "highlight: both frames render");
    if (!first.ok || !second.ok) {
        return;
    }
    check_eq(first.stats.scroll, 0, "highlight: window does not scroll at 1");
    const long long moved = differing_pixels(first.rgba, second.rgba);
    check(moved > 0, "highlight: the cursor changes the picture");
    int outside = 0;
    for (int y = 0; y < first.height; ++y) {
        for (int x = 0; x < first.width; ++x) {
            if (at(first, x, y) == at(second, x, y)) {
                continue;
            }
            const bool rows = y >= row_y(0) && y < row_y(1) + 8;
            if (!rows) {
                ++outside;
            }
        }
    }
    check_eq(outside, 0, "highlight: the cursor touches only rows 0 and 1");
    const gm::FontAtlas& selected_font = fonts.at(gm::kSaveMenuSelectedFont);
    const gm::FontAtlas& row_font = fonts.at(gm::kSaveMenuRowFont);
    check(frame_glyph_colours(second, selected_font, menu.label(1), gm::kSaveMenuRowX,
                              row_y(1))
                  .count(0xEFEFEFFFu) != 0,
          "highlight: slot 1 is white");
    check(frame_glyph_colours(second, row_font, menu.label(0), gm::kSaveMenuRowX, row_y(0))
                  .count(0x130F0FFFu) != 0,
          "highlight: slot 0 returns to font 0");

    std::cout << "save_menu_render highlight: selection 0->1 changes " << moved
              << " pixels (rows 0/1 only), slot 1 becomes font 4 white\n";
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

    const Frame real_frame = run(game, real, gm::SaveMenuRenderOptions{});
    const Frame blank_frame = run(game, blank, gm::SaveMenuRenderOptions{});
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
            if (y < row_y(0) || y >= row_y(0) + 8) {
                ++outside;
            }
        }
    }
    check_eq(outside, 0, "names: only row 0 differs");

    // The difference is exactly the glyph ink of "A SAVED GAME" in font 4.
    const gm::FontAtlas& selected_font = fonts.at(gm::kSaveMenuSelectedFont);
    const std::vector<std::uint32_t> band = compose_band(
        selected_font, "A SAVED GAME", gm::kSaveMenuRowX, gm::kSaveMenuPanelWidth,
        selected_font.cell_height);
    long long glyph_ink = 0;
    for (const std::uint32_t pixel : band) {
        glyph_ink += pixel == 0 ? 0 : 1;
    }
    check_eq(diff, glyph_ink, "names: the frame differs by the glyph ink of the title");

    std::cout << "save_menu_render names: patched saved0 title -> " << diff
              << " differing pixels, all in row 0 (y 28..35), equals the glyph ink of "
                 "\"A SAVED GAME\" ("
              << glyph_ink << ")\n";
    fs::remove_all(root, ec);
}

// --- PLATE as the background -------------------------------------------------
void test_plate(const fs::path& game) {
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    const Frame black = run(game, menu, gm::SaveMenuRenderOptions{});
    check(black.ok, "plate: the black frame renders");
    if (!black.ok) {
        return;
    }
    gm::SaveMenuRenderOptions options;
    options.background = std::string(gm::kSaveMenuPanelName);
    const Frame frame = run(game, menu, options);
    check(frame.ok, "plate: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels, black.stats.ink_pixels,
             "plate: the text ink does not change");
    check_eq(frame.stats.rows, black.stats.rows, "plate: rows do not change");
    check_eq(frame.stats.slot_rows, black.stats.slot_rows, "plate: slot_rows do not change");
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
    check_eq(image->width, gm::kSaveMenuPanelWidth, "plate: 196 wide");
    check_eq(image->height, gm::kSaveMenuPanelHeight, "plate: 131 high");

    int panel_pixels = 0;
    int band_pixels = 0;
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
                ++band_pixels;
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
            ++band_pixels;
            wrong += got == want ? 0 : 1;
        }
    }
    check_eq(wrong, 0, "plate: every pixel is the panel or the unchanged text");
    check_eq(panel_pixels, 320 * 200 - 196 * 131, "plate: pixels outside the band stay black");
    check_eq(band_pixels, 196 * 131, "plate: the band is covered");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::SaveGame)),
             std::string(gm::kSaveMenuPanelName), "plate: screen 14's panel is PLATE");
    const long long changed = differing_pixels(black.rgba, frame.rgba);
    check(changed > 0, "plate: the background changes pixels");
    std::cout << "save_menu_render plate: 196x131=" << 196 * 131 << " panel, outside="
              << 320 * 200 - 196 * 131 << " black, ink=" << frame.stats.ink_pixels
              << " (unchanged), differing pixels vs black=" << changed
              << ", mismatches=0\n";
}

// --- the FILESLOT bar (0x48ab90, slice 10) -----------------------------------
// `0x48ac5d` calls `0x48ab90(dest = 0x687aca + i*0x930, 0xc4, name)`; inside,
// ten rows (`mov ebx,0xa` at `0x48abab`) of `0x1f` dwords (124 bytes, `push
// 0x1f`) are copied forward from the `FILESLOT` buffer `0x661298` (source
// stride `0x7c` = 124), so the whole 124x10 image lands at (38, 27 + 12*i) of
// the 196x131 plate copy (`0x14d2 = 27*196 + 38`, `0x930 = 12*196`) before the
// row text.  Pinned here:
//   * the asset itself: 124x10, 2318 bytes (54 header + 1024 palette + 1240
//     pixels), 14 indices, no index 0;
//   * `fileslot = false` differs from the default frame exactly on the six
//     124x10 rectangles (per-bar count and bbox printed), `ink` unchanged;
//   * orientation: screen row r is the entry's *file* row r;
//   * every strip is the independent bar + glyph composite of the row;
//   * `PLATE` + bars: the bars override the panel, nothing outside them moves;
//   * a renamed `FILESLOT` is a `save_menu_render:` refusal, while
//     `fileslot = false` paints the same frame as the real install.
void check_refused(const fs::path& game, const gm::SaveMenu& menu,
                   const gm::SaveMenuRenderOptions& options, const std::string& what,
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
                   const BandAsset& bar) {
    const std::string tag = "fileslot: ";
    check(bar.ok, tag + "FILESLOT decodes from FRONTEND/BMP.IDX");
    if (!bar.ok) {
        return;
    }
    constexpr long long kBarPixels =
        static_cast<long long>(gm::kSaveMenuFileslotWidth) * gm::kSaveMenuFileslotHeight;
    check_eq(bar.width, gm::kSaveMenuFileslotWidth, tag + "FILESLOT is 124 wide");
    check_eq(bar.height, gm::kSaveMenuFileslotHeight, tag + "FILESLOT is 10 high");
    check_eq(static_cast<long long>(bar.rows.size()), kBarPixels,
             tag + "1240 pixel bytes");
    check_eq(static_cast<long long>(bar.bytes), 54 + 1024 + kBarPixels,
             tag + "2318 bytes (header + palette + 1240 px)");
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
    const Frame with = run(game, menu, gm::SaveMenuRenderOptions{});
    gm::SaveMenuRenderOptions no_bar_options;
    no_bar_options.fileslot = false;
    const Frame without = run(game, menu, no_bar_options);
    check(with.ok, tag + "default frame renders: " + with.error);
    check(without.ok, tag + "(--)fileslot frame renders: " + without.error);
    if (!with.ok || !without.ok) {
        return;
    }
    check_eq(with.stats.fileslot_bands, gm::kSaveMenuRows, tag + "default: six bars");
    check_eq(without.stats.fileslot_bands, 0, tag + "(--)fileslot: no bars");
    check_eq(with.stats.ink_pixels, without.stats.ink_pixels,
             tag + "the bar pixels are not glyph ink");
    check_eq(with.stats.slot_rows, without.stats.slot_rows, tag + "slot_rows unchanged");
    check_eq(with.stats.rows, without.stats.rows, tag + "rows unchanged");
    check_eq(with.stats.arrows, without.stats.arrows, tag + "arrows unchanged");

    long long total = 0;
    long long outside = 0;
    std::string bar_report;
    for (int row = 0; row < gm::kSaveMenuRows; ++row) {
        const int y0 = bar_y(row);
        const int x0 = gm::kSaveMenuFileslotX;
        const gm::FontAtlas& face =
            row == menu.selected() ? fonts.at(gm::kSaveMenuSelectedFont)
                                   : fonts.at(gm::kSaveMenuRowFont);
        const std::string_view text = menu.label(row);
        const long long glyphs = glyph_box(face, text, gm::kSaveMenuRowX).count;
        long long diff = 0;
        int bx0 = -1;
        int by0 = -1;
        int bx1 = -1;
        int by1 = -1;
        for (int y = y0; y < y0 + gm::kSaveMenuFileslotHeight; ++y) {
            for (int x = x0; x < x0 + gm::kSaveMenuFileslotWidth; ++x) {
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
        check_eq(diff, kBarPixels - glyphs,
                 tag + "bar " + std::to_string(row) + ": 1240 px minus the text over it");
        check_eq(bx0, gm::kSaveMenuFileslotX,
                 tag + "bar " + std::to_string(row) + ": bbox x0");
        check_eq(by0, y0, tag + "bar " + std::to_string(row) + ": bbox y0");
        check_eq(bx1, gm::kSaveMenuFileslotX + gm::kSaveMenuFileslotWidth - 1,
                 tag + "bar " + std::to_string(row) + ": bbox x1");
        check_eq(by1, y0 + gm::kSaveMenuFileslotHeight - 1,
                 tag + "bar " + std::to_string(row) + ": bbox y1");
        total += diff;
        if (!bar_report.empty()) {
            bar_report += ", ";
        }
        bar_report += "row " + std::to_string(row) + " y" + std::to_string(y0) + ": " +
                      std::to_string(diff) + " diff, bbox (" + std::to_string(bx0) + "," +
                      std::to_string(by0) + ")-(" + std::to_string(bx1) + "," +
                      std::to_string(by1) + "), glyphs " + std::to_string(glyphs);

        // Parity: the whole 196 x 10 strip of the row is the independent
        // `FILESLOT` + glyph composite (with and without the bar).
        check_slot_row(with, bar, face, text, row, true,
                       tag + "row " + std::to_string(row) + " with the bar");
        check_slot_row(without, bar, face, text, row, false,
                       tag + "row " + std::to_string(row) + " (--)fileslot");
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
    check_eq(static_cast<long long>(at(with, gm::kSaveMenuFileslotX, bar_y(0))),
             static_cast<long long>(bar.palette[bar.rows[0]]),
             tag + "bar top-left is file row 0 (flipV(parse_bmp))");
    check_eq(static_cast<long long>(at(with, gm::kSaveMenuFileslotX +
                                               gm::kSaveMenuFileslotWidth - 1,
                                        bar_y(0))),
             static_cast<long long>(
                 bar.palette[bar.rows[gm::kSaveMenuFileslotWidth - 1]]),
             tag + "bar top-right is file row 0");
    check_eq(static_cast<long long>(at(with, gm::kSaveMenuFileslotX,
                                        bar_y(0) + gm::kSaveMenuFileslotHeight - 1)),
             static_cast<long long>(bar.palette[bar.rows[static_cast<std::size_t>(
                 gm::kSaveMenuFileslotHeight - 1) * gm::kSaveMenuFileslotWidth]]),
             tag + "bar bottom-left is file row 9");

    // PLATE + bars: the bars are painted over the panel, the text is unchanged.
    gm::SaveMenuRenderOptions plate_bar;
    plate_bar.background = std::string(gm::kSaveMenuPanelName);
    const Frame plate_with = run(game, menu, plate_bar);
    gm::SaveMenuRenderOptions plate_without = plate_bar;
    plate_without.fileslot = false;
    const Frame plate_plain = run(game, menu, plate_without);
    check(plate_with.ok && plate_plain.ok, tag + "PLATE frames render");
    if (plate_with.ok && plate_plain.ok) {
        check_eq(plate_with.stats.ink_pixels, plate_plain.stats.ink_pixels,
                 tag + "PLATE: text ink unchanged by the bars");
        check_eq(plate_with.stats.fileslot_bands, gm::kSaveMenuRows,
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
        check_eq(static_cast<long long>(at(plate_with, gm::kSaveMenuFileslotX, bar_y(0))),
                 static_cast<long long>(bar.palette[bar.rows[0]]),
                 tag + "PLATE: the bar wins over the panel");
        std::printf("  save_menu_render fileslot PLATE: bars change %lld panel pixels "
                    "(vs the black-canvas bar diff %lld; some bar pixels equal the "
                    "panel colour)\n", plate_diff, total);
    }

    // A renamed FILESLOT entry is a text refusal; `fileslot = false` still
    // paints the same install (the documented escape for a partial install).
    const fs::path root = temp_root() / "no-fileslot";
    std::error_code ec;
    fs::remove_all(root, ec);
    std::string patch_error;
    const bool patched = archive_rename_entry(game, root, gm::kSaveMenuFileslotName,
                                              "FILESLOX", patch_error);
    check(patched, tag + "archive copy with FILESLOT renamed: " + patch_error);
    if (patched) {
        check_refused(root, menu, gm::SaveMenuRenderOptions{}, "no FILESLOT", "FILESLOT");
        const Frame patched_plain = run(root, menu, no_bar_options);
        check(patched_plain.ok, tag + "(--)fileslot on the patched archive: " +
                                     patched_plain.error);
        if (patched_plain.ok) {
            check_eq(differing_pixels(patched_plain.rgba, without.rgba), 0,
                     tag + "(--)fileslot is byte-identical without the asset");
        }
    }
    fs::remove_all(root, ec);

    std::printf("  save_menu_render fileslot: FILESLOT 124x10, 2318 bytes, 1240 px, "
                "14 indices, no index 0; (--)fileslot total %lld: %s\n",
                total, bar_report.c_str());
}

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::SaveMenu& menu,
                   const gm::SaveMenuRenderOptions& options, const std::string& what,
                   std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::SaveMenuRenderStats stats;
    stats.rows = 7;
    stats.scroll = 7;
    stats.selected_row = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.slot_rows = 7;
    stats.arrows = 7;
    stats.fileslot_bands = 7;
    std::string error;
    const bool ok =
        gm::render_save_menu(game, menu, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "save_menu_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error + "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.scroll, 0, what + ": scroll cleared");
    check_eq(stats.selected_row, 0, what + ": selected_row cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.slot_rows, 0, what + ": slot_rows cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
    check_eq(stats.fileslot_bands, 0, what + ": fileslot_bands cleared");
}

// --- exact fade of `0x682f90` at the palette-index level -------------------
//
// Slot 0 of the shipped save (mode `save`, `PLATE`): the renderer paints
// palette indices from the panel, `MENUARRW` and the `FONTS4` atlases.  The
// table is **built** from the installed `GAMEPAL` (no dump needed): level 11
// must reproduce the unfaded frame byte for byte, level 0 must blacken it, and
// 12..15 must be four different frames (the RGBA model of `pager_render` would
// alias all four to 11).  The dump, when present, is a second path to the same
// bytes.
void check_refused(const fs::path& game, const gm::SaveMenu& menu,
                   const gm::SaveMenuRenderOptions& options, const std::string& what,
                   std::string_view expected);

void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    gm::SaveMenu menu;
    menu.open(gm::SaveMenuMode::save, game);
    check_eq(static_cast<long long>(menu.slots().size()), 20, tag + "20 slots");

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
        gm::SaveMenuRenderOptions options;
        options.background = std::string(gm::kSaveMenuPanelName);
        options.fade_lut = true;
        options.fade_lut_level = level;
        options.fade_lut_table = &lut;
        frame = run(game, menu, options);
        check(frame.ok, tag + "level " + std::to_string(level) + " renders: " + frame.error);
        return frame.ok;
    };

    gm::SaveMenuRenderOptions plain_options;
    plain_options.background = std::string(gm::kSaveMenuPanelName);
    const Frame plain = run(game, menu, plain_options);
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
    check_eq(exact11.stats.slot_rows, plain.stats.slot_rows,
             tag + "level 11 slot_rows unchanged");

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
        gm::SaveMenuRenderOptions options = plain_options;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_dump = dump.string();
        const Frame loaded = run(game, menu, options);
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
    gm::SaveMenuRenderOptions none = plain_options;
    none.fade_lut = true;
    check_refused(game, menu, none, "fade without a table or a dump", "fade LUT");
    gm::SaveMenuRenderOptions missing = plain_options;
    missing.fade_lut = true;
    missing.fade_lut_dump =
        (fs::temp_directory_path() / "gm_save_menu_render_no_lut.bin").string();
    check_refused(game, menu, missing, "fade with a missing dump", "hud_lut");

    std::printf("  fade LUT: level 11 vs unfaded: %lld differing pixels; "
                "level 0: %d black pixels (unfaded %d), %lld differing pixels\n",
                identity_diffs, black0, black_plain, diff0);
    std::printf("  fade LUT level 12..15: adjacent diffs 12/13 %lld, 13/14 %lld, "
                "14/15 %lld; vs level 11: %lld/%lld/%lld/%lld px\n",
                pair12_13, pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);
}

void test_negatives(const fs::path& game) {
    gm::SaveMenu menu; // 20 empty views, no open() — a legal state to paint
    check_eq(static_cast<long long>(menu.slots().size()), 20,
             "negatives: the model always exposes 20 rows (the 0-row guard is a contract)");

    check_refused(fs::path{}, menu, gm::SaveMenuRenderOptions{}, "empty game path",
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
    check_refused(root / "game-without-bmp", menu, gm::SaveMenuRenderOptions{},
                  "no BMP.IDX", "BMP.IDX");

    // A valid BIGFILE index whose only entry is FONTS1: FONTS4 (font 0) is
    // missing from it.
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
    check_refused(root / "game-without-font4", menu, gm::SaveMenuRenderOptions{},
                  "missing FONTS4", "FONTS4");

    fs::remove_all(root, ec);

    if (game.empty()) {
        return;
    }

    // A background name that is not in the archive is a text, not black.
    gm::SaveMenuRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, menu, bad_background, "unknown background", "NOSUCH");

    // The same menu, but with a real game directory, paints 20 empty slots:
    // a closed model is a legitimate frame (all `::: empty slot :::`).
    const Frame frame = run(game, menu, gm::SaveMenuRenderOptions{});
    check(frame.ok, "negatives: a closed menu still renders: " + frame.error);
    if (frame.ok) {
        check_eq(frame.stats.rows, gm::kSaveMenuRows, "negatives: six empty rows");
        check_eq(frame.stats.slot_rows, 0, "negatives: no occupied rows");
        check_eq(frame.stats.fileslot_bands, gm::kSaveMenuRows,
                 "negatives: six FILESLOT bars");
        for (int row = 0; row < gm::kSaveMenuRows; ++row) {
            check_eq(std::string(menu.label(row)), std::string(gm::SaveMenu::kEmptyLabel),
                     "negatives: row " + std::to_string(row) + " is the empty label");
        }
        std::cout << "save_menu_render negatives: closed menu rows=" << frame.stats.rows
                  << " slot_rows=0 ink=" << frame.stats.ink_pixels << "\n";
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
        std::cout << "save_menu_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kSaveMenuRowFont, gm::kSaveMenuSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "save_menu_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    const BandAsset bar = load_band_asset(game, gm::kSaveMenuFileslotName);

    test_install_frame(game, fonts, bar);
    test_window(game, fonts, bar);
    test_arrows(game);
    test_highlight(game, fonts);
    test_names(game, fonts);
    test_plate(game);
    test_fileslot(game, fonts, bar);
    test_fade_lut(game);
    test_negatives(game);

    if (failures != 0) {
        std::cerr << "save_menu_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "save_menu_render: ok (" << checks
              << " checks) screen 14 -> 320x200 RGBA (6 rows at y 28 step 12 x 42, FILESLOT "
                 "124x10 at (38, 27 + 12*i), cursor font 4, ESC 38/101, arrows, names, "
                 "PLATE, negatives)\n";
    return 0;
}
