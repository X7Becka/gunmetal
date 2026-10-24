// One frame of screen 11, `SOUND OPTIONS` (`M8.8` slice 18,
// `ui/sound_options_render`, docs/SOUND_OPTIONS_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.2
// from the handler `0x4889f0` and from pixels:
//
//   * model: `rows` 7, step 10, title `SOUND OPTIONS` (font 5) centred at y 14,
//     music `Music Volume:` at (18,27), effects at (18,47), the checkbox label
//     `Switch Left Right` at (30,67), `Done` at (18,84), the sound-off layout
//     `rows` 1 with four lines at (18, 27/37/47/57); volumes 0..20, knob
//     `69 + 27*volume/10` (0/5/10/15/20 -> 69/82/96/109/123), the `.data`
//     defaults 10/20 versus the shipped CFG 16/20, and the disabled strings;
//   * the shipped install: 7 rows, 2 sliders, 1 check, 0 arrows, no missing
//     glyph, 320x200 RGBA, every pixel opaque; the title and every label band
//     are compared byte for byte with an independent blit and the bounding
//     boxes are printed; ink = text bands + `SLIDE`+`KNOB` unions + checkbox;
//   * the sliders: music 0/10/20 moves the knob 69/96/123 inside the 68..139
//     track, each volume changes only the 72x8 slider rectangle, and the whole
//     rectangle matches an independent `SLIDE`/`KNOB` composite; the effects
//     slider is pinned the same way;
//   * the checkbox: off paints `NOCHECK` 8 rows lower (20,67), on paints the
//     8x16 `CHECK` at (20,59); the toggle changes only that 8x16 rectangle and
//     both states match the independent composite;
//   * `music_low_memory`: one slider instead of two, the
//     `Music Disabled: Low Memory` label, and only the music slider row changes;
//   * `sound_enabled = false`: `rows` 1, 0 sliders, 0 checks, the four literal
//     lines and the same title;
//   * the cursor item 0/2/4/6 turns only its own band white (font 4), `-1`/99
//     highlight nothing, out-of-range ids leave the frame identical;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `FONTS4`, one with `FONTS4` but no `PLATE`, then no
//     `SLIDE`, no `KNOB`, no `CHECK`, no `NOCHECK`, a `PLATE` and a `SLIDE` of
//     the wrong size, an unknown background entry, `fade_lut` without a table
//     and a dump, and a missing dump file — all `sound_options_render:` texts
//     with a cleared buffer; a complete synthetic archive renders; two renders
//     are byte-identical.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/SOUND_OPTIONS_RENDER.md §3 pins them.

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
#include "ui/sound_options_render.h"

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
    gm::SoundOptionsRenderStats stats;
};

Frame run(const fs::path& game, const gm::SoundOptionsRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_sound_options(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kSoundOptionsBackground; }

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

/// The 8x16 checkbox rectangle at (20, 59): `CHECK` at the anchor when checked,
/// `NOCHECK` 8 rows lower otherwise.  0 = untouched.
std::vector<std::uint32_t> compose_checkbox(const Asset& check, const Asset& nocheck,
                                            bool checked) {
    const int width = gm::kSoundOptionsCheckWidth;
    const int height = gm::kSoundOptionsCheckHeight;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height),
                                   0u);
    const Asset& sprite = checked ? check : nocheck;
    const int dy = checked ? 0 : gm::kSoundOptionsNoCheckDrop;
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

