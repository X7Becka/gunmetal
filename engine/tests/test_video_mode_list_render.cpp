// One frame of screen 18, `SELECT VIDEO MODE` (`M8.8` slice 7,
// `ui/video_mode_list_render`, docs/VIDEO_MODE_LIST_RENDER.md).
//
// The renderer paints the 320x200 RGBA canvas with the `PLATE` band, the three
// `FONTS4` faces and the `MENUARRW` scroll arrows; the *list* is the documented
// model `kVideoModeListModes` (the driver enumeration is not reproducible from
// the shipped files).  The test pins the proven geometry of `0x48a8b0` from
// pixels and from the exported rules:
//
//   * the labels: `video_mode_list_label` is `0x486500` — every documented
//     branch (Direct 3D / Open GL / Glide 3Dfx / TV Mode / 256 Mode X /
//     256 Skip X / 256 Skip / 32k Skip / 64k Skip / Skip / 256 Colours /
//     32k Colours / 64k Colours / bare size) is pinned by strings, and the
//     15 model entries produce the 15 documented texts;
//   * the window rule of `0x48a8fa`..`0x48a921` (`video_mode_list_first_row`)
//     is pinned as numbers for the model count 15 and for synthetic counts
//     (3, 10, 20, 0), including cursors outside the list;
//   * counter 0: `rows == 15`, `first_row == 0`, `arrows == 1` (down only),
//     title centred at y 11, hint centred at y 20, ten bands at x 38 with the
//     9 px step (y 29..110) — every band is compared byte for byte with an
//     independent blit built here from `load_font`'s atlases;
//   * the faces: title font 5 (0x9b0f0f), rows font 0 (0x130f0f), cursor
//     font 4 (0xefefef);
//   * the cursor (0, 1, 14): the highlight moves to the matching band and the
//     frame differs from cursor 0 by a counted number of pixels; a cursor
//     outside the list highlights nothing;
//   * scrolling (cursor 6 -> first 1, both arrows; cursor 14 -> first 5, up
//     arrow only): the window content is verified band by band, so the first
//     and last visible labels and their bboxes are pinned;
//   * the arrows: the `MENUARRW` 16x8 sprite is decoded here from the raw BMP
//     bytes (file order, palette of the entry) and compared pixel by pixel —
//     the down arrow at (159,121), the up arrow at (159,2) rotated by 180
//     degrees, 41 pixels each, nothing else inside the sprite rectangles;
//   * `PLATE` as the background: the 25676 panel pixels are decoded here
//     independently, the other 38324 stay black and the glyph ink is unchanged;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a game
//     whose `BMP.IDX` has no `FONTS4`, an unknown background and an exact fade
//     without table/dump -- all texts with a cleared buffer and zeroed stats;
//     determinism of two renders;
//   * exact fade (`fade_lut`): the table is built from `GAMEPAL` in memory
//     (`build_hud_lut`, no file); level 11 is byte-identical to the unfaded
//     `PLATE` frame, level 0 blackens it, 12..15 are four different frames.
//
// The installation is only read; the pixel runs skip cleanly when it is absent
// (the pure-label/window checks and the empty-path negative always run).
// Numbers are printed and docs/VIDEO_MODE_LIST_RENDER.md §4 pins them.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "ui/menu_render.h"
#include "ui/video_mode_list_render.h"

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
    gm::VideoModeListRenderStats stats;
};

Frame run(const fs::path& game, const gm::VideoModeListRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_video_mode_list(game, options, frame.rgba, frame.width,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kVideoModeListBackground; }

/// `#000` with any alpha: the faded palette entries of low levels are black
/// too, but their alpha is the forced 255 and their RGB is what counts here.
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

/// Compare the rows `y..y+cell-1` of the 196 px band of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kVideoModeListPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kVideoModeListBackground : want;
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
/// `SpriteAsset::rows` are the BMP pixel bytes in *file* order; the standard BMP
/// file stores its rows bottom-up, so `parse_bmp` row `y` is file row
/// `height-1-y`.
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
        return approved / "gm_video_mode_list_render";
    }
    return fs::temp_directory_path() / "gm_video_mode_list_render";
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

/// The face of a band for a given cursor (the module's rule: the entry whose
/// index equals the cursor switches to font 4).
const gm::FontAtlas& face_of(const std::map<int, gm::FontAtlas>& fonts, int cursor,
                             int index) {
    return fonts.at(index == cursor ? gm::kVideoModeListSelectedFont
                                    : gm::kVideoModeListRowFont);
}

std::string label_of(int index) {
    return gm::video_mode_list_label(gm::kVideoModeListModes[static_cast<std::size_t>(index)]);
}

/// The centring of `0x485dd0`'s `centered` branch inside the 196 px band.
int centred_x(const gm::FontAtlas& face, std::string_view text) {
    return std::max(0, (gm::kVideoModeListPanelWidth - pen_width(face, text)) / 2);
}

