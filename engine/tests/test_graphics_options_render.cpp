// One frame of screen 12, `GRAPHICS OPTIONS` (`M8.8` slice 19,
// `ui/graphics_options_render`, docs/GRAPHICS_OPTIONS_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.3
// from the handler `0x485ee0` and from pixels:
//
//   * model: `rows` 6, step 15, title `GRAPHICS OPTIONS` (font 5) centred at
//     y 11, the `0x486500` mode line at (38,24), `Border:` at (38,39), the
//     `<name> Gamma Correction` line at (38,54), `Reflections` at (50,69), the
//     light-map label at (38,84) and `Done` at (38,98); border 0..9, gamma
//     0..3, light maps 0..2, reflections a boolean, the `.data` defaults
//     3/0/1/1 versus the shipped CFG 3/1/1/2, and the knob
//     `89 + 6*border` (0/3/9 -> 89/107/143);
//   * the shipped install: 6 rows, 1 slider, 1 check, 0 arrows, no missing
//     glyph, 320x200 RGBA, every pixel opaque; the title and every label band
//     are compared byte for byte with an independent blit and the bounding
//     boxes are printed; ink = the union of all text and widget pixels;
//   * the border slider: 0/3/9 move the knob 89/107/143 inside the 88..159
//     track, each border changes only the 72x8 slider rectangle, and the whole
//     rectangle matches an independent `SLIDE`/`KNOB` composite;
//   * the checkbox: off paints `NOCHECK` 8 rows lower (40,69), on paints the
//     8x16 `CHECK` at (40,61); the toggle changes only that 8x16 rectangle and
//     both states match the independent composite;
//   * gamma 0..3 and light maps 0..2 change only their own label rows and the
//     four/five literal strings;
//   * the cursor item 0..5 turns only its own band white (font 4), `-1`/99
//     highlight nothing, the title stays dark red;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `FONTS4`, one with `FONTS4` but no `PLATE`, then no
//     `SLIDE`, no `KNOB`, no `CHECK`, no `NOCHECK`, a `PLATE` and a `SLIDE` of
//     the wrong size, an unknown background entry, `fade_lut` without a table
//     and a dump, and a missing dump file — all `graphics_options_render:`
//     texts with a cleared buffer; a complete synthetic archive renders; two
//     renders are byte-identical.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/GRAPHICS_OPTIONS_RENDER.md §3 pins them.

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
#include "ui/graphics_options_render.h"

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
    gm::GraphicsOptionsRenderStats stats;
};

Frame run(const fs::path& game, const gm::GraphicsOptionsRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_graphics_options(game, options, frame.rgba, frame.width,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kGraphicsOptionsBackground; }

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
std::vector<std::uint32_t> compose_sprite(const Asset& sprite, int x, int y, int width,
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

/// The 8x16 checkbox rectangle at (40, 61): `CHECK` at the anchor when checked,
/// `NOCHECK` 8 rows lower otherwise.  0 = untouched.
std::vector<std::uint32_t> compose_checkbox(const Asset& check, const Asset& nocheck,
                                            bool checked) {
    const int width = gm::kGraphicsOptionsCheckWidth;
    const int height = gm::kGraphicsOptionsCheckHeight;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height),
                                   0u);
    const Asset& sprite = checked ? check : nocheck;
    const int dy = checked ? 0 : gm::kGraphicsOptionsNoCheckDrop;
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
    return out;
}

/// The 72x8 slider rectangle at (88, 39): `SLIDE` first, `KNOB` on top (the
/// 8x8 knob is opaque, its base column is 89).
std::vector<std::uint32_t> compose_slider(const Asset& slide, const Asset& knob,
                                          int knob_x) {
    const int width = gm::kGraphicsOptionsSlideWidth;
    const int height = gm::kGraphicsOptionsSlideHeight;
    std::vector<std::uint32_t> out = compose_sprite(slide, 0, 0, width, height);
    const std::vector<std::uint32_t> top =
        compose_sprite(knob, knob_x - gm::kGraphicsOptionsSlideX, 0, width, height);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (top[i] != 0) {
            out[i] = top[i];
        }
    }
    return out;
}

long long rect_mismatch(const Frame& frame, const std::vector<std::uint32_t>& want, int x,
                        int y, int width, int height) {
    long long wrong = 0;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t expected =
                want[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, x + column, y + row);
            if (expected == 0) {
                // Only untouched pixels must stay black; ink from a glyph may
                // legitimately sit on the same canvas row outside the widget.
                if (!is_black(got)) {
                    ++wrong;
                }
                continue;
            }
            if (got != expected) {
                ++wrong;
            }
        }
    }
    return wrong;
}