/// The 72x8 slider rectangle at (68, y): `SLIDE` first, `KNOB` on top (the
/// 8x8 knob is opaque, its base column is 69).
std::vector<std::uint32_t> compose_slider(const Asset& slide, const Asset& knob,
                                          int knob_x) {
    const int width = gm::kSoundOptionsSlideWidth;
    const int height = gm::kSoundOptionsSlideHeight;
    std::vector<std::uint32_t> out = compose_sprite(
        slide, 0, 0, width, height);
    const std::vector<std::uint32_t> top = compose_sprite(
        knob, knob_x - gm::kSoundOptionsSlideX, 0, width, height);
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

/// Compare the rows `y..y+height-1` of the 196-px band of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what, int x_min = 0) {
    const int width = gm::kSoundOptionsPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kSoundOptionsBackground : want;
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
        return approved / "gm_sound_options_render";
    }
    return fs::temp_directory_path() / "gm_sound_options_render";
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
void check_refused(const fs::path& game, const gm::SoundOptionsRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::SoundOptionsRenderStats stats;
    stats.rows = 7;
    stats.sliders = 7;
    stats.checks = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_sound_options(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "sound_options_render:"),
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

    // Geometry of 0x4889f0 (rows 7, step 10, title row 14, x 18/30).
    check_eq(gm::kSoundOptionsRows, 7, tag + "rows = 7 (0x67f10c at 0x488a1e)");
    check_eq(gm::kSoundOptionsItemStep, 10, tag + "step = 10 (0x662d58 at 0x488a2a)");
    check_eq(gm::kSoundOptionsTitleY, 14, tag + "title row 14 (push 0xe at 0x4889f7)");
    check_eq(gm::kSoundOptionsTitleX, 38, tag + "title x 38 fallback (push 0x26)");
    check_eq(gm::kSoundOptionsTitleFont, 5, tag + "title font 5");
    check_eq(gm::kSoundOptionsRowFont, 0, tag + "row font 0");
    check_eq(gm::kSoundOptionsSelectedFont, 4, tag + "highlight font 4");
    check_eq(std::string(gm::kSoundOptionsTitle), std::string("SOUND OPTIONS"),
             tag + "title 0x4bd534");
    check_eq(std::string(gm::kSoundOptionsPanelName), std::string("PLATE"), tag + "PLATE");
    check_eq(gm::kSoundOptionsPanelWidth, 196, tag + "PLATE 196 wide");
    check_eq(gm::kSoundOptionsPanelHeight, 131, tag + "PLATE 131 high");

    // The four label calls and the resulting y.
    const std::array<int, 4> want_items = {0, 2, 4, 6};
    const std::array<int, 4> want_x = {18, 18, 30, 18};
    const std::array<int, 4> want_y = {27, 47, 67, 84};
    for (std::size_t i = 0; i < want_items.size(); ++i) {
        const int item = want_items[i];
        check_eq(gm::sound_options_item_y(item), want_y[i],
                 tag + "y of item " + std::to_string(item));
        check_eq(gm::sound_options_item_x(item), want_x[i],
                 tag + "x of item " + std::to_string(item));
    }
    check_eq(gm::kSoundOptionsLabelRow, 27, tag + "label row immediate 27 (0x488a38)");
    check_eq(gm::kSoundOptionsDoneRow, 24, tag + "Done row immediate 24 (0x488b18)");

    // Sound-off layout: rows 1, step 0, four absolute lines.
    check_eq(gm::kSoundOptionsDisabledRows, 1, tag + "sound-off rows = 1 (0x488b38)");
    check_eq(gm::kSoundOptionsDisabledItemStep, 0,
             tag + "sound-off step = 0 (0x662d58 at 0x488b42)");
    const std::array<int, 4> want_off = {27, 37, 47, 57};
    for (std::size_t i = 0; i < want_off.size(); ++i) {
        check_eq(gm::sound_options_disabled_y(static_cast<int>(i)), want_off[i],
                 tag + "sound-off line " + std::to_string(i) + " y");
        check_eq(gm::kSoundOptionsDisabledItems[i], static_cast<int>(i) + 2,
                 tag + "sound-off line " + std::to_string(i) + " item id");
    }
    check_eq(gm::sound_options_disabled_y(-3), 27, tag + "sound-off y clamp low");
    check_eq(gm::sound_options_disabled_y(99), 57, tag + "sound-off y clamp high");

    // Sliders: SLIDE (68,37)/(68,57), KNOB base 69, 27 px per ten units.
    check_eq(gm::kSoundOptionsSlideX, 68, tag + "SLIDE x 68 (0x688290)");
    check_eq(gm::kSoundOptionsSlideWidth, 72, tag + "SLIDE 72 wide");
    check_eq(gm::kSoundOptionsSlideHeight, 8, tag + "SLIDE 8 high");
    check_eq(gm::kSoundOptionsKnobBaseX, 69, tag + "KNOB base x 69 (0x688291)");
    check_eq(gm::kSoundOptionsKnobWidth, 8, tag + "KNOB 8 wide");
    check_eq(gm::kSoundOptionsKnobHeight, 8, tag + "KNOB 8 high");
    check_eq(gm::kSoundOptionsSlideY[0], 37, tag + "music slider y 37");
    check_eq(gm::kSoundOptionsSlideY[1], 57, tag + "effects slider y 57");
    const std::array<int, 5> want_volume = {0, 5, 10, 15, 20};
    const std::array<int, 5> want_knob = {69, 82, 96, 109, 123};
    for (std::size_t i = 0; i < want_knob.size(); ++i) {
        check_eq(gm::sound_options_knob_x(want_volume[i]), want_knob[i],
                 tag + "knob x of volume " + std::to_string(want_volume[i]));
    }
    check_eq(gm::sound_options_knob_x(-10), 69, tag + "knob clamp low -> 69");
    check_eq(gm::sound_options_knob_x(99), 123, tag + "knob clamp high -> 123");

    // Regex parity with the engine's arithmetic: 27*volume/10 truncated.
    for (int volume = 0; volume <= 20; ++volume) {
        check_eq(gm::sound_options_knob_x(volume), 69 + 27 * volume / 10,
                 tag + "27*volume/10 model at " + std::to_string(volume));
    }

    // Volume clamp 0..20, step 1.
    check_eq(gm::sound_options_volume(-3), 0, tag + "volume clamp low 0");
    check_eq(gm::sound_options_volume(20), 20, tag + "volume max 20 (cmp 0x14)");
    check_eq(gm::sound_options_volume(21), 20, tag + "volume clamp high 20");
    check_eq(gm::kSoundOptionsVolumeStep, 1, tag + "volume step 1");
    check_eq(gm::kSoundOptionsMusicDefault, 10, tag + "music .data default 10");
    check_eq(gm::kSoundOptionsEffectsDefault, 20, tag + "effects .data default 20");
    check_eq(gm::kSoundOptionsMusicShippedCfg, 16, tag + "shipped CFG music 16");
    check_eq(gm::kSoundOptionsEffectsShippedCfg, 20, tag + "shipped CFG effects 20");

    // Checkbox: (20,59), CHECK 8x16, NOCHECK 8x8 eight rows lower.
    check_eq(gm::kSoundOptionsCheckX, 20, tag + "checkbox x 20 (0x689338)");
    check_eq(gm::kSoundOptionsCheckY, 59, tag + "checkbox anchor y 59");
    check_eq(gm::kSoundOptionsCheckWidth, 8, tag + "CHECK 8 wide");
    check_eq(gm::kSoundOptionsCheckHeight, 16, tag + "CHECK 16 high");
    check_eq(gm::kSoundOptionsNoCheckWidth, 8, tag + "NOCHECK 8 wide");
    check_eq(gm::kSoundOptionsNoCheckHeight, 8, tag + "NOCHECK 8 high");
    check_eq(gm::kSoundOptionsNoCheckDrop, 8, tag + "NOCHECK drawn 8 rows lower");
    check(!gm::sound_options_switch_on(0), tag + "swap 0 -> NOCHECK");
    check(gm::sound_options_switch_on(1), tag + "swap 1 -> CHECK");
    check(gm::sound_options_switch_on(-7), tag + "swap non-zero -> CHECK");

    // Defaults and entry cursor.
    check_eq(gm::kSoundOptionsSwitchDefault, 0, tag + "swap default 0 (.bss 0x5a494c)");
    check_eq(gm::kSoundOptionsDefaultCursor, 0, tag + "cursor default 0 (.bss 0x68ccf4)");
    check(gm::kSoundOptionsSoundEnabledDefault, tag + "sound enabled .data 0x4aa384 = 1");
    check(!gm::kSoundOptionsMusicLowMemoryDefault,
          tag + "music-low-memory default 0 (.bss 0x68cc9c)");

    // Labels are the handler's literal strings.
    gm::SoundOptionsRenderOptions options;
    check_eq(gm::sound_options_label(0, options), std::string("Music Volume:"),
             tag + "item 0 = 0x4bd508");
    check_eq(gm::sound_options_label(2, options), std::string("Effects Volume:"),
             tag + "item 2 = 0x4bd4f8");
    check_eq(gm::sound_options_label(4, options), std::string("Switch Left Right"),
             tag + "item 4 = 0x4bd4e4");
    check_eq(gm::sound_options_label(6, options), std::string("Done"),
             tag + "item 6 = 0x4bceb4");
    options.music_low_memory = true;
    check_eq(gm::sound_options_label(0, options),
             std::string("Music Disabled: Low Memory"), tag + "item 0 music low memory");
    options.sound_enabled = false;
    check_eq(gm::sound_options_label(2, options), std::string("Sound is disabled."),
             tag + "sound-off item 2 = 0x4bd4d0");
    check_eq(gm::sound_options_label(3, options),
             std::string("Rerun Gunmetal without the"),
             tag + "sound-off item 3 = 0x4bd4b4");
    check_eq(gm::sound_options_label(4, options),
             std::string("N command line option to"), tag + "sound-off item 4 = 0x4bd498");
    check_eq(gm::sound_options_label(5, options), std::string("enjoy sound."),
             tag + "sound-off item 5 = 0x4bd488");
    check_eq(gm::sound_options_label(99, options), std::string("Done"),
             tag + "out-of-range id -> Done");
}

