// One frame of screen 32, `PLAY OPTIONS` (`M8.8` slice 20,
// `ui/play_options_render`, docs/PLAY_OPTIONS_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.4
// from the handler `0x486170` and from pixels:
//
//   * model: `rows` 8, step 15, title `PLAY OPTIONS` (font 5) centred at y 11,
//     the label row immediates 21/17/13/15/18/20/15/9 (y 21, 32, 43, 60, 78,
//     95, 105, 114), the second lines of items 2/3/4 (y 51/68/86), x 25 for
//     items 0..5 and 13 for the camera and `Done`, the six bits of
//     `play_flags_p1`, the `.bss` default 0 versus the shipped CFG 0x10400, the
//     camera `.data` default 1 and its 0..2 clamp;
//   * the shipped install: 8 rows, 6 checks, 0 sliders, 0 arrows, no missing
//     glyph, 320x200 RGBA, every pixel opaque; every label and second line is
//     compared byte for byte with an independent blit and the bounding boxes
//     are printed; ink = union of all text bands and the six checkboxes;
//   * the checkboxes: six independent bits, off paints `NOCHECK` 8 rows below
//     the anchor (x 16, y 13/24/35/52/70/87 + 8) and on paints the 8x16 `CHECK`
//     at the anchor; every toggle changes only its own 8x16 rectangle and both
//     states match the independent composite; ink numbers for off and on;
//   * the cursor: each item 0..7 turns only its own band white (font 4), `-1`
//     and 99 highlight nothing, both lines of a two-line item whiten together,
//     and the in-game camera line (font 8) is never highlighted;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * the camera: 0/1/2 select None/Some/Full, out-of-range values clamp, the
//     in-game flag selects `DYNAMIC CAMERA: HUMAN` in font 8;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `FONTS4`, one with `FONTS4` but no `PLATE`, then no
//     `CHECK`, no `NOCHECK`, a `PLATE` of the wrong size, an unknown background
//     entry, `fade_lut` without a table and a dump, and a missing dump file —
//     all `play_options_render:` texts with a cleared buffer; a complete
//     synthetic archive renders; two renders are byte-identical.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/PLAY_OPTIONS_RENDER.md §3 pins them.

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
#include "ui/play_options_render.h"

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
    gm::PlayOptionsRenderStats stats;
};

Frame run(const fs::path& game, const gm::PlayOptionsRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_play_options(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kPlayOptionsBackground; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Differing pixels outside the rectangle `[x0..x1] x [y0..y1]`.
long long differing_outside_rect(const Frame& a, const Frame& b, int x0, int y0, int x1,
                                 int y1) {
    long long count = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
                continue;
            }
            if (at(a, x, y) != at(b, x, y)) {
                ++count;
            }
        }
    }
    return count;
}

/// Differing pixels outside the rows `y0..y1` (inclusive).
long long differing_outside_rows(const Frame& a, const Frame& b, int y0, int y1) {
    long long count = 0;
    for (int y = 0; y < a.height; ++y) {
        if (y >= y0 && y <= y1) {
            continue;
        }
        for (int x = 0; x < a.width; ++x) {
            if (at(a, x, y) != at(b, x, y)) {
                ++count;
            }
        }
    }
    return count;
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

/// Bounding box of the non-black pixels of the rows `y0..y1` (only columns
/// `x_min..x_limit-1`).
Box ink_box(const Frame& frame, int y0, int y1, int x_limit = 320, int x_min = 0) {
    Box box;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = std::max(0, x_min); x < std::min(frame.width, x_limit); ++x) {
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

/// Bounding box of a `0`-marked band (`compose_band` output).
Box band_box(const std::vector<std::uint32_t>& band, int width, int height) {
    Box box;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (band[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)] == 0) {
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
    const std::size_t entries = atlas.palette.size() / 4u;
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
                if (index == 0 || index >= entries) {
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

// --- assets, decoded independently -------------------------------------------

/// One 8-bpp `FRONTEND/BMP.IDX` entry as `parse_bmp` returns it (top-down rows,
/// RGBA palette forced opaque).
struct Asset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> indices;
    std::vector<std::uint32_t> palette;

    std::uint32_t colour(int x, int y) const {
        const std::uint8_t index =
            indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)];
        return palette[index];
    }
};

Asset load_asset(const fs::path& game, const std::string& name) {
    Asset asset;
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
    asset.width = image->width;
    asset.height = image->height;
    asset.indices = image->pixels;
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

/// Masked blit of a sprite into a `width` x `height` band, index 0 transparent.
[[maybe_unused]] std::vector<std::uint32_t> compose_sprite(const Asset& sprite, int x,
                                                          int y, int width,
                                          int height) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height),
                                   0u);
    for (int row = 0; row < sprite.height; ++row) {
        for (int column = 0; column < sprite.width; ++column) {
            const std::uint8_t index =
                sprite.indices[static_cast<std::size_t>(row) *
                                   static_cast<std::size_t>(sprite.width) +
                               static_cast<std::size_t>(column)];
            if (index == 0) {
                continue;
            }
            const int px = x + column;
            const int py = y + row;
            if (px < 0 || px >= width || py < 0 || py >= height) {
                continue;
            }
            out[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(px)] = sprite.palette[index];
        }
    }
    return out;
}

/// The 8x16 checkbox rectangle at (`x`, `anchor_y`): `CHECK` at the anchor when
/// checked, `NOCHECK` 8 rows lower otherwise.  0 = untouched.
std::vector<std::uint32_t> compose_checkbox(const Asset& check, const Asset& nocheck,
                                            int x, bool checked) {
    const int width = gm::kPlayOptionsCheckWidth;
    const int height = gm::kPlayOptionsCheckHeight;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height),
                                   0u);
    const Asset& sprite = checked ? check : nocheck;
    const int dy = checked ? 0 : gm::kPlayOptionsNoCheckDrop;
    for (int row = 0; row < sprite.height; ++row) {
        for (int column = 0; column < sprite.width; ++column) {
            const std::uint8_t index =
                sprite.indices[static_cast<std::size_t>(row) *
                                   static_cast<std::size_t>(sprite.width) +
                               static_cast<std::size_t>(column)];
            if (index == 0) {
                continue;
            }
            const int y = row + dy;
            if (y < 0 || y >= height || column >= width) {
                continue;
            }
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(column)] = sprite.palette[index];
        }
    }
    (void)x;
    return out;
}

