// One frame of screen 33, `QUIT GAME?` / `QUIT OPTIONS` (`M8.8` slice 4,
// `ui/quit_render`, docs/QUIT_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there
// is no state object.  The test pins the geometry of docs/UI_SCREENS.md §2 from
// the handler `0x488780` and from pixels, not from the code:
//
//   * the shipped install, both variants: outside the game `rows == 2` with
//     `Yes`/`No`, in game `rows == 3` with `Quit Game`/`Quit To Title`/
//     `Return To Game`; 320x200 RGBA, every pixel opaque, no missing glyph and
//     `arrows == 0` (screen 33 has no `MENUARRW` call);
//   * the titles `QUIT GAME?`/`QUIT OPTIONS` (font 5) centred at y 14, the item
//     bands at (38, 27 + 16*i) with font 0 and the `cursor` item in font 4;
//     every band is compared byte for byte with an independent blit, and the
//     bounding boxes of the title, the first and the last item are printed;
//   * `ink` is the sum of the independent band inks (the bands do not overlap);
//   * the cursor 0..1 / 0..2: each frame equals an independent composite with
//     font 4 on that row and differs from the plain frame only inside the two
//     involved row bands; the frames are pairwise different, and a cursor
//     outside the variant's range highlights nothing (the frame equals the
//     all-font-0 composite);
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel under the glyph mask; everything outside stays
//     black, the text ink is unchanged, `menu_panel_asset(Screen::Quit)` is
//     `PLATE`; the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames, the canvas outside the
//     PLATE band stays black, and the negative paths are `quit_render:` texts;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a
//     `BMP.IDX` without the `FONTS4` atlas (the title face is loaded first), a
//     missing background entry, and two renders that are deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/QUIT_RENDER.md §4 pins them.

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
#include "ui/quit_render.h"

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
    gm::QuitRenderStats stats;
};