/// Independent ink of the four labels plus the title of one layout.
long long text_ink(const std::map<int, gm::FontAtlas>& fonts,
                   const gm::SoundOptionsRenderOptions& options) {
    long long ink = 0;
    const gm::FontAtlas& title_face = fonts.at(gm::kSoundOptionsTitleFont);
    const int title_x = std::max(
        0, (gm::kSoundOptionsPanelWidth - pen_width(title_face, gm::kSoundOptionsTitle)) / 2);
    ink += band_ink(compose_band(title_face, gm::kSoundOptionsTitle, title_x,
                                 gm::kSoundOptionsPanelWidth, title_face.cell_height));
    const auto line_ink = [&](int item, int x, int /*y*/) {
        // The cursor item is font 4; the test passes the cursor separately
        // where it matters, so compute both fonts is not needed: the ink of a
        // word can differ per face, so use the face the renderer picks.
        const int font = item == options.cursor ? gm::kSoundOptionsSelectedFont
                                                : gm::kSoundOptionsRowFont;
        const gm::FontAtlas& face = fonts.at(font);
        return band_ink(compose_band(face, gm::sound_options_label(item, options), x,
                                     gm::kSoundOptionsPanelWidth, face.cell_height));
    };
    if (!options.sound_enabled) {
        for (std::size_t i = 0; i < gm::kSoundOptionsDisabledItems.size(); ++i) {
            ink += line_ink(gm::kSoundOptionsDisabledItems[i], gm::kSoundOptionsItemX,
                            gm::sound_options_disabled_y(static_cast<int>(i)));
        }
        return ink;
    }
    ink += line_ink(gm::kSoundOptionsMusicItem, gm::sound_options_item_x(0),
                    gm::sound_options_item_y(0));
    ink += line_ink(gm::kSoundOptionsEffectsItem, gm::sound_options_item_x(2),
                    gm::sound_options_item_y(2));
    ink += line_ink(gm::kSoundOptionsSwitchItem, gm::sound_options_item_x(4),
                    gm::sound_options_item_y(4));
    ink += line_ink(gm::kSoundOptionsDoneItem, gm::sound_options_item_x(6),
                    gm::sound_options_item_y(6));
    return ink;
}