/// Compare the rows `y..y+height-1` of the 196-px band of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what, int x_min = 0) {
    const int width = gm::kPlayOptionsPanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    int wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = std::max(0, x_min); column < width; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kPlayOptionsBackground : want;
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

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_play_options_render";
    }
    return fs::temp_directory_path() / "gm_play_options_render";
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

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::PlayOptionsRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::PlayOptionsRenderStats stats;
    stats.rows = 7;
    stats.sliders = 7;
    stats.checks = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_play_options(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "play_options_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.sliders, 0, what + ": sliders cleared");
    check_eq(stats.checks, 0, what + ": checks cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // Geometry of 0x486170 (rows 8, step 15, title row 11, x 25/13).
    check_eq(gm::kPlayOptionsRows, 8, tag + "rows = 8 (0x67f10c at 0x486170)");
    check_eq(gm::kPlayOptionsItemStep, 15, tag + "step = 15 (0x662d58 at 0x48617a)");
    check_eq(gm::kPlayOptionsTitleY, 11, tag + "title row 11 (push 0xb at 0x48618d)");
    check_eq(gm::kPlayOptionsTitleX, 13, tag + "title x 13 fallback (push 0xd)");
    check_eq(gm::kPlayOptionsTitleFont, 5, tag + "title font 5");
    check_eq(gm::kPlayOptionsRowFont, 0, tag + "row font 0");
    check_eq(gm::kPlayOptionsSelectedFont, 4, tag + "highlight font 4");
    check_eq(gm::kPlayOptionsInGameFont, 8, tag + "in-game camera font 8");
    check_eq(std::string(gm::kPlayOptionsTitle), std::string("PLAY OPTIONS"),
             tag + "title 0x4bd040");
    check_eq(std::string(gm::kPlayOptionsPanelName), std::string("PLATE"), tag + "PLATE");
    check_eq(gm::kPlayOptionsPanelWidth, 196, tag + "PLATE 196 wide");
    check_eq(gm::kPlayOptionsPanelHeight, 131, tag + "PLATE 131 high");

    // The row immediates and the resulting y of every item.
    const std::array<int, 8> want_row = {21, 17, 13, 15, 18, 20, 15, 9};
    const std::array<int, 8> want_y = {21, 32, 43, 60, 78, 95, 105, 114};
    const std::array<int, 8> want_x = {25, 25, 25, 25, 25, 25, 13, 13};
    for (int item = 0; item < 8; ++item) {
        check_eq(gm::play_options_item_row(item), want_row[static_cast<std::size_t>(item)],
                 tag + "row immediate of item " + std::to_string(item));
        check_eq(gm::play_options_item_y(item), want_y[static_cast<std::size_t>(item)],
                 tag + "y of item " + std::to_string(item));
        check_eq(gm::play_options_item_x(item), want_x[static_cast<std::size_t>(item)],
                 tag + "x of item " + std::to_string(item));
    }

    // Second lines of items 2/3/4 only.
    const std::array<int, 8> want_second = {-1, -1, 51, 68, 86, -1, -1, -1};
    for (int item = 0; item < 8; ++item) {
        check_eq(gm::play_options_second_y(item),
                 want_second[static_cast<std::size_t>(item)],
                 tag + "second line y of item " + std::to_string(item));
    }
    check_eq(gm::play_options_second_y(-5), -1, tag + "second line y clamp low");
    check_eq(gm::play_options_second_y(99), -1, tag + "second line y clamp high");

    // Bits of 0x5a46f8 (docs/CONFIG.md §3.1) and the proved mask.
    const std::array<std::uint32_t, 8> want_bit = {0x400u,  0x4000u, 0x8000u, 0x10000u,
                                                   0x1000u, 0x100000u, 0u,   0u};
    for (int item = 0; item < 8; ++item) {
        check_eq(static_cast<long long>(gm::play_options_flag_bit(item)),
                 static_cast<long long>(want_bit[static_cast<std::size_t>(item)]),
                 tag + "flag bit of item " + std::to_string(item));
    }
    check_eq(static_cast<long long>(gm::kPlayOptionsFlagMask), 0x11D400ll,
             tag + "proven mask 0x11D400 (0x44d68c)");

    // .bss default 0, shipped CFG 0x10400, camera .data 1.
    check_eq(gm::kPlayOptionsFlagsBssDefault, 0, tag + "flags .bss default 0");
    check_eq(gm::kPlayOptionsFlagsShippedCfg, 0x10400,
             tag + "shipped CFG flags 0x10400");
    check_eq(gm::kPlayOptionsCameraDefault, 1, tag + "camera .data default 1");
    check_eq(gm::kPlayOptionsDefaultCursor, 0, tag + "cursor .bss default 0");
    check(!gm::kPlayOptionsInGameDefault, tag + "[0x6e89c0] .bss default 0");
    // The shipped CFG value sets exactly items 0 and 3.
    const int shipped = gm::kPlayOptionsFlagsShippedCfg;
    for (int item = 0; item < 6; ++item) {
        const bool want = item == gm::kPlayOptionsSkipEmptyItem ||
                          item == gm::kPlayOptionsAutoSwitchItem;
        check(gm::play_options_checked(item, shipped) == want,
              tag + "shipped CFG item " + std::to_string(item) +
                  (want ? " checked" : " unchecked"));
    }
    // Unknown bits do not leak into any checkbox.
    for (int item = 0; item < 8; ++item) {
        check(!gm::play_options_checked(item, static_cast<int>(0x80000000u | 0x2u)),
              tag + "unproven bits ignored by item " + std::to_string(item));
    }

    // Camera clamp 0..2.
    check_eq(gm::play_options_camera(-3), 0, tag + "camera clamp low 0");
    check_eq(gm::play_options_camera(2), 2, tag + "camera max 2");
    check_eq(gm::play_options_camera(7), 2, tag + "camera clamp high 2");

    // Checkboxes: six of them, eight rows above the label, x 16.
    check_eq(gm::kPlayOptionsCheckboxCount, 6, tag + "six checkboxes");
    check_eq(gm::kPlayOptionsCheckX, 16, tag + "checkbox x 16 (0x686ffc..)");
    check_eq(gm::kPlayOptionsCheckWidth, 8, tag + "CHECK 8 wide");
    check_eq(gm::kPlayOptionsCheckHeight, 16, tag + "CHECK 16 high");
    check_eq(gm::kPlayOptionsNoCheckWidth, 8, tag + "NOCHECK 8 wide");
    check_eq(gm::kPlayOptionsNoCheckHeight, 8, tag + "NOCHECK 8 high");
    check_eq(gm::kPlayOptionsNoCheckDrop, 8, tag + "NOCHECK drawn 8 rows lower");
    const std::array<int, 8> want_check = {13, 24, 35, 52, 70, 87, -1, -1};
    for (int item = 0; item < 8; ++item) {
        check_eq(gm::play_options_check_y(item),
                 want_check[static_cast<std::size_t>(item)],
                 tag + "checkbox anchor y of item " + std::to_string(item));
        if (item < 6) {
            check_eq(gm::play_options_check_y(item),
                     gm::play_options_item_y(item) - 8,
                     tag + "checkbox of item " + std::to_string(item) +
                         " is 8 rows above its label");
        }
    }

    // Labels are the handler's literal strings.
    gm::PlayOptionsRenderOptions options;
    const std::array<std::string, 8> want_label = {
        "Skip Empty Weapons",
        "Skip Deadly Weapons",
        "Switch Weapon To Picked-Up",
        "Auto Switch Weapons When",
        "Don't Auto-Target when",
        "Mouse Cheating",
        "Dynamic Camera: Some",
        "Done",
    };
    for (int item = 0; item < 8; ++item) {
        check_eq(gm::play_options_label(item, options),
                 want_label[static_cast<std::size_t>(item)],
                 tag + "label of item " + std::to_string(item));
    }
    check_eq(gm::play_options_label(99, options), std::string("Done"),
             tag + "out-of-range id -> Done");
    const std::array<std::string, 8> want_second_label = {
        "", "", "Ammo", "Ammo Depleted", "Looking", "", "", ""};
    for (int item = 0; item < 8; ++item) {
        check_eq(gm::play_options_second_label(item),
                 want_second_label[static_cast<std::size_t>(item)],
                 tag + "second label of item " + std::to_string(item));
    }
    options.dynamic_camera = 0;
    check_eq(gm::play_options_label(6, options), std::string("Dynamic Camera: None"),
             tag + "camera 0 = 0x4bcf38");
    options.dynamic_camera = 2;
    check_eq(gm::play_options_label(6, options), std::string("Dynamic Camera: Full"),
             tag + "camera 2 = 0x4bcf68");
    options.dynamic_camera = 9;
    check_eq(gm::play_options_label(6, options), std::string("Dynamic Camera: Full"),
             tag + "camera 9 clamps to Full");
    options.in_game = true;
    options.dynamic_camera = 0;
    check_eq(gm::play_options_label(6, options), std::string("DYNAMIC CAMERA: HUMAN"),
             tag + "in-game camera line 0x4bcf80");

    // Font of every item: the cursor only replaces font 0.
    gm::PlayOptionsRenderOptions plain;
    for (int item = 0; item < 8; ++item) {
        const int want = item == plain.cursor ? gm::kPlayOptionsSelectedFont
                                              : gm::kPlayOptionsRowFont;
        check_eq(gm::play_options_item_font(item, plain), want,
                 tag + "font of item " + std::to_string(item) + " at cursor 0");
    }
    gm::PlayOptionsRenderOptions camera_cursor;
    camera_cursor.cursor = 6;
    check_eq(gm::play_options_item_font(6, camera_cursor), gm::kPlayOptionsSelectedFont,
             tag + "cursor 6 whitens the camera line in the front end");
    camera_cursor.in_game = true;
    check_eq(gm::play_options_item_font(6, camera_cursor), gm::kPlayOptionsInGameFont,
             tag + "in-game camera line keeps font 8 even at cursor 6");
    check_eq(gm::play_options_item_font(0, camera_cursor), gm::kPlayOptionsRowFont,
             tag + "cursor 6 does not whiten item 0");
    gm::PlayOptionsRenderOptions no_cursor;
    no_cursor.cursor = -1;
    check_eq(gm::play_options_item_font(3, no_cursor), gm::kPlayOptionsRowFont,
             tag + "cursor -1 highlights nothing");
}

/// An independent whole-frame model: black canvas, the title, then the
/// handler's emission order (for each checkable item the box, then the label
/// and its second line; then the camera line and `Done`), clipped to the
/// 196x131 band.  The checkbox areas of neighbouring items overlap (the anchors
/// are 11..18 rows apart while `CHECK` is 16 rows high), so the 8x16 band of a
/// single box cannot be compared against black — the model is the reference.
struct ModelCanvas {
    std::vector<std::uint32_t> pixels;
    std::vector<std::uint8_t> covered;
    long long ink = 0;
};

ModelCanvas model_canvas(const std::map<int, gm::FontAtlas>& fonts,
                         const gm::PlayOptionsRenderOptions& options,
                         const Asset& check_asset, const Asset& nocheck_asset) {
    const int width = gm::kPlayOptionsCanvasWidth;
    const int height = gm::kPlayOptionsCanvasHeight;
    ModelCanvas model;
    model.pixels.assign(static_cast<std::size_t>(width) *
                            static_cast<std::size_t>(height),
                        gm::kPlayOptionsBackground);
    model.covered.assign(static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(height),
                         0);
    const int clip_w = std::min(width, gm::kPlayOptionsPanelWidth);
    const int clip_h = std::min(height, gm::kPlayOptionsPanelHeight);
    const auto put = [&](int x, int y, std::uint32_t colour) {
        if (x < 0 || x >= clip_w || y < 0 || y >= clip_h) {
            return;
        }
        const std::size_t at = static_cast<std::size_t>(y) *
                                   static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
        model.pixels[at] = colour;
        if (model.covered[at] == 0) {
            model.covered[at] = 1;
            ++model.ink;
        }
    };
    const auto mark_text = [&](int font, const std::string& text, int x, int y) {
        const gm::FontAtlas& atlas = fonts.at(font);
        const std::vector<std::uint32_t> band =
            compose_band(atlas, text, x, gm::kPlayOptionsPanelWidth, atlas.cell_height);
        for (int row = 0; row < atlas.cell_height; ++row) {
            for (int column = 0; column < gm::kPlayOptionsPanelWidth; ++column) {
                const std::uint32_t colour =
                    band[static_cast<std::size_t>(row) *
                             static_cast<std::size_t>(gm::kPlayOptionsPanelWidth) +
                         static_cast<std::size_t>(column)];
                if (colour == 0) {
                    continue;
                }
                put(column, y + row, colour);
            }
        }
    };
    const auto mark_sprite = [&](const Asset& sprite, int x, int y) {
        for (int row = 0; row < sprite.height; ++row) {
            for (int column = 0; column < sprite.width; ++column) {
                const std::uint8_t index =
                    sprite.indices[static_cast<std::size_t>(row) *
                                       static_cast<std::size_t>(sprite.width) +
                                   static_cast<std::size_t>(column)];
                if (index == 0) {
                    continue;
                }
                put(x + column, y + row, sprite.palette[index]);
            }
        }
    };

    const gm::FontAtlas& title_face = fonts.at(gm::kPlayOptionsTitleFont);
    const int title_x = std::max(
        0, (gm::kPlayOptionsPanelWidth - pen_width(title_face, gm::kPlayOptionsTitle)) / 2);
    mark_text(gm::kPlayOptionsTitleFont, std::string(gm::kPlayOptionsTitle), title_x,
              gm::kPlayOptionsTitleY);
    for (int item = 0; item < gm::kPlayOptionsCheckboxCount; ++item) {
        const bool on = gm::play_options_checked(item, options.flags);
        mark_sprite(on ? check_asset : nocheck_asset, gm::kPlayOptionsCheckX,
                    gm::play_options_check_y(item) +
                        (on ? 0 : gm::kPlayOptionsNoCheckDrop));
        const int font = gm::play_options_item_font(item, options);
        mark_text(font, gm::play_options_label(item, options),
                  gm::play_options_item_x(item), gm::play_options_item_y(item));
        const int second_y = gm::play_options_second_y(item);
        if (second_y >= 0) {
            mark_text(font, gm::play_options_second_label(item),
                      gm::play_options_item_x(item), second_y);
        }
    }
    for (int item = gm::kPlayOptionsCameraItem; item < gm::kPlayOptionsItemCount; ++item) {
        mark_text(gm::play_options_item_font(item, options),
                  gm::play_options_label(item, options), gm::play_options_item_x(item),
                  gm::play_options_item_y(item));
    }
    return model;
}

/// Differing pixels of the frame against the whole-frame model over
/// `[0..width) x [0..height)`.
long long model_mismatch(const Frame& frame, const ModelCanvas& model) {
    if (frame.rgba.size() != model.pixels.size()) {
        return -1;
    }
    long long wrong = 0;
    for (std::size_t i = 0; i < model.pixels.size(); ++i) {
        wrong += frame.rgba[i] == model.pixels[i] ? 0 : 1;
    }
    return wrong;
}

/// Differing pixels of the frame against the model inside one rectangle.
long long rect_mismatch_model(const Frame& frame, const ModelCanvas& model, int x, int y,
                              int width, int height) {
    long long wrong = 0;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::size_t at =
                static_cast<std::size_t>(y + row) * static_cast<std::size_t>(frame.width) +
                static_cast<std::size_t>(x + column);
            wrong += frame.rgba[at] == model.pixels[at] ? 0 : 1;
        }
    }
    return wrong;
}

