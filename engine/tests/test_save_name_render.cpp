// One frame of screen 15, the save-name entry (`M8.8` slice 13,
// `ui/save_name_render`, docs/SAVE_NAME_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the frame of the state-15 branch of the input
// function `0x48b4d0` (`cmp ebp,0xf` at `0x48b691`) and the geometry of
// docs/UI_SCREENS.md §2/§3/§5.2 from pixels:
//
//   * model: `rows` 1, step 0 (the branch writes neither `0x67f10c` nor
//     `0x662d58`), the title `SAVE GAME` (`0x4bdc2c`) in font 5 centred at y 14
//     (emitted by `0x48abd0`, which the branch calls at `0x48b816`),
//     `ESC TO CANCEL` (`0x4bd9d8`) at (38, 101), the input line at x 42 and
//     y 28 + 12*row, the `FILESLOT` bar 124x10 at (38, 27 + 12*row), the 196x131
//     PLATE band, the 17-character / 18-byte buffer limit and the caret;
//   * the caret is the literal trailing `'-'` (0x2d) of `0x48b6d1`: no sprite,
//     no blink, no position.  `options.caret` appends that byte and the frame
//     changes exactly by the dash glyph in font 4;
//   * the shipped install: 1 row, 0 missing glyphs, no arrows, 320x200 RGBA,
//     every pixel opaque; the title, the input line and `ESC TO CANCEL` are
//     compared byte for byte with an independent blit and the bboxes printed;
//   * `FILESLOT`: the bar is decoded here in the *file* row order of the
//     original loader and compared pixel by pixel under the glyphs; the
//     `fileslot = false` frame differs exactly on the 124x10 rectangle;
//   * the name: changing it changes only the input row, a 30-character name is
//     cut to 17 + caret (13 bytes dropped) and is byte-identical to the 17-byte
//     prefix, `row` moves the two bands and nothing else;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an index
//     without `PLATE`, one without `FONTS4`, a `PLATE` of the wrong size, an
//     index without `FILESLOT` (refused while `fileslot` is on, paintable
//     without it), a `FILESLOT` of the wrong size, an unknown background entry,
//     `fade_lut` without a table and a dump, and a missing dump file — all
//     `save_name_render:` texts with a cleared buffer; two renders are
//     deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/SAVE_NAME_RENDER.md §5 pins them.

#include <algorithm>
#include <array>
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
#include <system_error>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "ui/save_name_render.h"

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
    gm::SaveNameRenderStats stats;
};