Frame run(const fs::path& game, const gm::QuitRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_quit(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kQuitBackground; }

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
/// `r` at `y + r`, palette index 0 transparent, the atlas palette forced
/// opaque.  0 marks an untouched pixel.
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
    const int width = gm::kQuitPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kQuitBackground : want;
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

int item_y(int item) { return gm::quit_item_y(item); }

/// The centring of `0x485dd0`'s `centered` branch: title x inside 196 px.
int title_x(const gm::FontAtlas& face, bool in_game) {
    return std::max(0, (gm::kQuitPanelWidth - pen_width(face, gm::quit_title(in_game))) / 2);
}

/// The font of row `item` for a given cursor (the module's rule).
const gm::FontAtlas& face_of(const std::map<int, gm::FontAtlas>& fonts, int cursor, int item) {
    return fonts.at(item == cursor ? gm::kQuitSelectedFont : gm::kQuitRowFont);
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
        return approved / "gm_quit_render";
    }
    return fs::temp_directory_path() / "gm_quit_render";
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

/// The whole canvas as an independent composite: black, the title at its
/// centred x and every item band with `face_of(cursor, item)`.
std::vector<std::uint32_t> composite(const std::map<int, gm::FontAtlas>& fonts,
                                     bool in_game, int cursor) {
    std::vector<std::uint32_t> canvas(static_cast<std::size_t>(gm::kQuitCanvasWidth) *
                                          static_cast<std::size_t>(gm::kQuitCanvasHeight),
                                      gm::kQuitBackground);
    const auto paint = [&canvas](const gm::FontAtlas& face, std::string_view text,
                                 int x, int y) {
        const std::vector<std::uint32_t> band =
            compose_band(face, text, x, gm::kQuitPanelWidth, face.cell_height);
        for (int row = 0; row < face.cell_height; ++row) {
            for (int column = 0; column < gm::kQuitPanelWidth; ++column) {
                const std::uint32_t pixel =
                    band[static_cast<std::size_t>(row) *
                             static_cast<std::size_t>(gm::kQuitPanelWidth) +
                         static_cast<std::size_t>(column)];
                if (pixel != 0) {
                    canvas[static_cast<std::size_t>(y + row) *
                               static_cast<std::size_t>(gm::kQuitCanvasWidth) +
                           static_cast<std::size_t>(column)] = pixel;
                }
            }
        }
    };
    const gm::FontAtlas& title_face = fonts.at(gm::kQuitTitleFont);
    paint(title_face, gm::quit_title(in_game), title_x(title_face, in_game),
          gm::kQuitTitleY);
    for (int item = 0; item < gm::quit_item_count(in_game); ++item) {
        paint(face_of(fonts, cursor, item), gm::quit_item_label(in_game, item),
              gm::kQuitItemX, item_y(item));
    }
    return canvas;
}

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::QuitRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::QuitRenderStats stats;
    stats.rows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok = gm::render_quit(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "quit_render:"),
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

// --- the installed frame -----------------------------------------------------
void test_variant_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                        bool in_game) {
    const std::string tag = in_game ? "in game: " : "outside: ";
    const int expected_rows = gm::quit_item_count(in_game);
    const Frame frame = run(game, gm::QuitRenderOptions{.background = {}, .in_game = in_game,
                             .fade_lut_dump = {}, .fade_lut_table = nullptr});
    check(frame.ok, tag + "frame renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kQuitCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kQuitCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, expected_rows, tag + "rows");
    check_eq(frame.stats.arrows, 0, tag + "no MENUARRW arrows on screen 33");
    check_eq(frame.stats.missing_glyphs, 0, tag + "no missing glyph");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    for (const std::uint32_t pixel : frame.rgba) {
        if ((pixel & 0xFFu) != 0xFFu) {
            check(false, tag + "every pixel is opaque");
            break;
        }
    }

    const gm::FontAtlas& title_face = fonts.at(gm::kQuitTitleFont);
    const int tx = title_x(title_face, in_game);
    check_band(frame, title_face, gm::quit_title(in_game), tx, gm::kQuitTitleY,
               tag + "title");
    const Box title = ink_box(frame, gm::kQuitTitleY,
                              gm::kQuitTitleY + title_face.cell_height - 1);
    check(!title.empty(), tag + "title has ink");
    check_eq(title.y0, gm::kQuitTitleY, tag + "title top row 14");
    check_eq(title.x0, tx, tag + "title x is the centring of 0x485dd0");
    check_eq(title.count, band_ink(compose_band(title_face, gm::quit_title(in_game), tx,
                                                gm::kQuitPanelWidth,
                                                title_face.cell_height)),
             tag + "title ink is the independent blit");

    long long bands_ink = band_ink(compose_band(title_face, gm::quit_title(in_game), tx,
                                                gm::kQuitPanelWidth,
                                                title_face.cell_height));
    for (int item = 0; item < expected_rows; ++item) {
        const gm::FontAtlas& face = face_of(fonts, 0, item);
        check_band(frame, face, gm::quit_item_label(in_game, item), gm::kQuitItemX,
                   item_y(item), tag + "row " + std::to_string(item));
        bands_ink += band_ink(compose_band(face, gm::quit_item_label(in_game, item),
                                           gm::kQuitItemX, gm::kQuitPanelWidth,
                                           face.cell_height));
    }
    check_eq(frame.stats.ink_pixels, bands_ink,
             tag + "ink is the sum of the title and the item bands");

    const Box first = ink_box(frame, item_y(0), item_y(0) + fonts.at(0).cell_height - 1);
    const Box last = ink_box(frame, item_y(expected_rows - 1),
                             item_y(expected_rows - 1) + fonts.at(0).cell_height - 1);
    check(!first.empty(), tag + "first row has ink");
    check(!last.empty(), tag + "last row has ink");
    check_eq(first.y0, item_y(0), tag + "first row top 27");
    check_eq(last.y0, item_y(expected_rows - 1), tag + "last row top");
    check_eq(last.y0 - first.y0, gm::kQuitItemStep * (expected_rows - 1),
             tag + "first-to-last row distance is 16*(rows-1)");

    // The cursor 0 (API default) highlights the first item with font 4.
    check_band(frame, fonts.at(gm::kQuitSelectedFont), gm::quit_item_label(in_game, 0),
               gm::kQuitItemX, item_y(0), tag + "cursor 0 is font 4");

    const Frame again = run(game, gm::QuitRenderOptions{.background = {}, .in_game = in_game,
                             .fade_lut_dump = {}, .fade_lut_table = nullptr});
    check(again.ok, tag + "second render");
    check_eq(differing_pixels(frame.rgba, again.rgba), 0, tag + "deterministic");

    std::string rows_report;
    for (int item = 0; item < expected_rows; ++item) {
        const Box box = ink_box(frame, item_y(item),
                                item_y(item) + fonts.at(0).cell_height - 1);
        rows_report += (item == 0 ? "" : "; ") + std::string(gm::quit_item_label(in_game, item)) +
                       " " + box_text(box);
    }

    std::printf("  quit_render %s: rows=%d ink=%d arrows=%d missing=%d; title \"%.*s\" "
                "x%d y%d %s; %s\n",
                in_game ? "in-game" : "outside", frame.stats.rows,
                frame.stats.ink_pixels, frame.stats.arrows, frame.stats.missing_glyphs,
                static_cast<int>(gm::quit_title(in_game).size()),
                gm::quit_title(in_game).data(), tx, gm::kQuitTitleY,
                box_text(title).c_str(), rows_report.c_str());
}

// --- the cursor --------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                 bool in_game) {
    const std::string tag = in_game ? "cursor in game: " : "cursor outside: ";
    const int items = gm::quit_item_count(in_game);
    const Frame plain = run(game, gm::QuitRenderOptions{.background = {}, .in_game = in_game,
                             .fade_lut_dump = {}, .fade_lut_table = nullptr});
    check(plain.ok, tag + "plain frame");
    if (!plain.ok) {
        return;
    }
    std::vector<Frame> frames;
    std::string report;
    for (int cursor = 0; cursor < items; ++cursor) {
        gm::QuitRenderOptions options;
        options.in_game = in_game;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "cursor " + std::to_string(cursor) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.rows, items,
                 tag + "cursor " + std::to_string(cursor) + ": rows");
        check_eq(frame.stats.arrows, 0,
                 tag + "cursor " + std::to_string(cursor) + ": no arrows");
        check(frame.stats.ink_pixels > 0,
              tag + "cursor " + std::to_string(cursor) + ": ink > 0");

        // The highlighted row is the font 4 blit, the others stay font 0.
        check_band(frame, fonts.at(gm::kQuitSelectedFont),
                   gm::quit_item_label(in_game, cursor), gm::kQuitItemX,
                   item_y(cursor), tag + "cursor " + std::to_string(cursor) + " row");

        // Count the pixels that differ from the plain (cursor 0) frame and
        // check they all lie inside the two involved row bands.
        long long diff_plain = 0;
        long long outside = 0;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                if (at(frame, x, y) == at(plain, x, y)) {
                    continue;
                }
                ++diff_plain;
                const int row = (y - gm::kQuitFirstRow) / gm::kQuitItemStep;
                const bool in_row = y >= gm::kQuitFirstRow && row >= 0 && row < items;
                const bool involved = in_row && (row == cursor || row == 0);
                outside += involved ? 0 : 1;
            }
        }
        check_eq(outside, 0, tag + "cursor " + std::to_string(cursor) +
                                 ": changes only the two involved row bands");
        if (cursor == 0) {
            check_eq(diff_plain, 0, tag + "cursor 0 equals the API default");
        } else {
            check(diff_plain > 0, tag + "cursor " + std::to_string(cursor) +
                                      ": differs from the default frame");
        }

        // The same frame compared with an independent composite: the whole
        // canvas is the title + the items with font 4 on `cursor`.
        check_eq(differing_pixels(frame.rgba, composite(fonts, in_game, cursor)), 0,
                 tag + "cursor " + std::to_string(cursor) +
                     ": whole canvas matches the independent composite");

        if (!report.empty()) {
            report += ", ";
        }
        report += "c" + std::to_string(cursor) + ": " + std::to_string(diff_plain) +
                  " px vs c0";
        frames.push_back(frame);
    }

    // Pairwise: every cursor paints a different frame.
    long long min_pair = -1;
    for (std::size_t a = 0; a < frames.size(); ++a) {
        for (std::size_t b = a + 1; b < frames.size(); ++b) {
            const long long diff = differing_pixels(frames[a].rgba, frames[b].rgba);
            check(diff > 0, tag + "frames " + std::to_string(a) + "/" +
                                std::to_string(b) + " differ");
            if (min_pair < 0 || diff < min_pair) {
                min_pair = diff;
            }
        }
    }

    // A cursor outside the variant's range highlights nothing: the frame is the
    // all-font-0 composite (the "no highlight" state of 0x485dd0).
    const std::vector<std::uint32_t> no_highlight =
        composite(fonts, in_game, /*cursor=*/items);
    long long out_of_range = 0;
    for (const int cursor : {items, items + 3, -1}) {
        gm::QuitRenderOptions options;
        options.in_game = in_game;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "cursor " + std::to_string(cursor) + " renders: " +
                            frame.error);
        if (!frame.ok) {
            continue;
        }
        const long long diff =
            differing_pixels(frame.rgba, no_highlight);
        out_of_range = diff;
        check_eq(diff, 0, tag + "cursor " + std::to_string(cursor) +
                              " highlights nothing");
    }

    std::printf("  quit_render %s: %s; min pairwise diff %lld px; cursor %d/%d/%d "
                "highlight nothing (diff %lld px from the all-font-0 composite)\n",
                in_game ? "cursor in-game" : "cursor outside", report.c_str(), min_pair,
                items, items + 3, -1, out_of_range);
}

