// One frame of screen 6, `RESTART GAME` (`M8.8` slice 6, `ui/restart_render`,
// docs/RESTART_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there
// is no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.8
// and docs/GAME_OVER.md §4.1 from the builder `0x488850` and from pixels:
//
//   * the shipped install: for `items` 2, 3 and 4 the frame has exactly that
//     many rows (`rows`), no missing glyph, no arrow, 320x200 RGBA, every pixel
//     opaque; the title `RESTART GAME` (font 5) is centred at y 14 and the rows
//     sit at x 10, y = 27 + 16*item (27, 43, 59, 75) with the count model of the
//     header (`Reload`/`R.P.V.`/`HQ` prefix + `Quit to title`); every one of the
//     text runs is compared byte for byte with an independent blit and the
//     bounding boxes are printed;
//   * `ink` is the sum of the independent band inks (the bands do not overlap);
//   * the cursor 0..items-1: each frame equals an independent composite with
//     font 4 on that row and differs from the no-highlight frame only inside the
//     involved bands; the frames are pairwise different; out-of-range values
//     (-1, 99) highlight nothing;
//   * `items` outside 2..4 is clamped (1/0 -> 2, 5/99 -> 4) and the clamp is
//     visible in the pixels and in `rows`;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel under the glyph mask; everything outside stays
//     black, the text ink is unchanged, `menu_panel_asset(Screen::Restart)` is
//     `PLATE`; the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames, the canvas outside the
//     PLATE band stays black, and the negative paths are `restart_render:` texts;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a
//     `BMP.IDX` without the `FONTS4` atlas (the title face is loaded first) and
//     a missing background entry; two renders are deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/RESTART_RENDER.md §4 pins them.

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
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "ui/frontend_menu.h"
#include "ui/menu_render.h"
#include "ui/restart_render.h"

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
    gm::RestartRenderStats stats;
};