Frame run(const fs::path& game, const gm::SaveNameRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_save_name(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kSaveNameBackground; }

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

/// Bounding box of the non-black pixels of the rows `y0..y1`.
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

/// Compare the rows `y..y+height-1` of the columns `x_from..x_to-1` of the
/// 196-px band of *frame* with the independent blit; every untouched pixel must
/// be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what, int x_from = 0,
                int x_to = gm::kSaveNamePanelWidth) {
    const int width = gm::kSaveNamePanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    int wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = x_from; column < x_to && column < width; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kSaveNameBackground : want;
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

/// `check_band` plus the bbox against the independent blit, and the band ink.
long long check_run(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                    int x, int y, const std::string& what, bool print,
                    int x_from = 0, int x_to = gm::kSaveNamePanelWidth) {
    check_band(frame, atlas, text, x, y, what, x_from, x_to);
    const int width = gm::kSaveNamePanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    const Box want = band_box(band, width, height);
    const Box got = ink_box(frame, y, y + height - 1, x_to, x_from);
    check_eq(got.x0, want.x0, what + " bbox x0");
    check_eq(got.x1, want.x1, what + " bbox x1");
    check_eq(got.y0, y + want.y0, what + " bbox y0");
    check_eq(got.y1, y + want.y1, what + " bbox y1");
    const long long ink = band_ink(band);
    if (print) {
        std::printf("  save_name \"%s\": x=%3d y=%3d ink=%lld bbox %s\n",
                    std::string(text).c_str(), x, y, ink, box_text(got).c_str());
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

/// One `FRONTEND/BMP.IDX` sprite in the *file* row order the original loader
/// `0x47efe0` sees (`fread` of `width*height` bytes, no flip) plus its palette.
struct BandAsset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rows;
    std::vector<std::uint32_t> palette;

    std::uint32_t colour(int x, int y) const {
        const std::uint8_t index =
            rows[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)];
        return palette[index];
    }
};

BandAsset load_band_asset(const fs::path& game, const std::string& name) {
    BandAsset asset;
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
    if (!image || image->bpp != 8 || image->palette.size() < 4 || bytes->size() < 54) {
        return asset;
    }
    const std::size_t image_size = static_cast<std::size_t>(image->width) *
                                   static_cast<std::size_t>(image->height);
    const std::size_t offset = static_cast<std::size_t>((*bytes)[10]) |
                               (static_cast<std::size_t>((*bytes)[11]) << 8) |
                               (static_cast<std::size_t>((*bytes)[12]) << 16) |
                               (static_cast<std::size_t>((*bytes)[13]) << 24);
    if (image_size == 0 || offset + image_size > bytes->size()) {
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

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_save_name_render";
    }
    return fs::temp_directory_path() / "gm_save_name_render";
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
void check_refused(const fs::path& game, const gm::SaveNameRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::SaveNameRenderStats stats;
    stats.rows = 7;
    stats.chars = 7;
    stats.truncated = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    stats.fileslot_bands = 7;
    std::string error;
    const bool ok =
        gm::render_save_name(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "save_name_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.chars, 0, what + ": chars cleared");
    check_eq(stats.truncated, 0, what + ": truncated cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
    check_eq(stats.fileslot_bands, 0, what + ": fileslot cleared");
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // The state-15 branch of 0x48b4d0 writes neither rows nor step.
    check_eq(gm::kSaveNameRows, 1, tag + "rows = 1 (0x67f10c not written)");
    check_eq(gm::kSaveNameItemStep, 0, tag + "step = 0 (0x662d58 not written)");
    check_eq(gm::kSaveNameTitleY, 14, tag + "title y 14 (push 0xe at 0x48ac28)");
    check_eq(gm::kSaveNameTitleX, 38, tag + "title x 38 fallback (push 0x26)");
    check_eq(gm::kSaveNameCancelX, 38, tag + "ESC x 38 (push 0x26 at 0x48acab)");
    check_eq(gm::kSaveNameCancelY, 101, tag + "ESC y 101 (push 0x65 at 0x48acaa)");
    check_eq(gm::kSaveNameTitleFont, 5, tag + "title font 5");
    check_eq(gm::kSaveNameRowFont, 0, tag + "body font 0");
    check_eq(gm::kSaveNameInputFont, 4, tag + "input font 4 (0x485e19)");
    check_eq(gm::kSaveNameTextX, 42, tag + "input x 42 (push 0x2a at 0x48b85c)");
    check_eq(gm::kSaveNameFirstRowY, 28, tag + "first row y 28 (row -0x1c)");
    check_eq(gm::kSaveNameRowStep, 12, tag + "row step 12 (12*c in 0x48b859)");
    check_eq(gm::kSaveNameFileslotX, 38, tag + "FILESLOT x 38 (0x14d2 mod 196)");
    check_eq(gm::kSaveNameFileslotFirstY, 27, tag + "FILESLOT first y 27 (0x14d2 / 196)");
    check_eq(gm::kSaveNameFileslotWidth, 124, tag + "FILESLOT 124 wide");
    check_eq(gm::kSaveNameFileslotHeight, 10, tag + "FILESLOT 10 high");
    check_eq(gm::kSaveNamePanelWidth, 196, tag + "PLATE 196 wide");
    check_eq(gm::kSaveNamePanelHeight, 131, tag + "PLATE 131 high");
    check_eq(gm::kSaveNameWindowRows, 6, tag + "six window rows of [0x68ccf4]");
    check_eq(gm::kSaveNameMaxChars, 17, tag + "17 name characters");
    check_eq(gm::kSaveNameMaxBytes, 18, tag + "18-byte buffer (cmp ecx,0x12)");
    check_eq(std::string(1, gm::kSaveNameCaret), std::string("-"),
             tag + "caret is the literal '-' of 0x48b6d1");
    check_eq(std::string(gm::kSaveNameDefault), std::string("A SAVED GAME"),
             tag + "default name 0x4bdd44");
    check_eq(std::string(gm::kSaveNameTitle), std::string("SAVE GAME"),
             tag + "title 0x4bdc2c");
    check_eq(std::string(gm::kSaveNameCancel), std::string("ESC TO CANCEL"),
             tag + "ESC line 0x4bd9d8");
    check_eq(std::string(gm::kSaveNamePanelName), std::string("PLATE"), tag + "PLATE");
    check_eq(std::string(gm::kSaveNameFileslotName), std::string("FILESLOT"),
             tag + "FILESLOT");

    // Row anchors: y = 28 + 12*row and the bar one pixel above it.
    const std::array<int, 6> want_y = {28, 40, 52, 64, 76, 88};
    for (int row = 0; row < 6; ++row) {
        check_eq(gm::save_name_text_y(row), want_y[static_cast<std::size_t>(row)],
                 tag + "text y of row " + std::to_string(row));
        check_eq(gm::save_name_band_y(row),
                 want_y[static_cast<std::size_t>(row)] - 1,
                 tag + "band y of row " + std::to_string(row));
    }
    check_eq(gm::save_name_text_y(99), 88, tag + "row clamp high");
    check_eq(gm::save_name_text_y(-5), 28, tag + "row clamp low");
    check_eq(gm::save_name_band_y(99), 87, tag + "band row clamp high");
    check_eq(gm::save_name_band_y(-5), 27, tag + "band row clamp low");

    // The buffer: 17 characters + the caret, never more than 18 bytes.
    check_eq(gm::save_name_buffer(gm::SaveNameRenderOptions{}),
             std::string("A SAVED GAME-"), tag + "default buffer ends with the caret");
    gm::SaveNameRenderOptions no_caret;
    no_caret.caret = false;
    check_eq(gm::save_name_buffer(no_caret), std::string("A SAVED GAME"),
             tag + "caret off = post-Enter content");
    gm::SaveNameRenderOptions empty;
    empty.name.clear();
    check_eq(gm::save_name_buffer(empty), std::string("-"),
             tag + "empty slot buffer is only the caret (0x4bdd28)");
    gm::SaveNameRenderOptions long_name;
    long_name.name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    check_eq(gm::save_name_buffer(long_name), std::string("ABCDEFGHIJKLMNOPQ-"),
             tag + "26 characters -> 17 + caret");
    long_name.caret = false;
    check_eq(gm::save_name_buffer(long_name), std::string("ABCDEFGHIJKLMNOPQ"),
             tag + "26 characters without caret -> 17");
    check_eq(static_cast<long long>(gm::save_name_buffer(long_name).size()), 17,
             tag + "truncated buffer is 17 bytes");
    gm::SaveNameRenderOptions c_string;
    c_string.name = std::string("AB\0CD", 5);
    c_string.caret = false;
    check_eq(gm::save_name_buffer(c_string), std::string("AB"),
             tag + "a NUL ends the buffer (repnz scas at 0x48b69a)");
}

// --- the title, the input line and the ESC hint ------------------------------
void test_text(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "text: ";
    const gm::FontAtlas& title_face = fonts.at(gm::kSaveNameTitleFont);
    const gm::FontAtlas& body_face = fonts.at(gm::kSaveNameRowFont);
    const gm::FontAtlas& input_face = fonts.at(gm::kSaveNameInputFont);

    const gm::SaveNameRenderOptions options;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kSaveNameCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kSaveNameCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, 1, tag + "stats rows 1");
    check_eq(frame.stats.chars, 13, tag + "stats chars 13 (\"A SAVED GAME-\")");
    check_eq(frame.stats.truncated, 0, tag + "stats truncated 0");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW)");
    check_eq(frame.stats.fileslot_bands, 1, tag + "stats one FILESLOT bar");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel is opaque");

    // The input line lies on the FILESLOT bar, so its glyphs are compared on a
    // text-only frame (same ink, no bar).
    gm::SaveNameRenderOptions plain_options = options;
    plain_options.fileslot = false;
    const Frame plain = run(game, plain_options);
    check(plain.ok, tag + "text-only frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    check_eq(plain.stats.ink_pixels, frame.stats.ink_pixels,
             tag + "FILESLOT does not change the ink");
    check_eq(plain.stats.fileslot_bands, 0, tag + "text-only frame has no bar");

    const int title_x =
        std::max(0, (gm::kSaveNamePanelWidth - pen_width(title_face, gm::kSaveNameTitle)) / 2);
    const long long title_ink =
        check_run(frame, title_face, gm::kSaveNameTitle, title_x, gm::kSaveNameTitleY,
                  tag + "title", true);
    const long long cancel_ink =
        check_run(frame, body_face, gm::kSaveNameCancel, gm::kSaveNameCancelX,
                  gm::kSaveNameCancelY, tag + "ESC hint", true);
    const std::string buffer = gm::save_name_buffer(options);
    const long long input_ink =
        check_run(plain, input_face, buffer, gm::kSaveNameTextX,
                  gm::save_name_text_y(options.row), tag + "input line", true);
    check_eq(frame.stats.ink_pixels, title_ink + cancel_ink + input_ink,
             tag + "ink = title + ESC + input line");
    check(has_colour(frame, gm::kSaveNameTitleY,
                     gm::kSaveNameTitleY + title_face.cell_height - 1, 0x9B0F0FFF),
          tag + "title painted with the font-5 palette");
    check(has_colour(frame, gm::kSaveNameCancelY,
                     gm::kSaveNameCancelY + body_face.cell_height - 1, 0x130F0FFF),
          tag + "ESC painted with the font-0 palette");
    check(has_colour(plain, gm::save_name_text_y(0),
                     gm::save_name_text_y(0) + input_face.cell_height - 1, 0xEFEFEFFF),
          tag + "input line painted with the font-4 palette (0x485e19)");

    const Box title_box = ink_box(frame, gm::kSaveNameTitleY,
                                  gm::kSaveNameTitleY + title_face.cell_height - 1);
    const Box input_box = ink_box(plain, gm::save_name_text_y(0),
                                  gm::save_name_text_y(0) + input_face.cell_height - 1);
    std::printf("  save_name frame: rows=%d chars=%d ink=%d (title %lld + ESC %lld + "
                "input %lld) missing=%d arrows=%d bars=%d\n",
                frame.stats.rows, frame.stats.chars, frame.stats.ink_pixels, title_ink,
                cancel_ink, input_ink, frame.stats.missing_glyphs, frame.stats.arrows,
                frame.stats.fileslot_bands);
    std::printf("  save_name bands: title y=%d x=%d bbox %s; input y=%d x=%d bbox %s; "
                "ESC y=%d x=%d\n",
                gm::kSaveNameTitleY, title_x, box_text(title_box).c_str(),
                gm::save_name_text_y(0), gm::kSaveNameTextX,
                box_text(input_box).c_str(), gm::kSaveNameCancelY, gm::kSaveNameCancelX);
}

// --- the name and the caret --------------------------------------------------
void test_name(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "name: ";
    const gm::FontAtlas& input_face = fonts.at(gm::kSaveNameInputFont);

    gm::SaveNameRenderOptions a;
    a.name = "A SAVED GAME";
    gm::SaveNameRenderOptions b = a;
    b.name = "GUNMETAL";
    const Frame fa = run(game, a);
    const Frame fb = run(game, b);
    check(fa.ok && fb.ok, tag + "two names render");
    if (!fa.ok || !fb.ok) {
        return;
    }
    const long long diff = differing_pixels(fa.rgba, fb.rgba);
    check(diff > 0, tag + "changing the name changes the frame");
    check_eq(differing_outside(fa, fb, gm::save_name_text_y(0),
                               gm::save_name_text_y(0) + input_face.cell_height - 1),
             0, tag + "the name changes only the input row");
    check(fb.stats.ink_pixels != fa.stats.ink_pixels ||
              fb.stats.chars != fa.stats.chars,
          tag + "the numbers follow the name");
    std::printf("  save_name names: \"%s\" (%d chars, ink %d) vs \"%s\" (%d chars, "
                "ink %d): diff=%lld px\n",
                a.name.c_str(), fa.stats.chars, fa.stats.ink_pixels, b.name.c_str(),
                fb.stats.chars, fb.stats.ink_pixels, diff);

    // Truncation: 30 characters are cut to 17 + caret, 13 bytes dropped.
    gm::SaveNameRenderOptions long_name;
    long_name.name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123";
    const Frame cut = run(game, long_name);
    check(cut.ok, tag + "30-character name renders: " + cut.error);
    if (!cut.ok) {
        return;
    }
    check_eq(cut.stats.chars, 18, tag + "30 characters -> 18 buffer bytes");
    check_eq(cut.stats.truncated, 13, tag + "30 characters -> 13 dropped");
    check_eq(cut.stats.missing_glyphs, 0, tag + "truncated frame has all glyphs");
    gm::SaveNameRenderOptions prefix;
    prefix.name = "ABCDEFGHIJKLMNOPQ";
    const Frame prefix_frame = run(game, prefix);
    check(prefix_frame.ok, tag + "17-character prefix renders");
    if (prefix_frame.ok) {
        check_eq(differing_pixels(cut.rgba, prefix_frame.rgba), 0,
                 tag + "30 characters paint the 17-character prefix");
        check_eq(prefix_frame.stats.truncated, 0, tag + "17 characters are not truncated");
        std::printf("  save_name cut: 30 chars -> chars=%d truncated=%d ink=%d, "
                    "diff vs 17-char prefix=%lld px\n",
                    cut.stats.chars, cut.stats.truncated, cut.stats.ink_pixels,
                    differing_pixels(cut.rgba, prefix_frame.rgba));
    }

    // The caret of 0x48b6d1: a trailing '-' in font 4, no sprite and no blink.
    gm::SaveNameRenderOptions with_caret;
    with_caret.name = "AB";
    with_caret.caret = true;
    gm::SaveNameRenderOptions without_caret = with_caret;
    without_caret.caret = false;
    const Frame caret_on = run(game, with_caret);
    const Frame caret_off = run(game, without_caret);
    check(caret_on.ok && caret_off.ok, tag + "caret frames render");
    if (!caret_on.ok || !caret_off.ok) {
        return;
    }
    check_eq(caret_on.stats.chars, 3, tag + "caret on: 3 buffer bytes");
    check_eq(caret_off.stats.chars, 2, tag + "caret off: 2 buffer bytes");
    const int caret_x = gm::kSaveNameTextX + pen_width(input_face, "AB");
    const std::vector<std::uint32_t> dash =
        compose_band(input_face, "-", caret_x, gm::kSaveNamePanelWidth,
                     input_face.cell_height);
    const int caret_ink = static_cast<int>(band_ink(dash));
    const Box caret_box = band_box(dash, gm::kSaveNamePanelWidth, input_face.cell_height);
    check(caret_ink > 0, tag + "the '-' face has ink in font 4");
    const long long caret_diff = differing_pixels(caret_on.rgba, caret_off.rgba);
    check_eq(caret_diff, caret_ink,
             tag + "the caret adds exactly the '-' glyph pixels");
    check_eq(differing_outside(caret_on, caret_off, gm::save_name_text_y(0),
                               gm::save_name_text_y(0) + input_face.cell_height - 1),
             0, tag + "the caret changes only the input row");
    check(has_colour(caret_on, gm::save_name_text_y(0),
                     gm::save_name_text_y(0) + input_face.cell_height - 1, 0xEFEFEFFF),
          tag + "the caret is drawn in font 4 (white)");
    std::printf("  save_name caret: '-' at x=%d y=%d ink=%d, caret on/off diff=%lld px, "
                "bbox (%d,%d)-(%d,%d)\n",
                caret_x, gm::save_name_text_y(0), caret_ink, caret_diff, caret_box.x0,
                gm::save_name_text_y(0) + caret_box.y0, caret_box.x1,
                gm::save_name_text_y(0) + caret_box.y1);

    // The window row moves the two bands and nothing else.
    gm::SaveNameRenderOptions row3 = a;
    row3.row = 3;
    const Frame moved = run(game, row3);
    check(moved.ok, tag + "row 3 frame: " + moved.error);
    if (moved.ok) {
        check_eq(differing_pixels(moved.rgba, fa.rgba) > 0 ? 1 : 0, 1,
                 tag + "row 3 moves the input bands");
        // Both bands live in the rows 27..72 window: row 0 at 27..36/28..35, row
        // 3 at 63..72/64..71.
        check_eq(differing_outside(moved, fa, gm::save_name_band_y(0),
                                   gm::save_name_text_y(3) + gm::kSaveNameFileslotHeight),
                 0, tag + "row 3 changes only the two input bands");
        check_eq(moved.stats.fileslot_bands, 1, tag + "row 3 keeps one bar");
        std::printf("  save_name rows: row 0 y=%d vs row 3 y=%d diff=%lld px\n",
                    gm::save_name_text_y(0), gm::save_name_text_y(3),
                    differing_pixels(moved.rgba, fa.rgba));
    }
}

// --- the FILESLOT bar --------------------------------------------------------
void test_fileslot(const fs::path& game) {
    const std::string tag = "fileslot: ";
    const BandAsset bar = load_band_asset(game, std::string(gm::kSaveNameFileslotName));
    check(bar.ok, tag + "FILESLOT decodes from FRONTEND/BMP.IDX");
    if (!bar.ok) {
        return;
    }
    check_eq(bar.width, gm::kSaveNameFileslotWidth, tag + "FILESLOT is 124 wide");
    check_eq(bar.height, gm::kSaveNameFileslotHeight, tag + "FILESLOT is 10 high");

    gm::SaveNameRenderOptions with;
    with.name = "GUNMETAL";
    gm::SaveNameRenderOptions without = with;
    without.fileslot = false;
    const Frame frame_with = run(game, with);
    const Frame frame_without = run(game, without);
    check(frame_with.ok && frame_without.ok, tag + "bar and text-only frames render");
    if (!frame_with.ok || !frame_without.ok) {
        return;
    }
    check_eq(frame_with.stats.fileslot_bands, 1, tag + "default: one bar");
    check_eq(frame_without.stats.fileslot_bands, 0, tag + "(--)fileslot: no bar");
    check_eq(frame_with.stats.ink_pixels, frame_without.stats.ink_pixels,
             tag + "the bar leaves the ink unchanged");

    // Screen row r of the bar is the entry's *file* row r (`0x47efe0` stores the
    // rows as they are, `0x484cc0` copies them forward).
    const int band_y = gm::save_name_band_y(with.row);
    long long bar_pixels = 0;
    long long wrong_bar = 0;
    long long wrong_glyph = 0;
    long long outside = 0;
    for (int r = 0; r < gm::kSaveNameFileslotHeight; ++r) {
        for (int c = 0; c < gm::kSaveNameFileslotWidth; ++c) {
            const int x = gm::kSaveNameFileslotX + c;
            const int y = band_y + r;
            const std::uint32_t want = bar.colour(c, r);
            const bool glyph = !is_black(at(frame_without, x, y));
            if (glyph) {
                wrong_glyph += at(frame_with, x, y) == at(frame_without, x, y) ? 0 : 1;
            } else {
                ++bar_pixels;
                wrong_bar += at(frame_with, x, y) == want ? 0 : 1;
            }
        }
    }
    for (int y = 0; y < frame_with.height; ++y) {
        for (int x = 0; x < frame_with.width; ++x) {
            const bool inside = x >= gm::kSaveNameFileslotX &&
                                x < gm::kSaveNameFileslotX + gm::kSaveNameFileslotWidth &&
                                y >= band_y &&
                                y < band_y + gm::kSaveNameFileslotHeight;
            if (inside) {
                continue;
            }
            outside += at(frame_with, x, y) == at(frame_without, x, y) ? 0 : 1;
        }
    }
    check_eq(wrong_bar, 0, tag + "bar pixels match the independent file-order decode");
    check_eq(wrong_glyph, 0, tag + "the name lies on top of the bar");
    check_eq(outside, 0, tag + "(--)fileslot changes only the 124x10 bar");
    const long long band_diff = differing_pixels(frame_with.rgba, frame_without.rgba);
    check(band_diff > 0, tag + "the bar changes the frame");
    std::printf("  save_name fileslot: 124x10 at (38,%d), %lld bar px under the glyphs, "
                "with/without diff=%lld px, ink %d unchanged\n",
                band_y, bar_pixels, band_diff, frame_with.stats.ink_pixels);
}

// --- plate -------------------------------------------------------------------
void test_plate(const fs::path& game) {
    const std::string tag = "plate: ";
    const Asset plate = load_asset(game, "PLATE");
    check(plate.ok, tag + "PLATE loaded");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kSaveNamePanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kSaveNamePanelHeight, tag + "PLATE 131 high");

    gm::SaveNameRenderOptions black;
    black.name = "TEST 1";
    black.row = 2;
    gm::SaveNameRenderOptions plated = black;
    plated.background = std::string(gm::kSaveNamePanelName);
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
            // Glyph pixels and the FILESLOT bar sit on top of the panel:
            // compare only the pixels the black frame left black.
            if (!is_black(at(black_frame, x, y))) {
                continue;
            }
            wrong += at(plate_frame, x, y) == want ? 0 : 1;
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
    std::printf("  save_name plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "text ink %d unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kSaveNameCanvasWidth) *
                                  gm::kSaveNameCanvasHeight;

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
        gm::SaveNameRenderOptions options;
        options.background = std::string(gm::kSaveNamePanelName);
        options.name = "FADE 12";
        options.row = 2;
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
                if (x < gm::kSaveNamePanelWidth && y < gm::kSaveNamePanelHeight) {
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

    // Determinism of the faded frame.
    const Frame again = make(true, 13);
    check(again.ok && differing_pixels(again.rgba, exact[1].rgba) == 0,
          tag + "level 13 is deterministic");
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::SaveNameRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::SaveNameRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    // Raw entries of the shipped archive, to build small synthetic archives.
    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> bar = installed_entry(game, "FILESLOT");
    const std::optional<std::vector<std::uint8_t>> small = installed_entry(game, "CHECK");
    check(fonts4.has_value() && plate.has_value() && bar.has_value() && small.has_value(),
          "negatives: FONTS4/PLATE/FILESLOT/CHECK bytes available");
    if (!fonts4 || !plate || !bar || !small) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. FONTS4 + FILESLOT: PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4", "FILESLOT"},
                        {*fonts4, *bar}),
          "negatives: no-PLATE archive written");
    check_refused(root / "game-without-plate", gm::SaveNameRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. PLATE + FILESLOT: the FONTS4 faces are missing.
    check(write_archive(root / "game-without-font4", {"PLATE", "FILESLOT"},
                        {*plate, *bar}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::SaveNameRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 3. A PLATE of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-plate", {"FONTS4", "PLATE", "FILESLOT"},
                        {*fonts4, *small, *bar}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::SaveNameRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 4. FONTS4 + PLATE: the FILESLOT bar is missing while fileslot is on.
    check(write_archive(root / "game-without-bar", {"FONTS4", "PLATE"}, {*fonts4, *plate}),
          "negatives: no-FILESLOT archive written");
    check_refused(root / "game-without-bar", gm::SaveNameRenderOptions{},
                  "archive without FILESLOT", "FILESLOT");
    {
        gm::SaveNameRenderOptions text_only;
        text_only.fileslot = false;
        const Frame frame = run(root / "game-without-bar", text_only);
        check(frame.ok, "negatives: (--)fileslot on the bar-less archive renders: " +
                            frame.error);
        if (frame.ok) {
            check_eq(frame.stats.fileslot_bands, 0,
                     "negatives: bar-less archive paints no bar");
            check(frame.stats.ink_pixels > 0, "negatives: bar-less archive has ink");
            std::printf("  save_name minimal archive (FONTS4+PLATE, no bar): rows=%d "
                        "chars=%d ink=%d missing=%d\n",
                        frame.stats.rows, frame.stats.chars, frame.stats.ink_pixels,
                        frame.stats.missing_glyphs);
        }
    }

    // 5. A FILESLOT of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-bar", {"FONTS4", "PLATE", "FILESLOT"},
                        {*fonts4, *plate, *small}),
          "negatives: wrong-size FILESLOT archive written");
    check_refused(root / "game-wrong-bar", gm::SaveNameRenderOptions{},
                  "8x16 FILESLOT", "expected 124x10");

    // 6. The minimal complete archive renders (no CHECK/NOCHECK, SLIDE/KNOB or
    // MENUARRW is needed by this screen).
    check(write_archive(root / "game-minimal", {"FONTS4", "PLATE", "FILESLOT"},
                        {*fonts4, *plate, *bar}),
          "negatives: minimal archive written");
    {
        gm::SaveNameRenderOptions options;
        options.name = "MIN";
        const Frame minimal = run(root / "game-minimal", options);
        check(minimal.ok, "negatives: minimal archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, 1, "negatives: minimal archive rows 1");
            check_eq(minimal.stats.chars, 4, "negatives: minimal archive chars 4");
            check_eq(minimal.stats.fileslot_bands, 1, "negatives: minimal archive one bar");
            check_eq(minimal.stats.arrows, 0, "negatives: minimal archive arrows 0");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  save_name minimal archive (FONTS4+PLATE+FILESLOT): rows=%d "
                        "chars=%d ink=%d missing=%d bars=%d\n",
                        minimal.stats.rows, minimal.stats.chars,
                        minimal.stats.ink_pixels, minimal.stats.missing_glyphs,
                        minimal.stats.fileslot_bands);
        }
    }
    fs::remove_all(root, ec);

    // 7. A background name that is not in the archive is a text, not black.
    gm::SaveNameRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 8. The exact fade needs a table or a dump.
    gm::SaveNameRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::SaveNameRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 9. The shipped install renders the API default and is deterministic.
    const Frame first = run(game, gm::SaveNameRenderOptions{});
    const Frame second = run(game, gm::SaveNameRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  save_name negatives: rows=%d chars=%d truncated=%d ink=%d "
                    "arrows=%d bars=%d missing=%d\n",
                    first.stats.rows, first.stats.chars, first.stats.truncated,
                    first.stats.ink_pixels, first.stats.arrows,
                    first.stats.fileslot_bands, first.stats.missing_glyphs);
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
        std::cout << "save_name_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kSaveNameTitleFont, gm::kSaveNameRowFont,
                             gm::kSaveNameInputFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "save_name_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_text(game, fonts);
    test_name(game, fonts);
    test_fileslot(game);
    test_plate(game);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "save_name_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "save_name_render: ok (" << checks << " checks)\n";
    return 0;
}