/// Compare the rows `y..y+rows-1` of the 196-px band of *frame* with the
/// independent blit; every untouched pixel in `x_min..x_limit-1` must be the
/// black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what, int x_min = 0, int x_limit = 196,
                int rows = -1) {
    const int width = gm::kGraphicsOptionsPanelWidth;
    const int height =
        rows < 0 ? atlas.cell_height : std::min(rows, atlas.cell_height);
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    int wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = std::max(0, x_min); column < std::min(x_limit, width); ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kGraphicsOptionsBackground : want;
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

/// Independent union of every painted pixel of one frame, in the 196x131 band.
/// 0 = untouched; used to pin `stats.ink_pixels` exactly (glyphs and sprites
/// are marked in the handler's emission order, so an overlapping widget still
/// counts once, like the renderer's own `covered` array).
struct InkModel {
    std::vector<std::uint8_t> mask =
        std::vector<std::uint8_t>(static_cast<std::size_t>(gm::kGraphicsOptionsPanelWidth) *
                                      static_cast<std::size_t>(gm::kGraphicsOptionsPanelHeight),
                                  0);

    void mark(int x, int y) {
        if (x < 0 || x >= gm::kGraphicsOptionsPanelWidth || y < 0 ||
            y >= gm::kGraphicsOptionsPanelHeight) {
            return;
        }
        mask[static_cast<std::size_t>(y) * gm::kGraphicsOptionsPanelWidth +
             static_cast<std::size_t>(x)] = 1;
    }

    void text(const gm::FontAtlas& atlas, std::string_view value, int x, int y) {
        int pen = x;
        for (const char raw : value) {
            const auto ch = static_cast<unsigned char>(raw);
            const gm::FontGlyph* glyph = atlas.glyph(ch);
            if (glyph == nullptr) {
                continue; // metrics-only byte: no ink, like the renderer
            }
            if (glyph->width > 0) {
                for (int row = 0; row < atlas.cell_height; ++row) {
                    for (int column = 0; column < glyph->width; ++column) {
                        const std::uint8_t index =
                            glyph->pixels[static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(glyph->width) +
                                          static_cast<std::size_t>(column)];
                        if (index != 0) {
                            mark(pen + column, y + row);
                        }
                    }
                }
            }
            pen += atlas.advance_of(ch);
        }
    }

    void sprite(const Asset& asset, int x, int y) {
        for (int row = 0; row < asset.height; ++row) {
            for (int column = 0; column < asset.width; ++column) {
                const std::uint8_t index =
                    asset.indices[static_cast<std::size_t>(row) *
                                      static_cast<std::size_t>(asset.width) +
                                  static_cast<std::size_t>(column)];
                if (index != 0) {
                    mark(x + column, y + row);
                }
            }
        }
    }

    long long ink() const {
        long long count = 0;
        for (const std::uint8_t pixel : mask) {
            count += pixel == 0 ? 0 : 1;
        }
        return count;
    }
};

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_graphics_options_render";
    }
    return fs::temp_directory_path() / "gm_graphics_options_render";
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
void check_refused(const fs::path& game, const gm::GraphicsOptionsRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::GraphicsOptionsRenderStats stats;
    stats.rows = 7;
    stats.checks = 7;
    stats.sliders = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_graphics_options(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "graphics_options_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.checks, 0, what + ": checks cleared");
    check_eq(stats.sliders, 0, what + ": sliders cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // Geometry of 0x485ee0 (rows 6, step 15, title row 11, x 38/50).
    check_eq(gm::kGraphicsOptionsRows, 6, tag + "rows = 6 (0x67f10c at 0x485ee3)");
    check_eq(gm::kGraphicsOptionsItemStep, 15, tag + "step = 15 (0x662d58 at 0x485eed)");
    check_eq(gm::kGraphicsOptionsTitleY, 11, tag + "title row 11 (push 0xb at 0x485f00)");
    check_eq(gm::kGraphicsOptionsTitleX, 38, tag + "title x 38 fallback (push 0x26)");
    check_eq(gm::kGraphicsOptionsTitleFont, 5, tag + "title font 5");
    check_eq(gm::kGraphicsOptionsRowFont, 0, tag + "row font 0");
    check_eq(gm::kGraphicsOptionsSelectedFont, 4, tag + "highlight font 4");
    check_eq(std::string(gm::kGraphicsOptionsTitle), std::string("GRAPHICS OPTIONS"),
             tag + "title 0x4bcf24");
    check_eq(std::string(gm::kGraphicsOptionsPanelName), std::string("PLATE"), tag + "PLATE");
    check_eq(gm::kGraphicsOptionsPanelWidth, 196, tag + "PLATE 196 wide");
    check_eq(gm::kGraphicsOptionsPanelHeight, 131, tag + "PLATE 131 high");

    // The six label calls and the resulting x/y.
    const std::array<int, 6> items = {0, 1, 2, 3, 4, 5};
    const std::array<int, 6> want_x = {38, 38, 38, 50, 38, 38};
    const std::array<int, 6> want_y = {24, 39, 54, 69, 84, 98};
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int item = items[i];
        check_eq(gm::graphics_options_item_y(item), want_y[i],
                 tag + "y of item " + std::to_string(item));
        check_eq(gm::graphics_options_item_x(item), want_x[i],
                 tag + "x of item " + std::to_string(item));
    }
    check_eq(gm::kGraphicsOptionsLabelRow, 24, tag + "label row immediate 24 (0x485f3f)");
    check_eq(gm::kGraphicsOptionsDoneRow, 23, tag + "Done row immediate 23 (0x486031)");
    check_eq(gm::kGraphicsOptionsReflectionsLabelX, 50,
             tag + "Reflections x 50 (push 0x32 at 0x485fe0)");

    // Border: 0..9, .data 3, knob 89 + 6*step.
    check_eq(gm::graphics_options_border(-3), 0, tag + "border clamp low 0");
    check_eq(gm::graphics_options_border(9), 9, tag + "border max 9 (cmp 0x9 at 0x48c773)");
    check_eq(gm::graphics_options_border(99), 9, tag + "border clamp high 9");
    check_eq(gm::kGraphicsOptionsBorderStep, 1, tag + "border step 1");
    check_eq(gm::kGraphicsOptionsBorderDefault, 3, tag + "border .data default 3 (0x4bc1c8)");
    const std::array<int, 5> want_border = {0, 1, 3, 5, 9};
    const std::array<int, 5> want_knob = {89, 95, 107, 119, 143};
    for (std::size_t i = 0; i < want_knob.size(); ++i) {
        check_eq(gm::graphics_options_knob_x(want_border[i]), want_knob[i],
                 tag + "knob x of border " + std::to_string(want_border[i]));
    }
    for (int border = 0; border <= 9; ++border) {
        check_eq(gm::graphics_options_knob_x(border), 89 + 6 * border,
                 tag + "89+6*border model at " + std::to_string(border));
    }
    check_eq(gm::graphics_options_knob_x(-5), 89, tag + "knob clamp low -> 89");
    check_eq(gm::graphics_options_knob_x(99), 143, tag + "knob clamp high -> 143");
    check_eq(gm::kGraphicsOptionsKnobWidth, 8, tag + "KNOB 8 wide");
    check_eq(gm::kGraphicsOptionsKnobHeight, 8, tag + "KNOB 8 high");
    check_eq(gm::kGraphicsOptionsSlideWidth, 72, tag + "SLIDE 72 wide");
    check_eq(gm::kGraphicsOptionsSlideHeight, 8, tag + "SLIDE 8 high");
    check_eq(gm::kGraphicsOptionsSlideX, 88, tag + "SLIDE x 88 (0x68842c)");
    check_eq(gm::kGraphicsOptionsSlideY, 39, tag + "SLIDE y 39");
    check_eq(gm::kGraphicsOptionsKnobBaseX, 89, tag + "KNOB base x 89 (0x68842d)");

    // Gamma: 0..3, .bss 0, shipped CFG 1.
    check_eq(gm::graphics_options_gamma(-1), 0, tag + "gamma clamp low 0");
    check_eq(gm::graphics_options_gamma(3), 3, tag + "gamma max 3 (cmp 3 at 0x48c7ba)");
    check_eq(gm::graphics_options_gamma(99), 3, tag + "gamma clamp high 3");
    check_eq(gm::kGraphicsOptionsGammaDefault, 0, tag + "gamma .bss default 0 (0x5a4950)");
    check_eq(gm::kGraphicsOptionsGammaShippedCfg, 1, tag + "shipped CFG gamma 1");
    check_eq(std::string(gm::graphics_options_gamma_name(0)), std::string("No"),
             tag + "gamma 0 = No (0x4bc7a4)");
    check_eq(std::string(gm::graphics_options_gamma_name(1)), std::string("Little"),
             tag + "gamma 1 = Little (0x4bc79c)");
    check_eq(std::string(gm::graphics_options_gamma_name(2)), std::string("Some"),
             tag + "gamma 2 = Some (0x4bc794)");
    check_eq(std::string(gm::graphics_options_gamma_name(3)), std::string("Max"),
             tag + "gamma 3 = Max (0x4bc790)");
    check_eq(std::string(gm::graphics_options_gamma_name(9)), std::string("Max"),
             tag + "gamma name clamps high");

    // Reflections: a boolean, .data 1.
    check(!gm::graphics_options_reflections_on(0), tag + "reflections 0 -> NOCHECK");
    check(gm::graphics_options_reflections_on(1), tag + "reflections 1 -> CHECK");
    check(gm::graphics_options_reflections_on(-7), tag + "reflections non-zero -> CHECK");
    check_eq(gm::kGraphicsOptionsReflectionsDefault, 1,
             tag + "reflections .data default 1 (0x4aac84)");

    // Light maps: 0..2, .data 1, shipped CFG 2.
    check_eq(gm::graphics_options_light_maps(-1), 0, tag + "light maps clamp low 0");
    check_eq(gm::graphics_options_light_maps(2), 2,
             tag + "light maps max 2 (0x48d7ce)");
    check_eq(gm::graphics_options_light_maps(9), 2, tag + "light maps clamp high 2");
    check_eq(gm::kGraphicsOptionsLightMapsDefault, 1,
             tag + "light maps .data default 1 (0x4aac88)");
    check_eq(gm::kGraphicsOptionsLightMapsShippedCfg, 2, tag + "shipped CFG light maps 2");
    check_eq(std::string(gm::graphics_options_light_maps_label(0)),
             std::string("Light Maps Off"), tag + "light maps 0 = 0x4bcebc");
    check_eq(std::string(gm::graphics_options_light_maps_label(1)),
             std::string("Light Maps Low Quality"), tag + "light maps 1 = 0x4bcee4");
    check_eq(std::string(gm::graphics_options_light_maps_label(2)),
             std::string("Light Maps High Quality"), tag + "light maps 2 = 0x4bcecc");

    // Checkbox anchor and sprites.
    check_eq(gm::kGraphicsOptionsCheckX, 40, tag + "checkbox x 40 (0x6894d4)");
    check_eq(gm::kGraphicsOptionsCheckY, 61, tag + "checkbox anchor y 61");
    check_eq(gm::kGraphicsOptionsCheckWidth, 8, tag + "CHECK 8 wide");
    check_eq(gm::kGraphicsOptionsCheckHeight, 16, tag + "CHECK 16 high");
    check_eq(gm::kGraphicsOptionsNoCheckWidth, 8, tag + "NOCHECK 8 wide");
    check_eq(gm::kGraphicsOptionsNoCheckHeight, 8, tag + "NOCHECK 8 high");
    check_eq(gm::kGraphicsOptionsNoCheckDrop, 8, tag + "NOCHECK drawn 8 rows lower");

    // Cursor default and item texts.
    check_eq(gm::kGraphicsOptionsDefaultCursor, 0, tag + "cursor default 0 (.bss 0x68ccf4)");
    gm::GraphicsOptionsRenderOptions options;
    check_eq(options.cursor, 0, tag + "options cursor default 0");
    check_eq(options.border, 3, tag + "options border default 3");
    check_eq(options.gamma, 0, tag + "options gamma default 0");
    check_eq(options.reflections, 1, tag + "options reflections default 1");
    check_eq(options.light_maps, 1, tag + "options light maps default 1");
    check_eq(options.mode.width, 800, tag + "mode model width 800");
    check_eq(options.mode.height, 600, tag + "mode model height 600");
    check_eq(options.mode.bpp, 32, tag + "mode model bpp 32");
    check_eq(static_cast<long long>(options.mode.flags), 0x804,
             tag + "mode model flags 0x804 (Glide)");

    const std::string mode = gm::graphics_options_item_text(options, 0);
    check_eq(mode, gm::video_mode_list_label(options.mode),
             tag + "item 0 is the 0x486500 label of the descriptor");
    check_eq(mode, std::string("800x600 Glide 3Dfx"), tag + "default mode line text");
    check_eq(gm::graphics_options_item_text(options, 1), std::string("Border:"),
             tag + "item 1 = 0x4bcf1c");
    check_eq(gm::graphics_options_item_text(options, 2), std::string("No Gamma Correction"),
             tag + "item 2 = 0x4bcf08 with gamma 0");
    check_eq(gm::graphics_options_item_text(options, 3), std::string("Reflections"),
             tag + "item 3 = 0x4bcefc");
    check_eq(gm::graphics_options_item_text(options, 4),
             std::string("Light Maps Low Quality"), tag + "item 4 = 0x4bcee4 for level 1");
    check_eq(gm::graphics_options_item_text(options, 5), std::string("Done"),
             tag + "item 5 = 0x4bceb4");
    check_eq(gm::graphics_options_item_text(options, 99), std::string("Done"),
             tag + "out-of-range id -> Done");
    options.gamma = 2;
    check_eq(gm::graphics_options_item_text(options, 2),
             std::string("Some Gamma Correction"), tag + "item 2 with gamma 2");
    options.light_maps = 2;
    check_eq(gm::graphics_options_item_text(options, 4),
             std::string("Light Maps High Quality"), tag + "item 4 with level 2");
}

// --- frame -------------------------------------------------------------------
void test_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const gm::GraphicsOptionsRenderOptions options;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kGraphicsOptionsCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kGraphicsOptionsCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kGraphicsOptionsRows, tag + "stats rows 6");
    check_eq(frame.stats.sliders, 1, tag + "stats sliders 1 (border track)");
    check_eq(frame.stats.checks, 1, tag + "stats checks 1");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel is opaque");

    // Title: centred at y 11 (font 5).
    const gm::FontAtlas& title_face = fonts.at(gm::kGraphicsOptionsTitleFont);
    const int title_x =
        std::max(0, (gm::kGraphicsOptionsPanelWidth - pen_width(title_face,
                                                                gm::kGraphicsOptionsTitle)) /
                        2);
    check_band(frame, title_face, gm::kGraphicsOptionsTitle, title_x,
               gm::kGraphicsOptionsTitleY, tag + "title");
    const Box title_box = ink_box(frame, gm::kGraphicsOptionsTitleY,
                                  gm::kGraphicsOptionsTitleY + title_face.cell_height - 1);
    const std::vector<std::uint32_t> title_band =
        compose_band(title_face, gm::kGraphicsOptionsTitle, title_x,
                     gm::kGraphicsOptionsPanelWidth, title_face.cell_height);
    const Box title_want =
        band_box(title_band, gm::kGraphicsOptionsPanelWidth, title_face.cell_height);
    check_eq(title_box.x0, title_want.x0, tag + "title bbox x0");
    check_eq(title_box.x1, title_want.x1, tag + "title bbox x1");
    check_eq(title_box.y0, gm::kGraphicsOptionsTitleY + title_want.y0, tag + "title bbox y0");
    check_eq(title_box.y1, gm::kGraphicsOptionsTitleY + title_want.y1, tag + "title bbox y1");
    std::printf("  graphics_options title: \"%s\" x=%d y=%d ink=%lld bbox %s\n",
                std::string(gm::kGraphicsOptionsTitle).c_str(), title_x,
                gm::kGraphicsOptionsTitleY, band_ink(title_band),
                box_text(title_box).c_str());

    // Every label, its bbox and its band.
    const std::array<int, 6> items = {0, 1, 2, 3, 4, 5};
    long long text_ink = band_ink(title_band); // title + the six item bands
    for (const int item : items) {
        const int font = item == options.cursor ? gm::kGraphicsOptionsSelectedFont
                                                : gm::kGraphicsOptionsRowFont;
        const gm::FontAtlas& face = fonts.at(font);
        const std::string label = gm::graphics_options_item_text(options, item);
        const int x = gm::graphics_options_item_x(item);
        const int y = gm::graphics_options_item_y(item);
        // The border label shares its rows with the SLIDE track (x 88..159) and
        // the gamma cell's last row touches the checkbox row 61; keep the band
        // check to the pixels no later handler call can overwrite.
        int x_min = 0;
        int x_limit = gm::kGraphicsOptionsPanelWidth;
        int rows = face.cell_height;
        if (item == gm::kGraphicsOptionsBorderItem) {
            x_limit = gm::kGraphicsOptionsSlideX;
        }
        if (item == gm::kGraphicsOptionsReflectionsItem) {
            x_min = x;
        }
        if (item == gm::kGraphicsOptionsGammaItem) {
            rows = std::min(rows, gm::kGraphicsOptionsCheckY - y);
        }
        check_band(frame, face, label, x, y,
                   tag + "row " + std::to_string(item) + " \"" + label + "\"", x_min,
                   x_limit, rows);
        const std::vector<std::uint32_t> band =
            compose_band(face, label, x, gm::kGraphicsOptionsPanelWidth, rows);
        const Box want = band_box(band, gm::kGraphicsOptionsPanelWidth, rows);
        const Box got = ink_box(frame, y, y + rows - 1, x_limit, x_min);
        check_eq(got.x0, want.x0, tag + "row " + std::to_string(item) + " bbox x0");
        check_eq(got.x1, want.x1, tag + "row " + std::to_string(item) + " bbox x1");
        check_eq(got.y0, y + want.y0, tag + "row " + std::to_string(item) + " bbox y0");
        check_eq(got.y1, y + want.y1, tag + "row " + std::to_string(item) + " bbox y1");
        const long long ink = band_ink(band);
        text_ink += ink;
        std::printf("  graphics_options row %d: x=%2d y=%2d \"%s\" ink=%lld bbox %s\n",
                    item, x, y, label.c_str(), ink, box_text(got).c_str());
    }

    // Slider: the whole 72x8 rectangle against the independent composite.
    const Asset slide = load_asset(game, "SLIDE");
    const Asset knob = load_asset(game, "KNOB");
    check(slide.ok && knob.ok, tag + "SLIDE and KNOB decoded");
    long long slider_ink = 0;
    if (slide.ok && knob.ok) {
        check_eq(slide.width, gm::kGraphicsOptionsSlideWidth, tag + "SLIDE 72 wide");
        check_eq(slide.height, gm::kGraphicsOptionsSlideHeight, tag + "SLIDE 8 high");
        check_eq(knob.width, gm::kGraphicsOptionsKnobWidth, tag + "KNOB 8 wide");
        check_eq(knob.height, gm::kGraphicsOptionsKnobHeight, tag + "KNOB 8 high");
        const int knob_x = gm::graphics_options_knob_x(options.border);
        const std::vector<std::uint32_t> want = compose_slider(slide, knob, knob_x);
        check_eq(rect_mismatch(frame, want, gm::kGraphicsOptionsSlideX,
                               gm::kGraphicsOptionsSlideY, gm::kGraphicsOptionsSlideWidth,
                               gm::kGraphicsOptionsSlideHeight),
                 0, tag + "slider matches the SLIDE+KNOB composite");
        slider_ink = band_ink(want);
        std::printf("  graphics_options slider: border=%d knob x=%d y=%d, "
                    "SLIDE+KNOB ink=%lld\n",
                    options.border, knob_x, gm::kGraphicsOptionsSlideY, slider_ink);
    }

    // Checkbox rectangle: CHECK at the anchor for the default reflections 1.
    const Asset check_asset = load_asset(game, "CHECK");
    const Asset nocheck_asset = load_asset(game, "NOCHECK");
    check(check_asset.ok && nocheck_asset.ok, tag + "CHECK and NOCHECK decoded");
    long long check_ink = 0;
    if (check_asset.ok && nocheck_asset.ok) {
        check_eq(check_asset.width, gm::kGraphicsOptionsCheckWidth, tag + "CHECK 8 wide");
        check_eq(check_asset.height, gm::kGraphicsOptionsCheckHeight,
                 tag + "CHECK 16 high");
        check_eq(nocheck_asset.width, gm::kGraphicsOptionsNoCheckWidth,
                 tag + "NOCHECK 8 wide");
        check_eq(nocheck_asset.height, gm::kGraphicsOptionsNoCheckHeight,
                 tag + "NOCHECK 8 high");
        const std::vector<std::uint32_t> want =
            compose_checkbox(check_asset, nocheck_asset, true);
        check_eq(rect_mismatch(frame, want, gm::kGraphicsOptionsCheckX,
                               gm::kGraphicsOptionsCheckY, gm::kGraphicsOptionsCheckWidth,
                               gm::kGraphicsOptionsCheckHeight),
                 0, tag + "default reflections 1 matches the CHECK composite");
        check_ink = band_ink(want);
    }

    // The exact union: glyphs and sprites marked independently.
    InkModel model;
    model.text(title_face, gm::kGraphicsOptionsTitle, title_x, gm::kGraphicsOptionsTitleY);
    for (const int item : items) {
        const int font = item == options.cursor ? gm::kGraphicsOptionsSelectedFont
                                                : gm::kGraphicsOptionsRowFont;
        model.text(fonts.at(font), gm::graphics_options_item_text(options, item),
                   gm::graphics_options_item_x(item), gm::graphics_options_item_y(item));
    }
    if (slide.ok && knob.ok) {
        model.sprite(slide, gm::kGraphicsOptionsSlideX, gm::kGraphicsOptionsSlideY);
        model.sprite(knob, gm::graphics_options_knob_x(options.border),
                     gm::kGraphicsOptionsSlideY);
    }
    if (check_asset.ok && nocheck_asset.ok) {
        model.sprite(check_asset, gm::kGraphicsOptionsCheckX, gm::kGraphicsOptionsCheckY);
    }
    check_eq(frame.stats.ink_pixels, model.ink(), tag + "ink = union of text + widgets");
    std::printf("  graphics_options frame: rows=%d sliders=%d checks=%d ink=%d "
                "(text %lld + slider %lld + check %lld, union %lld) missing=%d arrows=%d\n",
                frame.stats.rows, frame.stats.sliders, frame.stats.checks,
                frame.stats.ink_pixels, text_ink, slider_ink, check_ink, model.ink(),
                frame.stats.missing_glyphs, frame.stats.arrows);
}