Frame run(const fs::path& game, const gm::RestartRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_restart(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kRestartBackground; }

/// `#000` with any alpha: the faded palette entries of level 0 are black too,
/// but their alpha is the forced 255 and their RGB is what counts here.
bool is_rgb_black(std::uint32_t pixel) { return (pixel >> 8u) == 0u; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Does any pixel of the rows `y0..y1` have exactly `colour`?  The faces of
/// `FONTS4` paint a fixed palette index per face (title 5, rows 0, highlight 4),
/// so the presence of that index is a cheap colour check of the band.
bool has_colour(const Frame& frame, int y0, int y1, std::uint32_t colour) {
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (at(frame, x, y) == colour) {
                return true;
            }
        }
    }
    return false;
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

std::string box_text(const Box& box) {
    if (box.empty()) {
        return "empty";
    }
    return "(" + std::to_string(box.x0) + "," + std::to_string(box.y0) + ")-(" +
           std::to_string(box.x1) + "," + std::to_string(box.y1) + ") " +
           std::to_string(box.width()) + "x" + std::to_string(box.height()) + ", " +
           std::to_string(box.count) + " px";
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

/// Sum of the atlas advances of a run: what `0x485dd0` centres with
/// (`0x47fa50`).
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

/// Compare the rows `y..y+height-1` of the band `0..196` of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kRestartPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kRestartBackground : want;
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

/// The centring of `0x485dd0`'s `centered` branch: title x inside 196 px.
int title_x(const gm::FontAtlas& face) {
    return std::max(0, (gm::kRestartPanelWidth - pen_width(face, gm::kRestartTitle)) / 2);
}

/// The font of a row for a given cursor (the module's rule: exact `item ==
/// cursor`, so an out-of-range cursor selects the regular face for every row).
const gm::FontAtlas& face_of(const std::map<int, gm::FontAtlas>& fonts, int cursor,
                             int item) {
    return fonts.at(item == cursor ? gm::kRestartSelectedFont : gm::kRestartRowFont);
}

/// One `FRONTEND/BMP.IDX` entry as the blitters see it: the index bytes in
/// *file* order (`0x47efe0` freads `width*height` bytes) plus the palette.
struct SpriteAsset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rows;
    std::vector<std::uint32_t> palette;
};

SpriteAsset load_sprite_asset(const fs::path& game, const std::string& name) {
    SpriteAsset asset;
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
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

/// The `PLATE` panel as `parse_bmp` returns it (top-down rows, RGBA palette).
///
/// `SpriteAsset::rows` are the BMP pixel bytes in *file* order; the standard BMP
/// file stores its rows bottom-up, so `parse_bmp` row `y` is file row
/// `height-1-y` (docs/START_GAME_RENDER.md §1).
std::vector<std::uint32_t> plate_pixels(const SpriteAsset& plate) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(plate.width) *
                                       static_cast<std::size_t>(plate.height),
                                   0u);
    for (int y = 0; y < plate.height; ++y) {
        const int source_row = plate.height - 1 - y;
        for (int x = 0; x < plate.width; ++x) {
            const std::uint8_t index =
                plate.rows[static_cast<std::size_t>(source_row) *
                               static_cast<std::size_t>(plate.width) +
                           static_cast<std::size_t>(x)];
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(plate.width) +
                static_cast<std::size_t>(x)] = plate.palette[index];
        }
    }
    return out;
}

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_restart_render";
    }
    return fs::temp_directory_path() / "gm_restart_render";
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

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::RestartRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::RestartRenderStats stats;
    stats.rows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok = gm::render_restart(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "restart_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error + "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- labels and clamps -------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";
    // The count model of the header: prefix of the gated labels + Quit to title.
    check_eq(std::string(gm::restart_item_label(0, 2)),
             std::string(gm::kRestartReloadLastSaved), tag + "items 2 row 0 is Reload");
    check_eq(std::string(gm::restart_item_label(1, 2)),
             std::string(gm::kRestartQuitToTitle), tag + "items 2 row 1 is Quit");
    check_eq(std::string(gm::restart_item_label(0, 3)),
             std::string(gm::kRestartReloadLastSaved), tag + "items 3 row 0 is Reload");
    check_eq(std::string(gm::restart_item_label(1, 3)), std::string(gm::kRestartSameRpv),
             tag + "items 3 row 1 is R.P.V.");
    check_eq(std::string(gm::restart_item_label(2, 3)),
             std::string(gm::kRestartQuitToTitle), tag + "items 3 row 2 is Quit");
    check_eq(std::string(gm::restart_item_label(0, 4)),
             std::string(gm::kRestartReloadLastSaved), tag + "items 4 row 0 is Reload");
    check_eq(std::string(gm::restart_item_label(1, 4)), std::string(gm::kRestartSameRpv),
             tag + "items 4 row 1 is R.P.V.");
    check_eq(std::string(gm::restart_item_label(2, 4)), std::string(gm::kRestartFromHq),
             tag + "items 4 row 2 is HQ");
    check_eq(std::string(gm::restart_item_label(3, 4)),
             std::string(gm::kRestartQuitToTitle), tag + "items 4 row 3 is Quit");
    // The three labels are the handler's VAs 0x4bd44c/0x4bd42c/0x4bd3fc/0x4bd3ec.
    check_eq(std::string(gm::kRestartTitle), std::string("RESTART GAME"),
             tag + "title is 0x4bd464");
    check_eq(std::string(gm::kRestartSameRpv),
             std::string("Restart level with same R.P.V."), tag + "label 0x4bd42c");
    check_eq(std::string(gm::kRestartFromHq), std::string("Restart level from HQ"),
             tag + "label 0x4bd3fc");
    check_eq(std::string(gm::kRestartQuitToTitle), std::string("Quit to title"),
             tag + "label 0x4bd3ec");

    // `0x67f10c` is a runtime count clamped to the proven 2..4.
    check_eq(gm::restart_item_count(-7), 2, tag + "items -7 clamps to 2");
    check_eq(gm::restart_item_count(0), 2, tag + "items 0 clamps to 2");
    check_eq(gm::restart_item_count(1), 2, tag + "items 1 clamps to 2");
    check_eq(gm::restart_item_count(2), 2, tag + "items 2 stays 2");
    check_eq(gm::restart_item_count(3), 3, tag + "items 3 stays 3");
    check_eq(gm::restart_item_count(4), 4, tag + "items 4 stays 4");
    check_eq(gm::restart_item_count(5), 4, tag + "items 5 clamps to 4");
    check_eq(gm::restart_item_count(99), 4, tag + "items 99 clamps to 4");

    // The geometry constants are the handler's immediates.
    check_eq(gm::kRestartItemStep, 16, tag + "step 16 (0x488867)");
    check_eq(gm::kRestartItemX, 10, tag + "x 10 (push 0xa)");
    check_eq(gm::kRestartItemFirstY, 27, tag + "base row 27 (push 0x1b)");
    check_eq(gm::kRestartTitleY, 14, tag + "title row 14 (push 0xe)");
    check_eq(gm::restart_item_y(0), 27, tag + "row 0 y 27");
    check_eq(gm::restart_item_y(1), 43, tag + "row 1 y 43");
    check_eq(gm::restart_item_y(2), 59, tag + "row 2 y 59");
    check_eq(gm::restart_item_y(3), 75, tag + "row 3 y 75");
    check_eq(gm::kRestartTitleFont, 5, tag + "title face 5");
    check_eq(gm::kRestartRowFont, 0, tag + "row face 0");
    check_eq(gm::kRestartSelectedFont, 4, tag + "highlight face 4");
    check_eq(gm::kRestartPanelWidth, 196, tag + "panel 196 wide");
    check_eq(gm::kRestartPanelHeight, 131, tag + "panel 131 high");

    // The sibling snapshot of the same handler agrees: step 16, PLATE, a title.
    check_eq(gm::menu_row_step(gm::Screen::Restart), 16, tag + "menu_row_step(Restart) 16");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::Restart)),
             std::string(gm::kRestartPanelName),
             tag + "menu_panel_asset(Screen::Restart) is PLATE");
    check(gm::menu_has_title(gm::Screen::Restart), tag + "menu_has_title(Restart)");
}

