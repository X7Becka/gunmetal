// One frame of screen 27, `MULTIPLAY GAME OPTIONS` (`M8.8` slice 9,
// `ui/match_setup_render`, docs/MATCH_SETUP_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.8
// from the builder `0x4885c0` and from pixels:
//
//   * model: `rows` 8, step 16, title `MULTIPLAY GAME OPTIONS` (font 5) centred
//     at y 11, the two value rows at x 23 (match type y 22, `Starting Cash: %ld`
//     y 32) and the five option rows at x 35 (y 45/60/75/90/103), `START GAME!`
//     at (23,113) in font 5 (font 4 when the cursor is 7), the five checkbox
//     anchors (24, text_y - 8) = 37/52/67/82/95, match types 0..2 and the twelve
//     starting-cash values; `sliders` is 0 because `0x4885c0` has no
//     `SLIDE`/`KNOB` call;
//   * the shipped install: 8 rows, 0 sliders, 5 checks, 0 arrows, no missing
//     glyph, 320x200 RGBA, every pixel opaque; the title band and every item
//     band are compared byte for byte with an independent blit and the bounding
//     boxes are printed; ink = text bands + checkbox rectangles;
//   * the five checkboxes: each flag bit flips exactly its own 8x16 rectangle,
//     the rectangle matches an independent `CHECK`/`NOCHECK` composite, the
//     checked box reaches 8 rows above the text and the all-on/all-off frames
//     are different;
//   * the value rows: match type 0/2 and cash index 0/11 give different frames
//     and different labels (numbers printed);
//   * the cursor 0/7/… changes only the corresponding band (font 4 = white);
//     `START GAME!` is font 5 (dark red) without a highlight, out-of-range
//     cursors (-1, 99) highlight nothing and the frames are pairwise different;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `FONTS4`, one with `FONTS4` but no `PLATE`, one with
//     `FONTS4`+`PLATE` but no `CHECK`, one without `NOCHECK`, a `PLATE` of the
//     wrong size, an unknown background entry, `fade_lut` without a table and a
//     dump, and a missing dump file — all `match_setup_render:` texts with a
//     cleared buffer; an archive with `FONTS4`/`PLATE`/`CHECK`/`NOCHECK` and no
//     `SLIDE`/`KNOB` still renders (they are not part of this screen); two
//     renders are deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/MATCH_SETUP_RENDER.md §3 pins them.

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
#include "ui/match_setup_render.h"

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
    gm::MatchSetupRenderStats stats;
};

Frame run(const fs::path& game, const gm::MatchSetupRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_match_setup(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kMatchSetupBackground; }

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
/// `x < x_limit`).
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

/// Compare the rows `y..y+height-1` of the 196-px band of *frame* with the
/// independent blit; every untouched pixel must be the black background.  The
/// `x_limit` keeps the comparison away from a checkbox that may reach the text
/// row on its left.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what,
                int x_limit = gm::kMatchSetupPanelWidth, int x_min = 0) {
    const int width = gm::kMatchSetupPanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    int wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = std::max(0, x_min); column < width && column < x_limit; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kMatchSetupBackground : want;
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

/// The 8x16 rectangle at (`x`, `y`) of a checkbox: masked `CHECK` over black
/// when checked, masked `NOCHECK` 8 rows lower otherwise.  0 = untouched.
std::vector<std::uint32_t> compose_checkbox(const Asset& check, const Asset& nocheck,
                                            bool checked, int width, int height) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height),
                                   0u);
    const Asset& sprite = checked ? check : nocheck;
    const int dy = checked ? 0 : gm::kMatchSetupNoCheckDrop;
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

