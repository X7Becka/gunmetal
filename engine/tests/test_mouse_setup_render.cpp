// One frame of screen 22, `MOUSE SETUP` (`M8.8` slice 8, `ui/mouse_setup_render`,
// docs/MOUSE_SETUP_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.5
// from the builder `0x489a10` and from pixels:
//
//   * model: `rows` 12, step 8, base row 24, x 18 for `Used By:` and x 9 for the
//     other eleven rows, title row 11, the three `SLIDE` tracks at (105,
//     40/56/72), the `KNOB` base column 108 and the proven range 0.2..2.2 in
//     0.1 steps -> knob columns 108/124/129/135/162 (the engine arithmetic
//     `108 + trunc(27*(v-0.2))` of `0x489ad3`..`0x489af5`, clamped 2..22 tenths);
//     the labels are the handler's `%s` formatting of the three `BMP.IDX`/`.data`
//     tables (`0x4bc548`, `0x4bc468`, `0x4bc558`);
//   * the shipped install: 12 rows, 3 sliders, 0 arrows, no missing glyph,
//     320x200 RGBA, every pixel opaque; the title `MOUSE SETUP` (font 5) is
//     centred at y 11 and each of the twelve rows sits at (18/9, 24 + 8*item) —
//     every band is compared byte for byte with an independent blit and the
//     bounding boxes are printed;
//   * the three slider rectangles are compared with an independent composite of
//     `SLIDE` + `KNOB`; the knob column of value 2/12/22 is pinned (108/135/162),
//     a composite shifted by one pixel does not match, and the frames of the
//     three values differ;
//   * the cursor 0/1/11 changes only the corresponding band (font 4 = white),
//     out-of-range values (-1, 99) highlight nothing, the frames are pairwise
//     different;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `FONTS4`, an index with `FONTS4` but no `SLIDE`, one with
//     `FONTS4`+`SLIDE` but no `KNOB`, an unknown background entry, `fade_lut`
//     without a table and a dump, and a missing dump file — all
//     `mouse_setup_render:` texts with a cleared buffer; two renders are
//     deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/MOUSE_SETUP_RENDER.md §3 pins them.

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
#include "ui/mouse_setup_render.h"

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
    gm::MouseSetupRenderStats stats;
};

