// One frame of screen 36, the per-player joystick setup (`M8.8` slice 15,
// `ui/player_setup_render`, docs/PLAYER_SETUP_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.8
// from the handler `0x489d60` (slot `0x488454` -> stub `0x487f51`) and from
// pixels:
//
//   * model: `step` 9 (`0x662d58`), `[0x67f10c]` 1/33 + one per axis + two per
//     hat (`0x489d91`, `0x489f60`, `0x48a073`, `0x48a397`), the ten-band window
//     and the five-row scroll band (`0x489de0`..`0x489dec`), first row y 28 and
//     x 13, title y 11, hint y 20, the value column
//     `13 + width(font 0, "Front/Back: ")`, the two `MENUARRW` arrows at
//     (159,2)/(159,121), the 18 axis presets `0x4bc468`, the 31 action names
//     `0x4bc558`, the mask -> bit-position mapping `0x4863a0` and the shipped
//     `.data` button defaults `0x20`/`0x8000` (`0x4aa490`);
//   * the shipped install: the API default frame (no device: `Joystick Used:
//     None`, `rows` 1, no arrows), the full-device frame (six axes, two hats,
//     32 channels -> `rows` 43), every band compared byte for byte with an
//     independent blit, the bounding boxes and the inks printed;
//   * the values of the frame: axis presets, hat presets, button masks, the
//     device name, the player 1/2 title and the three live-input masks
//     (`axis_values`, `hat_bits`, `pressed_buttons`) each change only their own
//     band and the full-frame difference is printed;
//   * the cursor `[0x68ccf4]`: the highlight is the band whose content index
//     equals the cursor (font 4 through `0x485e19`), a pressed/deflected line
//     stays font 5, and the window top is `max(0, cursor - 5)`, so a cursor
//     beyond the fifth row scrolls and the arrows appear;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `PLATE`, one without `FONTS4`, a `PLATE` of the wrong size,
//     an index without `MENUARRW` for a device frame (the arrows are required),
//     an unknown background entry, `fade_lut` without a table and a dump, and a
//     missing dump file — all `player_setup_render:` texts with a cleared
//     buffer; an archive with only `FONTS4`+`PLATE` still renders the no-device
//     frame (no arrow is drawn); two renders are deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/PLAYER_SETUP_RENDER.md §4 pins them.

#include <algorithm>
#include <array>
#include <cmath>
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
#include "ui/player_setup_render.h"

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
    gm::PlayerSetupRenderStats stats;
};