long long checkbox_mismatch(const Frame& frame, const std::vector<std::uint32_t>& want,
                            int x, int y, int width, int height) {
    long long wrong = 0;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t expected =
                want[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, x + column, y + row);
            if (expected == 0) {
                continue; // only the sprite footprint is compared
            }
            if (got != expected) {
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
        return approved / "gm_match_setup_render";
    }
    return fs::temp_directory_path() / "gm_match_setup_render";
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
void check_refused(const fs::path& game, const gm::MatchSetupRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::MatchSetupRenderStats stats;
    stats.rows = 7;
    stats.sliders = 7;
    stats.checks = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_match_setup(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "match_setup_render:"),
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

    // Geometry of 0x4885c0 (rows 8, step 16, title row 11, x 23/35).
    check_eq(gm::kMatchSetupRows, 8, tag + "rows = 8 (0x67f10c at 0x4885c3)");
    check_eq(gm::kMatchSetupItemStep, 16, tag + "step = 16 (0x662d58 at 0x4885cd)");
    check_eq(gm::kMatchSetupTitleY, 11, tag + "title row 11 (push 0xb at 0x4885de)");
    check_eq(gm::kMatchSetupTitleX, 38, tag + "title x 38 fallback (push 0x26)");
    check_eq(gm::kMatchSetupTitleFont, 5, tag + "title font 5");
    check_eq(gm::kMatchSetupRowFont, 0, tag + "row font 0");
    check_eq(gm::kMatchSetupSelectedFont, 4, tag + "highlight font 4");
    check_eq(std::string(gm::kMatchSetupTitle), std::string("MULTIPLAY GAME OPTIONS"),
             tag + "title 0x4bd388");
    check_eq(std::string(gm::kMatchSetupPanelName), std::string("PLATE"), tag + "PLATE");
    check_eq(gm::kMatchSetupPanelWidth, 196, tag + "PLATE 196 wide");
    check_eq(gm::kMatchSetupPanelHeight, 131, tag + "PLATE 131 high");

    // The row immediates of the nine 0x485dd0 calls and the resulting y.
    const std::array<int, 8> want_y = {22, 32, 45, 60, 75, 90, 103, 113};
    const std::array<int, 8> want_row = {22, 16, 13, 12, 11, 10, 7, -113};
    for (int item = 0; item < gm::kMatchSetupRows; ++item) {
        const std::size_t slot = static_cast<std::size_t>(item);
        check_eq(gm::kMatchSetupItemRowArg[slot], want_row[slot],
                 tag + "row immediate of item " + std::to_string(item));
        check_eq(gm::match_setup_item_y(item), want_y[slot],
                 tag + "y of item " + std::to_string(item) + " = row + 16*item");
        // The same number the builder produces: row + step*item for row >= 0,
        // -row for the negative row of `START GAME!`.
        const int computed = want_row[slot] < 0
                                 ? -want_row[slot]
                                 : want_row[slot] + gm::kMatchSetupItemStep * item;
        check_eq(computed, want_y[slot],
                 tag + "arithmetic row+step*item of item " + std::to_string(item));
    }
    check_eq(gm::match_setup_item_y(-1), gm::kMatchSetupStartGameY,
             tag + "out-of-range item y -> START GAME row");
    check_eq(gm::kMatchSetupStartGameY, 113, tag + "START GAME! y 113 (push -113)");

    for (int item = 0; item < gm::kMatchSetupRows; ++item) {
        const int want = (item == 0 || item == 1 || item == gm::kMatchSetupStartGameItem)
                             ? 23
                             : 35;
        check_eq(gm::match_setup_item_x(item), want,
                 tag + "x of item " + std::to_string(item));
    }

    // Checkboxes: anchors (24, text_y - 8) and the five bit masks.
    check_eq(gm::kMatchSetupCheckboxCount, 5, tag + "five checkboxes");
    check_eq(gm::kMatchSetupCheckboxX, 24, tag + "checkbox x 24");
    check_eq(gm::kMatchSetupNoCheckDrop, 8, tag + "NOCHECK drawn 8 rows lower");
    const std::array<int, 5> want_anchor = {37, 52, 67, 82, 95};
    const std::array<int, 5> want_bit = {0x01, 0x04, 0x10, 0x20, 0x40};
    for (int i = 0; i < gm::kMatchSetupCheckboxCount; ++i) {
        const std::size_t slot = static_cast<std::size_t>(i);
        const int item = gm::kMatchSetupCheckboxFirstItem + i;
        check_eq(gm::match_setup_checkbox_y(item), want_anchor[slot],
                 tag + "checkbox anchor y of item " + std::to_string(item));
        check_eq(gm::kMatchSetupCheckboxFlag[slot], want_bit[slot],
                 tag + "checkbox bit of item " + std::to_string(item));
        check(gm::match_setup_checked(want_bit[slot], item),
              tag + "bit " + std::to_string(want_bit[slot]) + " checks item " +
                  std::to_string(item));
        check(!gm::match_setup_checked(want_bit[slot], item + 1),
              tag + "bit " + std::to_string(want_bit[slot]) + " does not check item " +
                  std::to_string(item + 1));
    }

    // Match types and starting-cash values (0x4bc6e8/0x4bc6f8 from .data).
    check_eq(gm::kMatchSetupMatchTypeCount, 3, tag + "three match types (idiv 3)");
    check_eq(std::string(gm::kMatchSetupMatchTypes[0]), std::string("Normal Deathmatch"),
             tag + "match type 0 = 0x4bc77c");
    check_eq(std::string(gm::kMatchSetupMatchTypes[1]),
             std::string("Scavenger Hunt Deathmatch"), tag + "match type 1 = 0x4bc760");
    check_eq(std::string(gm::kMatchSetupMatchTypes[2]), std::string("Deathmatch Classic"),
             tag + "match type 2 = 0x4bc74c");
    check_eq(gm::kMatchSetupCashCount, 12, tag + "twelve cash values (idiv 12)");
    const std::array<long long, 12> want_cash = {36000,  50000,  75000,  100000,
                                                 150000, 200000, 350000, 500000,
                                                 750000, 1000000, 2000000, 5000000};
    for (int i = 0; i < gm::kMatchSetupCashCount; ++i) {
        check_eq(gm::match_setup_starting_cash(i), want_cash[static_cast<std::size_t>(i)],
                 tag + "cash value " + std::to_string(i));
    }

    // Defaults: .bss 0 / .data 4 / .bss 0, entry cursor 7.
    check_eq(gm::kMatchSetupMatchTypeDefault, 0, tag + "default match type 0 (.bss)");
    check_eq(gm::kMatchSetupCashDefault, 4, tag + "default cash index 4 (.data 0x4bc728)");
    check_eq(gm::match_setup_starting_cash(gm::kMatchSetupCashDefault), 150000,
             tag + "default cash value 150000");
    check_eq(gm::kMatchSetupFlagsDefault, 0, tag + "default flags 0 (.bss)");
    check_eq(gm::kMatchSetupEntryCursor, 7, tag + "entry cursor 7 (0x48dcee/0x48dd81)");
    check_eq(gm::kMatchSetupDefaultCursor, 0, tag + "API default cursor 0");

    // Clamps: the input keeps the indices in range (idiv), the renderer clamps.
    check_eq(gm::match_setup_match_type(-3), 0, tag + "match type clamp low");
    check_eq(gm::match_setup_match_type(9), 2, tag + "match type clamp high");
    check_eq(gm::match_setup_cash_index(-1), 0, tag + "cash index clamp low");
    check_eq(gm::match_setup_cash_index(40), 11, tag + "cash index clamp high");
    check_eq(gm::match_setup_starting_cash(99), 5000000, tag + "cash value clamp high");

    // Labels are the handler's %s/%ld formatting.
    gm::MatchSetupRenderOptions options;
    check_eq(gm::match_setup_label(0, options), std::string("Normal Deathmatch"),
             tag + "item 0 default (0x4bc6e8[0])");
    check_eq(gm::match_setup_label(1, options), std::string("Starting Cash: 150000"),
             tag + "item 1 default (0x4bd374, value of index 4)");
    check_eq(gm::match_setup_label(2, options), std::string("Teamplay"),
             tag + "item 2 = 0x4bd368");
    check_eq(gm::match_setup_label(3, options), std::string("No Enemies"),
             tag + "item 3 = 0x4bd35c");
    check_eq(gm::match_setup_label(4, options), std::string("Can't Buy Ammo"),
             tag + "item 4 = 0x4bd34c");
    check_eq(gm::match_setup_label(5, options), std::string("Handicap Better Players"),
             tag + "item 5 = 0x4bd334");
    check_eq(gm::match_setup_label(6, options), std::string("Allow Mouse Cheating"),
             tag + "item 6 = 0x4bd31c");
    check_eq(gm::match_setup_label(7, options), std::string("START GAME!"),
             tag + "item 7 = 0x4bd310");
    options.match_type = 1;
    check_eq(gm::match_setup_label(0, options),
             std::string("Scavenger Hunt Deathmatch"), tag + "item 0 type 1");
    options.match_type = 2;
    options.starting_cash = 11;
    check_eq(gm::match_setup_label(0, options), std::string("Deathmatch Classic"),
             tag + "item 0 type 2");
    check_eq(gm::match_setup_label(1, options), std::string("Starting Cash: 5000000"),
             tag + "item 1 cash index 11");
}

// --- frame -------------------------------------------------------------------
void test_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "frame: ";
    const gm::MatchSetupRenderOptions options;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kMatchSetupCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kMatchSetupCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kMatchSetupRows, tag + "stats rows 8");
    check_eq(frame.stats.sliders, 0, tag + "stats sliders 0 (no SLIDE/KNOB at 0x4885c0)");
    check_eq(frame.stats.checks, gm::kMatchSetupCheckboxCount, tag + "stats checks 5");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel is opaque");

    // Title: centred at y 11 (font 5).
    const gm::FontAtlas& title_face = fonts.at(gm::kMatchSetupTitleFont);
    const int title_x =
        std::max(0, (gm::kMatchSetupPanelWidth - pen_width(title_face, gm::kMatchSetupTitle)) / 2);
    check_band(frame, title_face, gm::kMatchSetupTitle, title_x, gm::kMatchSetupTitleY,
               tag + "title");
    const Box title_box = ink_box(frame, gm::kMatchSetupTitleY,
                                  gm::kMatchSetupTitleY + title_face.cell_height - 1);
    const std::vector<std::uint32_t> title_band =
        compose_band(title_face, gm::kMatchSetupTitle, title_x, gm::kMatchSetupPanelWidth,
                     title_face.cell_height);
    const Box title_want = band_box(title_band, gm::kMatchSetupPanelWidth,
                                    title_face.cell_height);
    check_eq(title_box.x0, title_want.x0, tag + "title bbox x0");
    check_eq(title_box.x1, title_want.x1, tag + "title bbox x1");
    check_eq(title_box.y0, gm::kMatchSetupTitleY + title_want.y0, tag + "title bbox y0");
    check_eq(title_box.y1, gm::kMatchSetupTitleY + title_want.y1, tag + "title bbox y1");

    // Every row, its bbox and its band.  The default flags leave all five
    // checkboxes 8 rows below their own rows, so no band reaches a text row;
    // the comparison still stops at x 35 on the option rows as a guard.
    long long row_ink_total = 0;
    for (int item = 0; item < gm::kMatchSetupRows; ++item) {
        const int font = item == options.cursor
                             ? gm::kMatchSetupSelectedFont
                             : (item == gm::kMatchSetupStartGameItem
                                    ? gm::kMatchSetupTitleFont
                                    : gm::kMatchSetupRowFont);
        const gm::FontAtlas& face = fonts.at(font);
        const std::string label = gm::match_setup_label(item, options);
        const int x = gm::match_setup_item_x(item);
        const int y = gm::match_setup_item_y(item);
        // Columns left of the text may hold the checkbox of a *neighbouring*
        // row (the 8x16 `CHECK` reaches 8 rows up), so the band starts at x.
        check_band(frame, face, label, x, y,
                   tag + "row " + std::to_string(item) + " \"" + label + "\"",
                   gm::kMatchSetupPanelWidth, x);
        const std::vector<std::uint32_t> band =
            compose_band(face, label, x, gm::kMatchSetupPanelWidth, face.cell_height);
        const Box want = band_box(band, gm::kMatchSetupPanelWidth, face.cell_height);
        const Box got = ink_box(frame, y, y + face.cell_height - 1,
                                gm::kMatchSetupPanelWidth, x);
        check_eq(got.x0, want.x0, tag + "row " + std::to_string(item) + " bbox x0");
        check_eq(got.x1, want.x1, tag + "row " + std::to_string(item) + " bbox x1");
        check_eq(got.y0, y + want.y0, tag + "row " + std::to_string(item) + " bbox y0");
        check_eq(got.y1, y + want.y1, tag + "row " + std::to_string(item) + " bbox y1");
        const long long ink = band_ink(band);
        row_ink_total += ink;
        std::printf("  match_setup row %d: x=%2d y=%3d \"%s\" ink=%lld bbox %s\n", item, x,
                    y, label.c_str(), ink, box_text(got).c_str());
    }
    std::printf("  match_setup title: x=%d y=%d bbox %s\n", title_x,
                gm::kMatchSetupTitleY, box_text(title_box).c_str());
    const long long text_ink = band_ink(title_band) + row_ink_total;

    // The five checkbox rectangles are inside the ink total; with the default
    // flags they do not overlap any text band, so text ink + boxes = frame ink.
    const Asset check_asset = load_asset(game, "CHECK");
    const Asset nocheck_asset = load_asset(game, "NOCHECK");
    check(check_asset.ok && nocheck_asset.ok, tag + "CHECK and NOCHECK decoded");
    long long check_ink = 0;
    if (check_asset.ok && nocheck_asset.ok) {
        check_eq(check_asset.width, gm::kMatchSetupCheckWidth, tag + "CHECK 8 wide");
        check_eq(check_asset.height, gm::kMatchSetupCheckHeight, tag + "CHECK 16 high");
        check_eq(nocheck_asset.width, gm::kMatchSetupNoCheckWidth, tag + "NOCHECK 8 wide");
        check_eq(nocheck_asset.height, gm::kMatchSetupNoCheckHeight, tag + "NOCHECK 8 high");
        for (int i = 0; i < gm::kMatchSetupCheckboxCount; ++i) {
            const int item = gm::kMatchSetupCheckboxFirstItem + i;
            const std::vector<std::uint32_t> want =
                compose_checkbox(check_asset, nocheck_asset, false,
                                 gm::kMatchSetupCheckWidth, gm::kMatchSetupCheckHeight);
            check_eq(checkbox_mismatch(frame, want, gm::kMatchSetupCheckboxX,
                                       gm::match_setup_checkbox_y(item),
                                       gm::kMatchSetupCheckWidth, gm::kMatchSetupCheckHeight),
                     0, tag + "checkbox " + std::to_string(i + 1) +
                            " matches the NOCHECK composite");
            check_ink += band_ink(want);
        }
        check_eq(frame.stats.ink_pixels, text_ink + check_ink,
                 tag + "ink = text bands + checkbox rectangles");
    }
    std::printf("  match_setup frame: rows=%d sliders=%d checks=%d ink=%d (text %lld + "
                "checks %lld) missing=%d arrows=%d\n",
                frame.stats.rows, frame.stats.sliders, frame.stats.checks,
                frame.stats.ink_pixels, text_ink, check_ink, frame.stats.missing_glyphs,
                frame.stats.arrows);

    // The value rows change with the value: labels and pixels.
    gm::MatchSetupRenderOptions other;
    other.match_type = 2;
    const Frame type2 = run(game, other);
    gm::MatchSetupRenderOptions cash;
    cash.starting_cash = 11;
    const Frame cash11 = run(game, cash);
    check(type2.ok && cash11.ok, tag + "value-row frames render");
    if (type2.ok && cash11.ok) {
        const long long diff_type = differing_pixels(frame.rgba, type2.rgba);
        const long long diff_cash = differing_pixels(frame.rgba, cash11.rgba);
        check(diff_type > 0, tag + "match type 0 vs 2 differ");
        check(diff_cash > 0, tag + "cash index 4 vs 11 differ");
        // Only the corresponding 8-row band changes (rows 22 / 32).
        check_eq(differing_outside(type2, frame, gm::match_setup_item_y(0),
                                   gm::match_setup_item_y(0) + fonts.at(0).cell_height - 1),
                 0, tag + "match type changes only row 0");
        check_eq(differing_outside(cash11, frame, gm::match_setup_item_y(1),
                                   gm::match_setup_item_y(1) + fonts.at(0).cell_height - 1),
                 0, tag + "cash changes only row 1");
        std::printf("  match_setup values: type 0 vs 2 diff=%lld px, cash 4 vs 11 diff=%lld px\n",
                    diff_type, diff_cash);
    }
}