Frame run(const fs::path& game, const gm::MouseSetupRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_mouse_setup(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kMouseSetupBackground; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Differing pixels outside the rows `y0..y1` (inclusive).
long long differing_outside(const Frame& a, const Frame& b, int y0, int y1) {
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
/// `x < x_limit`).
Box ink_box(const Frame& frame, int y0, int y1, int x_limit = 320) {
    Box box;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < std::min(frame.width, x_limit); ++x) {
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

/// Compare the rows `y..y+height-1` of the 196-px band of *frame* with the
/// independent blit; every untouched pixel must be the black background.  The
/// `x_limit` keeps the comparison away from the slider rectangles on the
/// `Sensitivity:` rows.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what, int x_limit = gm::kMouseSetupPanelWidth) {
    const int width = gm::kMouseSetupPanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    int wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width && column < x_limit; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kMouseSetupBackground : want;
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

/// The slider rectangle `x` 105..184 of rows `y..y+7`: the masked `SLIDE` track
/// at (105, y) and the opaque `KNOB` at (`knob_x`, y), over black.  0 marks an
/// untouched pixel (the transparent key of the track).
std::vector<std::uint32_t> compose_slider(const Asset& slide, const Asset& knob,
                                          int knob_x, int y, int region_x, int region_width,
                                          int height) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(region_width) *
                                       static_cast<std::size_t>(height),
                                   0u);
    const auto blit = [&](const Asset& sprite, int px, int py, bool masked) {
        for (int row = 0; row < sprite.height; ++row) {
            for (int column = 0; column < sprite.width; ++column) {
                const std::uint8_t index =
                    sprite.indices[static_cast<std::size_t>(row) *
                                       static_cast<std::size_t>(sprite.width) +
                                   static_cast<std::size_t>(column)];
                if (masked && index == 0) {
                    continue;
                }
                const int x = px + column;
                const int yy = py + row;
                if (x < region_x || x >= region_x + region_width || yy < y ||
                    yy >= y + height) {
                    continue;
                }
                out[static_cast<std::size_t>(yy - y) *
                        static_cast<std::size_t>(region_width) +
                    static_cast<std::size_t>(x - region_x)] = sprite.palette[index];
            }
        }
    };
    blit(slide, gm::kMouseSetupSlideX, y, true);
    blit(knob, knob_x, y, false);
    return out;
}

/// Compare the slider rectangle with the frame; returns the number of wrong
/// pixels.
long long slider_mismatch(const Frame& frame, const std::vector<std::uint32_t>& want,
                          int y, int region_x, int region_width, int height) {
    long long wrong = 0;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < region_width; ++column) {
            const std::uint32_t expected =
                want[static_cast<std::size_t>(row) * static_cast<std::size_t>(region_width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, region_x + column, y + row);
            if (got != (expected == 0 ? gm::kMouseSetupBackground : expected)) {
                ++wrong;
            }
        }
    }
    return wrong;
}

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_mouse_setup_render";
    }
    return fs::temp_directory_path() / "gm_mouse_setup_render";
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
void check_refused(const fs::path& game, const gm::MouseSetupRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::MouseSetupRenderStats stats;
    stats.rows = 7;
    stats.sliders = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_mouse_setup(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "mouse_setup_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.sliders, 0, what + ": sliders cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // Geometry of 0x489a10 (rows 12, step 8, base row 24, x 18/9, title row 11).
    check_eq(gm::kMouseSetupRows, 12, tag + "rows = 12 (0x67f10c at 0x489a13)");
    check_eq(gm::kMouseSetupItemStep, 8, tag + "step = 8 (0x662d58 at 0x489a1d)");
    check_eq(gm::kMouseSetupItemFirstY, 24, tag + "base row 24 (push 0x18)");
    check_eq(gm::mouse_setup_item_y(0), 24, tag + "item 0 y");
    check_eq(gm::mouse_setup_item_y(11), 112, tag + "item 11 y");
    check_eq(gm::mouse_setup_item_x(0), 18, tag + "item_x(0) = 18 (push 0x12)");
    check_eq(gm::mouse_setup_item_x(1), 9, tag + "item_x(1) = 9 (push 0x9)");
    check_eq(gm::mouse_setup_item_x(11), 9, tag + "item_x(11) = 9");
    check_eq(gm::kMouseSetupTitleY, 11, tag + "title row 11 (push 0xb at 0x489a2e)");
    check_eq(gm::kMouseSetupTitleFont, 5, tag + "title font 5");
    check_eq(gm::kMouseSetupRowFont, 0, tag + "row font 0");
    check_eq(gm::kMouseSetupSelectedFont, 4, tag + "highlight font 4");
    check_eq(gm::kMouseSetupSlideX, 105, tag + "SLIDE x 105 (0x688501 - 0x6865f8)");
    check_eq(gm::kMouseSetupKnobBaseX, 108, tag + "KNOB base x 108 (0x688504)");
    check_eq(gm::kMouseSetupSlideY[0], 40, tag + "slider 1 y 40");
    check_eq(gm::kMouseSetupSlideY[1], 56, tag + "slider 2 y 56");
    check_eq(gm::kMouseSetupSlideY[2], 72, tag + "slider 3 y 72");
    check_eq(gm::kMouseSetupSensitivityMin, 2, tag + "sensitivity min 0.2 (0x4a2930)");
    check_eq(gm::kMouseSetupSensitivityMax, 22, tag + "sensitivity max 2.2 (0x4a2970)");
    check_eq(gm::kMouseSetupKnobPixelsPerUnit, 27, tag + "27 px per unit (0x4a2948)");
    check_eq(std::string(gm::kMouseSetupTitle), std::string("MOUSE SETUP"),
             tag + "title 0x4bd8c0");

    // Knob columns: 108 + trunc(27*(v-0.2)), v in 0.2..2.2 (0x489ad3..0x489af5).
    check_eq(gm::mouse_setup_knob_x(2), 108, tag + "knob 0.2 -> 108");
    check_eq(gm::mouse_setup_knob_x(8), 124, tag + "knob 0.8 -> 124");
    check_eq(gm::mouse_setup_knob_x(10), 129, tag + "knob 1.0 -> 129");
    check_eq(gm::mouse_setup_knob_x(12), 135, tag + "knob 1.2 -> 135");
    check_eq(gm::mouse_setup_knob_x(22), 162, tag + "knob 2.2 -> 162");
    check_eq(gm::mouse_setup_knob_x(-5), 108, tag + "knob clamp low -> 108");
    check_eq(gm::mouse_setup_knob_x(99), 162, tag + "knob clamp high -> 162");
    check_eq(gm::mouse_setup_sensitivity_tenths(0), 2, tag + "tenths clamp 0 -> 2");
    check_eq(gm::mouse_setup_sensitivity_tenths(100), 22, tag + "tenths clamp 100 -> 22");
    // 21 reachable positions, monotone, inside the 105..176 track.
    int previous = -1;
    bool monotone = true;
    bool inside = true;
    for (int tenths = gm::kMouseSetupSensitivityMin;
         tenths <= gm::kMouseSetupSensitivityMax; ++tenths) {
        const int x = gm::mouse_setup_knob_x(tenths);
        monotone = monotone && x >= previous;
        inside = inside && x >= gm::kMouseSetupSlideX &&
                 x + gm::kMouseSetupKnobWidth <=
                     gm::kMouseSetupSlideX + gm::kMouseSetupSlideWidth;
        previous = x;
    }
    check(monotone, tag + "knob columns are monotone over 0.2..2.2");
    check(inside, tag + "knob stays inside the 72-px track");

    // Labels are the handler's %s formatting (0x4bd8b4/0x4bd8a4/0x4bd894/...).
    gm::MouseSetupRenderOptions options;
    check_eq(gm::mouse_setup_label(0, options), std::string("Used By: Not Used"),
             tag + "item 0 default (0x4bd8b4, 0x5a4938 = 0)");
    options.used_by = 2;
    check_eq(gm::mouse_setup_label(0, options), std::string("Used By: Player 2"),
             tag + "item 0 used_by 2 (0x4bc938)");
    options.axes = {1, 6, 8};
    check_eq(gm::mouse_setup_label(1, options),
             std::string("Left/Right: Forward/Back"), tag + "item 1 axis 1 (0x4bca40)");
    check_eq(gm::mouse_setup_label(3, options), std::string("Forward/Back: Strafe L/R"),
             tag + "item 3 axis 6 (0x4bc9f4)");
    check_eq(gm::mouse_setup_label(5, options),
             std::string("Up/Down (3D): Look Up/Down"), tag + "item 5 axis 8 (0x4bc9d8)");
    check_eq(gm::mouse_setup_label(2, options), std::string("Sensitivity:"),
             tag + "item 2 (0x4bd894)");
    check_eq(gm::mouse_setup_label(4, options), std::string("Sensitivity:"),
             tag + "item 4 (0x4bd894)");
    check_eq(gm::mouse_setup_label(6, options), std::string("Sensitivity:"),
             tag + "item 6 (0x4bd894)");
    check_eq(gm::mouse_setup_label(7, options), std::string("Button 1: Move Forward"),
             tag + "item 7 button 0 (0x4bd85c, 0x4bc558[0])");
    check_eq(gm::mouse_setup_label(8, options), std::string("Button 2: Move Backward"),
             tag + "item 8 button 1 (0x4bd838, 0x4bc558[1])");
    check_eq(gm::mouse_setup_label(9, options), std::string("Button 3: Fire"),
             tag + "item 9 button 5 (0x4bd814, 0x4bc558[5])");
    check_eq(gm::mouse_setup_label(10, options), std::string("Button 4: Activate"),
             tag + "item 10 button 15 (0x4bd7f0, 0x4bc558[15])");
    options.buttons[0] = -1;
    check_eq(gm::mouse_setup_label(7, options), std::string("Button 1: NOT USED"),
             tag + "item 7 no binding (0x4bd848)");
    check_eq(gm::mouse_setup_label(11, options), std::string("Done"),
             tag + "item 11 (0x4bceb4)");
}

// --- frame -------------------------------------------------------------------
void test_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const gm::MouseSetupRenderOptions options;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kMouseSetupCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kMouseSetupCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kMouseSetupRows, tag + "stats rows 12");
    check_eq(frame.stats.sliders, gm::kMouseSetupSliderCount, tag + "stats sliders 3");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW at 0x489a10)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel is opaque");

    // Title: centred at y 11 (font 5).
    const gm::FontAtlas& title_face = fonts.at(gm::kMouseSetupTitleFont);
    const int title_x =
        std::max(0, (gm::kMouseSetupPanelWidth - pen_width(title_face, gm::kMouseSetupTitle)) / 2);
    check_band(frame, title_face, gm::kMouseSetupTitle, title_x, gm::kMouseSetupTitleY,
               tag + "title");
    const Box title_box = ink_box(frame, gm::kMouseSetupTitleY,
                                  gm::kMouseSetupTitleY + title_face.cell_height - 1);
    const std::vector<std::uint32_t> title_band =
        compose_band(title_face, gm::kMouseSetupTitle, title_x, gm::kMouseSetupPanelWidth,
                     title_face.cell_height);
    const Box title_want = band_box(title_band, gm::kMouseSetupPanelWidth,
                                    title_face.cell_height);
    check_eq(title_box.x0, title_want.x0, tag + "title bbox x0");
    check_eq(title_box.x1, title_want.x1, tag + "title bbox x1");
    check_eq(title_box.y0, gm::kMouseSetupTitleY + title_want.y0, tag + "title bbox y0");
    check_eq(title_box.y1, gm::kMouseSetupTitleY + title_want.y1, tag + "title bbox y1");

    // Every row, its bbox and its band.  The three `Sensitivity:` rows share
    // their y with a slider track, so the comparison stops at the track's x.
    long long text_ink = band_ink(title_band); // the title band above
    for (int item = 0; item < gm::kMouseSetupRows; ++item) {
        const gm::FontAtlas& face =
            fonts.at(item == options.cursor ? gm::kMouseSetupSelectedFont
                                            : gm::kMouseSetupRowFont);
        const std::string label = gm::mouse_setup_label(item, options);
        const int x = gm::mouse_setup_item_x(item);
        const int y = gm::mouse_setup_item_y(item);
        const bool slider_row = item == 2 || item == 4 || item == 6;
        const int x_limit = slider_row ? gm::kMouseSetupSlideX : gm::kMouseSetupPanelWidth;
        const int band_width = slider_row ? x_limit : gm::kMouseSetupPanelWidth;
        check_band(frame, face, label, x, y,
                   tag + "row " + std::to_string(item) + " \"" + label + "\"", x_limit);
        const std::vector<std::uint32_t> band =
            compose_band(face, label, x, band_width, face.cell_height);
        const Box want = band_box(band, band_width, face.cell_height);
        const Box got = ink_box(frame, y, y + face.cell_height - 1, x_limit);
        check_eq(got.x0, want.x0, tag + "row " + std::to_string(item) + " bbox x0");
        check_eq(got.x1, want.x1, tag + "row " + std::to_string(item) + " bbox x1");
        text_ink += band_ink(band);
        if (item == 0 || item == gm::kMouseSetupRows - 1) {
            std::printf("  mouse_setup row %2d: x=%d y=%d \"%s\" bbox %s\n", item, x, y,
                        label.c_str(), box_text(got).c_str());
        }
    }
    std::printf("  mouse_setup title: x=%d y=%d bbox %s\n", title_x, gm::kMouseSetupTitleY,
                box_text(title_box).c_str());

    // The three slider rectangles are inside the ink total; the twelve bands do
    // not overlap the sliders, so text ink + slider ink is the frame ink.
    const Asset slide_asset = load_asset(game, "SLIDE");
    const Asset knob_asset = load_asset(game, "KNOB");
    check(slide_asset.ok && knob_asset.ok, tag + "SLIDE and KNOB decoded");
    long long slider_ink = 0;
    if (slide_asset.ok && knob_asset.ok) {
        for (int i = 0; i < gm::kMouseSetupSliderCount; ++i) {
            const int y = gm::kMouseSetupSlideY[static_cast<std::size_t>(i)];
            const int knob_x =
                gm::mouse_setup_knob_x(options.sensitivity[static_cast<std::size_t>(i)]);
            const std::vector<std::uint32_t> want =
                compose_slider(slide_asset, knob_asset, knob_x, y, gm::kMouseSetupSlideX,
                               gm::kMouseSetupSlideWidth, gm::kMouseSetupSlideHeight);
            slider_ink += band_ink(want);
        }
        check_eq(frame.stats.ink_pixels, text_ink + slider_ink,
                 tag + "ink = text bands + slider rectangles");
    }
    std::printf("  mouse_setup frame: rows=%d sliders=%d ink=%d (text %lld + sliders %lld) "
                "missing=%d arrows=%d\n",
                frame.stats.rows, frame.stats.sliders, frame.stats.ink_pixels, text_ink,
                slider_ink, frame.stats.missing_glyphs, frame.stats.arrows);
}