// --- border slider -----------------------------------------------------------
void test_slider(const fs::path& game) {
    const std::string tag = "slider: ";
    const Asset slide = load_asset(game, "SLIDE");
    const Asset knob = load_asset(game, "KNOB");
    check(slide.ok && knob.ok, tag + "SLIDE and KNOB loaded");
    if (!slide.ok || !knob.ok) {
        return;
    }
    const auto make = [&](int border) {
        gm::GraphicsOptionsRenderOptions options;
        options.border = border;
        return run(game, options);
    };
    const Frame zero = make(0);
    const Frame three = make(3);
    const Frame nine = make(9);
    check(zero.ok && three.ok && nine.ok, tag + "border frames render");
    if (!zero.ok || !three.ok || !nine.ok) {
        return;
    }
    check_eq(gm::graphics_options_knob_x(0), 89, tag + "border 0 knob 89");
    check_eq(gm::graphics_options_knob_x(3), 107, tag + "border 3 knob 107");
    check_eq(gm::graphics_options_knob_x(9), 143, tag + "border 9 knob 143");

    const int y = gm::kGraphicsOptionsSlideY;
    const long long diff_0_9 = differing_pixels(zero.rgba, nine.rgba);
    check(diff_0_9 > 0, tag + "border 0 vs 9 differ");
    check_eq(differing_outside_rect(nine, zero, gm::kGraphicsOptionsSlideX, y,
                                    gm::kGraphicsOptionsSlideX +
                                        gm::kGraphicsOptionsSlideWidth - 1,
                                    y + gm::kGraphicsOptionsSlideHeight - 1),
             0, tag + "border changes only the 72x8 slider rectangle");
    check_eq(differing_pixels(zero.rgba, make(3).rgba) > 0, 1, tag + "border 0 vs 3 differ");
    for (const Frame* frame : {&zero, &three, &nine}) {
        const int border = frame == &zero ? 0 : (frame == &three ? 3 : 9);
        check_eq(rect_mismatch(*frame, compose_slider(slide, knob,
                                                      gm::graphics_options_knob_x(border)),
                               gm::kGraphicsOptionsSlideX, y,
                               gm::kGraphicsOptionsSlideWidth,
                               gm::kGraphicsOptionsSlideHeight),
                 0, tag + "border " + std::to_string(border) +
                        " matches the SLIDE+KNOB composite");
    }
    std::printf("  graphics_options slider: knob x 89/107/143, diff 0->9=%lld px, "
                "diff 0->3=%lld px\n",
                diff_0_9, differing_pixels(zero.rgba, three.rgba));
    // Clamping: a border outside 0..9 paints the same as its bound.
    gm::GraphicsOptionsRenderOptions low;
    low.border = -5;
    gm::GraphicsOptionsRenderOptions high;
    high.border = 42;
    const Frame low_frame = run(game, low);
    const Frame high_frame = run(game, high);
    check(low_frame.ok && high_frame.ok, tag + "out-of-range border frames render");
    if (low_frame.ok && high_frame.ok) {
        check_eq(differing_pixels(low_frame.rgba, zero.rgba), 0, tag + "border -5 == 0");
        check_eq(differing_pixels(high_frame.rgba, nine.rgba), 0, tag + "border 42 == 9");
    }
}