// --- the installed frames ----------------------------------------------------
/// `FONTS4` faces of docs/MENU_RENDER.md §3: title 5 -> (155,15,15), rows 0 ->
/// (19,15,15), highlight 4 -> (239,239,239), alpha forced opaque.
constexpr std::uint32_t kTitleInk = 0x9b0f0fFFu;
constexpr std::uint32_t kRowInk = 0x130f0fFFu;
constexpr std::uint32_t kHighlightInk = 0xefefefffu;

void test_counts(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const gm::FontAtlas& title_face = fonts.at(gm::kRestartTitleFont);
    const int tx = title_x(title_face);
    for (int items = gm::kRestartMinItems; items <= gm::kRestartMaxItems; ++items) {
        const std::string tag = "items " + std::to_string(items) + ": ";
        gm::RestartRenderOptions options;
        options.items = items;
        options.cursor = -1; // no highlight: the geometry bands are the plain face
        const Frame frame = run(game, options);
        check(frame.ok, tag + "frame renders: " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.width, gm::kRestartCanvasWidth, tag + "width 320");
        check_eq(frame.height, gm::kRestartCanvasHeight, tag + "height 200");
        check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
        check_eq(frame.stats.rows, items, tag + "rows is the item count");
        check_eq(frame.stats.arrows, 0, tag + "no MENUARRW arrows on screen 6");
        check_eq(frame.stats.missing_glyphs, 0, tag + "no missing glyph");
        check(frame.stats.ink_pixels > 0, tag + "ink > 0");
        for (const std::uint32_t pixel : frame.rgba) {
            if ((pixel & 0xFFu) != 0xFFu) {
                check(false, tag + "every pixel is opaque");
                break;
            }
        }

        check_band(frame, title_face, gm::kRestartTitle, tx, gm::kRestartTitleY,
                   tag + "title");
        const Box title = ink_box(frame, gm::kRestartTitleY,
                                  gm::kRestartTitleY + title_face.cell_height - 1);
        check(!title.empty(), tag + "title has ink");
        check_eq(title.y0, gm::kRestartTitleY, tag + "title top row 14");
        check_eq(title.x0, tx, tag + "title x is the centring of 0x485dd0");
        check_eq(title.count, band_ink(compose_band(title_face, gm::kRestartTitle, tx,
                                                    gm::kRestartPanelWidth,
                                                    title_face.cell_height)),
                 tag + "title ink is the independent blit");
        check(has_colour(frame, gm::kRestartTitleY,
                         gm::kRestartTitleY + title_face.cell_height - 1, kTitleInk),
              tag + "title uses the font 5 ink (155,15,15)");

        long long bands_ink = band_ink(compose_band(title_face, gm::kRestartTitle, tx,
                                                    gm::kRestartPanelWidth,
                                                    title_face.cell_height));
        std::string boxes;
        for (int item = 0; item < items; ++item) {
            const std::string_view text = gm::restart_item_label(item, items);
            const int y = gm::restart_item_y(item);
            check_eq(y, gm::kRestartItemFirstY + gm::kRestartItemStep * item,
                     tag + "row " + std::to_string(item) + " y");
            check_band(frame, fonts.at(gm::kRestartRowFont), text, gm::kRestartItemX, y,
                       tag + "row " + std::to_string(item) + " (" + std::string(text) + ")");
            bands_ink += band_ink(compose_band(fonts.at(gm::kRestartRowFont), text,
                                                gm::kRestartItemX, gm::kRestartPanelWidth,
                                                fonts.at(gm::kRestartRowFont).cell_height));
            const Box box =
                ink_box(frame, y, y + fonts.at(gm::kRestartRowFont).cell_height - 1);
            check(!box.empty(), tag + "row " + std::to_string(item) + " has ink");
            check_eq(box.y0, y,
                     tag + "row " + std::to_string(item) + " top " + std::to_string(y));
            check(has_colour(frame, y, y + fonts.at(gm::kRestartRowFont).cell_height - 1,
                             kRowInk),
                  tag + "row " + std::to_string(item) + " uses the font 0 ink (19,15,15)");
            if (!boxes.empty()) {
                boxes += "; ";
            }
            boxes += std::to_string(item) + " " + std::string(text) + " " + box_text(box);
        }
        check_eq(frame.stats.ink_pixels, bands_ink,
                 tag + "ink is the sum of the independent bands");

        // The row bands do not overlap: 16 px step vs an 8 px cell.
        const int cell = fonts.at(gm::kRestartRowFont).cell_height;
        check(cell <= gm::kRestartItemStep, tag + "cell " + std::to_string(cell) +
                                                " fits the 16 px step");
        const Box last = ink_box(frame, gm::restart_item_y(items - 1),
                                 gm::restart_item_y(items - 1) + cell - 1);
        check(!last.empty(), tag + "last row is inside the 131 px band");

        const Frame again = run(game, options);
        check(again.ok, tag + "second render");
        check_eq(differing_pixels(frame.rgba, again.rgba), 0, tag + "deterministic");

        std::printf("  restart_render items=%d: rows=%d ink=%d arrows=%d missing=%d; "
                    "title y%d %s; %s\n",
                    items, frame.stats.rows, frame.stats.ink_pixels, frame.stats.arrows,
                    frame.stats.missing_glyphs, gm::kRestartTitleY,
                    box_text(title).c_str(), boxes.c_str());
    }

    // Clamping is visible in the pixels: 1 -> 2 and 5 -> 4.
    gm::RestartRenderOptions low;
    low.items = 1;
    low.cursor = -1;
    const Frame clamped_low = run(game, low);
    gm::RestartRenderOptions base2;
    base2.items = 2;
    base2.cursor = -1;
    const Frame frame2 = run(game, base2);
    check(clamped_low.ok && frame2.ok, "clamp: items 1 and 2 render");
    if (clamped_low.ok && frame2.ok) {
        check_eq(clamped_low.stats.rows, 2, "clamp: items 1 -> rows 2");
        check_eq(differing_pixels(clamped_low.rgba, frame2.rgba), 0,
                 "clamp: items 1 paints the items 2 frame");
    }
    gm::RestartRenderOptions high;
    high.items = 99;
    high.cursor = -1;
    const Frame clamped_high = run(game, high);
    gm::RestartRenderOptions base4;
    base4.items = 4;
    base4.cursor = -1;
    const Frame frame4 = run(game, base4);
    check(clamped_high.ok && frame4.ok, "clamp: items 99 and 4 render");
    if (clamped_high.ok && frame4.ok) {
        check_eq(clamped_high.stats.rows, 4, "clamp: items 99 -> rows 4");
        check_eq(differing_pixels(clamped_high.rgba, frame4.rgba), 0,
                 "clamp: items 99 paints the items 4 frame");
    }
    std::printf("  restart_render clamp: items 1 == items 2 (%d rows), items 99 == "
                "items 4 (%d rows)\n",
                clamped_low.ok ? clamped_low.stats.rows : -1,
                clamped_high.ok ? clamped_high.stats.rows : -1);
}