/// The ink of one frame computed from the atlas blits alone: title + hint + the
/// ten bands of the window, the cursor's band in font 4.  Arrow pixels are not
/// glyph ink.
long long expected_ink(const std::map<int, gm::FontAtlas>& fonts, int cursor) {
    const gm::FontAtlas& title_face = fonts.at(gm::kVideoModeListTitleFont);
    const gm::FontAtlas& row_face = fonts.at(gm::kVideoModeListRowFont);
    long long ink =
        band_ink(compose_band(title_face, gm::kVideoModeListTitle,
                              centred_x(title_face, gm::kVideoModeListTitle),
                              gm::kVideoModeListPanelWidth, title_face.cell_height)) +
        band_ink(compose_band(row_face, gm::kVideoModeListHint,
                              centred_x(row_face, gm::kVideoModeListHint),
                              gm::kVideoModeListPanelWidth, row_face.cell_height));
    const int first = gm::video_mode_list_first_row(gm::kVideoModeListModeCount, cursor);
    for (int i = 0; i < gm::kVideoModeListVisibleRows; ++i) {
        const int index = first + i;
        const gm::FontAtlas& face = face_of(fonts, cursor, index);
        ink += band_ink(compose_band(face, label_of(index), gm::kVideoModeListRowX,
                                     gm::kVideoModeListPanelWidth, face.cell_height));
    }
    return ink;
}

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::VideoModeListRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::VideoModeListRenderStats stats;
    stats.rows = 7;
    stats.first_row = 7;
    stats.arrows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    std::string error;
    const bool ok =
        gm::render_video_mode_list(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "video_mode_list_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error + "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.first_row, 0, what + ": first_row cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
}

// --- the proven label formatter (`0x486500`) ---------------------------------
void test_labels() {
    const std::string tag = "labels: ";

    // Every documented branch of the `0x486510`..`0x4866f7` chain.
    struct Case {
        int width, height, bpp;
        std::uint32_t flags;
        const char* want;
    };
    const Case cases[] = {
        {640, 480, 8, 0x200, "640x480 Direct 3D"},
        {640, 480, 8, 0x400, "640x480 Open GL"},
        {640, 480, 8, 0x800, "640x480 Glide 3Dfx"},
        {640, 480, 8, 0x002, "640x480 TV Mode"},
        {640, 480, 8, 0x001, "640x480 256 Mode X"},
        {640, 480, 8, 0x101, "640x480 256 Skip X"},
        {640, 480, 8, 0x100, "640x480 256 Skip"},
        {640, 480, 15, 0x100, "640x480 32k Skip"},
        {640, 480, 16, 0x100, "640x480 64k Skip"},
        {640, 480, 32, 0x100, "640x480 Skip"},
        {640, 480, 8, 0x000, "640x480 256 Colours"},
        {640, 480, 15, 0x000, "640x480 32k Colours"},
        {640, 480, 16, 0x000, "640x480 64k Colours"},
        {640, 480, 32, 0x000, "640x480"},
        // The priority of the chain: 0x200 wins over 0x800 and 0x100.
        {800, 600, 8, 0xF02, "800x600 Direct 3D"},
    };
    int wrong = 0;
    for (const Case& item : cases) {
        const std::string got =
            gm::video_mode_list_label({item.width, item.height, item.bpp, item.flags});
        if (got != item.want) {
            ++wrong;
            std::cerr << "FAIL: " << tag << "flags " << item.flags << " bpp " << item.bpp
                      << " (got \"" << got << "\", want \"" << item.want << "\")\n";
        }
    }
    check_eq(wrong, 0, tag + "15 documented branches of 0x486500");

    // The model list, entry by entry.
    check_eq(static_cast<long long>(gm::kVideoModeListModeCount), 15,
             tag + "the model has 15 entries");
    const char* want[15] = {
        "800x600 Glide 3Dfx", "800x300 64k Skip",  "800x300 256 Skip",
        "640x480 Glide 3Dfx", "640x240 64k Skip",  "640x240 256 Skip",
        "640x200 64k Skip",   "640x200 256 Skip",  "512x384 Glide 3Dfx",
        "360x240 64k Skip",   "360x240 256 Skip",  "360x200 64k Skip",
        "360x200 256 Skip",   "320x200 64k Skip",  "320x200 256 Skip",
    };
    std::string all;
    for (int i = 0; i < gm::kVideoModeListModeCount; ++i) {
        check_eq(label_of(i), want[i], tag + "model entry " + std::to_string(i));
        if (!all.empty()) {
            all += " | ";
        }
        all += label_of(i);
    }

    // The proven descending `(width, height, bpp)` sort of `0x406189`.
    bool sorted = true;
    for (int i = 1; i < gm::kVideoModeListModeCount; ++i) {
        const gm::VideoModeListEntry& prev = gm::kVideoModeListModes[i - 1];
        const gm::VideoModeListEntry& cur = gm::kVideoModeListModes[i];
        const long long a = (static_cast<long long>(prev.width) * 8192 + prev.height) * 64 +
                            prev.bpp;
        const long long b = (static_cast<long long>(cur.width) * 8192 + cur.height) * 64 +
                            cur.bpp;
        if (a < b) {
            sorted = false;
        }
    }
    check(sorted, tag + "the model follows the descending sort of 0x406189");
    std::printf("  video_mode_list_render labels: %s\n", all.c_str());
}