// --- sliders -----------------------------------------------------------------
void test_sliders(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "sliders: ";
    const Asset slide = load_asset(game, "SLIDE");
    const Asset knob = load_asset(game, "KNOB");
    check(slide.ok, tag + "SLIDE loaded");
    check(knob.ok, tag + "KNOB loaded");
    if (!slide.ok || !knob.ok) {
        return;
    }
    check_eq(slide.width, gm::kMouseSetupSlideWidth, tag + "SLIDE 72 wide");
    check_eq(slide.height, gm::kMouseSetupSlideHeight, tag + "SLIDE 8 high");
    check_eq(knob.width, gm::kMouseSetupKnobWidth, tag + "KNOB 8 wide");
    check_eq(knob.height, gm::kMouseSetupKnobHeight, tag + "KNOB 8 high");

    // Default sensitivities 1.0/0.8/0.8 -> knob columns 129/124/124.
    const Frame plain = run(game, gm::MouseSetupRenderOptions{});
    check(plain.ok, tag + "default frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    long long total_wrong = 0;
    for (int i = 0; i < gm::kMouseSetupSliderCount; ++i) {
        const int y = gm::kMouseSetupSlideY[static_cast<std::size_t>(i)];
        const int knob_x = gm::mouse_setup_knob_x(
            gm::kMouseSetupSensitivityDefault[static_cast<std::size_t>(i)]);
        const std::vector<std::uint32_t> want =
            compose_slider(slide, knob, knob_x, y, gm::kMouseSetupSlideX,
                           gm::kMouseSetupSlideWidth, gm::kMouseSetupSlideHeight);
        const long long wrong =
            slider_mismatch(plain, want, y, gm::kMouseSetupSlideX,
                            gm::kMouseSetupSlideWidth, gm::kMouseSetupSlideHeight);
        total_wrong += wrong;
        check_eq(wrong, 0, tag + "slider " + std::to_string(i + 1) + " matches the " +
                               "independent SLIDE+KNOB composite");
        std::printf("  mouse_setup slider %d: track (%d,%d) 72x8, knob x=%d (%.1f)\n",
                    i + 1, gm::kMouseSetupSlideX, y, knob_x,
                    gm::kMouseSetupSensitivityDefault[static_cast<std::size_t>(i)] / 10.0);
    }
    check_eq(total_wrong, 0, tag + "all three slider rectangles match");

    // The knob column is pinned: a composite shifted by one pixel must differ.
    for (const int tenths : {2, 12, 22}) {
        gm::MouseSetupRenderOptions options;
        options.sensitivity = {tenths, tenths, tenths};
        const Frame frame = run(game, options);
        check(frame.ok, tag + "value " + std::to_string(tenths) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        const int knob_x = gm::mouse_setup_knob_x(tenths);
        long long exact_wrong = 0;
        long long shifted_wrong = 0;
        for (int i = 0; i < gm::kMouseSetupSliderCount; ++i) {
            const int y = gm::kMouseSetupSlideY[static_cast<std::size_t>(i)];
            exact_wrong += slider_mismatch(
                frame,
                compose_slider(slide, knob, knob_x, y, gm::kMouseSetupSlideX,
                               gm::kMouseSetupSlideWidth, gm::kMouseSetupSlideHeight),
                y, gm::kMouseSetupSlideX, gm::kMouseSetupSlideWidth,
                gm::kMouseSetupSlideHeight);
            shifted_wrong += slider_mismatch(
                frame,
                compose_slider(slide, knob, knob_x + 1, y, gm::kMouseSetupSlideX,
                               gm::kMouseSetupSlideWidth, gm::kMouseSetupSlideHeight),
                y, gm::kMouseSetupSlideX, gm::kMouseSetupSlideWidth,
                gm::kMouseSetupSlideHeight);
        }
        check_eq(exact_wrong, 0, tag + "knob at " + std::to_string(knob_x) +
                                     " matches (value " + std::to_string(tenths) + ")");
        check(shifted_wrong > 0, tag + "knob shifted by 1 px does not match (value " +
                                     std::to_string(tenths) + ")");
        std::printf("  mouse_setup knob value %2d (%.1f): x=%d, exact mismatch %lld, "
                    "shifted mismatch %lld\n",
                    tenths, tenths / 10.0, knob_x, exact_wrong, shifted_wrong);
    }

    // Values 0.2 and 2.2 give different frames.
    gm::MouseSetupRenderOptions low;
    low.sensitivity = {2, 2, 2};
    gm::MouseSetupRenderOptions high;
    high.sensitivity = {22, 22, 22};
    const Frame low_frame = run(game, low);
    const Frame high_frame = run(game, high);
    check(low_frame.ok && high_frame.ok, tag + "0.2 and 2.2 frames render");
    if (low_frame.ok && high_frame.ok) {
        const long long diff = differing_pixels(low_frame.rgba, high_frame.rgba);
        check(diff > 0, tag + "0.2 vs 2.2 differ");
        std::printf("  mouse_setup slider travel: 0.2 vs 2.2 diff=%lld px (54 px per "
                    "knob, 3 sliders)\n",
                    diff);
    }
    (void)fonts;
}

// --- cursor ------------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    constexpr std::uint32_t kWhite = 0xEFEFEFFF; // FONTS4 face 4 (239,239,239)

    const auto make = [&](int cursor) {
        gm::MouseSetupRenderOptions options;
        options.cursor = cursor;
        return run(game, options);
    };
    const Frame none = make(-1);
    check(none.ok, tag + "no highlight frame: " + none.error);
    if (!none.ok) {
        return;
    }
    check(!has_colour(none, gm::mouse_setup_item_y(0),
                      gm::mouse_setup_item_y(0) +
                          fonts.at(gm::kMouseSetupSelectedFont).cell_height - 1,
                      kWhite),
          tag + "cursor -1 has no white glyph on row 0");

    const int items[3] = {0, 1, gm::kMouseSetupRows - 1};
    std::vector<Frame> frames;
    for (const int item : items) {
        Frame frame = make(item);
        check(frame.ok, tag + "cursor " + std::to_string(item) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        const int y0 = gm::mouse_setup_item_y(item);
        const int y1 = y0 + fonts.at(gm::kMouseSetupSelectedFont).cell_height - 1;
        check(has_colour(frame, y0, y1, kWhite),
              tag + "cursor " + std::to_string(item) + " paints font 4 in its band");
        check_eq(differing_outside(frame, none, y0, y1), 0,
                 tag + "cursor " + std::to_string(item) + " changes only its band");
        const long long diff = differing_pixels(frame.rgba, none.rgba);
        check(diff > 0, tag + "cursor " + std::to_string(item) + " differs");
        std::printf("  mouse_setup cursor %2d: band y=%d..%d, diff vs no highlight=%lld px\n",
                    item, y0, y1, diff);
        frames.push_back(std::move(frame));
    }
    check_eq(differing_pixels(frames[0].rgba, frames[1].rgba) > 0 ? 1 : 0, 1,
             tag + "cursor 0 and 1 differ");
    check_eq(differing_pixels(frames[1].rgba, frames[2].rgba) > 0 ? 1 : 0, 1,
             tag + "cursor 1 and 11 differ");

    const Frame high = make(99);
    check(high.ok, tag + "cursor 99 renders: " + high.error);
    if (high.ok) {
        check_eq(differing_pixels(high.rgba, none.rgba), 0,
                 tag + "cursor 99 highlights nothing (0x48d502 clamps to 0..11)");
    }
}

// --- plate -------------------------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "plate: ";
    const Asset plate = load_asset(game, "PLATE");
    check(plate.ok, tag + "PLATE loaded");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kMouseSetupPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kMouseSetupPanelHeight, tag + "PLATE 131 high");

    gm::MouseSetupRenderOptions black;
    gm::MouseSetupRenderOptions plated;
    plated.background = std::string(gm::kMouseSetupPanelName);
    const Frame black_frame = run(game, black);
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
            const std::uint32_t want = plate.colour(x, y);
            const std::uint32_t got = at(plate_frame, x, y);
            // Glyph/sprite pixels sit on top of the panel: compare only the
            // pixels the black frame left black.
            if (!is_black(at(black_frame, x, y))) {
                continue;
            }
            wrong += got == want ? 0 : 1;
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
    std::printf("  mouse_setup plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "text ink %d unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
    (void)fonts;
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kMouseSetupCanvasWidth) *
                                  gm::kMouseSetupCanvasHeight;

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
        gm::MouseSetupRenderOptions options;
        options.background = std::string(gm::kMouseSetupPanelName);
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
                if (x < gm::kMouseSetupPanelWidth && y < gm::kMouseSetupPanelHeight) {
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

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::MouseSetupRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::MouseSetupRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    // FONTS4 bytes, to build small archives that carry the atlas but no sprite.
    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> slide = installed_entry(game, "SLIDE");
    check(fonts4.has_value(), "negatives: FONTS4 bytes available");
    check(slide.has_value(), "negatives: SLIDE bytes available");
    if (!fonts4 || !slide) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. An index with FONTS4 only: the title face loads, SLIDE is missing.
    check(write_archive(root / "game-without-slide", {"FONTS4"}, {*fonts4}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-slide", gm::MouseSetupRenderOptions{},
                  "archive without SLIDE", "SLIDE");

    // 2. FONTS4 + SLIDE: the track loads, KNOB is missing.
    check(write_archive(root / "game-without-knob", {"FONTS4", "SLIDE"}, {*fonts4, *slide}),
          "negatives: FONTS4+SLIDE archive written");
    check_refused(root / "game-without-knob", gm::MouseSetupRenderOptions{},
                  "archive without KNOB", "KNOB");

    // 3. An index without FONTS4: the title face is loaded first.
    const std::optional<std::vector<std::uint8_t>> fonts1 = installed_entry(game, "FONTS1");
    if (fonts1) {
        check(write_archive(root / "game-without-font4", {"FONTS1"}, {*fonts1}),
              "negatives: FONTS1-only archive written");
        check_refused(root / "game-without-font4", gm::MouseSetupRenderOptions{},
                      "archive without FONTS4", "FONTS4");
    }
    fs::remove_all(root, ec);

    // 4. A background name that is not in the archive is a text, not black.
    gm::MouseSetupRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 5. The exact fade needs a table or a dump.
    gm::MouseSetupRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::MouseSetupRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 6. The shipped install renders the API default and is deterministic.
    const Frame first = run(game, gm::MouseSetupRenderOptions{});
    const Frame second = run(game, gm::MouseSetupRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  mouse_setup negatives: rows=%d sliders=%d ink=%d arrows=%d "
                    "missing=%d\n",
                    first.stats.rows, first.stats.sliders, first.stats.ink_pixels,
                    first.stats.arrows, first.stats.missing_glyphs);
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
        std::cout << "mouse_setup_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kMouseSetupTitleFont, gm::kMouseSetupRowFont,
                             gm::kMouseSetupSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "mouse_setup_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_frame(game, fonts);
    test_sliders(game, fonts);
    test_cursor(game, fonts);
    test_plate(game, fonts);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "mouse_setup_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mouse_setup_render: ok (" << checks
              << " checks) screen 22 -> 320x200 RGBA (title y 11, rows x 18/9 y 24 step "
                 "8, 3 sliders (105,40/56/72) with the knob at 108+27*(v-0.2), cursor "
                 "font 4, no arrows, PLATE, exact fade, negatives)\n";
    return 0;
}