// --- frame -------------------------------------------------------------------
void test_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const gm::SoundOptionsRenderOptions options;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kSoundOptionsCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kSoundOptionsCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kSoundOptionsRows, tag + "stats rows 7");
    check_eq(frame.stats.sliders, gm::kSoundOptionsSliderCount, tag + "stats sliders 2");
    check_eq(frame.stats.checks, 1, tag + "stats checks 1");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel is opaque");

    // Title: centred at y 14 (font 5).
    const gm::FontAtlas& title_face = fonts.at(gm::kSoundOptionsTitleFont);
    const int title_x = std::max(
        0, (gm::kSoundOptionsPanelWidth - pen_width(title_face, gm::kSoundOptionsTitle)) / 2);
    check_band(frame, title_face, gm::kSoundOptionsTitle, title_x, gm::kSoundOptionsTitleY,
               tag + "title");
    const Box title_box = ink_box(frame, gm::kSoundOptionsTitleY,
                                  gm::kSoundOptionsTitleY + title_face.cell_height - 1);
    const std::vector<std::uint32_t> title_band =
        compose_band(title_face, gm::kSoundOptionsTitle, title_x,
                     gm::kSoundOptionsPanelWidth, title_face.cell_height);
    const Box title_want =
        band_box(title_band, gm::kSoundOptionsPanelWidth, title_face.cell_height);
    check_eq(title_box.x0, title_want.x0, tag + "title bbox x0");
    check_eq(title_box.x1, title_want.x1, tag + "title bbox x1");
    check_eq(title_box.y0, gm::kSoundOptionsTitleY + title_want.y0, tag + "title bbox y0");
    check_eq(title_box.y1, gm::kSoundOptionsTitleY + title_want.y1, tag + "title bbox y1");
    std::printf("  sound_options title: \"%s\" x=%d y=%d ink=%lld bbox %s\n",
                std::string(gm::kSoundOptionsTitle).c_str(), title_x,
                gm::kSoundOptionsTitleY, band_ink(title_band),
                box_text(title_box).c_str());

    // Every label, its bbox and its band.
    const std::array<int, 4> items = {0, 2, 4, 6};
    long long row_ink_total = 0;
    (void)row_ink_total;
    for (const int item : items) {
        const int font = item == options.cursor ? gm::kSoundOptionsSelectedFont
                                                : gm::kSoundOptionsRowFont;
        const gm::FontAtlas& face = fonts.at(font);
        const std::string label = gm::sound_options_label(item, options);
        const int x = gm::sound_options_item_x(item);
        const int y = gm::sound_options_item_y(item);
        check_band(frame, face, label, x, y,
                   tag + "row " + std::to_string(item) + " \"" + label + "\"", x);
        const std::vector<std::uint32_t> band =
            compose_band(face, label, x, gm::kSoundOptionsPanelWidth, face.cell_height);
        const Box want = band_box(band, gm::kSoundOptionsPanelWidth, face.cell_height);
        const Box got = ink_box(frame, y, y + face.cell_height - 1,
                                gm::kSoundOptionsPanelWidth, x);
        check_eq(got.x0, want.x0, tag + "row " + std::to_string(item) + " bbox x0");
        check_eq(got.x1, want.x1, tag + "row " + std::to_string(item) + " bbox x1");
        check_eq(got.y0, y + want.y0, tag + "row " + std::to_string(item) + " bbox y0");
        check_eq(got.y1, y + want.y1, tag + "row " + std::to_string(item) + " bbox y1");
        const long long ink = band_ink(band);
        row_ink_total += ink;
        std::printf("  sound_options row %d: x=%2d y=%2d \"%s\" ink=%lld bbox %s\n", item,
                    x, y, label.c_str(), ink, box_text(got).c_str());
    }

    // Sliders: the whole 72x8 rectangle against the independent composite.
    const Asset slide = load_asset(game, "SLIDE");
    const Asset knob = load_asset(game, "KNOB");
    check(slide.ok && knob.ok, tag + "SLIDE and KNOB decoded");
    long long slider_ink_total = 0;
    if (slide.ok && knob.ok) {
        check_eq(slide.width, gm::kSoundOptionsSlideWidth, tag + "SLIDE 72 wide");
        check_eq(slide.height, gm::kSoundOptionsSlideHeight, tag + "SLIDE 8 high");
        check_eq(knob.width, gm::kSoundOptionsKnobWidth, tag + "KNOB 8 wide");
        check_eq(knob.height, gm::kSoundOptionsKnobHeight, tag + "KNOB 8 high");
        const std::array<int, 2> volumes = {options.music_volume, options.effects_volume};
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            const int y = gm::kSoundOptionsSlideY[i];
            const int knob_x = gm::sound_options_knob_x(volumes[i]);
            const std::vector<std::uint32_t> want = compose_slider(slide, knob, knob_x);
            check_eq(rect_mismatch(frame, want, gm::kSoundOptionsSlideX, y,
                                   gm::kSoundOptionsSlideWidth,
                                   gm::kSoundOptionsSlideHeight),
                     0, tag + "slider " + std::to_string(i) +
                            " matches the SLIDE+KNOB composite");
            const long long ink = band_ink(want);
            slider_ink_total += ink;
            std::printf("  sound_options slider %zu: volume=%d knob x=%d y=%d, "
                        "SLIDE+KNOB ink=%lld\n",
                        i, volumes[i], knob_x, y, ink);
        }
    }

    // Checkbox rectangle: NOCHECK 8 rows below the anchor by default.
    const Asset check_asset = load_asset(game, "CHECK");
    const Asset nocheck_asset = load_asset(game, "NOCHECK");
    check(check_asset.ok && nocheck_asset.ok, tag + "CHECK and NOCHECK decoded");
    long long check_ink = 0;
    if (check_asset.ok && nocheck_asset.ok) {
        check_eq(check_asset.width, gm::kSoundOptionsCheckWidth, tag + "CHECK 8 wide");
        check_eq(check_asset.height, gm::kSoundOptionsCheckHeight, tag + "CHECK 16 high");
        check_eq(nocheck_asset.width, gm::kSoundOptionsNoCheckWidth, tag + "NOCHECK 8 wide");
        check_eq(nocheck_asset.height, gm::kSoundOptionsNoCheckHeight,
                 tag + "NOCHECK 8 high");
        const std::vector<std::uint32_t> want =
            compose_checkbox(check_asset, nocheck_asset, false);
        check_eq(rect_mismatch(frame, want, gm::kSoundOptionsCheckX,
                               gm::kSoundOptionsCheckY, gm::kSoundOptionsCheckWidth,
                               gm::kSoundOptionsCheckHeight),
                 0, tag + "default switch off matches the NOCHECK composite");
        check_ink = band_ink(compose_checkbox(check_asset, nocheck_asset, false));
    }

    const long long expected_ink =
        text_ink(fonts, options) + slider_ink_total + check_ink;
    check_eq(frame.stats.ink_pixels, expected_ink,
             tag + "ink = text bands + slider unions + checkbox");
    std::printf("  sound_options frame: rows=%d sliders=%d checks=%d ink=%d "
                "(text+widgets model %lld) missing=%d arrows=%d\n",
                frame.stats.rows, frame.stats.sliders, frame.stats.checks,
                frame.stats.ink_pixels, expected_ink, frame.stats.missing_glyphs,
                frame.stats.arrows);
}