// --- the window rule (`0x48a8fa`..`0x48a921`) --------------------------------
void test_window() {
    const std::string tag = "window: ";
    // The model: 15 entries, ten bands.  cursor 0..4 -> 0; 5 -> 0; 6 -> 1;
    // 7 -> 2; 8 -> 3; 9 -> 4; 10..14 -> 5 (= count - 10).
    const int want[15] = {0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 5, 5, 5, 5};
    std::string report;
    for (int cursor = 0; cursor < 15; ++cursor) {
        const int got = gm::video_mode_list_first_row(15, cursor);
        check_eq(got, want[cursor], tag + "count 15 cursor " + std::to_string(cursor));
        if (!report.empty()) {
            report += ",";
        }
        report += std::to_string(got);
    }
    // A list that fits in one window never scrolls, whatever the cursor.
    check_eq(gm::video_mode_list_first_row(10, 0), 0, tag + "count 10 cursor 0");
    check_eq(gm::video_mode_list_first_row(10, 9), 0, tag + "count 10 cursor 9");
    check_eq(gm::video_mode_list_first_row(3, 99), 0, tag + "count 3 cursor 99");
    check_eq(gm::video_mode_list_first_row(0, 5), 0, tag + "count 0");
    check_eq(gm::video_mode_list_first_row(-4, 5), 0, tag + "negative count");
    // 20 entries: the window follows the cursor from the sixth band on and
    // stops at 10 (= count - 10).
    check_eq(gm::video_mode_list_first_row(20, 5), 0, tag + "count 20 cursor 5");
    check_eq(gm::video_mode_list_first_row(20, 6), 1, tag + "count 20 cursor 6");
    check_eq(gm::video_mode_list_first_row(20, 15), 10, tag + "count 20 cursor 15");
    check_eq(gm::video_mode_list_first_row(20, 19), 10, tag + "count 20 cursor 19");
    // The cursor is not clamped by the original: a negative one gives the first
    // window, a huge one the last.
    check_eq(gm::video_mode_list_first_row(15, -3), 0, tag + "negative cursor");
    check_eq(gm::video_mode_list_first_row(15, 1000), 5, tag + "huge cursor");
    std::printf("  video_mode_list_render window: count 15 first_row[0..14] = %s\n",
                report.c_str());
}

