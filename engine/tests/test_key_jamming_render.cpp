// One frame of screen 16, `KEY JAMMING` (`M8.8` slice 11,
// `ui/key_jamming_render`, docs/KEY_JAMMING_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.8
// from the handler `0x488e90` and from pixels:
//
//   * model: `rows` 1 (`0x67f10c`), step 0 (`0x662d58`), title `KEY JAMMING`
//     (font 5) centred at y 14, the hints `Hold down keys to check` (y 26),
//     `if they jam.` (y 35) and `Press Esc to close.` (y 44) in font 0, the
//     4-column key grid (x 30/64/98/132, y 54 + 8*row, 28 slots), the clamp of
//     `0x488f1b` and the 123-entry key-name table `0x4a5b88`;
//   * the shipped install: 1 row, 0 keys on the default frame, no missing
//     glyph, 320x200 RGBA, every pixel opaque; the title and the three hint
//     bands are compared byte for byte with an independent blit and the
//     bounding boxes are printed;
//   * the key readout: codes 17/30/31/57/109 give the names `W`, `A`, `S`,
//     `SPACE`, `UP` in ascending code order at their grid slots, every band
//     matches an independent blit, the order of the input vector does not
//     matter, duplicates collapse, codes outside 0..122 are ignored and the
//     28-name cap of `0x488f1b` is exact (30 codes -> 28 names);
//   * the inherited cursor `[0x68ccf4]`: 4 (entry) and every non-zero value
//     paint the names in font 0, `0` repaints all of them with font 4 (white)
//     through `0x485e15`/`0x485e19`, changing nothing outside the key grid;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `PLATE`, one without `FONTS4`, a `PLATE` of the wrong size,
//     an unknown background entry, `fade_lut` without a table and a dump, and a
//     missing dump file — all `key_jamming_render:` texts with a cleared
//     buffer; an archive with only `FONTS4`+`PLATE` still renders (the screen
//     needs no `CHECK`/`NOCHECK`/`SLIDE`/`KNOB`/`MENUARRW`); two renders are
//     deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/KEY_JAMMING_RENDER.md §5 pins them.

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

#include "input/controls.h"
#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "ui/key_jamming_render.h"

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
    gm::KeyJammingRenderStats stats;
};