// --- the cursor --------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    const gm::FontAtlas& title_face = fonts.at(gm::kRestartTitleFont);
    long long min_pair = -1;
    for (int items = gm::kRestartMinItems; items <= gm::kRestartMaxItems; ++items) {
        const std::string count_tag = tag + "items " + std::to_string(items) + ": ";
        gm::RestartRenderOptions plain_options;
        plain_options.items = items;
        plain_options.cursor = -1;
        const Frame plain = run(game, plain_options);
        check(plain.ok, count_tag + "no-highlight frame");
        if (!plain.ok) {
            return;
        }

        std::vector<Frame> frames;
        std::string report;
        for (int cursor = 0; cursor < items; ++cursor) {
            gm::RestartRenderOptions options;
            options.items = items;
            options.cursor = cursor;
            const Frame frame = run(game, options);
            check(frame.ok, count_tag + "cursor " + std::to_string(cursor) + ": " +
                                 frame.error);
            if (!frame.ok) {
                return;
            }
            check_eq(frame.stats.rows, items,
                     count_tag + "cursor " + std::to_string(cursor) + ": rows");
            check_eq(frame.stats.arrows, 0,
                     count_tag + "cursor " + std::to_string(cursor) + ": no arrows");
            check_eq(frame.stats.ink_pixels, plain.stats.ink_pixels,
                     count_tag + "cursor " + std::to_string(cursor) + ": ink unchanged");

            // The highlighted row is the font 4 blit.
            const std::string_view text = gm::restart_item_label(cursor, items);
            const int y = gm::restart_item_y(cursor);
            check_band(frame, fonts.at(gm::kRestartSelectedFont), text,
                       gm::kRestartItemX, y,
                       count_tag + "cursor " + std::to_string(cursor) + " row " +
                           std::string(text));
            check_band(plain, fonts.at(gm::kRestartRowFont), text, gm::kRestartItemX, y,
                       count_tag + "cursor " + std::to_string(cursor) +
                           " plain row is font 0");
            check(has_colour(frame, y, y + fonts.at(gm::kRestartSelectedFont).cell_height - 1,
                             kHighlightInk),
                  count_tag + "cursor " + std::to_string(cursor) +
                      " uses the font 4 ink (239,239,239)");

            // Differences from the no-highlight frame are limited to that row.
            long long diff_plain = 0;
            long long outside = 0;
            const int cell = fonts.at(gm::kRestartSelectedFont).cell_height;
            for (int py = 0; py < frame.height; ++py) {
                for (int px = 0; px < frame.width; ++px) {
                    if (at(frame, px, py) == at(plain, px, py)) {
                        continue;
                    }
                    ++diff_plain;
                    if (py < y || py >= y + cell) {
                        ++outside;
                    }
                }
            }
            check_eq(outside, 0, count_tag + "cursor " + std::to_string(cursor) +
                                     ": changes only the selected row band");
            check(diff_plain > 0, count_tag + "cursor " + std::to_string(cursor) +
                                      ": differs from the no-highlight frame");

            // The whole canvas is the title + the rows with font 4 on `cursor`.
            Frame expect;
            expect.width = gm::kRestartCanvasWidth;
            expect.height = gm::kRestartCanvasHeight;
            expect.rgba.assign(static_cast<std::size_t>(expect.width) *
                                   static_cast<std::size_t>(expect.height),
                               gm::kRestartBackground);
            const auto paint = [&expect](const gm::FontAtlas& face, std::string_view text,
                                         int x, int y) {
                const std::vector<std::uint32_t> band =
                    compose_band(face, text, x, gm::kRestartPanelWidth, face.cell_height);
                for (int row = 0; row < face.cell_height; ++row) {
                    for (int column = 0; column < gm::kRestartPanelWidth; ++column) {
                        const std::uint32_t pixel =
                            band[static_cast<std::size_t>(row) *
                                     static_cast<std::size_t>(gm::kRestartPanelWidth) +
                                 static_cast<std::size_t>(column)];
                        if (pixel != 0) {
                            expect.rgba[static_cast<std::size_t>(y + row) *
                                            static_cast<std::size_t>(expect.width) +
                                        static_cast<std::size_t>(column)] = pixel;
                        }
                    }
                }
            };
            paint(title_face, gm::kRestartTitle, title_x(title_face),
                  gm::kRestartTitleY);
            for (int item = 0; item < items; ++item) {
                paint(face_of(fonts, cursor, item), gm::restart_item_label(item, items),
                      gm::kRestartItemX, gm::restart_item_y(item));
            }
            check_eq(differing_pixels(frame.rgba, expect.rgba), 0,
                     count_tag + "cursor " + std::to_string(cursor) +
                         ": whole canvas matches the independent composite");

            if (!report.empty()) {
                report += ", ";
            }
            report += "c" + std::to_string(cursor) + ": " + std::to_string(diff_plain) +
                      " px";
            frames.push_back(frame);
        }

        long long local_min = -1;
        for (std::size_t a = 0; a < frames.size(); ++a) {
            for (std::size_t b = a + 1; b < frames.size(); ++b) {
                const long long diff = differing_pixels(frames[a].rgba, frames[b].rgba);
                check(diff > 0, count_tag + "frames " + std::to_string(a) + "/" +
                                    std::to_string(b) + " differ");
                if (local_min < 0 || diff < local_min) {
                    local_min = diff;
                }
            }
        }
        if (min_pair < 0 || local_min < min_pair) {
            min_pair = local_min;
        }

        // Out-of-range cursors highlight nothing: the exact comparison of
        // `0x485e0d` never matches.
        gm::RestartRenderOptions low;
        low.items = items;
        low.cursor = -1;
        const Frame none = run(game, low);
        gm::RestartRenderOptions high;
        high.items = items;
        high.cursor = 99;
        const Frame none99 = run(game, high);
        check(none.ok && none99.ok, count_tag + "cursor -1/99 render");
        if (none.ok && none99.ok) {
            check_eq(differing_pixels(none.rgba, none99.rgba), 0,
                     count_tag + "cursor -1 and 99 paint the same no-highlight frame");
            check(differing_pixels(none.rgba, frames[0].rgba) > 0,
                  count_tag + "no-highlight frame differs from cursor 0");
        }
        std::printf("  restart_render cursor items=%d: %s; min pairwise diff %lld px; "
                    "-1/99 highlight nothing\n",
                    items, report.c_str(), local_min);
    }
    std::printf("  restart_render cursor: overall min pairwise diff %lld px\n", min_pair);
}