// --- the installed frame -----------------------------------------------------
void test_install_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "install: ";
    const Frame frame = run(game, gm::VideoModeListRenderOptions{});
    check(frame.ok, tag + "frame renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kVideoModeListCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kVideoModeListCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kVideoModeListModeCount, tag + "rows 15");
    check_eq(frame.stats.first_row, 0, tag + "first_row 0");
    check_eq(frame.stats.arrows, 1, tag + "only the down arrow");
    check_eq(frame.stats.missing_glyphs, 0, tag + "no missing glyph");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    for (const std::uint32_t pixel : frame.rgba) {
        if ((pixel & 0xFFu) != 0xFFu) {
            check(false, tag + "every pixel is opaque");
            break;
        }
    }

    // The title: font 5, centred, row 11.
    const gm::FontAtlas& title_face = fonts.at(gm::kVideoModeListTitleFont);
    const int tx = centred_x(title_face, gm::kVideoModeListTitle);
    check_band(frame, title_face, gm::kVideoModeListTitle, tx, gm::kVideoModeListTitleY,
               tag + "title");
    const Box title =
        ink_box(frame, gm::kVideoModeListTitleY,
                gm::kVideoModeListTitleY + title_face.cell_height - 1);
    check(!title.empty(), tag + "title has ink");
    check_eq(title.y0, gm::kVideoModeListTitleY, tag + "title top row 11");
    check_eq(title.x0, tx, tag + "title x is the centring of 0x485dd0");

    // The hint: font 0, centred, row 20.
    const gm::FontAtlas& row_face = fonts.at(gm::kVideoModeListRowFont);
    const int hx = centred_x(row_face, gm::kVideoModeListHint);
    check_band(frame, row_face, gm::kVideoModeListHint, hx, gm::kVideoModeListHintY,
               tag + "hint");
    const Box hint = ink_box(frame, gm::kVideoModeListHintY,
                             gm::kVideoModeListHintY + row_face.cell_height - 1);
    check(!hint.empty(), tag + "hint has ink");
    check_eq(hint.y0, gm::kVideoModeListHintY, tag + "hint top row 20");

    // The ten bands: x 38, y 29 + 9*i, cursor 0 -> font 4 on band 0.
    const long long bands_ink = expected_ink(fonts, 0);
    std::string boxes;
    for (int i = 0; i < gm::kVideoModeListVisibleRows; ++i) {
        const int index = i;
        const int y = gm::kVideoModeListFirstRowY + gm::kVideoModeListRowStep * i;
        const gm::FontAtlas& face = face_of(fonts, 0, index);
        const std::string text = label_of(index);
        check_band(frame, face, text, gm::kVideoModeListRowX, y,
                   tag + "band " + std::to_string(i));
        const Box box = ink_box(frame, y, y + face.cell_height - 1);
        check(!box.empty(), tag + "band " + std::to_string(i) + " has ink");
        check_eq(box.y0, y, tag + "band " + std::to_string(i) + " top " +
                                  std::to_string(y));
        check_eq(box.x0, gm::kVideoModeListRowX,
                 tag + "band " + std::to_string(i) + " x 38");
        if (!boxes.empty()) {
            boxes += "; ";
        }
        boxes += "b" + std::to_string(i) + " " + box_text(box);
    }
    check_eq(frame.stats.ink_pixels, bands_ink,
             tag + "ink is the sum of the title, the hint and ten bands");
    // The proven 9 px step: consecutive band tops differ by 9.
    const Box first_band = ink_box(frame, 29, 29 + row_face.cell_height - 1);
    const Box second_band = ink_box(frame, 38, 38 + row_face.cell_height - 1);
    check_eq(second_band.y0 - first_band.y0, gm::kVideoModeListRowStep,
             tag + "band 1 is 9 px below band 0");
    check_eq(gm::kVideoModeListFirstRowY + gm::kVideoModeListRowStep * 9, 110,
             tag + "the tenth band sits at y 110");

    // Determinism: two renders are byte-identical.
    const Frame again = run(game, gm::VideoModeListRenderOptions{});
    check(again.ok, tag + "second render");
    check_eq(differing_pixels(frame.rgba, again.rgba), 0, tag + "deterministic");

    std::printf("  video_mode_list_render install: rows=%d first=%d ink=%d arrows=%d "
                "missing=%d; title y11 %s; hint y20 %s; %s\n",
                frame.stats.rows, frame.stats.first_row, frame.stats.ink_pixels,
                frame.stats.arrows, frame.stats.missing_glyphs, box_text(title).c_str(),
                box_text(hint).c_str(), boxes.c_str());
}

// --- the cursor --------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    const Frame plain = run(game, gm::VideoModeListRenderOptions{});
    check(plain.ok, tag + "plain frame");
    if (!plain.ok) {
        return;
    }
    const int cursors[] = {0, 1, gm::kVideoModeListModeCount - 1};
    std::string report;
    for (const int cursor : cursors) {
        gm::VideoModeListRenderOptions options;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "cursor " + std::to_string(cursor) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.rows, gm::kVideoModeListModeCount,
                 tag + "cursor " + std::to_string(cursor) + ": rows 15");
        check_eq(frame.stats.ink_pixels, expected_ink(fonts, cursor),
                 tag + "cursor " + std::to_string(cursor) +
                     ": ink is the independent blit sum");

        // The selected band is the font 4 blit, and it is the band of the
        // entry whose index is the cursor (inside the window).
        const int first = gm::video_mode_list_first_row(gm::kVideoModeListModeCount, cursor);
        const int band = cursor - first;
        check(band >= 0 && band < gm::kVideoModeListVisibleRows,
              tag + "cursor " + std::to_string(cursor) + ": band in the window");
        if (band >= 0 && band < gm::kVideoModeListVisibleRows) {
            const int y = gm::kVideoModeListFirstRowY + gm::kVideoModeListRowStep * band;
            check_band(frame, fonts.at(gm::kVideoModeListSelectedFont), label_of(cursor),
                       gm::kVideoModeListRowX, y,
                       tag + "cursor " + std::to_string(cursor) + ": highlighted band");
            // The neighbour band is font 0.
            if (band + 1 < gm::kVideoModeListVisibleRows) {
                check_band(frame, fonts.at(gm::kVideoModeListRowFont),
                           label_of(cursor + 1), gm::kVideoModeListRowX, y + 9,
                           tag + "cursor " + std::to_string(cursor) + ": next band plain");
            }
        }
        const long long diff = differing_pixels(frame.rgba, plain.rgba);
        check(cursor == 0 ? diff == 0 : diff > 0,
              tag + "cursor " + std::to_string(cursor) + ": diff vs cursor 0 is " +
                  std::to_string(diff));
        if (!report.empty()) {
            report += "; ";
        }
        report += "cursor " + std::to_string(cursor) + " diff " + std::to_string(diff);
    }

    // A cursor outside the list highlights nothing but keeps the first window.
    for (const int cursor : {-1, gm::kVideoModeListModeCount}) {
        gm::VideoModeListRenderOptions options;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "outside cursor " + std::to_string(cursor) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.arrows, 1,
                 tag + "outside cursor " + std::to_string(cursor) + ": down arrow");
        check_eq(frame.stats.first_row,
                 gm::video_mode_list_first_row(gm::kVideoModeListModeCount, cursor),
                 tag + "outside cursor " + std::to_string(cursor) + ": proven window");
        check_eq(frame.stats.ink_pixels, expected_ink(fonts, cursor),
                 tag + "outside cursor " + std::to_string(cursor) + ": no highlight ink");
        const long long diff = differing_pixels(frame.rgba, plain.rgba);
        check(diff > 0, tag + "outside cursor " + std::to_string(cursor) +
                            ": no highlight, frame differs from cursor 0 by " +
                            std::to_string(diff));
        report += "; cursor " + std::to_string(cursor) + " diff " + std::to_string(diff);
    }
    std::printf("  video_mode_list_render cursor: %s\n", report.c_str());
}