// --- sliders -----------------------------------------------------------------
void test_sliders(const fs::path& game) {
    const std::string tag = "sliders: ";
    const Asset slide = load_asset(game, "SLIDE");
    const Asset knob = load_asset(game, "KNOB");
    check(slide.ok && knob.ok, tag + "SLIDE and KNOB loaded");
    if (!slide.ok || !knob.ok) {
        return;
    }

    const auto music = [&](int volume) {
        gm::SoundOptionsRenderOptions options;
        options.music_volume = volume;
        return run(game, options);
    };
    const Frame zero = music(0);
    const Frame ten = music(10);
    const Frame twenty = music(20);
    check(zero.ok && ten.ok && twenty.ok, tag + "music frames render");
    if (!zero.ok || !ten.ok || !twenty.ok) {
        return;
    }
    check_eq(gm::sound_options_knob_x(0), 69, tag + "music volume 0 knob 69");
    check_eq(gm::sound_options_knob_x(10), 96, tag + "music volume 10 knob 96");
    check_eq(gm::sound_options_knob_x(20), 123, tag + "music volume 20 knob 123");

    const int y = gm::kSoundOptionsSlideY[0];
    const long long diff_0_10 = differing_pixels(zero.rgba, ten.rgba);
    const long long diff_10_20 = differing_pixels(ten.rgba, twenty.rgba);
    check(diff_0_10 > 0, tag + "music 0 vs 10 differ");
    check(diff_10_20 > 0, tag + "music 10 vs 20 differ");
    check_eq(differing_outside_rect(ten, zero, gm::kSoundOptionsSlideX, y,
                                    gm::kSoundOptionsSlideX +
                                        gm::kSoundOptionsSlideWidth - 1,
                                    y + gm::kSoundOptionsSlideHeight - 1),
             0, tag + "music volume changes only the 72x8 slider rectangle");
    check_eq(differing_outside_rect(twenty, ten, gm::kSoundOptionsSlideX, y,
                                    gm::kSoundOptionsSlideX +
                                        gm::kSoundOptionsSlideWidth - 1,
                                    y + gm::kSoundOptionsSlideHeight - 1),
             0, tag + "music 10 vs 20 changes only the 72x8 slider rectangle");
    const std::vector<std::uint32_t> want = compose_slider(slide, knob, 123);
    check_eq(rect_mismatch(twenty, want, gm::kSoundOptionsSlideX, y,
                           gm::kSoundOptionsSlideWidth, gm::kSoundOptionsSlideHeight),
             0, tag + "music volume 20 matches the SLIDE+KNOB composite at x 123");
    std::printf("  sound_options music slider: knob x 69/96/123, diff 0->10=%lld px, "
                "10->20=%lld px\n",
                diff_0_10, diff_10_20);

    const auto effects = [&](int volume) {
        gm::SoundOptionsRenderOptions options;
        options.music_volume = 0;
        options.effects_volume = volume;
        return run(game, options);
    };
    const Frame e0 = effects(0);
    const Frame e20 = effects(20);
    check(e0.ok && e20.ok, tag + "effects frames render");
    if (e0.ok && e20.ok) {
        const int ey = gm::kSoundOptionsSlideY[1];
        check(differing_pixels(e0.rgba, e20.rgba) > 0, tag + "effects 0 vs 20 differ");
        check_eq(differing_outside_rect(e20, e0, gm::kSoundOptionsSlideX, ey,
                                        gm::kSoundOptionsSlideX +
                                            gm::kSoundOptionsSlideWidth - 1,
                                        ey + gm::kSoundOptionsSlideHeight - 1),
                 0, tag + "effects volume changes only its 72x8 rectangle");
        check_eq(rect_mismatch(e0, compose_slider(slide, knob, 69),
                               gm::kSoundOptionsSlideX, gm::kSoundOptionsSlideY[0],
                               gm::kSoundOptionsSlideWidth,
                               gm::kSoundOptionsSlideHeight),
                 0, tag + "effect frames keep the music slider at knob 69");
        std::printf("  sound_options effects slider: knob x 69->123, diff=%lld px\n",
                    differing_pixels(e0.rgba, e20.rgba));
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
    const Frame off = run(game, gm::SoundOptionsRenderOptions{});
    gm::SoundOptionsRenderOptions on_options;
    on_options.switch_left_right = 1;
    const Frame on = run(game, on_options);
    check(off.ok && on.ok, tag + "on/off frames render");
    if (!off.ok || !on.ok) {
        return;
    }
    const long long diff = differing_pixels(off.rgba, on.rgba);
    check(diff > 0, tag + "switch 0 vs 1 differs");
    check_eq(differing_outside_rect(on, off, gm::kSoundOptionsCheckX,
                                    gm::kSoundOptionsCheckY,
                                    gm::kSoundOptionsCheckX +
                                        gm::kSoundOptionsCheckWidth - 1,
                                    gm::kSoundOptionsCheckY +
                                        gm::kSoundOptionsCheckHeight - 1),
             0, tag + "toggle changes only the 8x16 checkbox rectangle");
    check_eq(rect_mismatch(on, compose_checkbox(check_asset, nocheck_asset, true),
                           gm::kSoundOptionsCheckX, gm::kSoundOptionsCheckY,
                           gm::kSoundOptionsCheckWidth, gm::kSoundOptionsCheckHeight),
             0, tag + "switch 1 matches the CHECK composite at (20,59)");
    check_eq(rect_mismatch(off, compose_checkbox(check_asset, nocheck_asset, false),
                           gm::kSoundOptionsCheckX, gm::kSoundOptionsCheckY,
                           gm::kSoundOptionsCheckWidth, gm::kSoundOptionsCheckHeight),
             0, tag + "switch 0 matches the NOCHECK composite at (20,67)");
    check_eq(on.stats.checks, 1, tag + "stats checks 1 when on");
    check_eq(off.stats.checks, 1, tag + "stats checks 1 when off");
    std::printf("  sound_options check: off NOCHECK at (20,67) ink=%lld, on CHECK at "
                "(20,59) ink=%lld, diff=%lld px\n",
                band_ink(compose_checkbox(check_asset, nocheck_asset, false)),
                band_ink(compose_checkbox(check_asset, nocheck_asset, true)), diff);
    // A non-zero value other than 1 is checked too.
    gm::SoundOptionsRenderOptions seven;
    seven.switch_left_right = 7;
    const Frame frame7 = run(game, seven);
    check(frame7.ok && differing_pixels(frame7.rgba, on.rgba) == 0,
          tag + "switch 7 paints the same CHECK as 1");
}

// --- music low memory --------------------------------------------------------
void test_music_low_memory(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "music-low-memory: ";
    const Frame normal = run(game, gm::SoundOptionsRenderOptions{});
    gm::SoundOptionsRenderOptions options;
    options.music_low_memory = true;
    const Frame low = run(game, options);
    check(normal.ok && low.ok, tag + "frames render");
    if (!normal.ok || !low.ok) {
        return;
    }
    check_eq(low.stats.rows, gm::kSoundOptionsRows, tag + "rows stays 7");
    check_eq(low.stats.sliders, 1, tag + "only the effects slider is drawn");
    check_eq(low.stats.checks, 1, tag + "checkbox still drawn");
    check_eq(gm::sound_options_label(0, options),
             std::string("Music Disabled: Low Memory"), tag + "label 0x4bd518");
    const int font = options.cursor == 0 ? gm::kSoundOptionsSelectedFont
                                         : gm::kSoundOptionsRowFont;
    check_band(low, fonts.at(font), "Music Disabled: Low Memory",
               gm::kSoundOptionsItemX, gm::sound_options_item_y(0),
               tag + "music-disabled label");
    // The music slider rows 37..44 disappear, the label band changes; the
    // effects slider + checkbox + Done stay.
    const long long diff = differing_pixels(normal.rgba, low.rgba);
    check(diff > 0, tag + "frame differs");
    check_eq(differing_outside_rows(normal, low, 27, 44), 0,
             tag + "only rows 27..44 change");
    std::printf("  sound_options music low memory: sliders 2 -> 1, diff=%lld px in "
                "rows 27..44\n",
                diff);
}

// --- sound disabled ----------------------------------------------------------
void test_sound_disabled(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "sound-off: ";
    gm::SoundOptionsRenderOptions options;
    options.sound_enabled = false;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.rows, gm::kSoundOptionsDisabledRows, tag + "stats rows 1");
    check_eq(frame.stats.sliders, 0, tag + "stats sliders 0");
    check_eq(frame.stats.checks, 0, tag + "stats checks 0");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0");
    const gm::FontAtlas& face = fonts.at(gm::kSoundOptionsRowFont);
    long long ink = 0;
    for (std::size_t i = 0; i < gm::kSoundOptionsDisabledItems.size(); ++i) {
        const int item = gm::kSoundOptionsDisabledItems[i];
        const std::string label = gm::sound_options_label(item, options);
        const int y = gm::sound_options_disabled_y(static_cast<int>(i));
        const int font = item == options.cursor ? gm::kSoundOptionsSelectedFont
                                                : gm::kSoundOptionsRowFont;
        check_band(frame, fonts.at(font), label, gm::kSoundOptionsItemX, y,
                   tag + "line " + std::to_string(i) + " \"" + label + "\"");
        const long long line = band_ink(compose_band(face, label, gm::kSoundOptionsItemX,
                                                     gm::kSoundOptionsPanelWidth,
                                                     face.cell_height));
        ink += line;
        std::printf("  sound_options off line %zu: item=%d x=%d y=%d \"%s\"\n", i, item,
                    gm::kSoundOptionsItemX, y, label.c_str());
    }
    // The title is unchanged and no widget is drawn.
    check(frame.stats.ink_pixels > 0, tag + "ink present");
    check(frame.stats.ink_pixels > ink, tag + "title ink is included");
    check(differing_pixels(frame.rgba, run(game, gm::SoundOptionsRenderOptions{}).rgba) > 0,
          tag + "sound-off differs from the sound-on frame");
}