// --- frame -------------------------------------------------------------------
void test_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const gm::PlayOptionsRenderOptions options;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kPlayOptionsCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kPlayOptionsCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kPlayOptionsRows, tag + "stats rows 8");
    check_eq(frame.stats.checks, gm::kPlayOptionsCheckboxCount, tag + "stats checks 6");
    check_eq(frame.stats.sliders, 0, tag + "stats sliders 0");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel is opaque");

    // Title: centred at y 11 (font 5).
    const gm::FontAtlas& title_face = fonts.at(gm::kPlayOptionsTitleFont);
    const int title_x = std::max(
        0, (gm::kPlayOptionsPanelWidth - pen_width(title_face, gm::kPlayOptionsTitle)) / 2);
    check_band(frame, title_face, gm::kPlayOptionsTitle, title_x, gm::kPlayOptionsTitleY,
               tag + "title");
    const Box title_box = ink_box(frame, gm::kPlayOptionsTitleY,
                                  gm::kPlayOptionsTitleY + title_face.cell_height - 1);
    const std::vector<std::uint32_t> title_band =
        compose_band(title_face, gm::kPlayOptionsTitle, title_x,
                     gm::kPlayOptionsPanelWidth, title_face.cell_height);
    const Box title_want =
        band_box(title_band, gm::kPlayOptionsPanelWidth, title_face.cell_height);
    check_eq(title_box.x0, title_want.x0, tag + "title bbox x0");
    check_eq(title_box.x1, title_want.x1, tag + "title bbox x1");
    check_eq(title_box.y0, gm::kPlayOptionsTitleY + title_want.y0, tag + "title bbox y0");
    check_eq(title_box.y1, gm::kPlayOptionsTitleY + title_want.y1, tag + "title bbox y1");
    std::printf("  play_options title: \"%s\" x=%d y=%d ink=%lld bbox %s\n",
                std::string(gm::kPlayOptionsTitle).c_str(), title_x,
                gm::kPlayOptionsTitleY, band_ink(title_band),
                box_text(title_box).c_str());

    // Every label (and second line), its bbox and its band.
    long long row_ink_total = 0;
    for (int item = 0; item < gm::kPlayOptionsItemCount; ++item) {
        const int font = gm::play_options_item_font(item, options);
        const gm::FontAtlas& face = fonts.at(font);
        const std::string label = gm::play_options_label(item, options);
        const int x = gm::play_options_item_x(item);
        const int y = gm::play_options_item_y(item);
        check_band(frame, face, label, x, y,
                   tag + "row " + std::to_string(item) + " \"" + label + "\"", x);
        const std::vector<std::uint32_t> band =
            compose_band(face, label, x, gm::kPlayOptionsPanelWidth, face.cell_height);
        const Box want = band_box(band, gm::kPlayOptionsPanelWidth, face.cell_height);
        const Box got = ink_box(frame, y, y + face.cell_height - 1,
                                gm::kPlayOptionsPanelWidth, x);
        check_eq(got.x0, want.x0, tag + "row " + std::to_string(item) + " bbox x0");
        check_eq(got.x1, want.x1, tag + "row " + std::to_string(item) + " bbox x1");
        check_eq(got.y0, y + want.y0, tag + "row " + std::to_string(item) + " bbox y0");
        check_eq(got.y1, y + want.y1, tag + "row " + std::to_string(item) + " bbox y1");
        const long long ink = band_ink(band);
        row_ink_total += ink;
        std::printf("  play_options row %d: x=%2d y=%3d \"%s\" ink=%lld bbox %s\n", item,
                    x, y, label.c_str(), ink, box_text(got).c_str());

        const int second_y = gm::play_options_second_y(item);
        if (second_y < 0) {
            check_eq(second_y, -1,
                     tag + "row " + std::to_string(item) + " has one line");
            continue;
        }
        const std::string second = gm::play_options_second_label(item);
        check_band(frame, face, second, x, second_y,
                   tag + "row " + std::to_string(item) + " line 2 \"" + second + "\"", x);
        const std::vector<std::uint32_t> band2 =
            compose_band(face, second, x, gm::kPlayOptionsPanelWidth, face.cell_height);
        const Box want2 = band_box(band2, gm::kPlayOptionsPanelWidth, face.cell_height);
        const Box got2 = ink_box(frame, second_y, second_y + face.cell_height - 1,
                                 gm::kPlayOptionsPanelWidth, x);
        check_eq(got2.x0, want2.x0,
                 tag + "row " + std::to_string(item) + " line 2 bbox x0");
        check_eq(got2.x1, want2.x1,
                 tag + "row " + std::to_string(item) + " line 2 bbox x1");
        check_eq(got2.y0, second_y + want2.y0,
                 tag + "row " + std::to_string(item) + " line 2 bbox y0");
        check_eq(got2.y1, second_y + want2.y1,
                 tag + "row " + std::to_string(item) + " line 2 bbox y1");
        const long long ink2 = band_ink(band2);
        row_ink_total += ink2;
        std::printf("  play_options row %d line 2: x=%2d y=%3d \"%s\" ink=%lld bbox %s\n",
                    item, x, second_y, second.c_str(), ink2, box_text(got2).c_str());
    }

    // The six checkboxes: all off by default (flags 0), NOCHECK 8 rows lower.
    const Asset check_asset = load_asset(game, "CHECK");
    const Asset nocheck_asset = load_asset(game, "NOCHECK");
    check(check_asset.ok && nocheck_asset.ok, tag + "CHECK and NOCHECK decoded");
    if (!check_asset.ok || !nocheck_asset.ok) {
        return;
    }
    check_eq(check_asset.width, gm::kPlayOptionsCheckWidth, tag + "CHECK 8 wide");
    check_eq(check_asset.height, gm::kPlayOptionsCheckHeight, tag + "CHECK 16 high");
    check_eq(nocheck_asset.width, gm::kPlayOptionsNoCheckWidth, tag + "NOCHECK 8 wide");
    check_eq(nocheck_asset.height, gm::kPlayOptionsNoCheckHeight, tag + "NOCHECK 8 high");
    const ModelCanvas model = model_canvas(fonts, options, check_asset, nocheck_asset);
    for (int item = 0; item < gm::kPlayOptionsCheckboxCount; ++item) {
        const int y0 = gm::play_options_check_y(item);
        const std::vector<std::uint32_t> want =
            compose_checkbox(check_asset, nocheck_asset, gm::kPlayOptionsCheckX,
                             gm::play_options_checked(item, options.flags));
        check_eq(rect_mismatch_model(frame, model, gm::kPlayOptionsCheckX, y0,
                                    gm::kPlayOptionsCheckWidth,
                                    gm::kPlayOptionsCheckHeight),
                 0, tag + "checkbox " + std::to_string(item) +
                        " matches the whole-frame model");
        std::printf("  play_options check %d: x=%d y=%d off NOCHECK at y=%d ink=%lld\n",
                    item, gm::kPlayOptionsCheckX, y0, y0 + gm::kPlayOptionsNoCheckDrop,
                    band_ink(want));
    }

    // Whole-frame model: every pixel of the default frame.
    check_eq(model_mismatch(frame, model), 0,
             tag + "whole frame matches the model (320x200)");
    check_eq(frame.stats.ink_pixels, model.ink,
             tag + "ink = union of text bands and six checkboxes");
    std::printf("  play_options frame: rows=%d checks=%d sliders=%d arrows=%d "
                "ink=%d (model ink %lld) text-bands-only=%lld missing=%d\n",
                frame.stats.rows, frame.stats.checks, frame.stats.sliders,
                frame.stats.arrows, frame.stats.ink_pixels, model.ink, row_ink_total,
                frame.stats.missing_glyphs);
}