// --- scrolling ---------------------------------------------------------------
void test_scroll(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "scroll: ";
    const Frame plain = run(game, gm::VideoModeListRenderOptions{});
    check(plain.ok, tag + "plain frame");
    if (!plain.ok) {
        return;
    }
    struct Step {
        int cursor;
        int first;
        int arrows;
        int first_index;
        int last_index;
    };
    const Step steps[] = {
        {6, 1, 2, 1, 10},
        {9, 4, 2, 4, 13},
        {14, 5, 1, 5, 14},
    };
    std::string report;
    for (const Step& step : steps) {
        gm::VideoModeListRenderOptions options;
        options.cursor = step.cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "cursor " + std::to_string(step.cursor) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.first_row, step.first,
                 tag + "cursor " + std::to_string(step.cursor) + ": first_row");
        check_eq(frame.stats.arrows, step.arrows,
                 tag + "cursor " + std::to_string(step.cursor) + ": arrows");

        // The visible window, band by band.
        for (int i = 0; i < gm::kVideoModeListVisibleRows; ++i) {
            const int index = step.first + i;
            const int y = gm::kVideoModeListFirstRowY + gm::kVideoModeListRowStep * i;
            const gm::FontAtlas& face = face_of(fonts, step.cursor, index);
            check_band(frame, face, label_of(index), gm::kVideoModeListRowX, y,
                       tag + "cursor " + std::to_string(step.cursor) + " band " +
                           std::to_string(i));
        }

        // The first and the last visible labels, by their bbox.
        const gm::FontAtlas& row_face = fonts.at(gm::kVideoModeListRowFont);
        const Box top = ink_box(frame, gm::kVideoModeListFirstRowY,
                                gm::kVideoModeListFirstRowY + row_face.cell_height - 1);
        const int last_y = gm::kVideoModeListFirstRowY +
                           gm::kVideoModeListRowStep * (gm::kVideoModeListVisibleRows - 1);
        const Box bottom = ink_box(frame, last_y, last_y + row_face.cell_height - 1);
        check(!top.empty() && !bottom.empty(),
              tag + "cursor " + std::to_string(step.cursor) + ": both end bands have ink");
        check_eq(top.y0, gm::kVideoModeListFirstRowY,
                 tag + "cursor " + std::to_string(step.cursor) + ": first band top");
        check_eq(bottom.y0, last_y,
                 tag + "cursor " + std::to_string(step.cursor) + ": last band top");
        if (!report.empty()) {
            report += "; ";
        }
        report += "cursor " + std::to_string(step.cursor) + " first " +
                  std::to_string(step.first) + " arrows " +
                  std::to_string(step.arrows) + " [" + label_of(step.first_index) + " .. " +
                  label_of(step.last_index) + "] top " + box_text(top) + " bottom " +
                  box_text(bottom);
    }
    std::printf("  video_mode_list_render scroll: %s\n", report.c_str());
}