// --- cursor ------------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    constexpr std::uint32_t kWhite = 0xEFEFEFFF; // FONTS4 face 4 (239,239,239)

    const auto make = [&](int cursor) {
        gm::SoundOptionsRenderOptions options;
        options.cursor = cursor;
        return run(game, options);
    };
    const Frame none = make(-1);
    check(none.ok, tag + "no highlight frame: " + none.error);
    if (!none.ok) {
        return;
    }
    std::vector<Frame> frames;
    const std::array<int, 4> items = {0, 2, 4, 6};
    for (const int item : items) {
        Frame frame = make(item);
        check(frame.ok, tag + "cursor " + std::to_string(item) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        const int y0 = gm::sound_options_item_y(item);
        const int y1 = y0 + fonts.at(gm::kSoundOptionsSelectedFont).cell_height - 1;
        check(has_colour(frame, y0, y1, kWhite),
              tag + "cursor " + std::to_string(item) + " paints font 4 in its band");
        check_eq(differing_outside_rows(frame, none, y0, y1), 0,
                 tag + "cursor " + std::to_string(item) + " changes only its rows");
        const long long diff = differing_pixels(frame.rgba, none.rgba);
        check(diff > 0, tag + "cursor " + std::to_string(item) + " differs");
        std::printf("  sound_options cursor %d: rows y=%d..%d, diff vs no highlight="
                    "%lld px\n",
                    item, y0, y1, diff);
        frames.push_back(std::move(frame));
    }
    check(differing_pixels(frames[0].rgba, frames[1].rgba) > 0,
          tag + "cursor 0 and 2 differ");
    check(differing_pixels(frames[1].rgba, frames[2].rgba) > 0,
          tag + "cursor 2 and 4 differ");
    check(differing_pixels(frames[2].rgba, frames[3].rgba) > 0,
          tag + "cursor 4 and 6 differ");

    const Frame high = make(99);
    check(high.ok, tag + "cursor 99 renders: " + high.error);
    if (high.ok) {
        check_eq(differing_pixels(high.rgba, none.rgba), 0,
                 tag + "cursor 99 highlights nothing");
    }
    const Frame entry = make(gm::kSoundOptionsDefaultCursor);
    check(entry.ok && differing_pixels(entry.rgba, frames[0].rgba) == 0,
          tag + "default cursor 0 is the music highlight");
    // The title (font 5) is never highlighted: cursor 0 leaves it dark red.
    constexpr std::uint32_t kRed = 0x9B0F0FFF; // FONTS4 face 5
    check(has_colour(frames[0], gm::kSoundOptionsTitleY,
                     gm::kSoundOptionsTitleY + fonts.at(5).cell_height - 1, kRed),
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
    check_eq(plate.width, gm::kSoundOptionsPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kSoundOptionsPanelHeight, tag + "PLATE 131 high");

    const Frame black_frame = run(game, gm::SoundOptionsRenderOptions{});
    gm::SoundOptionsRenderOptions plated;
    plated.background = std::string(gm::kSoundOptionsPanelName);
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
    std::printf("  sound_options plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "ink %d unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kSoundOptionsCanvasWidth) *
                                  gm::kSoundOptionsCanvasHeight;

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
        gm::SoundOptionsRenderOptions options;
        options.background = std::string(gm::kSoundOptionsPanelName);
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
                if (x < gm::kSoundOptionsPanelWidth && y < gm::kSoundOptionsPanelHeight) {
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
    check_eq(static_cast<long long>(differing_pixels(identity.rgba, plain.rgba)), 0,
             tag + "level 11 diff 0/64000");
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
    check_refused(fs::path{}, gm::SoundOptionsRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::SoundOptionsRenderOptions{}, "no BMP.IDX",
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
    check_refused(root / "game-without-plate", gm::SoundOptionsRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. FONTS4 + PLATE: SLIDE is missing.
    check(write_archive(root / "game-without-slide", {"FONTS4", "PLATE"},
                        {*fonts4, *plate}),
          "negatives: FONTS4+PLATE archive written");
    check_refused(root / "game-without-slide", gm::SoundOptionsRenderOptions{},
                  "archive without SLIDE", "SLIDE");

    // 3. + SLIDE: KNOB is missing.
    check(write_archive(root / "game-without-knob", {"FONTS4", "PLATE", "SLIDE"},
                        {*fonts4, *plate, *slide}),
          "negatives: FONTS4+PLATE+SLIDE archive written");
    check_refused(root / "game-without-knob", gm::SoundOptionsRenderOptions{},
                  "archive without KNOB", "KNOB");

    // 4. + KNOB: CHECK is missing.
    check(write_archive(root / "game-without-check", {"FONTS4", "PLATE", "SLIDE", "KNOB"},
                        {*fonts4, *plate, *slide, *knob}),
          "negatives: FONTS4+PLATE+SLIDE+KNOB archive written");
    check_refused(root / "game-without-check", gm::SoundOptionsRenderOptions{},
                  "archive without CHECK", "CHECK");

    // 5. + CHECK: NOCHECK is missing.
    check(write_archive(root / "game-without-nocheck",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK"},
                        {*fonts4, *plate, *slide, *knob, *check_bytes}),
          "negatives: FONTS4+PLATE+SLIDE+KNOB+CHECK archive written");
    check_refused(root / "game-without-nocheck", gm::SoundOptionsRenderOptions{},
                  "archive without NOCHECK", "NOCHECK");

    // 6. Without FONTS4 the title face is missing.
    check(write_archive(root / "game-without-font4",
                        {"PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*plate, *slide, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::SoundOptionsRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 7. A PLATE of the wrong size (the 8x16 CHECK renamed).
    check(write_archive(root / "game-wrong-plate",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*fonts4, *check_bytes, *slide, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::SoundOptionsRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 8. A SLIDE of the wrong size (the 8x8 KNOB renamed).
    check(write_archive(root / "game-wrong-slide",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*fonts4, *plate, *knob, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: wrong-size SLIDE archive written");
    check_refused(root / "game-wrong-slide", gm::SoundOptionsRenderOptions{},
                  "8x8 SLIDE", "expected 72x8");

    // 9. A complete synthetic archive renders.
    check(write_archive(root / "game-complete",
                        {"FONTS4", "PLATE", "SLIDE", "KNOB", "CHECK", "NOCHECK"},
                        {*fonts4, *plate, *slide, *knob, *check_bytes, *nocheck_bytes}),
          "negatives: complete archive written");
    {
        const Frame minimal = run(root / "game-complete", gm::SoundOptionsRenderOptions{});
        check(minimal.ok, "negatives: complete synthetic archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, gm::kSoundOptionsRows,
                     "negatives: synthetic rows 7");
            check_eq(minimal.stats.sliders, 2, "negatives: synthetic sliders 2");
            check_eq(minimal.stats.checks, 1, "negatives: synthetic checks 1");
            check(minimal.stats.ink_pixels > 0, "negatives: synthetic ink > 0");
            std::printf("  sound_options synthetic archive: ink=%d rows=%d sliders=%d "
                        "checks=%d missing=%d\n",
                        minimal.stats.ink_pixels, minimal.stats.rows,
                        minimal.stats.sliders, minimal.stats.checks,
                        minimal.stats.missing_glyphs);
            // Determinism on the synthetic archive too.
            const Frame again = run(root / "game-complete", gm::SoundOptionsRenderOptions{});
            check(again.ok && differing_pixels(minimal.rgba, again.rgba) == 0,
                  "negatives: synthetic renders are byte-identical");
        }
    }
    fs::remove_all(root, ec);

    // 10. An unknown background entry is a text, not black.
    gm::SoundOptionsRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 11. The exact fade needs a table or a dump.
    gm::SoundOptionsRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::SoundOptionsRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 12. Shipped install: default and deterministic.
    const Frame first = run(game, gm::SoundOptionsRenderOptions{});
    const Frame second = run(game, gm::SoundOptionsRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  sound_options negatives: rows=%d sliders=%d checks=%d ink=%d "
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
        std::cout << "sound_options_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kSoundOptionsTitleFont, gm::kSoundOptionsRowFont,
                             gm::kSoundOptionsSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "sound_options_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_frame(game, fonts);
    test_sliders(game);
    test_checkbox(game);
    test_music_low_memory(game, fonts);
    test_sound_disabled(game, fonts);
    test_cursor(game, fonts);
    test_plate(game);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "sound_options_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "sound_options_render: ok (" << checks << " checks)\n";
    return 0;
}