// --- checkboxes --------------------------------------------------------------
void test_checkboxes(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "check: ";
    const Asset check_asset = load_asset(game, "CHECK");
    const Asset nocheck_asset = load_asset(game, "NOCHECK");
    check(check_asset.ok && nocheck_asset.ok, tag + "CHECK and NOCHECK loaded");
    if (!check_asset.ok || !nocheck_asset.ok) {
        return;
    }
    const gm::PlayOptionsRenderOptions off_options;
    const Frame off = run(game, off_options);
    check(off.ok, tag + "all-off frame: " + off.error);
    if (!off.ok) {
        return;
    }
    const ModelCanvas off_model =
        model_canvas(fonts, off_options, check_asset, nocheck_asset);
    check_eq(model_mismatch(off, off_model), 0, tag + "all-off frame matches the model");

    const long long off_ink = band_ink(
        compose_checkbox(check_asset, nocheck_asset, gm::kPlayOptionsCheckX, false));
    const long long on_ink = band_ink(
        compose_checkbox(check_asset, nocheck_asset, gm::kPlayOptionsCheckX, true));

    std::vector<Frame> frames;
    for (int item = 0; item < gm::kPlayOptionsCheckboxCount; ++item) {
        gm::PlayOptionsRenderOptions options;
        options.flags = static_cast<int>(gm::play_options_flag_bit(item));
        Frame frame = run(game, options);
        check(frame.ok, tag + "item " + std::to_string(item) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.checks, gm::kPlayOptionsCheckboxCount,
                 tag + "item " + std::to_string(item) + " keeps 6 checks");
        const int y0 = gm::play_options_check_y(item);
        const long long diff = differing_pixels(off.rgba, frame.rgba);
        check(diff > 0, tag + "item " + std::to_string(item) + " toggle differs");
        check_eq(differing_outside_rect(frame, off, gm::kPlayOptionsCheckX, y0,
                                        gm::kPlayOptionsCheckX +
                                            gm::kPlayOptionsCheckWidth - 1,
                                        y0 + gm::kPlayOptionsCheckHeight - 1),
                 0, tag + "item " + std::to_string(item) +
                        " toggle changes only its 8x16 rectangle");
        const ModelCanvas model =
            model_canvas(fonts, options, check_asset, nocheck_asset);
        check_eq(model_mismatch(frame, model), 0,
                 tag + "item " + std::to_string(item) + " frame matches the model");
        check_eq(rect_mismatch_model(frame, model, gm::kPlayOptionsCheckX, y0,
                                     gm::kPlayOptionsCheckWidth,
                                     gm::kPlayOptionsCheckHeight),
                 0, tag + "item " + std::to_string(item) +
                        " matches the CHECK composite");
        std::printf("  play_options check %d: off NOCHECK (16,%d) ink=%lld, on CHECK "
                    "(16,%d) ink=%lld, frame diff=%lld px\n",
                    item, y0 + gm::kPlayOptionsNoCheckDrop, off_ink, y0, on_ink, diff);
        frames.push_back(std::move(frame));
    }

    // All six at once (the proved mask) and the shipped-CFG pair.
    gm::PlayOptionsRenderOptions all;
    all.flags = static_cast<int>(gm::kPlayOptionsFlagMask);
    const Frame all_on = run(game, all);
    check(all_on.ok, tag + "all-on frame: " + all_on.error);
    if (all_on.ok) {
        check_eq(all_on.stats.checks, 6, tag + "all-on checks 6");
        for (int item = 0; item < 6; ++item) {
            check(gm::play_options_checked(item, all.flags),
                  tag + "all-on bit of item " + std::to_string(item));
        }
        const ModelCanvas model = model_canvas(fonts, all, check_asset, nocheck_asset);
        check_eq(model_mismatch(all_on, model), 0, tag + "all-on frame matches the model");
        std::printf("  play_options checks: off ink=%d, all-on ink=%d, diff=%lld px\n",
                    off.stats.ink_pixels, all_on.stats.ink_pixels,
                    differing_pixels(off.rgba, all_on.rgba));
    }
    gm::PlayOptionsRenderOptions shipped;
    shipped.flags = gm::kPlayOptionsFlagsShippedCfg;
    const Frame shipped_frame = run(game, shipped);
    check(shipped_frame.ok, tag + "shipped-CFG frame: " + shipped_frame.error);
    if (shipped_frame.ok) {
        const ModelCanvas model = model_canvas(fonts, shipped, check_asset, nocheck_asset);
        check_eq(model_mismatch(shipped_frame, model), 0,
                 tag + "shipped-CFG frame matches the model");
        // Only boxes 0 and 3 (x 16..23) may differ from the all-off frame.
        long long outside = 0;
        for (int y = 0; y < shipped_frame.height; ++y) {
            for (int x = 0; x < shipped_frame.width; ++x) {
                if (x >= gm::kPlayOptionsCheckX &&
                    x < gm::kPlayOptionsCheckX + gm::kPlayOptionsCheckWidth) {
                    continue;
                }
                outside += at(shipped_frame, x, y) == at(off, x, y) ? 0 : 1;
            }
        }
        check_eq(outside, 0, tag + "shipped CFG changes only the checkbox columns");
        std::printf("  play_options shipped CFG (0x10400): flags on items 0 and 3, "
                    "diff vs all-off=%lld px\n",
                    differing_pixels(shipped_frame.rgba, off.rgba));
    }
}