// --- checkboxes --------------------------------------------------------------
void test_checkboxes(const fs::path& game) {
    const std::string tag = "checks: ";
    const Asset check_sprite = load_asset(game, "CHECK");
    const Asset nocheck_sprite = load_asset(game, "NOCHECK");
    check(check_sprite.ok && nocheck_sprite.ok, tag + "CHECK and NOCHECK loaded");
    if (!check_sprite.ok || !nocheck_sprite.ok) {
        return;
    }

    const Frame off = run(game, gm::MatchSetupRenderOptions{});
    check(off.ok, tag + "all-off frame: " + off.error);
    if (!off.ok) {
        return;
    }
    long long total_diff = 0;
    for (int i = 0; i < gm::kMatchSetupCheckboxCount; ++i) {
        const std::size_t slot = static_cast<std::size_t>(i);
        const int item = gm::kMatchSetupCheckboxFirstItem + i;
        const int y = gm::match_setup_checkbox_y(item);
        gm::MatchSetupRenderOptions options;
        options.flags = gm::kMatchSetupCheckboxFlag[slot];
        const Frame on = run(game, options);
        check(on.ok, tag + "bit " + std::to_string(options.flags) + ": " + on.error);
        if (!on.ok) {
            return;
        }
        const long long diff = differing_pixels(off.rgba, on.rgba);
        total_diff += diff;
        check(diff > 0, tag + "bit " + std::to_string(options.flags) + " changes the frame");
        check_eq(differing_outside_rect(on, off, gm::kMatchSetupCheckboxX, y,
                                        gm::kMatchSetupCheckboxX +
                                            gm::kMatchSetupCheckWidth - 1,
                                        y + gm::kMatchSetupCheckHeight - 1),
                 0, tag + "bit " + std::to_string(options.flags) +
                        " changes only its 8x16 rectangle");
        const std::vector<std::uint32_t> want =
            compose_checkbox(check_sprite, nocheck_sprite, true, gm::kMatchSetupCheckWidth,
                             gm::kMatchSetupCheckHeight);
        check_eq(checkbox_mismatch(on, want, gm::kMatchSetupCheckboxX, y,
                                   gm::kMatchSetupCheckWidth, gm::kMatchSetupCheckHeight),
                 0, tag + "bit " + std::to_string(options.flags) +
                        " matches the CHECK composite");
        const std::vector<std::uint32_t> want_off =
            compose_checkbox(check_sprite, nocheck_sprite, false, gm::kMatchSetupCheckWidth,
                             gm::kMatchSetupCheckHeight);
        check_eq(checkbox_mismatch(off, want_off, gm::kMatchSetupCheckboxX, y,
                                   gm::kMatchSetupCheckWidth, gm::kMatchSetupCheckHeight),
                 0, tag + "bit " + std::to_string(options.flags) +
                        " off matches the NOCHECK composite");
        std::printf("  match_setup check %d (bit 0x%02x, y=%d..%d): on/off diff=%lld px, "
                    "checked ink=%lld, unchecked ink=%lld\n",
                    i + 1, options.flags, y, y + gm::kMatchSetupCheckHeight - 1, diff,
                    band_ink(want), band_ink(want_off));
    }
    // All five on.
    gm::MatchSetupRenderOptions all;
    all.flags = gm::kMatchSetupFlagMask;
    const Frame all_on = run(game, all);
    check(all_on.ok, tag + "all-on frame: " + all_on.error);
    if (all_on.ok) {
        const long long diff = differing_pixels(off.rgba, all_on.rgba);
        check(diff > 0, tag + "all-on differs from all-off");
        check_eq(all_on.stats.checks, gm::kMatchSetupCheckboxCount, tag + "all-on checks 5");
        check_eq(all_on.stats.ink_pixels - off.stats.ink_pixels > 0 ? 1 : 0, 1,
                 tag + "checked boxes paint more ink");
        std::printf("  match_setup checks: 5 bits, all-on vs all-off diff=%lld px, "
                    "ink %d -> %d\n",
                    diff, off.stats.ink_pixels, all_on.stats.ink_pixels);
    }
    // A bit outside the mask is ignored.
    gm::MatchSetupRenderOptions unused;
    unused.flags = 0x1000;
    const Frame unused_frame = run(game, unused);
    check(unused_frame.ok, tag + "unused bit frame: " + unused_frame.error);
    if (unused_frame.ok) {
        check_eq(differing_pixels(unused_frame.rgba, off.rgba), 0,
                 tag + "bits outside 0x01/0x04/0x10/0x20/0x40 change nothing");
    }
    std::printf("  match_setup checks total: per-bit diffs sum to %lld px\n", total_diff);
}