// --- PLATE -------------------------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "plate: ";
    gm::RestartRenderOptions plain_options;
    plain_options.items = gm::kRestartMaxItems;
    plain_options.cursor = -1;
    const Frame black = run(game, plain_options);
    const SpriteAsset plate = load_sprite_asset(game, std::string(gm::kRestartPanelName));
    if (!black.ok || !plate.ok) {
        std::cout << "  plate: SKIP (no PLATE in BMP.IDX)\n";
        return;
    }
    check_eq(plate.width, gm::kRestartPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kRestartPanelHeight, tag + "PLATE 131 high");

    gm::RestartRenderOptions options = plain_options;
    options.background = std::string(gm::kRestartPanelName);
    const Frame frame = run(game, options);
    check(frame.ok, tag + "renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels, black.stats.ink_pixels,
             tag + "text ink does not change");
    check_eq(frame.stats.rows, black.stats.rows, tag + "rows do not change");
    check_eq(frame.stats.arrows, 0, tag + "no arrows");

    // Glyph mask: every pixel any band writes.
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(frame.width) *
                                       static_cast<std::size_t>(frame.height),
                                   0);
    const auto mark = [&mask, &frame](const gm::FontAtlas& face, std::string_view text,
                                      int x, int y) {
        const std::vector<std::uint32_t> band =
            compose_band(face, text, x, gm::kRestartPanelWidth, face.cell_height);
        for (int row = 0; row < face.cell_height; ++row) {
            for (int column = 0; column < gm::kRestartPanelWidth; ++column) {
                if (band[static_cast<std::size_t>(row) *
                             static_cast<std::size_t>(gm::kRestartPanelWidth) +
                         static_cast<std::size_t>(column)] != 0) {
                    mask[static_cast<std::size_t>(y + row) *
                             static_cast<std::size_t>(frame.width) +
                         static_cast<std::size_t>(column)] = 1;
                }
            }
        }
    };
    const gm::FontAtlas& title_face = fonts.at(gm::kRestartTitleFont);
    mark(title_face, gm::kRestartTitle, title_x(title_face), gm::kRestartTitleY);
    for (int item = 0; item < gm::kRestartMaxItems; ++item) {
        mark(fonts.at(gm::kRestartRowFont), gm::restart_item_label(item, gm::kRestartMaxItems),
             gm::kRestartItemX, gm::restart_item_y(item));
    }

    const std::vector<std::uint32_t> panel = plate_pixels(plate);
    long long panel_in = 0;
    long long panel_ink = 0;
    long long outside = 0;
    long long wrong = 0;
    int first_x = -1;
    int first_y = -1;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t got = at(frame, x, y);
            if (x >= plate.width || y >= plate.height) {
                ++outside;
                wrong += is_black(got) ? 0 : 1;
                continue;
            }
            ++panel_in;
            const std::size_t index = static_cast<std::size_t>(y) *
                                          static_cast<std::size_t>(plate.width) +
                                      static_cast<std::size_t>(x);
            if (mask[static_cast<std::size_t>(y) *
                         static_cast<std::size_t>(frame.width) +
                     static_cast<std::size_t>(x)] != 0) {
                panel_ink += 1;
                wrong += got == at(black, x, y) ? 0 : 1;
            } else {
                wrong += got == panel[index] ? 0 : 1;
            }
            if (wrong != 0 && first_x < 0) {
                first_x = x;
                first_y = y;
            }
        }
    }
    check_eq(outside, static_cast<long long>(frame.width) * frame.height -
                          static_cast<long long>(plate.width) * plate.height,
             tag + "38324 pixels outside the panel");
    check_eq(wrong, 0, tag + "panel matches the independent decode under the glyph "
                            "mask (first " + std::to_string(first_x) + "," +
                            std::to_string(first_y) + ")");
    check_eq(panel_ink, black.stats.ink_pixels,
             tag + "the glyph pixels over the panel are the black-frame ink");

    const long long diff = differing_pixels(frame.rgba, black.rgba);
    check(diff > 0, tag + "PLATE changes the frame");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::Restart)),
             std::string(gm::kRestartPanelName),
             tag + "menu_panel_asset(Screen::Restart) is PLATE");

    std::printf("  restart_render plate: PLATE %dx%d, panel %lld px, %lld under "
                "glyphs, %lld outside black; plate-vs-black diff %lld px\n",
                plate.width, plate.height, panel_in, panel_ink, outside, diff);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kRestartCanvasWidth) *
                                  gm::kRestartCanvasHeight;

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

    const auto make = [&](bool fade, int level) {
        gm::RestartRenderOptions options;
        options.background = std::string(gm::kRestartPanelName);
        options.items = gm::kRestartMaxItems;
        options.cursor = -1;
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, options);
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
                if (x < gm::kRestartPanelWidth && y < gm::kRestartPanelHeight) {
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

    std::printf("  restart_render fade: level 11 diffs %lld/%lld; level 0 black "
                "%lld (plain %lld), diffs %lld; adjacent 12/13 %lld, 13/14 %lld, "
                "14/15 %lld; vs 11 %lld/%lld/%lld/%lld\n",
                identity_diffs, kCanvas, black0, black_plain, diff0, pair12_13,
                pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);

    // 4. Negatives: no table and no dump, an unreadable dump, and the table
    // winning over the dump.
    {
        gm::RestartRenderOptions options;
        options.background = std::string(gm::kRestartPanelName);
        options.fade_lut = true;
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "no table and no dump refused");
        check(starts_with(refusal.error, "restart_render: ") &&
                  contains(refusal.error, "fade LUT needs"),
              tag + "refusal names the missing table: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "refusal clears the buffer");
        check_eq(refusal.width, 0, tag + "refusal zeroes the width");
        check_eq(refusal.height, 0, tag + "refusal zeroes the height");
        check_eq(refusal.stats.ink_pixels, 0, tag + "refusal zeroes the stats");
    }
    {
        gm::RestartRenderOptions options;
        options.background = std::string(gm::kRestartPanelName);
        options.fade_lut = true;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "missing dump refused");
        check(starts_with(refusal.error, "restart_render: ") &&
                  contains(refusal.error, "hud_lut:"),
              tag + "missing dump carries the hud_lut text: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "missing dump clears the buffer");
    }
    {
        gm::RestartRenderOptions options;
        options.background = std::string(gm::kRestartPanelName);
        options.items = gm::kRestartMaxItems;
        options.cursor = -1;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame both = run(game, options);
        check(both.ok, tag + "a table wins over a missing dump: " + both.error);
        if (both.ok) {
            check_eq(differing_pixels(both.rgba, plain.rgba), 0,
                     tag + "table path equals the unfaded level 11 frame");
        }
    }
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::RestartRenderOptions{}, "empty game path",
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
    check_refused(root / "game-without-bmp", gm::RestartRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

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
    check_refused(root / "game-without-font4", gm::RestartRenderOptions{},
                  "missing FONTS4 (the face atlas)", "FONTS4");

    fs::remove_all(root, ec);

    if (game.empty()) {
        return;
    }

    // A background name that is not in the archive is a text, not black.
    gm::RestartRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // The shipped install renders the API default: two rows, cursor 0.
    const Frame frame = run(game, gm::RestartRenderOptions{});
    check(frame.ok, "negatives: shipped install renders: " + frame.error);
    if (frame.ok) {
        check_eq(frame.stats.rows, gm::kRestartMinItems, "negatives: rows 2");
        check_eq(frame.stats.arrows, 0, "negatives: no arrows");
        std::printf("  restart_render negatives: rows=%d ink=%d arrows=%d missing=%d\n",
                    frame.stats.rows, frame.stats.ink_pixels, frame.stats.arrows,
                    frame.stats.missing_glyphs);
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
        std::cout << "restart_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kRestartTitleFont, gm::kRestartRowFont,
                             gm::kRestartSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "restart_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_counts(game, fonts);
    test_cursor(game, fonts);
    test_plate(game, fonts);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "restart_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "restart_render: ok (" << checks
              << " checks) screen 6 -> 320x200 RGBA (title y 14, rows x 10 y 27 step "
                 "16, 2..4 gated rows, cursor font 4, no arrows, PLATE, exact fade, "
                 "negatives)\n";
    return 0;
}