Frame run(const fs::path& game, const gm::KeyJammingRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_key_jamming(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kKeyJammingBackground; }

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
/// be the black background.  The x range keeps the comparison inside the column
/// of the slot (the key grid puts four names on one row).
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what, int x_from = 0,
                int x_to = gm::kKeyJammingPanelWidth) {
    const int width = gm::kKeyJammingPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kKeyJammingBackground : want;
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
                    int x_from = 0, int x_to = gm::kKeyJammingPanelWidth) {
    check_band(frame, atlas, text, x, y, what, x_from, x_to);
    const int width = gm::kKeyJammingPanelWidth;
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
        std::printf("  key_jamming \"%s\": x=%3d y=%3d ink=%lld bbox %s\n",
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

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_key_jamming_render";
    }
    return fs::temp_directory_path() / "gm_key_jamming_render";
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
void check_refused(const fs::path& game, const gm::KeyJammingRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::KeyJammingRenderStats stats;
    stats.rows = 7;
    stats.keys = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_key_jamming(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "key_jamming_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.keys, 0, what + ": keys cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // Geometry of 0x488e90 (rows 1, step 0, title row 14, hints 26/35/44).
    check_eq(gm::kKeyJammingRows, 1, tag + "rows = 1 (0x67f10c at 0x488e93)");
    check_eq(gm::kKeyJammingItemStep, 0, tag + "step = 0 (0x662d58 at 0x488e9d)");
    check_eq(gm::kKeyJammingTitleY, 14, tag + "title y 14 (push 0xe at 0x488eae)");
    check_eq(gm::kKeyJammingTitleX, 38, tag + "title x 38 fallback (push 0x26)");
    check_eq(gm::kKeyJammingHintHoldY, 26, tag + "hint 1 y 26 (push 0x1a)");
    check_eq(gm::kKeyJammingHintJamY, 35, tag + "hint 2 y 35 (push 0x23)");
    check_eq(gm::kKeyJammingHintEscY, 44, tag + "hint 3 y 44 (push 0x2c)");
    check_eq(gm::kKeyJammingHintX, 38, tag + "hints x 38 fallback (ignored by centred)");
    check_eq(gm::kKeyJammingTitleFont, 5, tag + "title font 5");
    check_eq(gm::kKeyJammingRowFont, 0, tag + "hint/key font 0");
    check_eq(gm::kKeyJammingSelectedFont, 4, tag + "highlight font 4 (0x485e19)");
    check_eq(std::string(gm::kKeyJammingTitle), std::string("KEY JAMMING"),
             tag + "title 0x4bd640");
    check_eq(std::string(gm::kKeyJammingHintHold),
             std::string("Hold down keys to check"), tag + "hint 1 = 0x4bd628");
    check_eq(std::string(gm::kKeyJammingHintJam), std::string("if they jam."),
             tag + "hint 2 = 0x4bd618");
    check_eq(std::string(gm::kKeyJammingHintEsc), std::string("Press Esc to close."),
             tag + "hint 3 = 0x4bd604");
    check_eq(std::string(gm::kKeyJammingPanelName), std::string("PLATE"), tag + "PLATE");
    check_eq(gm::kKeyJammingPanelWidth, 196, tag + "PLATE 196 wide");
    check_eq(gm::kKeyJammingPanelHeight, 131, tag + "PLATE 131 high");
    check_eq(gm::kKeyJammingKeyNameCount, 123, tag + "123 key codes (0x4a5b88..0x4a5e6a / 6)");

    // The key grid of 0x488f20..0x488f4e: x = 30 + 34*(n%4), y = 54 + 8*(n/4).
    check_eq(gm::kKeyJammingKeyColumns, 4, tag + "4 columns (esi%4)");
    check_eq(gm::kKeyJammingMaxKeys, 28, tag + "28 names (cmp esi,0x1c at 0x488f1b)");
    const std::array<int, 4> want_x = {30, 64, 98, 132};
    for (int slot = 0; slot < 4; ++slot) {
        check_eq(gm::key_jamming_key_x(slot), want_x[static_cast<std::size_t>(slot)],
                 tag + "x of slot " + std::to_string(slot));
    }
    const std::array<int, 7> want_y = {54, 62, 70, 78, 86, 94, 102};
    for (int row = 0; row < 7; ++row) {
        check_eq(gm::key_jamming_key_y(row * gm::kKeyJammingKeyColumns),
                 want_y[static_cast<std::size_t>(row)],
                 tag + "y of row " + std::to_string(row));
    }
    check_eq(gm::key_jamming_key_x(gm::kKeyJammingMaxKeys - 1), 132,
             tag + "x of the last slot 27");
    check_eq(gm::key_jamming_key_y(gm::kKeyJammingMaxKeys - 1), 102,
             tag + "y of the last slot 27");
    check_eq(gm::key_jamming_key_x(99), 132, tag + "x clamp high");
    check_eq(gm::key_jamming_key_x(-5), 30, tag + "x clamp low");
    check_eq(gm::key_jamming_key_y(99), 102, tag + "y clamp high");
    check_eq(gm::key_jamming_key_y(-1), 54, tag + "y clamp low");

    // The cursor quirk of 0x485dd0: only 0 selects font 4 for the `item = 0`
    // key-name calls.
    check_eq(gm::kKeyJammingEntryCursor, 4, tag + "entry cursor 4 (0x48d1ce keeps it)");
    check_eq(gm::kKeyJammingDefaultCursor, 4, tag + "API default cursor 4");
    check_eq(gm::key_jamming_key_font(4), 0, tag + "cursor 4 -> key font 0");
    check_eq(gm::key_jamming_key_font(0), 4, tag + "cursor 0 -> key font 4");
    check_eq(gm::key_jamming_key_font(-1), 0, tag + "cursor -1 -> key font 0");
    check_eq(gm::key_jamming_key_font(99), 0, tag + "cursor 99 -> key font 0");

    // The state array scan: ascending, de-duplicated, 0..122 only, 28 max.
    const auto keys = [](std::vector<int> codes) { return gm::key_jamming_keys(codes); };
    check(keys({57, 17, 31}) == std::vector<int>({17, 31, 57}),
          tag + "codes are sorted ascending (0x488f11 loop order)");
    check(keys({17, 17, 17}) == std::vector<int>({17}), tag + "duplicates collapse");
    check(keys({-1, 17, 999}) == std::vector<int>({17}),
          tag + "codes outside 0..122 are ignored");
    check(keys({0}) == std::vector<int>({0}), tag + "code 0 = NONE is a legal state byte");
    std::vector<int> many;
    for (int code = 0; code < 30; ++code) {
        many.push_back(code);
    }
    check_eq(static_cast<long long>(keys(many).size()), 28, tag + "30 codes -> 28 names");
    check_eq(keys(many).back(), 27, tag + "the cap keeps the first 28 ascending codes");

    // The name table is the game's own 123-entry table (0x4a5b88).
    check_eq(gm::Controls::key_name(17), std::string("W"), tag + "code 17 = W");
    check_eq(gm::Controls::key_name(30), std::string("A"), tag + "code 30 = A");
    check_eq(gm::Controls::key_name(57), std::string("SPACE"), tag + "code 57 = SPACE");
    check_eq(gm::Controls::key_name(109), std::string("UP"), tag + "code 109 = UP");
    check_eq(gm::Controls::key_name(0), std::string("NONE"), tag + "code 0 = NONE");
}

// --- the four centred lines ---------------------------------------------------
void test_text(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "text: ";
    const Frame frame = run(game, gm::KeyJammingRenderOptions{});
    check(frame.ok, tag + "default frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kKeyJammingCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kKeyJammingCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    check_eq(frame.stats.rows, gm::kKeyJammingRows, tag + "stats rows 1");
    check_eq(frame.stats.keys, 0, tag + "stats keys 0 (no key held)");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0 (no MENUARRW at 0x488e90)");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");
    check(frame.stats.ink_pixels > 0, tag + "ink > 0");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel is opaque");

    const gm::FontAtlas& title_face = fonts.at(gm::kKeyJammingTitleFont);
    const gm::FontAtlas& row_face = fonts.at(gm::kKeyJammingRowFont);
    const int title_x =
        std::max(0, (gm::kKeyJammingPanelWidth - pen_width(title_face, gm::kKeyJammingTitle)) /
                        2);
    const int hold_x = std::max(
        0, (gm::kKeyJammingPanelWidth - pen_width(row_face, gm::kKeyJammingHintHold)) / 2);
    const int jam_x = std::max(
        0, (gm::kKeyJammingPanelWidth - pen_width(row_face, gm::kKeyJammingHintJam)) / 2);
    const int esc_x = std::max(
        0, (gm::kKeyJammingPanelWidth - pen_width(row_face, gm::kKeyJammingHintEsc)) / 2);
    const long long title_ink =
        check_run(frame, title_face, gm::kKeyJammingTitle, title_x, gm::kKeyJammingTitleY,
                  tag + "title", true);
    const long long hold_ink =
        check_run(frame, row_face, gm::kKeyJammingHintHold, hold_x,
                  gm::kKeyJammingHintHoldY, tag + "hint 1", true);
    const long long jam_ink = check_run(frame, row_face, gm::kKeyJammingHintJam, jam_x,
                                        gm::kKeyJammingHintJamY, tag + "hint 2", true);
    const long long esc_ink = check_run(frame, row_face, gm::kKeyJammingHintEsc, esc_x,
                                        gm::kKeyJammingHintEscY, tag + "hint 3", true);
    const long long text_ink = title_ink + hold_ink + jam_ink + esc_ink;
    check_eq(frame.stats.ink_pixels, text_ink, tag + "ink = title + three hint bands");
    // The title is dark red (155,15,15), the hints dark grey (19,15,15).
    check(has_colour(frame, gm::kKeyJammingTitleY,
                     gm::kKeyJammingTitleY + title_face.cell_height - 1, 0x9B0F0FFF),
          tag + "title painted with the font-5 palette");
    check(has_colour(frame, gm::kKeyJammingHintHoldY,
                     gm::kKeyJammingHintHoldY + row_face.cell_height - 1, 0x130F0FFF),
          tag + "hints painted with the font-0 palette");
    std::printf("  key_jamming frame: rows=%d keys=%d ink=%d (text %lld) missing=%d "
                "arrows=%d\n",
                frame.stats.rows, frame.stats.keys, frame.stats.ink_pixels, text_ink,
                frame.stats.missing_glyphs, frame.stats.arrows);
    std::printf("  key_jamming centred x: title %d, hints %d/%d/%d (band 196)\n", title_x,
                hold_x, jam_x, esc_x);
}

// --- the key readout ----------------------------------------------------------
void test_keys(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "keys: ";
    const gm::FontAtlas& row_face = fonts.at(gm::kKeyJammingRowFont);

    gm::KeyJammingRenderOptions options;
    options.pressed_keys = {109, 17, 30, 57, 31}; // deliberately unsorted
    const Frame frame = run(game, options);
    check(frame.ok, tag + "5 keys: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.keys, 5, tag + "stats keys 5");
    check_eq(frame.stats.rows, 1, tag + "stats rows 1");
    check_eq(frame.stats.arrows, 0, tag + "stats arrows 0");
    check_eq(frame.stats.missing_glyphs, 0, tag + "stats missing 0");

    const std::vector<int> want_codes = {17, 30, 31, 57, 109};
    const std::array<int, 5> want_xy_x = {30, 64, 98, 132, 30};
    const std::array<int, 5> want_xy_y = {54, 54, 54, 54, 62};
    long long ink_total = 0;
    for (std::size_t slot = 0; slot < want_codes.size(); ++slot) {
        const int code = want_codes[slot];
        const std::string name = gm::Controls::key_name(code);
        check_eq(gm::key_jamming_key_x(static_cast<int>(slot)),
                 want_xy_x[slot], tag + "x of slot " + std::to_string(slot));
        check_eq(gm::key_jamming_key_y(static_cast<int>(slot)),
                 want_xy_y[slot], tag + "y of slot " + std::to_string(slot));
        const int y = gm::key_jamming_key_y(static_cast<int>(slot));
        // One row holds four names, so the band is compared inside the slot's
        // own 34-px column (the glyphs are narrower than the pitch).
        const int x_to = std::min(gm::kKeyJammingPanelWidth,
                                  want_xy_x[slot] + gm::kKeyJammingKeyColumnPitch);
        check_band(frame, row_face, name, want_xy_x[slot], y,
                   tag + "name " + std::to_string(code) + " \"" + name + "\"",
                   want_xy_x[slot], x_to);
        const Box got = ink_box(frame, y, y + row_face.cell_height - 1, x_to,
                                want_xy_x[slot]);
        const long long ink = band_ink(compose_band(row_face, name, want_xy_x[slot],
                                                    gm::kKeyJammingPanelWidth,
                                                    row_face.cell_height));
        ink_total += ink;
        std::printf("  key_jamming key %3d \"%s\": x=%3d y=%3d ink=%lld bbox %s\n", code,
                    name.c_str(), want_xy_x[slot], y, ink, box_text(got).c_str());
    }
    const auto centred_ink = [](const gm::FontAtlas& face, std::string_view text) {
        const int x =
            std::max(0, (gm::kKeyJammingPanelWidth - pen_width(face, text)) / 2);
        return band_ink(compose_band(face, text, x, gm::kKeyJammingPanelWidth,
                                     face.cell_height));
    };
    const long long text_ink = centred_ink(fonts.at(gm::kKeyJammingTitleFont),
                                           gm::kKeyJammingTitle) +
                               centred_ink(row_face, gm::kKeyJammingHintHold) +
                               centred_ink(row_face, gm::kKeyJammingHintJam) +
                               centred_ink(row_face, gm::kKeyJammingHintEsc);
    check_eq(frame.stats.ink_pixels, text_ink + ink_total,
             tag + "ink = four text bands + five key bands");
    std::printf("  key_jamming 5-key frame: ink=%d (text %lld + keys %lld)\n",
                frame.stats.ink_pixels, text_ink, ink_total);

    // The order of the input vector does not matter, one key changes the frame
    // and the empty frame is the text-only frame.
    gm::KeyJammingRenderOptions sorted;
    sorted.pressed_keys = want_codes;
    const Frame sorted_frame = run(game, sorted);
    check(sorted_frame.ok && differing_pixels(sorted_frame.rgba, frame.rgba) == 0,
          tag + "input order does not change the frame");
    gm::KeyJammingRenderOptions one;
    one.pressed_keys = {17};
    const Frame one_frame = run(game, one);
    const Frame empty = run(game, gm::KeyJammingRenderOptions{});
    check(one_frame.ok && empty.ok, tag + "1-key and 0-key frames render");
    if (one_frame.ok && empty.ok) {
        const long long diff_one = differing_pixels(one_frame.rgba, empty.rgba);
        check(diff_one > 0, tag + "one key changes the frame");
        check_eq(one_frame.stats.keys, 1, tag + "one key -> keys 1");
        check_eq(differing_outside(one_frame, empty, gm::kKeyJammingKeyY,
                                   gm::kKeyJammingKeyY + row_face.cell_height - 1),
                 0, tag + "one key changes only the first key row");
        std::printf("  key_jamming keys: 0 vs 1 key diff=%lld px (row y=%d..%d)\n", diff_one,
                    gm::kKeyJammingKeyY, gm::kKeyJammingKeyY + row_face.cell_height - 1);
    }

    // Duplicates and out-of-range codes change nothing.
    gm::KeyJammingRenderOptions dup;
    dup.pressed_keys = {17, 17, -1, 999, 5000};
    const Frame dup_frame = run(game, dup);
    check(dup_frame.ok && differing_pixels(dup_frame.rgba, one_frame.rgba) == 0,
          tag + "duplicates and out-of-range codes match the single-key frame");

    // The 28-name cap: 30 codes paint the 28 lowest, 29 codes the same frame.
    std::vector<int> many;
    for (int code = 0; code < 30; ++code) {
        many.push_back(code);
    }
    gm::KeyJammingRenderOptions cap30;
    cap30.pressed_keys = many;
    many.pop_back();
    gm::KeyJammingRenderOptions cap29;
    cap29.pressed_keys = many;
    const Frame f30 = run(game, cap30);
    const Frame f29 = run(game, cap29);
    check(f30.ok && f29.ok, tag + "28-name cap frames render");
    if (f30.ok && f29.ok) {
        check_eq(f30.stats.keys, 28, tag + "30 codes -> 28 names (0x488f1b)");
        check_eq(f29.stats.keys, 28, tag + "29 codes -> 28 names");
        check_eq(differing_pixels(f30.rgba, f29.rgba), 0,
                 tag + "the 29th and 30th code are not painted");
        std::printf("  key_jamming cap: 29/30 codes -> keys=%d, diff=%lld px, ink=%d\n",
                    f30.stats.keys, differing_pixels(f30.rgba, f29.rgba),
                    f30.stats.ink_pixels);
    }
}

// --- inherited cursor ---------------------------------------------------------
void test_cursor(const fs::path& game) {
    const std::string tag = "cursor: ";
    constexpr std::uint32_t kWhite = 0xEFEFEFFF; // FONTS4 face 4 (239,239,239)

    gm::KeyJammingRenderOptions four;
    four.pressed_keys = {17, 30, 31, 57, 109};
    const Frame entry = run(game, four);
    check(entry.ok, tag + "cursor 4 frame: " + entry.error);
    if (!entry.ok) {
        return;
    }
    check(!has_colour(entry, gm::kKeyJammingKeyY, gm::kKeyJammingKeyY + 55, kWhite),
          tag + "cursor 4 paints no white in the key grid");

    gm::KeyJammingRenderOptions zero = four;
    zero.cursor = 0;
    const Frame white = run(game, zero);
    check(white.ok, tag + "cursor 0 frame: " + white.error);
    if (!white.ok) {
        return;
    }
    check(has_colour(white, gm::kKeyJammingKeyY, gm::kKeyJammingKeyY + 55, kWhite),
          tag + "cursor 0 paints the key names with font 4 (0x485e19)");
    const long long diff = differing_pixels(white.rgba, entry.rgba);
    check(diff > 0, tag + "cursor 0 differs from cursor 4");
    check_eq(differing_outside(white, entry, gm::kKeyJammingKeyY,
                               gm::kKeyJammingKeyY + gm::kKeyJammingKeyRows *
                                                          gm::kKeyJammingKeyRowPitch),
             0, tag + "cursor 0 changes only the key grid rows");

    gm::KeyJammingRenderOptions other = four;
    other.cursor = 99;
    const Frame high = run(game, other);
    check(high.ok && differing_pixels(high.rgba, entry.rgba) == 0,
          tag + "cursor 99 paints like cursor 4");
    std::printf("  key_jamming cursor: 4 vs 0 diff=%lld px (key grid y=%d..%d), "
                "white=%s\n",
                diff, gm::kKeyJammingKeyY,
                gm::kKeyJammingKeyY + gm::kKeyJammingKeyRows * gm::kKeyJammingKeyRowPitch,
                has_colour(white, gm::kKeyJammingKeyY, gm::kKeyJammingKeyY + 55, kWhite)
                    ? "yes"
                    : "no");
}

// --- plate -------------------------------------------------------------------
void test_plate(const fs::path& game) {
    const std::string tag = "plate: ";
    const Asset plate = load_asset(game, "PLATE");
    check(plate.ok, tag + "PLATE loaded");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kKeyJammingPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kKeyJammingPanelHeight, tag + "PLATE 131 high");

    gm::KeyJammingRenderOptions black;
    black.pressed_keys = {17, 57};
    gm::KeyJammingRenderOptions plated = black;
    plated.background = std::string(gm::kKeyJammingPanelName);
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
            // Glyph pixels sit on top of the panel: compare only the pixels the
            // black frame left black.
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
    std::printf("  key_jamming plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "text ink %d unchanged\n",
                differing_pixels(black_frame.rgba, plate_frame.rgba),
                plate_frame.stats.ink_pixels);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kKeyJammingCanvasWidth) *
                                  gm::kKeyJammingCanvasHeight;

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
        gm::KeyJammingRenderOptions options;
        options.background = std::string(gm::kKeyJammingPanelName);
        options.pressed_keys = {17, 30, 31, 57, 109, 110};
        options.cursor = 0; // the white face too: every painted index is faded
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
                if (x < gm::kKeyJammingPanelWidth && y < gm::kKeyJammingPanelHeight) {
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
    check_refused(fs::path{}, gm::KeyJammingRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::KeyJammingRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    // Raw entries of the shipped archive, to build small synthetic archives.
    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> small = installed_entry(game, "CHECK");
    check(fonts4.has_value() && plate.has_value() && small.has_value(),
          "negatives: FONTS4/PLATE/CHECK bytes available");
    if (!fonts4 || !plate || !small) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. An index with FONTS4 only: the faces load, PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4"}, {*fonts4}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-plate", gm::KeyJammingRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. PLATE only: the FONTS4 faces are missing.
    check(write_archive(root / "game-without-font4", {"PLATE"}, {*plate}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::KeyJammingRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 3. A PLATE of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-plate", {"FONTS4", "PLATE"},
                        {*fonts4, *small}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::KeyJammingRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 4. FONTS4 + PLATE and nothing else: screen 16 has no CHECK/NOCHECK,
    // SLIDE/KNOB or MENUARRW call, so this minimal archive must render.
    check(write_archive(root / "game-minimal", {"FONTS4", "PLATE"}, {*fonts4, *plate}),
          "negatives: minimal archive written");
    {
        gm::KeyJammingRenderOptions options;
        options.pressed_keys = {17, 57};
        const Frame minimal = run(root / "game-minimal", options);
        check(minimal.ok, "negatives: minimal archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, 1, "negatives: minimal archive rows 1");
            check_eq(minimal.stats.keys, 2, "negatives: minimal archive keys 2");
            check_eq(minimal.stats.arrows, 0, "negatives: minimal archive arrows 0");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  key_jamming minimal archive (FONTS4+PLATE): rows=%d keys=%d "
                        "ink=%d missing=%d\n",
                        minimal.stats.rows, minimal.stats.keys,
                        minimal.stats.ink_pixels, minimal.stats.missing_glyphs);
        }
    }
    fs::remove_all(root, ec);

    // 5. A background name that is not in the archive is a text, not black.
    gm::KeyJammingRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 6. The exact fade needs a table or a dump.
    gm::KeyJammingRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::KeyJammingRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 7. The shipped install renders the API default and is deterministic.
    const Frame first = run(game, gm::KeyJammingRenderOptions{});
    const Frame second = run(game, gm::KeyJammingRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  key_jamming negatives: rows=%d keys=%d ink=%d arrows=%d "
                    "missing=%d\n",
                    first.stats.rows, first.stats.keys, first.stats.ink_pixels,
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
        std::cout << "key_jamming_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kKeyJammingTitleFont, gm::kKeyJammingRowFont,
                             gm::kKeyJammingSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "key_jamming_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_text(game, fonts);
    test_keys(game, fonts);
    test_cursor(game);
    test_plate(game);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "key_jamming_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "key_jamming_render: ok (" << checks << " checks)\n";
    return 0;
}