// --- cursor ------------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    constexpr std::uint32_t kWhite = 0xEFEFEFFF; // FONTS4 face 4 (239,239,239)

    const auto make = [&](int cursor) {
        gm::PlayOptionsRenderOptions options;
        options.cursor = cursor;
        return run(game, options);
    };
    const Frame none = make(-1);
    check(none.ok, tag + "no highlight frame: " + none.error);
    if (!none.ok) {
        return;
    }
    std::vector<Frame> frames;
    for (int item = 0; item < gm::kPlayOptionsItemCount; ++item) {
        Frame frame = make(item);
        check(frame.ok, tag + "cursor " + std::to_string(item) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        const int y0 = gm::play_options_item_y(item);
        int y1 = y0 + fonts.at(gm::kPlayOptionsSelectedFont).cell_height - 1;
        const int second_y = gm::play_options_second_y(item);
        if (second_y >= 0) {
            y1 = second_y + fonts.at(gm::kPlayOptionsSelectedFont).cell_height - 1;
        }
        check(has_colour(frame, y0, y1, kWhite),
              tag + "cursor " + std::to_string(item) + " paints font 4 in its band");
        check_eq(differing_outside_rows(frame, none, y0, y1), 0,
                 tag + "cursor " + std::to_string(item) + " changes only its rows");
        const long long diff = differing_pixels(frame.rgba, none.rgba);
        check(diff > 0, tag + "cursor " + std::to_string(item) + " differs");
        std::printf("  play_options cursor %d: rows y=%d..%d, diff vs no highlight="
                    "%lld px\n",
                    item, y0, y1, diff);
        frames.push_back(std::move(frame));
    }
    for (std::size_t i = 1; i < frames.size(); ++i) {
        check(differing_pixels(frames[i - 1].rgba, frames[i].rgba) > 0,
              tag + "cursor " + std::to_string(i - 1) + " and " + std::to_string(i) +
                  " differ");
    }
    // Both lines of a two-line item whiten together: cursor 2 changes rows
    // 43..58 (two 8-row bands) and nothing between them is left dark.
    const int two_y = gm::play_options_second_y(gm::kPlayOptionsSwitchAmmoItem);
    check_eq(differing_outside_rows(frames[2], none, gm::play_options_item_y(2),
                                    two_y + fonts.at(4).cell_height - 1),
             0, tag + "cursor 2 whitens both of its lines");

    const Frame high = make(99);
    check(high.ok, tag + "cursor 99 renders: " + high.error);
    if (high.ok) {
        check_eq(differing_pixels(high.rgba, none.rgba), 0,
                 tag + "cursor 99 highlights nothing");
    }
    const Frame entry = make(gm::kPlayOptionsDefaultCursor);
    check(entry.ok && differing_pixels(entry.rgba, frames[0].rgba) == 0,
          tag + "default cursor 0 is the Skip Empty Weapons highlight");
    // The title (font 5) is never highlighted: cursor 0 leaves it dark red.
    constexpr std::uint32_t kRed = 0x9B0F0FFF; // FONTS4 face 5
    check(has_colour(frames[0], gm::kPlayOptionsTitleY,
                     gm::kPlayOptionsTitleY + fonts.at(5).cell_height - 1, kRed),
          tag + "title stays font 5 under a highlight");

    // In-game the camera line is font 8 and therefore never whitens.
    gm::PlayOptionsRenderOptions in_game;
    in_game.in_game = true;
    in_game.cursor = -1;
    const Frame in_game_plain = run(game, in_game);
    gm::PlayOptionsRenderOptions in_game_camera = in_game;
    in_game_camera.cursor = 6;
    const Frame in_game_hi = run(game, in_game_camera);
    check(in_game_plain.ok && in_game_hi.ok, tag + "in-game frames render");
    if (in_game_plain.ok && in_game_hi.ok) {
        check_eq(differing_pixels(in_game_plain.rgba, in_game_hi.rgba), 0,
                 tag + "cursor 6 does not whiten the in-game camera line (font 8)");
        check_eq(in_game_hi.stats.ink_pixels, in_game_plain.stats.ink_pixels,
                 tag + "in-game camera highlight leaves the ink unchanged");
    }
}