// --- the MENUARRW arrows -----------------------------------------------------
void test_arrows(const fs::path& game) {
    const std::string tag = "arrows: ";
    const SpriteAsset sprite =
        load_sprite_asset(game, std::string(gm::kVideoModeListArrowName));
    check(sprite.ok, tag + "MENUARRW decoded from FRONTEND/BMP.IDX");
    if (!sprite.ok) {
        return;
    }
    check_eq(sprite.width, gm::kVideoModeListArrowWidth, tag + "MENUARRW is 16 px wide");
    check_eq(sprite.height, gm::kVideoModeListArrowHeight, tag + "MENUARRW is 8 px tall");
    check_eq(static_cast<long long>(sprite.rows.size()), 128, tag + "128 sprite bytes");

    // One arrow: the expected pixels of the sprite rectangle, 0 = transparent.
    const auto expected_arrow = [&sprite](bool up, std::vector<std::uint32_t>& out) {
        out.assign(static_cast<std::size_t>(gm::kVideoModeListArrowWidth) *
                       static_cast<std::size_t>(gm::kVideoModeListArrowHeight),
                   0u);
        for (int row = 0; row < gm::kVideoModeListArrowHeight; ++row) {
            for (int column = 0; column < gm::kVideoModeListArrowWidth; ++column) {
                const int source_row =
                    up ? gm::kVideoModeListArrowHeight - 1 - row : row;
                const int source_column =
                    up ? gm::kVideoModeListArrowWidth - 1 - column : column;
                // `sprite.rows` is the file order, which *is* the blitter order
                // (`0x485cc0` reads bytes forward, `0x485d00` backwards);
                // `parse_bmp` returns the rows bottom-up, so the renderer's
                // `pixels[height-1-row]` is this same byte.
                const std::uint8_t value =
                    sprite.rows[static_cast<std::size_t>(source_row) *
                                    static_cast<std::size_t>(sprite.width) +
                                static_cast<std::size_t>(source_column)];
                out[static_cast<std::size_t>(row) * gm::kVideoModeListArrowWidth +
                    static_cast<std::size_t>(column)] =
                    value == 0 ? 0u : sprite.palette[value];
            }
        }
    };
    std::vector<std::uint32_t> down;
    std::vector<std::uint32_t> up;
    expected_arrow(false, down);
    expected_arrow(true, up);
    long long down_ink = 0;
    long long up_ink = 0;
    for (const std::uint32_t pixel : down) {
        down_ink += pixel == 0 ? 0 : 1;
    }
    for (const std::uint32_t pixel : up) {
        up_ink += pixel == 0 ? 0 : 1;
    }
    check(down_ink > 0, tag + "the sprite has ink");
    check_eq(up_ink, down_ink, tag + "the up arrow is the same sprite rotated");

    // Compare one rectangle of the frame with the expected sprite (0 = the
    // black canvas must be there).
    const auto check_arrow = [&](const Frame& frame, const std::vector<std::uint32_t>& want,
                                 int x, int y, const std::string& what) {
        int wrong = 0;
        long long seen = 0;
        for (int row = 0; row < gm::kVideoModeListArrowHeight; ++row) {
            for (int column = 0; column < gm::kVideoModeListArrowWidth; ++column) {
                const std::uint32_t expected =
                    want[static_cast<std::size_t>(row) * gm::kVideoModeListArrowWidth +
                         static_cast<std::size_t>(column)];
                const std::uint32_t got = at(frame, x + column, y + row);
                if (expected == 0) {
                    if (!is_black(got)) {
                        ++wrong;
                    }
                } else {
                    ++seen;
                    if (got != expected) {
                        ++wrong;
                    }
                }
            }
        }
        check_eq(wrong, 0, what + ": sprite matches (seen " + std::to_string(seen) + ")");
        return seen;
    };

    // The rectangle of an arrow that must *not* be painted: every pixel black.
    const auto check_absent = [&](const Frame& frame, int x, int y,
                                  const std::string& what) {
        int stray = 0;
        for (int row = 0; row < gm::kVideoModeListArrowHeight; ++row) {
            for (int column = 0; column < gm::kVideoModeListArrowWidth; ++column) {
                if (!is_black(at(frame, x + column, y + row))) {
                    ++stray;
                }
            }
        }
        check_eq(stray, 0, what + ": the rectangle is empty");
    };

    // Cursor 0: the window starts at 0, so only the down arrow is painted.
    const Frame top = run(game, gm::VideoModeListRenderOptions{});
    check(top.ok, tag + "cursor 0 frame: " + top.error);
    if (!top.ok) {
        return;
    }
    const long long top_down = check_arrow(top, down, gm::kVideoModeListArrowX,
                                           gm::kVideoModeListArrowDownY,
                                           tag + "cursor 0 down arrow");
    check_absent(top, gm::kVideoModeListArrowX, gm::kVideoModeListArrowUpY,
                 tag + "cursor 0 has no up arrow");
    const long long top_up = 0;

    // Cursor 9: first 4, so both arrows are painted.
    gm::VideoModeListRenderOptions middle_options;
    middle_options.cursor = 9;
    const Frame middle = run(game, middle_options);
    check(middle.ok, tag + "cursor 9 frame: " + middle.error);
    if (!middle.ok) {
        return;
    }
    check_eq(middle.stats.arrows, 2, tag + "cursor 9: both arrows");
    const long long middle_up = check_arrow(middle, up, gm::kVideoModeListArrowX,
                                            gm::kVideoModeListArrowUpY,
                                            tag + "cursor 9 up arrow");
    const long long middle_down = check_arrow(middle, down, gm::kVideoModeListArrowX,
                                              gm::kVideoModeListArrowDownY,
                                              tag + "cursor 9 down arrow");

    // Cursor 14: first 5, so only the up arrow is painted.
    gm::VideoModeListRenderOptions bottom_options;
    bottom_options.cursor = 14;
    const Frame bottom = run(game, bottom_options);
    check(bottom.ok, tag + "cursor 14 frame: " + bottom.error);
    if (!bottom.ok) {
        return;
    }
    check_eq(bottom.stats.arrows, 1, tag + "cursor 14: only the up arrow");
    const long long bottom_up = check_arrow(bottom, up, gm::kVideoModeListArrowX,
                                            gm::kVideoModeListArrowUpY,
                                            tag + "cursor 14 up arrow");
    check_absent(bottom, gm::kVideoModeListArrowX, gm::kVideoModeListArrowDownY,
                 tag + "cursor 14 has no down arrow");
    const long long bottom_down = 0;

    std::printf("  video_mode_list_render arrows: MENUARRW %dx%d, %lld px each; "
                "cursor 0 down %lld/up %lld, cursor 9 up %lld/down %lld, "
                "cursor 14 up %lld/down %lld; up = down rotated 180\n",
                sprite.width, sprite.height, down_ink, top_down, top_up, middle_up,
                middle_down, bottom_up, bottom_down);
}