Frame run(const fs::path& game, const gm::PlayerSetupRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_player_setup(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kPlayerSetupBackground; }

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

/// Differing pixels inside the rows `y0..y1` (inclusive) and the columns
/// `x0..x1` (inclusive).
long long differing_inside(const Frame& a, const Frame& b, int x0, int y0, int x1,
                           int y1) {
    long long count = 0;
    for (int y = std::max(0, y0); y <= std::min(a.height - 1, y1); ++y) {
        for (int x = std::max(0, x0); x <= std::min(a.width - 1, x1); ++x) {
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

/// Bounding box of the non-black pixels of the rows `y0..y1` and the columns
/// `x_min..x_limit-1`.
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
                int x_to = gm::kPlayerSetupPanelWidth) {
    const int width = gm::kPlayerSetupPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kPlayerSetupBackground : want;
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

/// `check_band` plus the bbox against the independent blit and the band ink.
long long check_run(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                    int x, int y, const std::string& what, bool print,
                    int x_from = 0, int x_to = gm::kPlayerSetupPanelWidth) {
    check_band(frame, atlas, text, x, y, what, x_from, x_to);
    const int width = gm::kPlayerSetupPanelWidth;
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
        std::printf("  player_setup \"%s\": x=%3d y=%3d ink=%lld bbox %s\n",
                    std::string(text).c_str(), x, y, ink, box_text(got).c_str());
    }
    return ink;
}

// --- assets, decoded independently -------------------------------------------

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
        return approved / "gm_player_setup_render";
    }
    return fs::temp_directory_path() / "gm_player_setup_render";
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
void check_refused(const fs::path& game, const gm::PlayerSetupRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::PlayerSetupRenderStats stats;
    stats.rows = 7;
    stats.sliders = 7;
    stats.checks = 7;
    stats.arrows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    std::string error;
    const bool ok =
        gm::render_player_setup(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "player_setup_render:"),
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
    check_eq(stats.arrows, 0, what + ": arrows cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
}

/// A device frame used by most pixel tests: player 1, device 0 with a name, all
/// six axes, both hats, the shipped button defaults.
gm::PlayerSetupRenderOptions device_options() {
    gm::PlayerSetupRenderOptions options;
    options.player = 0;
    options.joystick = 0;
    options.joystick_name = "SideWinder Precision Pro";
    options.axes = {1, 2, 3, 4, 5, 6};
    options.hats = {0, 8, 10, 0};
    return options;
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // Geometry of 0x489d60.
    check_eq(gm::kPlayerSetupItemStep, 9, tag + "step = 9 (0x662d58 at 0x489d7e)");
    check_eq(gm::kPlayerSetupFirstRowY, 28, tag + "first row y 28 (row -0x1c)");
    check_eq(gm::kPlayerSetupRowX, 13, tag + "row x 13 (push 0xd)");
    check_eq(gm::kPlayerSetupTitleY, 11, tag + "title y 11 (push 0xb at 0x489dae)");
    check_eq(gm::kPlayerSetupHintY, 20, tag + "hint y 20 (push 0x14 at 0x489dc9)");
    check_eq(gm::kPlayerSetupVisibleRows, 10, tag + "ten-band window (0x48a19b)");
    check_eq(gm::kPlayerSetupScrollBand, 5, tag + "scroll band 5 (lea esi,[eax-5])");
    check_eq(gm::kPlayerSetupRowsNoDevice, 1, tag + "rows without device = 1");
    check_eq(gm::kPlayerSetupRowsDevice, 33, tag + "rows with device = 0x21");
    check_eq(gm::kPlayerSetupDeflectionThreshold == 0.5, true, tag + "threshold 0.5 (0x4a2928)");
    check_eq(gm::kPlayerSetupButtonChannels, 32, tag + "32 button channels");
    check_eq(gm::kPlayerSetupNameMaxChars, 30, tag + "device name %.30s");
    check(gm::kPlayerSetupTitlePlayer1 == "PLAYER 1 JOYSTICK SETUP",
          tag + "title 1 (0x4bd9c0)");
    check(gm::kPlayerSetupTitlePlayer2 == "PLAYER 2 JOYSTICK SETUP",
          tag + "title 2 (0x4bd9a8)");
    check(gm::kPlayerSetupHint == "Esc Exit, Button To Highlight", tag + "hint 0x4bd988");
    check(gm::kPlayerSetupNoDevice == "Joystick Used: None", tag + "0x4bd974");
    check(gm::kPlayerSetupArrowName == "MENUARRW", tag + "arrow sprite name");
    check_eq(gm::kPlayerSetupArrowX, 159, tag + "arrow column 159");
    check_eq(gm::kPlayerSetupArrowUpY, 2, tag + "up arrow row 2");
    check_eq(gm::kPlayerSetupArrowDownY, 121, tag + "down arrow row 121");

    // Row arithmetic.
    check_eq(gm::player_setup_row_y(0), 28, tag + "row 0 -> y 28");
    check_eq(gm::player_setup_row_y(1), 37, tag + "row 1 -> y 37");
    check_eq(gm::player_setup_row_y(10), 118, tag + "row 10 -> y 118");
    check_eq(gm::player_setup_scroll(-1), 0, tag + "scroll(-1) = 0");
    check_eq(gm::player_setup_scroll(0), 0, tag + "scroll(0) = 0");
    check_eq(gm::player_setup_scroll(4), 0, tag + "scroll(4) = 0");
    check_eq(gm::player_setup_scroll(5), 0, tag + "scroll(5) = 0");
    check_eq(gm::player_setup_scroll(6), 1, tag + "scroll(6) = 1");
    check_eq(gm::player_setup_scroll(42), 37, tag + "scroll(42) = 37");

    // Presence bits and labels.
    check_eq(gm::kPlayerSetupAxisPresentBits[0], 0x2u, tag + "axis 0 mask 0x2");
    check_eq(gm::kPlayerSetupAxisPresentBits[1], 0x4u, tag + "axis 1 mask 0x4");
    check_eq(gm::kPlayerSetupAxisPresentBits[5], 0x40u, tag + "axis 5 mask 0x40");
    check_eq(gm::kPlayerSetupAllAxes, 0x7Eu, tag + "all axes mask 0x7e");
    check(gm::kPlayerSetupAxisLabels[0] == "Left/Right: ", tag + "0x4bd94c");
    check(gm::kPlayerSetupAxisLabels[1] == "Front/Back: ", tag + "0x4bd95c");
    check(gm::kPlayerSetupAxisLabels[2] == "Up/Down: ", tag + "0x4bd940");
    check(gm::kPlayerSetupAxisLabels[3] == "Angular X: ", tag + "0x4bd934");
    check(gm::kPlayerSetupAxisLabels[4] == "Angular Y: ", tag + "0x4bd928");
    check(gm::kPlayerSetupAxisLabels[5] == "Angular Z: ", tag + "0x4bd91c");
    check(gm::kPlayerSetupHatLabels[0] == "Hat 1 X: ", tag + "0x4bd910");
    check(gm::kPlayerSetupHatLabels[1] == "Hat 1 Y: ", tag + "0x4bd904");
    check(gm::kPlayerSetupHatLabels[2] == "Hat 2 X: ", tag + "0x4bd8f8");
    check(gm::kPlayerSetupHatLabels[3] == "Hat 2 Y: ", tag + "0x4bd8ec");
    check_eq(gm::kPlayerSetupValueMeasureLabel == "Front/Back: ", true,
             tag + "value measure label 0x4bd95c");

    // Preset and action tables.
    check(gm::player_setup_preset_name(0) == "Not Used", tag + "preset 0");
    check(gm::player_setup_preset_name(1) == "Forward/Back", tag + "preset 1");
    check(gm::player_setup_preset_name(17) == "Equip Cycle -/+", tag + "preset 17");
    check(gm::player_setup_preset_name(-3) == "Not Used", tag + "preset clamp low");
    check(gm::player_setup_preset_name(99) == "Equip Cycle -/+", tag + "preset clamp high");
    check(gm::player_setup_action_name(0) == "Move Forward", tag + "action 0");
    check(gm::player_setup_action_name(5) == "Fire", tag + "action 5");
    check(gm::player_setup_action_name(15) == "Activate", tag + "action 15");
    check(gm::player_setup_action_name(30) == "Linear Right", tag + "action 30");
    check(gm::player_setup_action_name(-1) == "Move Forward", tag + "action clamp low");
    check(gm::player_setup_action_name(31) == "Linear Right", tag + "action clamp high");

    // Buttons: 0x4863a0 mask -> bit position, -1 = None, non-single-bit = None.
    check_eq(gm::player_setup_button_text(0, 0x20), "Button 1: Fire",
             tag + "0x20 -> Button 1: Fire");
    check_eq(gm::player_setup_button_text(1, 0x8000), "Button 2: Activate",
             tag + "0x8000 -> Button 2: Activate");
    check_eq(gm::player_setup_button_text(2, -1), "Button 3: None",
             tag + "-1 -> Button 3: None");
    check_eq(gm::player_setup_button_text(31, 1 << 15), "Button 32: Activate",
             tag + "channel 31 numbering");
    check_eq(gm::player_setup_button_text(0, 0), "Button 1: None",
             tag + "mask 0 -> None");
    check_eq(gm::player_setup_button_text(0, 0x21), "Button 1: None",
             tag + "multi-bit mask -> None");
    check_eq(gm::player_setup_button_text(0, 0x80000000), "Button 1: None",
             tag + "bit 31 is past the 31-name table -> None");

    // Rows of the handler.
    gm::PlayerSetupRenderOptions no_device;
    check_eq(gm::player_setup_rows(no_device), 1, tag + "no device: rows 1");
    check_eq(gm::player_setup_content_rows(no_device), 1, tag + "no device: 1 line");
    gm::PlayerSetupRenderOptions full = device_options();
    check_eq(gm::player_setup_rows(full), 43, tag + "full device: rows 33+6+4 = 43");
    check_eq(gm::player_setup_content_rows(full), 43,
             tag + "full device: 1+6+4+32 = 43 lines");
    gm::PlayerSetupRenderOptions bare;
    bare.joystick = 3;
    bare.axis_present = 0;
    bare.hat1 = false;
    bare.hat2 = false;
    check_eq(gm::player_setup_rows(bare), 33, tag + "device without axes/hats: rows 33");
    check_eq(gm::player_setup_content_rows(bare), 33,
             tag + "device without axes/hats: 1+32 = 33 lines");
    bare.axis_present = 0x6;
    bare.hat1 = true;
    check_eq(gm::player_setup_rows(bare), 37, tag + "two axes + hat 1: 33+2+2 = 37");
    // Same numbers the handler leaves in [0x67f10c].
    std::printf("  player_setup rows: no device %d, full device %d, two axes + hat 1 %d\n",
                gm::player_setup_rows(no_device), gm::player_setup_rows(full),
                gm::player_setup_rows(bare));
}

// --- the shipped install -----------------------------------------------------
void test_default_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "default: ";
    const gm::PlayerSetupRenderOptions options;
    const Frame frame = run(game, options);
    check(frame.ok, tag + "frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kPlayerSetupCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kPlayerSetupCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel opaque");
    check_eq(frame.stats.rows, 1, tag + "rows 1 (0x489d91)");
    check_eq(frame.stats.sliders, 0, tag + "no SLIDE/KNOB: sliders 0");
    check_eq(frame.stats.checks, 0, tag + "no CHECK/NOCHECK: checks 0");
    check_eq(frame.stats.arrows, 0, tag + "no device: arrows 0");
    check_eq(frame.stats.missing_glyphs, 0, tag + "missing glyphs 0");

    // Title and hint are centred in the 196 px band; the single row is
    // `Joystick Used: None` at (13, 28).
    const gm::FontAtlas& title = fonts.at(gm::kPlayerSetupTitleFont);
    const gm::FontAtlas& body = fonts.at(gm::kPlayerSetupRowFont);
    const Box title_box = ink_box(frame, gm::kPlayerSetupTitleY,
                                  gm::kPlayerSetupTitleY + title.cell_height - 1);
    const int title_x =
        std::max(0, (gm::kPlayerSetupPanelWidth - title.measure(gm::kPlayerSetupTitlePlayer1)) / 2);
    const long long title_ink = check_run(frame, title, gm::kPlayerSetupTitlePlayer1,
                                          title_x, gm::kPlayerSetupTitleY, tag + "title",
                                          true);
    const int hint_x =
        std::max(0, (gm::kPlayerSetupPanelWidth - body.measure(gm::kPlayerSetupHint)) / 2);
    const long long hint_ink = check_run(frame, body, gm::kPlayerSetupHint, hint_x,
                                         gm::kPlayerSetupHintY, tag + "hint", true);
    const long long row_ink = check_run(frame, fonts.at(gm::kPlayerSetupSelectedFont),
                                        gm::kPlayerSetupNoDevice, gm::kPlayerSetupRowX,
                                        gm::kPlayerSetupFirstRowY, tag + "device row", true);
    check_eq(frame.stats.ink_pixels, title_ink + hint_ink + row_ink,
             tag + "ink = title + hint + row");
    check_eq(title_box.y0, gm::kPlayerSetupTitleY, tag + "title bbox y0");
    std::printf("  player_setup default: rows=%d sliders=%d checks=%d arrows=%d ink=%d "
                "missing=%d\n",
                frame.stats.rows, frame.stats.sliders, frame.stats.checks,
                frame.stats.arrows, frame.stats.ink_pixels, frame.stats.missing_glyphs);
}

void test_device_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "device: ";
    const gm::PlayerSetupRenderOptions options = device_options();
    const Frame frame = run(game, options);
    check(frame.ok, tag + "frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.rows, 43, tag + "rows 43 = 33 + 6 axes + 4 hat rows");
    check_eq(frame.stats.sliders, 0, tag + "sliders 0");
    check_eq(frame.stats.checks, 0, tag + "checks 0");
    check_eq(frame.stats.arrows, 1, tag + "one down arrow at window top 0");
    check_eq(frame.stats.missing_glyphs, 0, tag + "missing glyphs 0");

    const gm::FontAtlas& title = fonts.at(gm::kPlayerSetupTitleFont);
    const gm::FontAtlas& body = fonts.at(gm::kPlayerSetupRowFont);
    const int value_x = gm::player_setup_value_x(body);
    check_eq(value_x, 67, tag + "value column x 67 (13 + 54)");
    std::printf("  player_setup value column: x=%d (label \"%s\")\n", value_x,
                std::string(gm::kPlayerSetupValueMeasureLabel).c_str());

    // Title, hint and the device name row.
    const int title_x =
        std::max(0, (gm::kPlayerSetupPanelWidth - title.measure(gm::kPlayerSetupTitlePlayer1)) / 2);
    const int hint_x =
        std::max(0, (gm::kPlayerSetupPanelWidth - body.measure(gm::kPlayerSetupHint)) / 2);
    long long ink = check_run(frame, title, gm::kPlayerSetupTitlePlayer1, title_x,
                              gm::kPlayerSetupTitleY, tag + "title", true);
    ink += check_run(frame, body, gm::kPlayerSetupHint, hint_x, gm::kPlayerSetupHintY,
                     tag + "hint", true);
    ink += check_run(frame, fonts.at(gm::kPlayerSetupSelectedFont), options.joystick_name,
                     gm::kPlayerSetupRowX, gm::kPlayerSetupFirstRowY, tag + "device name",
                     true);

    // Six axis rows at content rows 1..6: label at x 13, value at the common
    // value column; the cursor is row 0, so no axis row is highlighted.
    int row = 1;
    for (int axis = 0; axis < 6; ++axis) {
        const int y = gm::player_setup_row_y(row);
        ink += check_run(frame, body,
                         gm::kPlayerSetupAxisLabels[static_cast<std::size_t>(axis)],
                         gm::kPlayerSetupRowX, y, tag + "axis " + std::to_string(axis) +
                                                    " label",
                         true, 0, value_x);
        ink += check_run(frame, body,
                         gm::player_setup_preset_name(options.axes[static_cast<std::size_t>(axis)]),
                         value_x, y, tag + "axis " + std::to_string(axis) + " value", true,
                         value_x, 196);
        ++row;
    }
    // Four hat rows at content rows 7..10; the Hat 2 Y value sits one column
    // further right (`0x48a136` adds 13 before that blit).
    for (int slot = 0; slot < 4; ++slot) {
        const int y = gm::player_setup_row_y(row);
        const int x = slot == 3 ? value_x + gm::kPlayerSetupRowX : value_x;
        ink += check_run(frame, body,
                         gm::kPlayerSetupHatLabels[static_cast<std::size_t>(slot)],
                         gm::kPlayerSetupRowX, y,
                         tag + "hat " + std::to_string(slot) + " label", true, 0, value_x);
        ink += check_run(frame, body,
                         gm::player_setup_preset_name(options.hats[static_cast<std::size_t>(slot)]),
                         x, y, tag + "hat " + std::to_string(slot) + " value", true, x,
                         slot == 3 ? gm::kPlayerSetupArrowX : 196);
        ++row;
    }
    // With the window top 0 and ten bands no button row fits (the first one is
    // content row 11): the loop stops at `row - 0 >= 10`.
    const Box below = ink_box(frame, gm::player_setup_row_y(11),
                              gm::kPlayerSetupPanelHeight - 1, gm::kPlayerSetupArrowX);
    check_eq(below.count, 0, tag + "no button row in the top window");
    check_eq(ink, frame.stats.ink_pixels - 0, tag + "ink = the 11 checked bands");
    std::printf("  player_setup device frame: rows=%d arrows=%d ink=%d lines=%d\n",
                frame.stats.rows, frame.stats.arrows, frame.stats.ink_pixels,
                gm::player_setup_content_rows(options));
}

void test_values(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "values: ";
    const gm::PlayerSetupRenderOptions base = device_options();
    const Frame plain = run(game, base);
    check(plain.ok, tag + "base frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    const gm::FontAtlas& body = fonts.at(gm::kPlayerSetupRowFont);
    const int value_x = gm::player_setup_value_x(body);

    // Axis preset text changes its own value band (row 1 = Front/Back).
    gm::PlayerSetupRenderOptions axis = base;
    axis.axes[1] = 8; // Look Up/Down
    const Frame axis_frame = run(game, axis);
    check(axis_frame.ok, tag + "axis frame: " + axis_frame.error);
    if (axis_frame.ok) {
        const int y = gm::player_setup_row_y(2); // content row 1 + axis 1
        const long long inside = differing_inside(plain, axis_frame, value_x, y, 195,
                                                  y + body.cell_height - 1);
        const long long outside =
            differing_outside(plain, axis_frame, y, y + body.cell_height - 1);
        check(inside > 0, tag + "axis preset changes its value band");
        check_eq(outside, 0, tag + "axis preset changes nothing outside its band");
        check_eq(differing_pixels(plain.rgba, axis_frame.rgba), inside,
                 tag + "axis diff equals the band diff");
        std::printf("  player_setup axis 1 preset 2 -> 8: %lld px differ\n", inside);
    }

    // Hat preset text changes its own value band (row 9 = Hat 2 X).
    gm::PlayerSetupRenderOptions hat = base;
    hat.hats[2] = 4; // Linear Turn L/R
    const Frame hat_frame = run(game, hat);
    check(hat_frame.ok, tag + "hat frame: " + hat_frame.error);
    if (hat_frame.ok) {
        const int y = gm::player_setup_row_y(9); // content row 7 + slot 2
        const long long inside = differing_inside(plain, hat_frame, value_x, y, 195,
                                                  y + body.cell_height - 1);
        check(inside > 0, tag + "hat preset changes its value band");
        check_eq(differing_outside(plain, hat_frame, y, y + body.cell_height - 1), 0,
                 tag + "hat preset changes nothing outside its band");
        std::printf("  player_setup hat 2 X preset 10 -> 4: %lld px differ\n", inside);
    }

    // Device name: an empty field draws nothing on row 0.
    gm::PlayerSetupRenderOptions unnamed = base;
    unnamed.joystick_name.clear();
    const Frame unnamed_frame = run(game, unnamed);
    check(unnamed_frame.ok, tag + "unnamed frame: " + unnamed_frame.error);
    if (unnamed_frame.ok) {
        const Box row0 = ink_box(unnamed_frame, 28, 28 + body.cell_height - 1, value_x);
        check_eq(row0.count, 0, tag + "empty device name leaves row 0 blank");
        check_eq(differing_outside(unnamed_frame, plain, 28, 28 + body.cell_height - 1), 0,
                 tag + "device name only changes row 0");
    }

    // The name is capped at 30 bytes ("%.30s").
    gm::PlayerSetupRenderOptions long_name = base;
    long_name.cursor = -1; // the name row must not be the highlighted one
    long_name.joystick_name = "0123456789abcdefghijklmnopqrstuvwxyz";
    const Frame long_frame = run(game, long_name);
    check(long_frame.ok, tag + "long name frame: " + long_frame.error);
    if (long_frame.ok) {
        const std::string expected = long_name.joystick_name.substr(0, 30);
        check_run(long_frame, body, expected, gm::kPlayerSetupRowX,
                  gm::kPlayerSetupFirstRowY, tag + "30-byte name", false, 0, value_x + 100);
    }

    // Player 2 title.
    gm::PlayerSetupRenderOptions player2 = base;
    player2.player = 1;
    const Frame player2_frame = run(game, player2);
    check(player2_frame.ok, tag + "player 2 frame: " + player2_frame.error);
    if (player2_frame.ok) {
        check_eq(differing_outside(player2_frame, plain, gm::kPlayerSetupTitleY,
                                   gm::kPlayerSetupTitleY + 7),
                 0, tag + "player 2 changes only the title band");
        check(differing_pixels(player2_frame.rgba, plain.rgba) > 0,
              tag + "player 2 title differs");
    }

    // Live axis deflection: |v| >= 0.5 paints the label font 5 (dark red).
    gm::PlayerSetupRenderOptions passing = base;
    passing.axis_values[0] = 0.6;
    const Frame passing_frame = run(game, passing);
    check(passing_frame.ok, tag + "deflected frame: " + passing_frame.error);
    if (passing_frame.ok) {
        const int y = gm::player_setup_row_y(1);
        const long long inside = differing_inside(plain, passing_frame, 0, y, value_x - 1,
                                                  y + body.cell_height - 1);
        check(inside > 0, tag + "deflected axis label turns red");
        check_eq(differing_outside(plain, passing_frame, y, y + body.cell_height - 1), 0,
                 tag + "deflected axis label changes only its band");
        const std::uint32_t red =
            (static_cast<std::uint32_t>(155) << 24) | (15u << 16) | (15u << 8) | 0xFFu;
        check(has_colour(passing_frame, y, y + body.cell_height - 1, red),
              tag + "deflected label contains the font-5 red");
        check(!has_colour(plain, y, y + body.cell_height - 1, red),
              tag + "neutral label has no font-5 red");
        std::printf("  player_setup axis 0 value 0.6: label band %lld px differ\n", inside);
    }
    gm::PlayerSetupRenderOptions below = base;
    below.axis_values[0] = 0.49;
    const Frame below_frame = run(game, below);
    check(below_frame.ok && differing_pixels(plain.rgba, below_frame.rgba) == 0,
          tag + "0.49 is below the 0.5 threshold");

    // Live hat bits: 0x2 paints the Hat 1 X label font 5.
    gm::PlayerSetupRenderOptions hats = base;
    hats.hat_bits = 0x2;
    const Frame hats_frame = run(game, hats);
    check(hats_frame.ok, tag + "hat bits frame: " + hats_frame.error);
    if (hats_frame.ok) {
        const int y = gm::player_setup_row_y(7);
        check(differing_inside(plain, hats_frame, 0, y, value_x - 1,
                               y + body.cell_height - 1) > 0,
              tag + "hat bit 0x2 turns the Hat 1 X label red");
        check_eq(differing_outside(plain, hats_frame, y, y + body.cell_height - 1), 0,
                 tag + "hat bit 0x2 changes only its band");
        std::printf("  player_setup hat bits 0x2: %lld px differ\n",
                    differing_pixels(plain.rgba, hats_frame.rgba));
    }

    // Live pressed buttons: bit 5 paints Button 1 font 5 (row 11, not visible
    // at window top 0) — use a cursor that scrolls the button rows in.
    gm::PlayerSetupRenderOptions buttons = base;
    buttons.cursor = 11;
    const Frame buttons_plain = run(game, buttons);
    check(buttons_plain.ok, tag + "button window frame: " + buttons_plain.error);
    if (buttons_plain.ok) {
        gm::PlayerSetupRenderOptions pressed = buttons;
        pressed.pressed_buttons = 0x1; // channel 0
        const Frame pressed_frame = run(game, pressed);
        check(pressed_frame.ok, tag + "pressed button frame: " + pressed_frame.error);
        if (pressed_frame.ok) {
            const int y = gm::player_setup_row_y(11 - 6); // content row 11 at top 6
            const long long inside = differing_inside(buttons_plain, pressed_frame, 0, y, 195,
                                                      y + body.cell_height - 1);
            check(inside > 0, tag + "pressed button line turns red");
            check_eq(differing_outside(buttons_plain, pressed_frame, y,
                                       y + body.cell_height - 1),
                     0, tag + "pressed button changes only its band");
            const std::uint32_t red =
                (static_cast<std::uint32_t>(155) << 24) | (15u << 16) | (15u << 8) | 0xFFu;
            check(has_colour(pressed_frame, y, y + body.cell_height - 1, red),
                  tag + "pressed button contains the font-5 red");
            std::printf("  player_setup button 1 pressed: %lld px differ\n", inside);
        }
    }
}

int value_x_of(const std::map<int, gm::FontAtlas>& fonts) {
    return gm::player_setup_value_x(fonts.at(gm::kPlayerSetupRowFont));
}

void test_cursor_and_scroll(const fs::path& game,
                            const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    const gm::PlayerSetupRenderOptions base = device_options();
    const gm::FontAtlas& body = fonts.at(gm::kPlayerSetupRowFont);
    const std::uint32_t white = (static_cast<std::uint32_t>(239) << 24) | (239u << 16) |
                                (239u << 8) | 0xFFu;

    // Row 0 is the device name: cursor 0 paints it with font 4 (white).
    const Frame cursor0 = run(game, base);
    check(cursor0.ok, tag + "cursor 0: " + cursor0.error);
    if (!cursor0.ok) {
        return;
    }
    const int row0_y = gm::kPlayerSetupFirstRowY;
    check(has_colour(cursor0, row0_y, row0_y + body.cell_height - 1, white),
          tag + "cursor 0 paints the device name white");
    check_run(cursor0, fonts.at(gm::kPlayerSetupSelectedFont), base.joystick_name,
              gm::kPlayerSetupRowX, row0_y, tag + "device name in font 4", false);

    // Cursor -1: no white highlight at all.
    gm::PlayerSetupRenderOptions no_highlight = base;
    no_highlight.cursor = -1;
    const Frame none = run(game, no_highlight);
    check(none.ok, tag + "cursor -1: " + none.error);
    if (none.ok) {
        check_eq(none.stats.arrows, 1, tag + "cursor -1: window top stays 0");
        bool any_white = false;
        for (const std::uint32_t pixel : none.rgba) {
            any_white = any_white || pixel == white;
        }
        check(!any_white, tag + "cursor -1 paints no white");
        check(differing_outside(none, cursor0, row0_y, row0_y + body.cell_height - 1) == 0,
              tag + "cursor -1 changes only the device-name band");
    }

    // Cursor 5 does not scroll (top 0) but the highlight moves to row 5; only
    // that row differs from the no-highlight frame.
    gm::PlayerSetupRenderOptions cursor5 = base;
    cursor5.cursor = 5;
    const Frame five = run(game, cursor5);
    check(five.ok, tag + "cursor 5: " + five.error);
    if (five.ok && none.ok) {
        check_eq(five.stats.arrows, 1, tag + "cursor 5: still one arrow (top 0)");
        const int y = gm::player_setup_row_y(5);
        check(has_colour(five, y, y + body.cell_height - 1, white),
              tag + "cursor 5 paints its own row white");
        check(differing_inside(five, none, 0, y, 195, y + body.cell_height - 1) > 0,
              tag + "cursor 5 band differs from the unhighlighted frame");
        check_eq(differing_outside(five, none, y, y + body.cell_height - 1), 0,
                 tag + "cursor 5 changes only its row");
        std::printf("  player_setup cursor 5: band y=%d, %lld px differ from cursor -1\n", y,
                    differing_pixels(five.rgba, none.rgba));
    }

    // Cursor 7 scrolls by 2: the top two content rows are skipped, the first
    // button row (content 11) enters the tenth band.
    gm::PlayerSetupRenderOptions cursor7 = base;
    cursor7.cursor = 7;
    const Frame seven = run(game, cursor7);
    check(seven.ok, tag + "cursor 7: " + seven.error);
    if (seven.ok) {
        check_eq(seven.stats.arrows, 2, tag + "cursor 7: both arrows (top 2, 12 < 43)");
        const int top_y = gm::kPlayerSetupFirstRowY;
        // The old top row (device name) is gone; the new top row is axis 1.
        check_run(seven, body, gm::kPlayerSetupAxisLabels[1], gm::kPlayerSetupRowX, top_y,
                  tag + "scrolled top row is Front/Back", true, 0, value_x_of(fonts));
        // One button row fits at the bottom (content 11, y 109).
        const int button_y = gm::player_setup_row_y(9);
        check_run(seven, body, "Button 1: Fire", gm::kPlayerSetupRowX, button_y,
                  tag + "first button row after scroll", true);
        // The cursor row (content 7 = Hat 1 X) is white.
        const int cursor_y = gm::player_setup_row_y(5);
        check(has_colour(seven, cursor_y, cursor_y + body.cell_height - 1, white),
              tag + "cursor 7 highlight lands on visible row 5");
        std::printf("  player_setup cursor 7: top=2, arrows=%d\n", seven.stats.arrows);
    }

    // Cursor 40 scrolls by 35: only the up arrow is left.
    gm::PlayerSetupRenderOptions cursor40 = base;
    cursor40.cursor = 40;
    const Frame far = run(game, cursor40);
    check(far.ok, tag + "cursor 40: " + far.error);
    if (far.ok) {
        check_eq(far.stats.arrows, 1, tag + "cursor 40: one up arrow (35 + 10 >= 43)");
    }
    // Cursor 32 scrolls by 27: rows 27..36 are visible, both arrows still show.
    gm::PlayerSetupRenderOptions cursor32 = base;
    cursor32.cursor = 32;
    const Frame middle = run(game, cursor32);
    check(middle.ok, tag + "cursor 32: " + middle.error);
    if (middle.ok) {
        check_eq(middle.stats.arrows, 2, tag + "cursor 32: both arrows (27 + 10 < 43)");
    }

    // The ten-band window: exactly ten rows carry ink for any scroll offset.
    for (int cursor = 0; cursor <= 42; ++cursor) {
        gm::PlayerSetupRenderOptions options = base;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "window cursor " + std::to_string(cursor) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        const int top = gm::player_setup_scroll(cursor);
        const int expected = std::min(10, frame.stats.rows - top);
        int bands = 0;
        for (int band = 0; band < expected; ++band) {
            const int y = gm::player_setup_row_y(band);
            if (ink_box(frame, y, y + body.cell_height - 1, 150).count > 0) {
                ++bands;
            }
        }
        check_eq(bands, expected, tag + "cursor " + std::to_string(cursor) +
                                      " fills all " + std::to_string(expected) + " bands");
        check(frame.stats.ink_pixels > 0, tag + "cursor " + std::to_string(cursor) +
                                               " has ink");
    }
}

/// One pixel of the `MENUARRW` blit as `0x485cc0`/`0x485d00` place it: the
/// engine reads the sprite bytes as the file stores them, while `parse_bmp`
/// returns them top-down, so engine row `r` is `pixels[height-1-r]`.  The up
/// arrow blits the same bytes backwards, i.e. rotated by 180 degrees.
std::uint32_t arrow_pixel(const Asset& sprite, bool up, int row, int column) {
    const int source_row = up ? sprite.height - 1 - row : row;
    const int source_column = up ? sprite.width - 1 - column : column;
    const std::uint8_t index =
        sprite.indices[static_cast<std::size_t>(sprite.height - 1 - source_row) *
                           static_cast<std::size_t>(sprite.width) +
                       static_cast<std::size_t>(source_column)];
    if (index == 0) {
        return gm::kPlayerSetupBackground; // skips the byte, the canvas stays
    }
    return sprite.palette[index];
}

void test_arrows(const fs::path& game) {
    const std::string tag = "arrows: ";
    const gm::PlayerSetupRenderOptions options = [] {
        gm::PlayerSetupRenderOptions value = device_options();
        value.cursor = 7; // window top 2: both arrows
        return value;
    }();
    const Frame frame = run(game, options);
    check(frame.ok, tag + "frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.arrows, 2, tag + "two arrows at window top 2");

    const Asset sprite = load_asset(game, "MENUARRW");
    check(sprite.ok, tag + "MENUARRW decoded independently");
    check(sprite.width == gm::kPlayerSetupArrowWidth &&
              sprite.height == gm::kPlayerSetupArrowHeight,
          tag + "MENUARRW is 16x8");
    if (!sprite.ok || sprite.width != gm::kPlayerSetupArrowWidth ||
        sprite.height != gm::kPlayerSetupArrowHeight) {
        return;
    }
    int wrong_up = 0;
    int wrong_down = 0;
    int lit_up = 0;
    int lit_down = 0;
    for (int row = 0; row < gm::kPlayerSetupArrowHeight; ++row) {
        for (int column = 0; column < gm::kPlayerSetupArrowWidth; ++column) {
            const std::uint32_t want_up = arrow_pixel(sprite, true, row, column);
            const std::uint32_t want_down = arrow_pixel(sprite, false, row, column);
            if (want_up != gm::kPlayerSetupBackground) {
                ++lit_up;
            }
            if (want_down != gm::kPlayerSetupBackground) {
                ++lit_down;
            }
            if (at(frame, gm::kPlayerSetupArrowX + column,
                   gm::kPlayerSetupArrowUpY + row) != want_up) {
                ++wrong_up;
            }
            if (at(frame, gm::kPlayerSetupArrowX + column,
                   gm::kPlayerSetupArrowDownY + row) != want_down) {
                ++wrong_down;
            }
        }
    }
    check_eq(wrong_up, 0, tag + "up arrow matches the rotated MENUARRW");
    check_eq(wrong_down, 0, tag + "down arrow matches MENUARRW");
    check_eq(lit_up, lit_down, tag + "both orientations have the same pixel count");
    // The up arrow is the down arrow rotated by 180 degrees, pixel for pixel.
    int rotated_wrong = 0;
    for (int row = 0; row < gm::kPlayerSetupArrowHeight; ++row) {
        for (int column = 0; column < gm::kPlayerSetupArrowWidth; ++column) {
            const std::uint32_t up = arrow_pixel(sprite, true, row, column);
            const std::uint32_t down =
                arrow_pixel(sprite, false, gm::kPlayerSetupArrowHeight - 1 - row,
                            gm::kPlayerSetupArrowWidth - 1 - column);
            rotated_wrong += up == down ? 0 : 1;
        }
    }
    check_eq(rotated_wrong, 0, tag + "up = down rotated 180");
    std::printf("  player_setup arrows: MENUARRW 16x8, %d lit px, up (159,2), down "
                "(159,121)\n",
                lit_down);

    // A cursor at the very top has no up arrow; at the bottom none has a down
    // arrow left (35 + 10 >= 43).
    gm::PlayerSetupRenderOptions top = options;
    top.cursor = 0;
    const Frame top_frame = run(game, top);
    check(top_frame.ok && top_frame.stats.arrows == 1,
          tag + "window top 0: down arrow only");
    gm::PlayerSetupRenderOptions bottom = options;
    bottom.cursor = 42;
    const Frame bottom_frame = run(game, bottom);
    check(bottom_frame.ok && bottom_frame.stats.arrows == 1,
          tag + "window top 37: up arrow only");
}

void test_plate(const fs::path& game) {
    const std::string tag = "plate: ";
    const gm::PlayerSetupRenderOptions options = device_options();
    const Frame black = run(game, options);
    check(black.ok, tag + "black frame: " + black.error);
    if (!black.ok) {
        return;
    }
    gm::PlayerSetupRenderOptions plated = options;
    plated.background = "PLATE";
    const Frame plate = run(game, plated);
    check(plate.ok, tag + "PLATE frame: " + plate.error);
    if (!plate.ok) {
        return;
    }
    check_eq(plate.stats.ink_pixels, black.stats.ink_pixels, tag + "PLATE keeps the ink");

    const Asset asset = load_asset(game, "PLATE");
    check(asset.ok && asset.width == 196 && asset.height == 131,
          tag + "PLATE decoded independently as 196x131");
    if (!asset.ok) {
        return;
    }
    // Text is painted over the panel, so only the pixels the black frame leaves
    // black can be compared with the raw PLATE decode.
    long long wrong = 0;
    long long compared = 0;
    for (int y = 0; y < 131; ++y) {
        for (int x = 0; x < 196; ++x) {
            if (!is_black(at(black, x, y))) {
                continue;
            }
            ++compared;
            if (at(plate, x, y) != asset.colour(x, y)) {
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, tag + "PLATE pixels match the independent decode");
    std::printf("  player_setup plate: %lld of 25676 panel pixels are text-free and match\n",
                compared);
    long long outside = 0;
    for (int y = 0; y < plate.height; ++y) {
        for (int x = 0; x < plate.width; ++x) {
            if (x < 196 && y < 131) {
                continue;
            }
            outside += is_black(at(plate, x, y)) ? 0 : 1;
        }
    }
    check_eq(outside, 0, tag + "nothing outside the 196x131 panel");
    const long long diff = differing_pixels(plate.rgba, black.rgba);
    check(diff > 0, tag + "PLATE differs from the black canvas");
    std::printf("  player_setup plate: 196x131 decoded, black vs PLATE diff=%lld px, ink=%d "
                "unchanged\n",
                diff, plate.stats.ink_pixels);
}

void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade: ";
    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  player_setup fade: SKIP (no 1024-byte GAMEPAL: " << palette_error
                  << ")\n";
        return;
    }
    gm::HudLut lut;
    std::string lut_error;
    if (!gm::build_hud_lut(*palette, lut, &lut_error)) {
        check(false, tag + "build_hud_lut: " + lut_error);
        return;
    }
    const gm::PlayerSetupRenderOptions base = device_options();
    const Frame plain = run(game, base);
    check(plain.ok, tag + "unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    const auto make = [&](bool fade, int level) {
        gm::PlayerSetupRenderOptions options = base;
        options.background = "PLATE";
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, options);
    };
    const Frame plain_plate = make(false, gm::kHudLutIdentityLevel);
    check(plain_plate.ok, tag + "unfaded plate frame: " + plain_plate.error);
    const Frame identity = make(true, gm::kHudLutIdentityLevel);
    check(identity.ok, tag + "level 11 frame: " + identity.error);
    if (!plain_plate.ok || !identity.ok) {
        return;
    }
    check_eq(differing_pixels(identity.rgba, plain_plate.rgba), 0,
             tag + "level 11 is byte-identical to no fade");
    check_eq(identity.stats.ink_pixels, plain_plate.stats.ink_pixels,
             tag + "level 11 ink unchanged");

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
        check(differing_pixels(exact[static_cast<std::size_t>(i)].rgba, plain_plate.rgba) > 0,
              tag + "level " + std::to_string(level) + " changes the frame");
        check_eq(exact[static_cast<std::size_t>(i)].stats.ink_pixels,
                 plain_plate.stats.ink_pixels, tag + "level " + std::to_string(level) +
                                                     " ink unchanged");
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
    check_refused(fs::path{}, gm::PlayerSetupRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::PlayerSetupRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    const std::optional<std::vector<std::uint8_t>> fonts4 = installed_entry(game, "FONTS4");
    const std::optional<std::vector<std::uint8_t>> plate = installed_entry(game, "PLATE");
    const std::optional<std::vector<std::uint8_t>> small = installed_entry(game, "CHECK");
    check(fonts4.has_value() && plate.has_value() && small.has_value(),
          "negatives: FONTS4/PLATE/CHECK bytes available");
    if (!fonts4 || !plate || !small) {
        fs::remove_all(root, ec);
        return;
    }

    // 1. FONTS4 only: PLATE is missing.
    check(write_archive(root / "game-without-plate", {"FONTS4"}, {*fonts4}),
          "negatives: FONTS4-only archive written");
    check_refused(root / "game-without-plate", gm::PlayerSetupRenderOptions{},
                  "archive without PLATE", "PLATE");

    // 2. PLATE only: the FONTS4 faces are missing.
    check(write_archive(root / "game-without-font4", {"PLATE"}, {*plate}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", gm::PlayerSetupRenderOptions{},
                  "archive without FONTS4", "FONTS4");

    // 3. A PLATE of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-plate", {"FONTS4", "PLATE"},
                        {*fonts4, *small}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", gm::PlayerSetupRenderOptions{},
                  "8x16 PLATE", "expected 196x131");

    // 4. FONTS4 + PLATE and nothing else: the no-device frame has no arrow, so
    // it must render; a device frame needs MENUARRW and is refused.
    check(write_archive(root / "game-minimal", {"FONTS4", "PLATE"}, {*fonts4, *plate}),
          "negatives: minimal archive written");
    {
        const Frame minimal = run(root / "game-minimal", gm::PlayerSetupRenderOptions{});
        check(minimal.ok, "negatives: minimal archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, 1, "negatives: minimal archive rows 1");
            check_eq(minimal.stats.arrows, 0, "negatives: minimal archive arrows 0");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  player_setup minimal archive (FONTS4+PLATE): rows=%d arrows=%d "
                        "ink=%d missing=%d\n",
                        minimal.stats.rows, minimal.stats.arrows,
                        minimal.stats.ink_pixels, minimal.stats.missing_glyphs);
        }
        // The full device frame is 43 rows: the down arrow is mandatory there.
        check_refused(root / "game-minimal", device_options(),
                      "archive without MENUARRW for a device frame", "MENUARRW");
    }
    fs::remove_all(root, ec);

    // 5. A background name that is not in the archive is a text, not black.
    gm::PlayerSetupRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 6. The exact fade needs a table or a dump.
    gm::PlayerSetupRenderOptions no_lut;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::PlayerSetupRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 7. The shipped install renders both API defaults and is deterministic.
    const Frame first = run(game, gm::PlayerSetupRenderOptions{});
    const Frame second = run(game, gm::PlayerSetupRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  player_setup negatives: rows=%d sliders=%d checks=%d arrows=%d "
                    "ink=%d missing=%d\n",
                    first.stats.rows, first.stats.sliders, first.stats.checks,
                    first.stats.arrows, first.stats.ink_pixels,
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
        std::cout << "player_setup_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kPlayerSetupTitleFont, gm::kPlayerSetupRowFont,
                             gm::kPlayerSetupSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "player_setup_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_default_frame(game, fonts);
    test_device_frame(game, fonts);
    test_values(game, fonts);
    test_cursor_and_scroll(game, fonts);
    test_arrows(game);
    test_plate(game);
    test_fade_lut(game);
    test_negatives(game);
    if (failures != 0) {
        std::cerr << "player_setup_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "player_setup_render: ok (" << checks << " checks)\n";
    return 0;
}