// --- camera ------------------------------------------------------------------
void test_camera(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "camera: ";
    const auto make = [&](int camera, bool in_game) {
        gm::PlayOptionsRenderOptions options;
        options.cursor = -1;
        options.dynamic_camera = camera;
        options.in_game = in_game;
        return run(game, options);
    };
    std::vector<Frame> frames;
    for (int camera = 0; camera <= 2; ++camera) {
        Frame frame = make(camera, false);
        check(frame.ok, tag + "camera " + std::to_string(camera) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        gm::PlayOptionsRenderOptions options;
        options.cursor = -1;
        options.dynamic_camera = camera;
        const std::string label = gm::play_options_label(6, options);
        check_band(frame, fonts.at(gm::kPlayOptionsRowFont), label,
                   gm::play_options_item_x(6), gm::play_options_item_y(6),
                   tag + "camera " + std::to_string(camera) + " \"" + label + "\"");
        const long long ink = band_ink(compose_band(
            fonts.at(gm::kPlayOptionsRowFont), label, gm::play_options_item_x(6),
            gm::kPlayOptionsPanelWidth, fonts.at(gm::kPlayOptionsRowFont).cell_height));
        std::printf("  play_options camera %d: \"%s\" ink=%lld\n", camera, label.c_str(),
                    ink);
        frames.push_back(std::move(frame));
    }
    check(differing_pixels(frames[0].rgba, frames[1].rgba) > 0,
          tag + "camera 0 and 1 differ");
    check(differing_pixels(frames[1].rgba, frames[2].rgba) > 0,
          tag + "camera 1 and 2 differ");
    check_eq(differing_outside_rows(frames[0], frames[1], gm::play_options_item_y(6),
                                    gm::play_options_item_y(6) + 7),
             0, tag + "camera 0 vs 1 changes only the camera row");
    const Frame clamped = make(7, false);
    check(clamped.ok && differing_pixels(clamped.rgba, frames[2].rgba) == 0,
          tag + "camera 7 clamps to Full");

    // In-game: `DYNAMIC CAMERA: HUMAN` in font 8, camera index ignored.
    const Frame human = make(0, true);
    const Frame human_full = make(2, true);
    check(human.ok && human_full.ok, tag + "in-game frames render");
    if (!human.ok || !human_full.ok) {
        return;
    }
    gm::PlayOptionsRenderOptions options;
    options.cursor = -1;
    options.in_game = true;
    const std::string label = gm::play_options_label(6, options);
    check_eq(label, std::string("DYNAMIC CAMERA: HUMAN"), tag + "in-game label");
    check_band(human, fonts.at(gm::kPlayOptionsInGameFont), label,
               gm::play_options_item_x(6), gm::play_options_item_y(6),
               tag + "in-game camera line");
    check_eq(differing_pixels(human.rgba, human_full.rgba), 0,
             tag + "in-game ignores the camera index");
    check(differing_pixels(human.rgba, frames[0].rgba) > 0,
          tag + "in-game camera line differs from the front-end one");
    std::printf("  play_options camera in-game: \"%s\" font 8 ink=%lld\n", label.c_str(),
                band_ink(compose_band(fonts.at(gm::kPlayOptionsInGameFont), label,
                                      gm::play_options_item_x(6),
                                      gm::kPlayOptionsPanelWidth,
                                      fonts.at(gm::kPlayOptionsInGameFont).cell_height)));
}

// --- plate -------------------------------------------------------------------
void test_plate(const fs::path& game) {
    const std::string tag = "plate: ";
    const Asset plate = load_asset(game, "PLATE");
    check(plate.ok, tag + "PLATE loaded");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kPlayOptionsPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kPlayOptionsPanelHeight, tag + "PLATE 131 high");

    const Frame black_frame = run(game, gm::PlayOptionsRenderOptions{});
    gm::PlayOptionsRenderOptions plated;
    plated.background = std::string(gm::kPlayOptionsPanelName);
    const Frame plate_frame = run(game, plated);
    check(black_frame.ok && plate_frame.ok, tag + "black and PLATE frames render");
    if (!black_frame.ok || !plate_frame.ok) {
        return;
    }
    check_eq(differing_pixels(black_frame.rgba, plate_frame.rgba) > 0 ? 1 : 0, 1,
             tag + "PLATE changes the frame");
    check_eq(plate_frame.stats.ink_pixels, black_frame.stats.ink_pixels,
             tag + "PLATE leaves the text ink unchanged");
    long long wrong = 0;
    for (int y = 0; y < plate.height; ++y) {
        for (int x = 0; x < plate.width; ++x) {
            if (!is_black(at(black_frame, x, y))) {
                continue; // glyph/sprite pixels sit on top of the panel
            }
            wrong += at(plate_frame, x, y) == plate.colour(x, y) ? 0 : 1;
        }
    }
    check_eq(wrong, 0, tag + "PLATE pixels match the independent decode under the ink");
    long long outside = 0;
    for (int y = 0; y < plate_frame.height; ++y) {
        for (int x = 0; x < plate_frame.width; ++x) {
            if (x < plate.width && y < plate.height) {
                continue;
            }
            outside += is_black(at(plate_frame, x, y)) ? 0 : 1;
        }
    }
    check_eq(outside, 0, tag + "everything outside the 196x131 band stays black");
    std::printf("  play_options plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "ink %d unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kPlayOptionsCanvasWidth) *
                                  gm::kPlayOptionsCanvasHeight;

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
        gm::PlayOptionsRenderOptions options;
        options.background = std::string(gm::kPlayOptionsPanelName);
        options.cursor = -1;
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
                if (x < gm::kPlayOptionsPanelWidth && y < gm::kPlayOptionsPanelHeight) {
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
             tag + "level 11 is byte-identical to no fade (0/64000)");
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
        check_eq(outside_panel(exact[static_cast<std::size_t>(i)]), 0,
                 tag + "level " + std::to_string(level) + " leaves the canvas black");
        check_eq(exact[static_cast<std::size_t>(i)].stats.ink_pixels, plain.stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const long long pair12_13 = differing_pixels(exact[0].rgba, exact[1].rgba);
    const long long pair13_14 = differing_pixels(exact[1].rgba, exact[2].rgba);
    const long long pair14_15 = differing_pixels(exact[2].rgba, exact[3].rgba);
    check(pair12_13 > 0 && pair13_14 > 0 && pair14_15 > 0,
          tag + "levels 12..15 are pairwise different");
    std::printf("  fade: level 11 vs no fade: 0/64000 px differ; adjacent 12..15 diffs "
                "%lld/%lld/%lld; vs 11: %lld/%lld/%lld/%lld\n",
                pair12_13, pair13_14, pair14_15,
                differing_pixels(exact[0].rgba, identity.rgba),
                differing_pixels(exact[1].rgba, identity.rgba),
                differing_pixels(exact[2].rgba, identity.rgba),
                differing_pixels(exact[3].rgba, identity.rgba));

    const Frame again = make(true, 13);
    check(again.ok && differing_pixels(again.rgba, exact[1].rgba) == 0,
          tag + "level 13 is deterministic");
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::PlayOptionsRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::PlayOptionsRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> check_bytes = installed_entry(game, "CHECK");
    const std::optional<std::vector<std::uint8_t>> nocheck_bytes =
        installed_entry(game, "NOCHECK");
    check(fonts4 && plate && check_bytes && nocheck_bytes,
          "negatives: FONTS4/PLATE/CHECK/NOCHECK bytes available");
    if (!fonts4 || !plate || !check_bytes || !nocheck_bytes) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. CHECK + NOCHECK + PLATE: FONTS4 is missing.
    check(write_archive(root / "game-without-font4", {"PLATE", "CHECK", "NOCHECK"},
                        {*plate, *check_bytes, *nocheck_bytes}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::PlayOptionsRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 2. FONTS4 + CHECK + NOCHECK: PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4", "CHECK", "NOCHECK"},
                        {*fonts4, *check_bytes, *nocheck_bytes}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-plate", gm::PlayOptionsRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 3. + PLATE: CHECK is missing.
    check(write_archive(root / "game-without-check", {"FONTS4", "PLATE", "NOCHECK"},
                        {*fonts4, *plate, *nocheck_bytes}),
          "negatives: FONTS4+PLATE archive written");
    check_refused(root / "game-without-check", gm::PlayOptionsRenderOptions{},
                  "archive without CHECK", "CHECK");

    // 4. + CHECK: NOCHECK is missing.
    check(write_archive(root / "game-without-nocheck", {"FONTS4", "PLATE", "CHECK"},
                        {*fonts4, *plate, *check_bytes}),
          "negatives: FONTS4+PLATE+CHECK archive written");
    check_refused(root / "game-without-nocheck", gm::PlayOptionsRenderOptions{},
                  "archive without NOCHECK", "NOCHECK");

    // 5. A PLATE of the wrong size (the 8x16 CHECK renamed).
    check(write_archive(root / "game-wrong-plate",
                        {"FONTS4", "PLATE", "CHECK", "NOCHECK"},
                        {*fonts4, *check_bytes, *check_bytes, *nocheck_bytes}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::PlayOptionsRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 6. A complete synthetic archive renders.
    check(write_archive(root / "game-complete", {"FONTS4", "PLATE", "CHECK", "NOCHECK"},
                        {*fonts4, *plate, *check_bytes, *nocheck_bytes}),
          "negatives: complete archive written");
    {
        const Frame minimal = run(root / "game-complete", gm::PlayOptionsRenderOptions{});
        check(minimal.ok, "negatives: complete synthetic archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, gm::kPlayOptionsRows,
                     "negatives: synthetic rows 8");
            check_eq(minimal.stats.checks, gm::kPlayOptionsCheckboxCount,
                     "negatives: synthetic checks 6");
            check_eq(minimal.stats.sliders, 0, "negatives: synthetic sliders 0");
            check(minimal.stats.ink_pixels > 0, "negatives: synthetic ink > 0");
            std::printf("  play_options synthetic archive: ink=%d rows=%d checks=%d "
                        "sliders=%d missing=%d\n",
                        minimal.stats.ink_pixels, minimal.stats.rows, minimal.stats.checks,
                        minimal.stats.sliders, minimal.stats.missing_glyphs);
            const Frame again = run(root / "game-complete", gm::PlayOptionsRenderOptions{});
            check(again.ok && differing_pixels(minimal.rgba, again.rgba) == 0,
                  "negatives: synthetic renders are byte-identical");
        }
    }
    fs::remove_all(root, ec);

    // 7. An unknown background entry is a text, not black.
    gm::PlayOptionsRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 8. The exact fade needs a table or a dump.
    gm::PlayOptionsRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::PlayOptionsRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 9. Shipped install: deterministic.
    const Frame first = run(game, gm::PlayOptionsRenderOptions{});
    const Frame second = run(game, gm::PlayOptionsRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  play_options negatives: rows=%d checks=%d sliders=%d ink=%d "
                    "arrows=%d missing=%d\n",
                    first.stats.rows, first.stats.checks, first.stats.sliders,
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
        std::cout << "play_options_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kPlayOptionsTitleFont, gm::kPlayOptionsRowFont,
                             gm::kPlayOptionsSelectedFont, gm::kPlayOptionsInGameFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "play_options_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_frame(game, fonts);
    test_checkboxes(game, fonts);
    test_cursor(game, fonts);
    test_camera(game, fonts);
    test_plate(game);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "play_options_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "play_options_render: ok (" << checks << " checks)\n";
    return 0;
}