// --- cursor ------------------------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    constexpr std::uint32_t kWhite = 0xEFEFEFFF;  // FONTS4 face 4 (239,239,239)
    constexpr std::uint32_t kRed = 0x9B0F0FFF;    // FONTS4 face 5 (155,15,15)

    const auto make = [&](int cursor) {
        gm::MatchSetupRenderOptions options;
        options.cursor = cursor;
        return run(game, options);
    };
    const Frame none = make(-1);
    check(none.ok, tag + "no highlight frame: " + none.error);
    if (!none.ok) {
        return;
    }
    // Without a highlight `START GAME!` still uses font 5 (dark red).
    const int last_y = gm::match_setup_item_y(gm::kMatchSetupStartGameItem);
    check(!has_colour(none, last_y, last_y + fonts.at(4).cell_height - 1, kWhite),
          tag + "cursor -1 has no white glyph on START GAME!");
    check(has_colour(none, last_y, last_y + fonts.at(5).cell_height - 1, kRed),
          tag + "cursor -1 paints START GAME! in font 5 (dark red)");

    std::vector<Frame> frames;
    const std::array<int, 3> items = {0, 6, gm::kMatchSetupStartGameItem};
    for (const int item : items) {
        Frame frame = make(item);
        check(frame.ok, tag + "cursor " + std::to_string(item) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        const int y0 = gm::match_setup_item_y(item);
        const int y1 = y0 + fonts.at(gm::kMatchSetupSelectedFont).cell_height - 1;
        check(has_colour(frame, y0, y1, kWhite),
              tag + "cursor " + std::to_string(item) + " paints font 4 in its band");
        check_eq(differing_outside(frame, none, y0, y1), 0,
                 tag + "cursor " + std::to_string(item) + " changes only its band");
        const long long diff = differing_pixels(frame.rgba, none.rgba);
        check(diff > 0, tag + "cursor " + std::to_string(item) + " differs");
        std::printf("  match_setup cursor %d: band y=%d..%d, diff vs no highlight=%lld px\n",
                    item, y0, y1, diff);
        frames.push_back(std::move(frame));
    }
    check(differing_pixels(frames[0].rgba, frames[1].rgba) > 0,
          tag + "cursor 0 and 6 differ");
    check(differing_pixels(frames[1].rgba, frames[2].rgba) > 0,
          tag + "cursor 6 and 7 differ");

    const Frame high = make(99);
    check(high.ok, tag + "cursor 99 renders: " + high.error);
    if (high.ok) {
        check_eq(differing_pixels(high.rgba, none.rgba), 0,
                 tag + "cursor 99 highlights nothing (0x48dba9 clamps to 0..7)");
    }
    const Frame entry = make(gm::kMatchSetupEntryCursor);
    check(entry.ok, tag + "entry cursor frame: " + entry.error);
    if (entry.ok) {
        check(has_colour(entry, last_y, last_y + fonts.at(4).cell_height - 1, kWhite),
              tag + "entry cursor 7 highlights START GAME! in white");
    }
}