// --- PLATE -------------------------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                bool in_game) {
    const std::string tag = in_game ? "plate in game: " : "plate outside: ";
    const Frame black = run(game, gm::QuitRenderOptions{.background = {}, .in_game = in_game,
                             .fade_lut_dump = {}, .fade_lut_table = nullptr});
    const SpriteAsset plate = load_sprite_asset(game, std::string(gm::kQuitPanelName));
    if (!black.ok || !plate.ok) {
        std::cout << "  " << tag << "SKIP (no PLATE in BMP.IDX)\n";
        return;
    }
    check_eq(plate.width, gm::kQuitPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kQuitPanelHeight, tag + "PLATE 131 high");

    gm::QuitRenderOptions options;
    options.in_game = in_game;
    options.background = std::string(gm::kQuitPanelName);
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
            compose_band(face, text, x, gm::kQuitPanelWidth, face.cell_height);
        for (int row = 0; row < face.cell_height; ++row) {
            for (int column = 0; column < gm::kQuitPanelWidth; ++column) {
                if (band[static_cast<std::size_t>(row) *
                             static_cast<std::size_t>(gm::kQuitPanelWidth) +
                         static_cast<std::size_t>(column)] != 0) {
                    mask[static_cast<std::size_t>(y + row) *
                             static_cast<std::size_t>(frame.width) +
                         static_cast<std::size_t>(column)] = 1;
                }
            }
        }
    };
    const gm::FontAtlas& title_face = fonts.at(gm::kQuitTitleFont);
    mark(title_face, gm::quit_title(in_game), title_x(title_face, in_game),
         gm::kQuitTitleY);
    for (int item = 0; item < gm::quit_item_count(in_game); ++item) {
        mark(face_of(fonts, 0, item), gm::quit_item_label(in_game, item), gm::kQuitItemX,
             item_y(item));
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
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::Quit)),
             std::string(gm::kQuitPanelName),
             tag + "menu_panel_asset(Screen::Quit) is PLATE");

    std::printf("  quit_render %s: PLATE %dx%d, panel %lld px, %lld under glyphs, "
                "%lld outside black; plate-vs-black diff %lld px\n",
                in_game ? "plate in-game" : "plate outside", plate.width, plate.height,
                panel_in, panel_ink, outside, diff);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kQuitCanvasWidth) *
                                  gm::kQuitCanvasHeight;

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

    const auto make = [&](bool in_game, bool fade, int level) {
        gm::QuitRenderOptions options;
        options.in_game = in_game;
        options.background = std::string(gm::kQuitPanelName);
        options.cursor = 0;
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
                if (x < gm::kQuitPanelWidth && y < gm::kQuitPanelHeight) {
                    continue;
                }
                if (!is_black(at(frame, x, y))) {
                    ++count;
                }
            }
        }
        return count;
    };

    const Frame plain = make(true, false, gm::kHudLutIdentityLevel);
    check(plain.ok, tag + "unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    check_eq(static_cast<long long>(plain.rgba.size()), kCanvas,
             tag + "unfaded canvas 64000");

    // 1. Level 11 is the identity: 0 differing pixels.
    const Frame identity = make(true, true, gm::kHudLutIdentityLevel);
    check(identity.ok, tag + "level 11 frame: " + identity.error);
    if (!identity.ok) {
        return;
    }
    const long long identity_diffs = differing_pixels(identity.rgba, plain.rgba);
    check_eq(identity_diffs, 0, tag + "level 11 is byte-identical to no fade");
    check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels,
             tag + "level 11 ink unchanged");
    check_eq(outside_panel(identity), 0, tag + "level 11 leaves the canvas black");

    // 2. The outside variant fades too: same tables, same ink.
    const Frame outside_plain = make(false, false, gm::kHudLutIdentityLevel);
    const Frame outside_identity = make(false, true, gm::kHudLutIdentityLevel);
    check(outside_plain.ok && outside_identity.ok, tag + "outside variant renders");
    if (outside_plain.ok && outside_identity.ok) {
        check_eq(differing_pixels(outside_identity.rgba, outside_plain.rgba), 0,
                 tag + "outside variant level 11 is identical too");
        check_eq(outside_identity.stats.ink_pixels, outside_plain.stats.ink_pixels,
                 tag + "outside variant ink unchanged");
    }

    // 3. Level 0: counted black pixels and a different frame.
    const Frame level0 = make(true, true, 0);
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

    // 4. Levels 12..15: four different frames, all different from level 11.
    std::vector<Frame> exact(4);
    for (int i = 0; i < 4; ++i) {
        const int level = gm::kHudLutIdentityLevel + 1 + i;
        exact[static_cast<std::size_t>(i)] = make(true, true, level);
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
    const Frame clamp99 = make(true, true, 99);
    check(clamp99.ok, tag + "level 99 frame: " + clamp99.error);
    if (clamp99.ok) {
        check_eq(differing_pixels(clamp99.rgba, exact[3].rgba), 0,
                 tag + "level 99 clamps to 15");
    }

    std::printf("  quit_render fade: level 11 diffs %lld/%lld; level 0 black "
                "%lld (plain %lld), diffs %lld; adjacent 12/13 %lld, 13/14 %lld, "
                "14/15 %lld; vs 11 %lld/%lld/%lld/%lld\n",
                identity_diffs, kCanvas, black0, black_plain, diff0, pair12_13,
                pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);

    // 5. Negatives: no table and no dump, an unreadable dump, and the table
    // winning over the dump (the dump path is the CLI's option, not wired here).
    {
        gm::QuitRenderOptions options;
        options.background = std::string(gm::kQuitPanelName);
        options.fade_lut = true;
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "no table and no dump refused");
        check(starts_with(refusal.error, "quit_render: ") &&
                  contains(refusal.error, "fade LUT needs"),
              tag + "refusal names the missing table: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "refusal clears the buffer");
        check_eq(refusal.width, 0, tag + "refusal zeroes the width");
        check_eq(refusal.height, 0, tag + "refusal zeroes the height");
        check_eq(refusal.stats.ink_pixels, 0, tag + "refusal zeroes the stats");
    }
    {
        gm::QuitRenderOptions options;
        options.background = std::string(gm::kQuitPanelName);
        options.fade_lut = true;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "missing dump refused");
        check(starts_with(refusal.error, "quit_render: ") &&
                  contains(refusal.error, "hud_lut:"),
              tag + "missing dump carries the hud_lut text: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "missing dump clears the buffer");
    }
    {
        gm::QuitRenderOptions options;
        options.background = std::string(gm::kQuitPanelName);
        options.cursor = 0;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame both = run(game, options);
        check(both.ok, tag + "a table wins over a missing dump: " + both.error);
        if (both.ok) {
            check_eq(differing_pixels(both.rgba, outside_plain.rgba), 0,
                     tag + "table path equals the unfaded outside level 11 frame");
        }
    }
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::QuitRenderOptions{}, "empty game path",
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
    check_refused(root / "game-without-bmp", gm::QuitRenderOptions{}, "no BMP.IDX",
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
    check(write_bytes(fontless / "BMP.DAT", {}),
          "negatives: synthetic BMP.DAT written");
    check_refused(root / "game-without-font4", gm::QuitRenderOptions{},
                  "missing FONTS4 (the face atlas)", "FONTS4");

    fs::remove_all(root, ec);

    if (game.empty()) {
        return;
    }

    // A background name that is not in the archive is a text, not black.
    gm::QuitRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // The shipped install: both variants render with their proven row counts.
    for (const bool in_game : {false, true}) {
        const Frame frame = run(game, gm::QuitRenderOptions{.background = {}, .in_game = in_game,
                             .fade_lut_dump = {}, .fade_lut_table = nullptr});
        check(frame.ok, std::string("negatives: install ") +
                            (in_game ? "in-game" : "outside") +
                            " renders: " + frame.error);
        if (frame.ok) {
            check_eq(frame.stats.rows, gm::quit_item_count(in_game),
                     std::string("negatives: ") + (in_game ? "in-game" : "outside") +
                         " rows");
            check_eq(frame.stats.arrows, 0,
                     std::string("negatives: ") + (in_game ? "in-game" : "outside") +
                         " no arrows");
            std::printf("  quit_render negatives %s: rows=%d ink=%d arrows=%d "
                        "missing=%d\n",
                        in_game ? "in-game" : "outside", frame.stats.rows,
                        frame.stats.ink_pixels, frame.stats.arrows,
                        frame.stats.missing_glyphs);
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
        if (fs::exists(root / "FRONTEND" / "BMP.IDX", ec)) {
            game = root;
            break;
        }
    }
    if (game.empty()) {
        std::cout << "quit_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number :
         {gm::kQuitTitleFont, gm::kQuitRowFont, gm::kQuitSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "quit_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_variant_frame(game, fonts, false);
    test_variant_frame(game, fonts, true);
    test_cursor(game, fonts, false);
    test_cursor(game, fonts, true);
    test_plate(game, fonts, false);
    test_plate(game, fonts, true);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "quit_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "quit_render: ok (" << checks
              << " checks) screen 33 -> 320x200 RGBA (QUIT GAME? Yes/No and "
                 "QUIT OPTIONS with three items, title y 14, rows y 27 step 16 x 38, "
                 "cursor font 4, no arrows, PLATE, exact fade, negatives)\n";
    return 0;
}