// --- checkbox ----------------------------------------------------------------
void test_checkbox(const fs::path& game) {
    const std::string tag = "check: ";
    const Asset check_asset = load_asset(game, "CHECK");
    const Asset nocheck_asset = load_asset(game, "NOCHECK");
    check(check_asset.ok && nocheck_asset.ok, tag + "CHECK and NOCHECK loaded");
    if (!check_asset.ok || !nocheck_asset.ok) {
        return;
    }
    const Frame on = run(game, gm::GraphicsOptionsRenderOptions{});
    gm::GraphicsOptionsRenderOptions off_options;
    off_options.reflections = 0;
    const Frame off = run(game, off_options);
    check(on.ok && off.ok, tag + "on/off frames render");
    if (!on.ok || !off.ok) {
        return;
    }
    const long long diff = differing_pixels(off.rgba, on.rgba);
    check(diff > 0, tag + "reflections 0 vs 1 differs");
    check_eq(differing_outside_rect(off, on, gm::kGraphicsOptionsCheckX,
                                    gm::kGraphicsOptionsCheckY,
                                    gm::kGraphicsOptionsCheckX +
                                        gm::kGraphicsOptionsCheckWidth - 1,
                                    gm::kGraphicsOptionsCheckY +
                                        gm::kGraphicsOptionsCheckHeight - 1),
             0, tag + "toggle changes only the 8x16 checkbox rectangle");
    check_eq(rect_mismatch(on, compose_checkbox(check_asset, nocheck_asset, true),
                           gm::kGraphicsOptionsCheckX, gm::kGraphicsOptionsCheckY,
                           gm::kGraphicsOptionsCheckWidth, gm::kGraphicsOptionsCheckHeight),
             0, tag + "reflections 1 matches the CHECK composite at (40,61)");
    check_eq(rect_mismatch(off, compose_checkbox(check_asset, nocheck_asset, false),
                           gm::kGraphicsOptionsCheckX, gm::kGraphicsOptionsCheckY,
                           gm::kGraphicsOptionsCheckWidth, gm::kGraphicsOptionsCheckHeight),
             0, tag + "reflections 0 matches the NOCHECK composite at (40,69)");
    check_eq(on.stats.checks, 1, tag + "stats checks 1 when on");
    check_eq(off.stats.checks, 1, tag + "stats checks 1 when off");
    std::printf("  graphics_options check: off NOCHECK at (40,69) ink=%lld, on CHECK at "
                "(40,61) ink=%lld, diff=%lld px\n",
                band_ink(compose_checkbox(check_asset, nocheck_asset, false)),
                band_ink(compose_checkbox(check_asset, nocheck_asset, true)), diff);
    // A non-zero value other than 1 is checked too.
    gm::GraphicsOptionsRenderOptions seven;
    seven.reflections = 7;
    const Frame frame7 = run(game, seven);
    check(frame7.ok && differing_pixels(frame7.rgba, on.rgba) == 0,
          tag + "reflections 7 paints the same CHECK as 1");
}