// --- plate -------------------------------------------------------------------
void test_plate(const fs::path& game) {
    const std::string tag = "plate: ";
    const Asset plate = load_asset(game, "PLATE");
    check(plate.ok, tag + "PLATE loaded");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kMatchSetupPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kMatchSetupPanelHeight, tag + "PLATE 131 high");

    gm::MatchSetupRenderOptions black;
    gm::MatchSetupRenderOptions plated;
    plated.background = std::string(gm::kMatchSetupPanelName);
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
    std::printf("  match_setup plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "text ink %d unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kMatchSetupCanvasWidth) *
                                  gm::kMatchSetupCanvasHeight;

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
        gm::MatchSetupRenderOptions options;
        options.background = std::string(gm::kMatchSetupPanelName);
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
                if (x < gm::kMatchSetupPanelWidth && y < gm::kMatchSetupPanelHeight) {
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
    check_refused(fs::path{}, gm::MatchSetupRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::MatchSetupRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    // Raw entries of the shipped archive, to build small synthetic archives.
    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> check_bytes =
        installed_entry(game, "CHECK");
    const std::optional<std::vector<std::uint8_t>> nocheck_bytes =
        installed_entry(game, "NOCHECK");
    check(fonts4.has_value() && plate.has_value() && check_bytes.has_value() &&
              nocheck_bytes.has_value(),
          "negatives: FONTS4/PLATE/CHECK/NOCHECK bytes available");
    if (!fonts4 || !plate || !check_bytes || !nocheck_bytes) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. An index with FONTS4 only: the faces load, PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4"}, {*fonts4}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-plate", gm::MatchSetupRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. FONTS4 + PLATE: CHECK is missing.
    check(write_archive(root / "game-without-check", {"FONTS4", "PLATE"},
                        {*fonts4, *plate}),
          "negatives: FONTS4+PLATE archive written");
    check_refused(root / "game-without-check", gm::MatchSetupRenderOptions{},
                  "archive without CHECK", "CHECK");

    // 3. FONTS4 + PLATE + CHECK: NOCHECK is missing.
    check(write_archive(root / "game-without-nocheck", {"FONTS4", "PLATE", "CHECK"},
                        {*fonts4, *plate, *check_bytes}),
          "negatives: FONTS4+PLATE+CHECK archive written");
    check_refused(root / "game-without-nocheck", gm::MatchSetupRenderOptions{},
                  "archive without NOCHECK", "NOCHECK");

    // 4. PLATE, CHECK and NOCHECK without FONTS4: the title face is missing.
    check(write_archive(root / "game-without-font4",
                        {"PLATE", "CHECK", "NOCHECK"},
                        {*plate, *check_bytes, *nocheck_bytes}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::MatchSetupRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 5. A PLATE of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-plate",
                        {"FONTS4", "PLATE", "CHECK", "NOCHECK"},
                        {*fonts4, *check_bytes, *check_bytes, *nocheck_bytes}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::MatchSetupRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 6. FONTS4 + PLATE + CHECK + NOCHECK and **no SLIDE/KNOB**: screen 27 has
    // no slider, so this minimal archive must render.
    check(write_archive(root / "game-no-slide",
                        {"FONTS4", "PLATE", "CHECK", "NOCHECK"},
                        {*fonts4, *plate, *check_bytes, *nocheck_bytes}),
          "negatives: minimal archive without SLIDE/KNOB written");
    {
        const Frame minimal = run(root / "game-no-slide", gm::MatchSetupRenderOptions{});
        check(minimal.ok, "negatives: minimal archive renders without SLIDE/KNOB: " +
                              minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.sliders, 0, "negatives: minimal archive sliders 0");
            check_eq(minimal.stats.checks, gm::kMatchSetupCheckboxCount,
                     "negatives: minimal archive checks 5");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  match_setup minimal archive (no SLIDE/KNOB): ink=%d checks=%d "
                        "missing=%d\n",
                        minimal.stats.ink_pixels, minimal.stats.checks,
                        minimal.stats.missing_glyphs);
        }
    }
    fs::remove_all(root, ec);

    // 7. A background name that is not in the archive is a text, not black.
    gm::MatchSetupRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 8. The exact fade needs a table or a dump.
    gm::MatchSetupRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::MatchSetupRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 9. The shipped install renders the API default and is deterministic.
    const Frame first = run(game, gm::MatchSetupRenderOptions{});
    const Frame second = run(game, gm::MatchSetupRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  match_setup negatives: rows=%d sliders=%d checks=%d ink=%d arrows=%d "
                    "missing=%d\n",
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
        std::cout << "match_setup_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kMatchSetupTitleFont, gm::kMatchSetupRowFont,
                             gm::kMatchSetupSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "match_setup_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_frame(game, fonts);
    test_checkboxes(game);
    test_cursor(game, fonts);
    test_plate(game);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "match_setup_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "match_setup_render: ok (" << checks << " checks)\n";
    return 0;
}