// --- PLATE as the background -------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "PLATE: ";
    (void)fonts;
    const Frame black = run(game, gm::VideoModeListRenderOptions{});
    check(black.ok, tag + "black frame: " + black.error);
    if (!black.ok) {
        return;
    }
    const SpriteAsset plate = load_sprite_asset(game, std::string(gm::kVideoModeListPanelName));
    check(plate.ok, tag + "PLATE decoded from FRONTEND/BMP.IDX");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kVideoModeListPanelWidth, tag + "PLATE is 196 px wide");
    check_eq(plate.height, gm::kVideoModeListPanelHeight, tag + "PLATE is 131 px tall");
    const std::vector<std::uint32_t> panel = plate_pixels(plate);

    gm::VideoModeListRenderOptions options;
    options.background = std::string(gm::kVideoModeListPanelName);
    const Frame frame = run(game, options);
    check(frame.ok, tag + "PLATE frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels, black.stats.ink_pixels,
             tag + "ink unchanged over the panel");

    long long panel_in = 0;
    long long panel_ink = 0;
    long long outside = 0;
    long long outside_ink = 0;
    int wrong = 0;
    int first_x = -1;
    int first_y = -1;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t got = at(frame, x, y);
            if (x >= plate.width || y >= plate.height) {
                ++outside;
                if (!is_black(got)) {
                    ++outside_ink;
                }
                continue;
            }
            const std::uint32_t want =
                panel[static_cast<std::size_t>(y) * static_cast<std::size_t>(plate.width) +
                      static_cast<std::size_t>(x)];
            if (!is_black(at(black, x, y))) {
                ++panel_ink;
                continue;
            }
            ++panel_in;
            if (got != want) {
                if (wrong == 0) {
                    first_x = x;
                    first_y = y;
                }
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, tag + "panel matches the independent decode under the glyph "
                            "mask (first " + std::to_string(first_x) + "," +
                            std::to_string(first_y) + ")");
    check_eq(outside, static_cast<long long>(frame.width) * frame.height -
                          static_cast<long long>(plate.width) * plate.height,
             tag + "38324 pixels outside the panel");
    check_eq(outside_ink, 0, tag + "everything outside the panel stays black");
    // The panel pixels that are not glyph ink are the 41 arrow pixels of the
    // down `MENUARRW` sprite.
    check_eq(panel_ink, black.stats.ink_pixels + 41,
             tag + "the non-glyph panel ink is the 41 arrow pixels");
    check_eq(panel_in + panel_ink, static_cast<long long>(plate.width) * plate.height,
             tag + "panel in + covered = 25676");
    const long long diff = differing_pixels(frame.rgba, black.rgba);
    check(diff > 0, tag + "PLATE changes the frame");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::VideoMode)),
             std::string(gm::kVideoModeListPanelName),
             tag + "menu_panel_asset(Screen::VideoMode) is PLATE");

    std::printf("  video_mode_list_render PLATE: %dx%d, panel %lld px, %lld under ink "
                "(glyphs %lld + arrows %lld), %lld outside black; plate-vs-black diff "
                "%lld px\n",
                plate.width, plate.height, panel_in, panel_ink,
                static_cast<long long>(black.stats.ink_pixels),
                panel_ink - black.stats.ink_pixels, outside_ink, diff);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kVideoModeListCanvasWidth) *
                                  gm::kVideoModeListCanvasHeight;

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
    check(error.empty(), tag + "build_hud_lut leaves no error");

    const auto make = [&](bool fade, int level) {
        gm::VideoModeListRenderOptions options;
        options.background = std::string(gm::kVideoModeListPanelName);
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
                if (x < gm::kVideoModeListPanelWidth && y < gm::kVideoModeListPanelHeight) {
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
    check_eq(static_cast<long long>(plain.rgba.size()), kCanvas, tag + "unfaded canvas 64000");

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

    // 2. Level 0: a different frame with counted black pixels.
    const Frame level0 = make(true, 0);
    check(level0.ok, tag + "level 0 frame: " + level0.error);
    if (!level0.ok) {
        return;
    }
    const long long black0 = black_pixels(level0);
    const long long diff0 = differing_pixels(level0.rgba, plain.rgba);
    check(black0 > 0, tag + "level 0 still has black pixels to count");
    check(diff0 > 0, tag + "level 0 differs from the unfaded frame");
    check_eq(outside_panel(level0), 0, tag + "level 0 leaves the canvas black");
    check_eq(level0.stats.ink_pixels, plain.stats.ink_pixels, tag + "level 0 ink unchanged");

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
        check_eq(differing_pixels(clamp99.rgba, exact[3].rgba), 0, tag + "level 99 clamps to 15");
    }

    std::printf("  video_mode_list_render fade: level 11 diffs %lld/%lld; level 0 black "
                "%lld, diffs %lld; adjacent 12/13 %lld, 13/14 %lld, 14/15 %lld; "
                "vs 11 %lld/%lld/%lld/%lld\n",
                identity_diffs, kCanvas, black0, diff0, pair12_13, pair13_14, pair14_15,
                to11_12, to11_13, to11_14, to11_15);

    // 4. Negatives: no table and no dump, and an unreadable dump.
    {
        gm::VideoModeListRenderOptions options;
        options.background = std::string(gm::kVideoModeListPanelName);
        options.fade_lut = true;
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "no table and no dump refused");
        check(starts_with(refusal.error, "video_mode_list_render: ") &&
                  contains(refusal.error, "fade LUT needs"),
              tag + "refusal names the missing table: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "refusal clears the buffer");
        check_eq(refusal.width, 0, tag + "refusal zeroes the width");
        check_eq(refusal.stats.ink_pixels, 0, tag + "refusal zeroes the stats");
    }
    {
        gm::VideoModeListRenderOptions options;
        options.background = std::string(gm::kVideoModeListPanelName);
        options.fade_lut = true;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "missing dump refused");
        check(starts_with(refusal.error, "video_mode_list_render: ") &&
                  contains(refusal.error, "hud_lut:"),
              tag + "missing dump carries the hud_lut text: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "missing dump clears the buffer");
    }
    {
        gm::VideoModeListRenderOptions options;
        options.background = std::string(gm::kVideoModeListPanelName);
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
    check_refused(fs::path{}, gm::VideoModeListRenderOptions{}, "empty game path",
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
    check_refused(root / "game-without-bmp", gm::VideoModeListRenderOptions{}, "no BMP.IDX",
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
    check_refused(root / "game-without-font4", gm::VideoModeListRenderOptions{},
                  "missing FONTS4 (the face atlas)", "FONTS4");

    fs::remove_all(root, ec);

    if (game.empty()) {
        return;
    }

    // A background name that is not in the archive is a text, not black.
    gm::VideoModeListRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // The shipped install renders the model list and the API default
    // highlights entry 0.
    const Frame frame = run(game, gm::VideoModeListRenderOptions{});
    check(frame.ok, "negatives: shipped install renders: " + frame.error);
    if (frame.ok) {
        check_eq(frame.stats.rows, gm::kVideoModeListModeCount, "negatives: rows 15");
        check_eq(frame.stats.arrows, 1, "negatives: the down arrow");
        std::printf("  video_mode_list_render negatives: rows=%d first=%d ink=%d arrows=%d "
                    "missing=%d\n",
                    frame.stats.rows, frame.stats.first_row, frame.stats.ink_pixels,
                    frame.stats.arrows, frame.stats.missing_glyphs);
    }
}

} // namespace

int main() {
    // The label formatter and the window rule need no installation.
    test_labels();
    test_window();

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
        std::cout << "video_mode_list_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kVideoModeListTitleFont, gm::kVideoModeListRowFont,
                             gm::kVideoModeListSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "video_mode_list_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_install_frame(game, fonts);
    test_cursor(game, fonts);
    test_scroll(game, fonts);
    test_arrows(game);
    test_plate(game, fonts);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "video_mode_list_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "video_mode_list_render: ok (" << checks
              << " checks) screen 18 -> 320x200 RGBA (title y 11, hint y 20, ten "
                 "bands at x 38 y 29+9*i step 9, cursor font 4, MENUARRW up/down, "
                 "PLATE, exact fade, negatives)\n";
    return 0;
}