// --- gamma and light maps ----------------------------------------------------
void test_values(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "values: ";
    const gm::FontAtlas& face = fonts.at(gm::kGraphicsOptionsRowFont);

    // gamma 0..3: only the item-2 rows change, each string is the handler's.
    const int gy = gm::graphics_options_item_y(gm::kGraphicsOptionsGammaItem);
    long long previous = -1;
    std::vector<Frame> gamma_frames;
    for (int gamma = 0; gamma <= 3; ++gamma) {
        gm::GraphicsOptionsRenderOptions options;
        options.gamma = gamma;
        Frame frame = run(game, options);
        check(frame.ok, tag + "gamma " + std::to_string(gamma) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_band(frame, face, gm::graphics_options_item_text(options, 2),
                   gm::kGraphicsOptionsItemX, gy,
                   tag + "gamma " + std::to_string(gamma) + " label", 0,
                   gm::kGraphicsOptionsPanelWidth,
                   std::min(face.cell_height, gm::kGraphicsOptionsCheckY - gy));
        const long long diff =
            gamma_frames.empty() ? 0 : differing_pixels(frame.rgba, gamma_frames[0].rgba);
        if (!gamma_frames.empty()) {
            check(diff > 0, tag + "gamma " + std::to_string(gamma) + " differs from 0");
            check_eq(differing_outside_rows(frame, gamma_frames[0], gy,
                                            gy + face.cell_height - 1),
                     0, tag + "gamma " + std::to_string(gamma) +
                            " changes only the item-2 rows");
        }
        std::printf("  graphics_options gamma %d: \"%s\" diff vs 0=%lld px\n", gamma,
                    gm::graphics_options_item_text(options, 2).c_str(), diff);
        previous = diff;
        gamma_frames.push_back(std::move(frame));
    }
    check(previous > 0, tag + "gamma frames differ");

    // light maps 0..2: only the item-4 rows change.
    const int ly = gm::graphics_options_item_y(gm::kGraphicsOptionsLightMapsItem);
    std::vector<Frame> map_frames;
    for (int level = 0; level <= 2; ++level) {
        gm::GraphicsOptionsRenderOptions options;
        options.light_maps = level;
        Frame frame = run(game, options);
        check(frame.ok, tag + "light maps " + std::to_string(level) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_band(frame, face, gm::graphics_options_item_text(options, 4),
                   gm::kGraphicsOptionsItemX, ly,
                   tag + "light maps " + std::to_string(level) + " label");
        if (!map_frames.empty()) {
            check(differing_pixels(frame.rgba, map_frames[0].rgba) > 0,
                  tag + "light maps " + std::to_string(level) + " differs from 0");
            check_eq(differing_outside_rows(frame, map_frames[0], ly,
                                            ly + face.cell_height - 1),
                     0, tag + "light maps " + std::to_string(level) +
                            " changes only the item-4 rows");
        }
        std::printf("  graphics_options light maps %d: \"%s\"\n", level,
                    gm::graphics_options_item_text(options, 4).c_str());
        map_frames.push_back(std::move(frame));
    }
    check(map_frames.size() == 3, tag + "three light-map frames rendered");

    // The mode line is item 0 and changing it touches only its rows.
    gm::GraphicsOptionsRenderOptions other_mode;
    other_mode.mode = {640, 480, 32, 0x804};
    const Frame mode_frame = run(game, other_mode);
    const Frame default_frame = run(game, gm::GraphicsOptionsRenderOptions{});
    check(mode_frame.ok && default_frame.ok, tag + "mode-line frames render");
    if (mode_frame.ok && default_frame.ok) {
        const int my = gm::graphics_options_item_y(gm::kGraphicsOptionsModeItem);
        check_eq(gm::graphics_options_item_text(other_mode, 0),
                 std::string("640x480 Glide 3Dfx"), tag + "640x480 label");
        check_eq(mode_frame.stats.rows, gm::kGraphicsOptionsRows, tag + "mode rows stay 6");
        check(differing_pixels(mode_frame.rgba, default_frame.rgba) > 0,
              tag + "mode line change differs");
        check_eq(differing_outside_rows(mode_frame, default_frame, my,
                                        my + face.cell_height - 1),
                 0, tag + "mode line change touches only the item-0 rows");
        std::printf("  graphics_options mode: \"%s\" vs \"%s\", diff=%lld px\n",
                    gm::graphics_options_item_text(other_mode, 0).c_str(),
                    gm::graphics_options_item_text(gm::GraphicsOptionsRenderOptions{}, 0)
                        .c_str(),
                    differing_pixels(mode_frame.rgba, default_frame.rgba));
    }
}

// --- cursor ------------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    constexpr std::uint32_t kWhite = 0xEFEFEFFF; // FONTS4 face 4 (239,239,239)

    const auto make = [&](int cursor) {
        gm::GraphicsOptionsRenderOptions options;
        options.cursor = cursor;
        return run(game, options);
    };
    const Frame none = make(-1);
    check(none.ok, tag + "no highlight frame: " + none.error);
    if (!none.ok) {
        return;
    }
    std::array<Frame, 6> frames;
    for (int item = 0; item < gm::kGraphicsOptionsRows; ++item) {
        Frame frame = make(item);
        check(frame.ok, tag + "cursor " + std::to_string(item) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        const int y0 = gm::graphics_options_item_y(item);
        const int y1 = y0 + fonts.at(gm::kGraphicsOptionsSelectedFont).cell_height - 1;
        check(has_colour(frame, y0, y1, kWhite),
              tag + "cursor " + std::to_string(item) + " paints font 4 in its band");
        check_eq(differing_outside_rows(frame, none, y0, y1), 0,
                 tag + "cursor " + std::to_string(item) + " changes only its rows");
        const long long diff = differing_pixels(frame.rgba, none.rgba);
        check(diff > 0, tag + "cursor " + std::to_string(item) + " differs");
        std::printf("  graphics_options cursor %d: rows y=%d..%d, diff vs no highlight="
                    "%lld px\n",
                    item, y0, y1, diff);
        frames[static_cast<std::size_t>(item)] = std::move(frame);
    }
    for (int item = 1; item < gm::kGraphicsOptionsRows; ++item) {
        check(differing_pixels(frames[static_cast<std::size_t>(item)].rgba,
                               frames[static_cast<std::size_t>(item - 1)].rgba) > 0,
              tag + "cursor " + std::to_string(item) + " differs from " +
                  std::to_string(item - 1));
    }
    const Frame high = make(99);
    check(high.ok, tag + "cursor 99 renders: " + high.error);
    if (high.ok) {
        check_eq(differing_pixels(high.rgba, none.rgba), 0,
                 tag + "cursor 99 highlights nothing");
    }
    const Frame entry = make(gm::kGraphicsOptionsDefaultCursor);
    check(entry.ok && differing_pixels(entry.rgba, frames[0].rgba) == 0,
          tag + "default cursor 0 is the mode-line highlight");
    // The title (font 5) is never highlighted: cursor 0 leaves it dark red.
    constexpr std::uint32_t kRed = 0x9B0F0FFF; // FONTS4 face 5
    check(has_colour(frames[0], gm::kGraphicsOptionsTitleY,
                     gm::kGraphicsOptionsTitleY + fonts.at(5).cell_height - 1, kRed),
          tag + "title stays font 5 under a highlight");
}

// --- plate -------------------------------------------------------------------
void test_plate(const fs::path& game) {
    const std::string tag = "plate: ";
    const Asset plate = load_asset(game, "PLATE");
    check(plate.ok, tag + "PLATE loaded");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kGraphicsOptionsPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kGraphicsOptionsPanelHeight, tag + "PLATE 131 high");

    const Frame black_frame = run(game, gm::GraphicsOptionsRenderOptions{});
    gm::GraphicsOptionsRenderOptions plated;
    plated.background = std::string(gm::kGraphicsOptionsPanelName);
    const Frame plate_frame = run(game, plated);
    check(black_frame.ok && plate_frame.ok, tag + "black and PLATE frames render");
    if (!black_frame.ok || !plate_frame.ok) {
        return;
    }
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
    std::printf("  graphics_options plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "ink %d unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kGraphicsOptionsCanvasWidth) *
                                  gm::kGraphicsOptionsCanvasHeight;

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
        gm::GraphicsOptionsRenderOptions options;
        options.background = std::string(gm::kGraphicsOptionsPanelName);
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
                if (x < gm::kGraphicsOptionsPanelWidth && y < gm::kGraphicsOptionsPanelHeight) {
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
    check_refused(fs::path{}, gm::GraphicsOptionsRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::GraphicsOptionsRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> slide = installed_entry(game, "SLIDE");
    const std::optional<std::vector<std::uint8_t>> knob = installed_entry(game, "KNOB");
    const std::optional<std::vector<std::uint8_t>> check_bytes = installed_entry(game, "CHECK");
    const std::optional<std::vector<std::uint8_t>> nocheck_bytes =
        installed_entry(game, "NOCHECK");
    check(fonts4 && plate && slide && knob && check_bytes && nocheck_bytes,
          "negatives: FONTS4/PLATE/SLIDE/KNOB/CHECK/NOCHECK bytes available");
    if (!fonts4 || !plate || !slide || !knob || !check_bytes || !nocheck_bytes) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. FONTS4 only: PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4"}, {*fonts4}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-plate", gm::GraphicsOptionsRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. FONTS4 + PLATE: SLIDE is missing.
    check(write_archive(root / "game-without-slide", {"FONTS4", "PLATE"},
                        {*fonts4, *plate}),
          "negatives: FONTS4+PLATE archive written");
    check_refused(root / "game-without-slide", gm::GraphicsOptionsRenderOptions{},
                  "archive without SLIDE", "SLIDE");

    // 3. + SLIDE: KNOB is missing.
    check(write_archive(root / "game-without-knob", {"FONTS4", "PLATE", "SLIDE"},
                        {*fonts4, *plate, *slide}),
          "negatives: FONTS4+PLATE+SLIDE archive written");
    check_refused(root / "game-without-knob", gm::GraphicsOptionsRenderOptions{},
                  "archive without KNOB", "KNOB");

    // 4. + KNOB: CHECK is missing.
    check(write_archive(root / "game-without-check", {"FONTS4", "PLATE", "SLIDE", "KNOB"},
                        {*fonts4, *plate, *slide, *knob}),
          "negatives: FONTS4+PLATE+SLIDE+KNOB archive written");
    check_refused(root / "game-without-check", gm::GraphicsOptionsRenderOptions{},
                  "archive without CHECK", "CHECK");

    // 5. + CHECK: NOCHECK is missing.
    check(write_archive(root / "game-without-nocheck",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK"},
                        {*fonts4, *plate, *slide, *knob, *check_bytes}),
          "negatives: FONTS4+PLATE+SLIDE+KNOB+CHECK archive written");
    check_refused(root / "game-without-nocheck", gm::GraphicsOptionsRenderOptions{},
                  "archive without NOCHECK", "NOCHECK");

    // 6. Without FONTS4 the title face is missing.
    check(write_archive(root / "game-without-font4",
                        {"PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*plate, *slide, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::GraphicsOptionsRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 7. A PLATE of the wrong size (the 8x16 CHECK renamed).
    check(write_archive(root / "game-wrong-plate",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*fonts4, *check_bytes, *slide, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::GraphicsOptionsRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 8. A SLIDE of the wrong size (the 8x8 KNOB renamed).
    check(write_archive(root / "game-wrong-slide",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*fonts4, *plate, *knob, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: wrong-size SLIDE archive written");
    check_refused(root / "game-wrong-slide", gm::GraphicsOptionsRenderOptions{},
                  "8x8 SLIDE", "expected 72x8");

    // 9. A complete synthetic archive renders.
    check(write_archive(root / "game-complete",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*fonts4, *plate, *slide, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: complete archive written");
    {
        const Frame minimal = run(root / "game-complete", gm::GraphicsOptionsRenderOptions{});
        check(minimal.ok, "negatives: complete synthetic archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, gm::kGraphicsOptionsRows,
                     "negatives: synthetic rows 6");
            check_eq(minimal.stats.sliders, 1, "negatives: synthetic sliders 1");
            check_eq(minimal.stats.checks, 1, "negatives: synthetic checks 1");
            check(minimal.stats.ink_pixels > 0, "negatives: synthetic ink > 0");
            std::printf("  graphics_options synthetic archive: ink=%d rows=%d sliders=%d "
                        "checks=%d missing=%d\n",
                        minimal.stats.ink_pixels, minimal.stats.rows,
                        minimal.stats.sliders, minimal.stats.checks,
                        minimal.stats.missing_glyphs);
            // Determinism on the synthetic archive too.
            const Frame again = run(root / "game-complete", gm::GraphicsOptionsRenderOptions{});
            check(again.ok && differing_pixels(minimal.rgba, again.rgba) == 0,
                  "negatives: synthetic renders are byte-identical");
        }
    }
    fs::remove_all(root, ec);

    // 10. An unknown background entry is a text, not black.
    gm::GraphicsOptionsRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 11. The exact fade needs a table or a dump.
    gm::GraphicsOptionsRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::GraphicsOptionsRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 12. Shipped install: default and deterministic.
    const Frame first = run(game, gm::GraphicsOptionsRenderOptions{});
    const Frame second = run(game, gm::GraphicsOptionsRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  graphics_options negatives: rows=%d sliders=%d checks=%d ink=%d "
                    "arrows=%d missing=%d\n",
                    first.stats.rows, first.stats.sliders, first.stats.checks,
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
        std::cout << "graphics_options_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kGraphicsOptionsTitleFont, gm::kGraphicsOptionsRowFont,
                             gm::kGraphicsOptionsSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "graphics_options_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_frame(game, fonts);
    test_slider(game);
    test_checkbox(game);
    test_values(game, fonts);
    test_cursor(game, fonts);
    test_plate(game);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "graphics_options_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "graphics_options_render: ok (" << checks << " checks)\n";
    return 0;
}
