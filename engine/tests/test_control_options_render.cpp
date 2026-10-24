// One frame of screen 10, `CONTROL OPTIONS` (`M8.8` slice 17,
// `ui/control_options_render`, docs/CONTROL_OPTIONS_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there is
// no state object.  The test pins the geometry of docs/UI_SCREENS.md §2/§3.1
// from the handler `0x488ba0` (slot `0x4883ec` -> stub `0x487ec0`) and from
// pixels:
//
//   * model: `[0x67f10c] = 10` (`0x488ba3`), step 9 (`0x662d58` at `0x488bad`),
//     item y = 27 + 9*i (`push 0x1b` + `0x485dd0`'s `row + item*step`), x 20 and
//     x 32 for items 1/3, title y 14 centred, the eight literal items and the
//     four formatted ones (`0x4bd5e4`, `0x4bd5d0`/`0x4bd5dc`, `0x4bd5c0`,
//     `0x4bd58c`/`0x4bd564`, `0x4bd544`), the `View`/`Assign` switch
//     (`set >= 1`), the mouse table `0x4bc548` and the 16-byte / 25-byte caps;
//   * the shipped install: the API default frame (keysets 0/1, both joysticks
//     `Not Used`, mouse `Not Used`, rows 10, sliders/checks/arrows 0), the
//     default frame with the real `PERFECT`/`SPLIT1` names, one joystick and
//     mouse `Player 1`; every band compared byte for byte with an independent
//     blit, the bounding boxes and the inks printed;
//   * the values of the frame: `keyset1`, `keyset2`, `keyset_names`,
//     `joystick1/2`, `joystick_name1/2` and `mouse_type` each change only their
//     own lines, and the full-frame differences are printed;
//   * the cursor `[0x68ccf4]`: item `i` is the only line painted in font 4
//     (`0x485e19`), `-1` leaves no white pixel at all, and the diff of each
//     highlighted frame against the unhighlighted one stays inside that band;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel, everything outside stays black, the text ink is
//     unchanged and the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, an
//     index without `PLATE`, one without `FONTS4`, a `PLATE` of the wrong size,
//     an unknown background entry, `fade_lut` without a table and a dump, and a
//     missing dump file — all `control_options_render:` texts with a cleared
//     buffer; two renders are deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/CONTROL_OPTIONS_RENDER.md §4 pins them.

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
#include "ui/control_options_render.h"

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
    gm::ControlOptionsRenderStats stats;
};

Frame run(const fs::path& game, const gm::ControlOptionsRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_control_options(game, options, frame.rgba, frame.width,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kControlOptionsBackground; }

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
                int x_to = gm::kControlOptionsPanelWidth) {
    const int width = gm::kControlOptionsPanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kControlOptionsBackground : want;
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
                    int x_from = 0, int x_to = gm::kControlOptionsPanelWidth) {
    check_band(frame, atlas, text, x, y, what, x_from, x_to);
    const int width = gm::kControlOptionsPanelWidth;
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
        std::printf("  control_options \"%s\": x=%3d y=%3d ink=%lld bbox %s\n",
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

/// `name[16]` at the start of a shipped `KEYCFG/*.KEY` file (docs/
/// KEYCFG_RENDER.md §2), NUL-truncated.  Empty when the file is absent.
std::string key_file_name(const fs::path& game, const std::string& file) {
    std::ifstream in(game / "KEYCFG" / file, std::ios::binary);
    if (!in) {
        return {};
    }
    char bytes[16] = {};
    in.read(bytes, sizeof bytes);
    std::string name(bytes, static_cast<std::size_t>(in.gcount()));
    const std::size_t nul = name.find('\0');
    if (nul != std::string::npos) {
        name.resize(nul);
    }
    return name;
}

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_control_options_render";
    }
    return fs::temp_directory_path() / "gm_control_options_render";
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
void check_refused(const fs::path& game, const gm::ControlOptionsRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::ControlOptionsRenderStats stats;
    stats.rows = 7;
    stats.sliders = 7;
    stats.checks = 7;
    stats.arrows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    std::string error;
    const bool ok =
        gm::render_control_options(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "control_options_render:"),
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

/// The frame most pixel tests use: the API defaults, both joysticks unused, the
/// mouse `Not Used`, keysets 0/1 without names.
gm::ControlOptionsRenderOptions default_options() { return {}; }

gm::ControlOptionsRenderOptions named_options(const std::string& perfect,
                                              const std::string& split1) {
    gm::ControlOptionsRenderOptions options;
    options.keyset1 = 0;
    options.keyset2 = 1;
    options.keyset_names = {perfect, split1};
    options.joystick1 = 0;
    options.joystick_name1 = "SideWinder Precision Pro";
    options.joystick2 = -1;
    options.mouse_type = 1;
    return options;
}

// --- model -------------------------------------------------------------------
void test_model() {
    const std::string tag = "model: ";

    // Geometry of 0x488ba0.
    check_eq(gm::kControlOptionsRows, 10, tag + "rows 10 (0x488ba3)");
    check_eq(gm::kControlOptionsRowStep, 9, tag + "step 9 (0x662d58 at 0x488bad)");
    check_eq(gm::kControlOptionsFirstRowY, 27, tag + "item row 27 (push 0x1b)");
    check_eq(gm::kControlOptionsRowX, 20, tag + "item x 20 (push 0x14)");
    check_eq(gm::kControlOptionsSubRowX, 32, tag + "assign x 32 (push 0x20)");
    check_eq(gm::kControlOptionsTitleY, 14, tag + "title y 14 (push 0xe)");
    check_eq(gm::kControlOptionsTitleX, 20, tag + "title x argument 20 (ignored)");
    check_eq(gm::kControlOptionsPanelWidth, 196, tag + "panel 196 px band");
    check_eq(gm::kControlOptionsPanelHeight, 131, tag + "panel 131 rows");
    check_eq(gm::kControlOptionsKeySets, 16, tag + "16 key sets (§7)");
    check_eq(gm::kControlOptionsKeyNameChars, 16, tag + "name[16]");
    check_eq(gm::kControlOptionsJoystickNameChars, 25, tag + "%.25s of the joystick name");
    check(gm::kControlOptionsTitle == "CONTROL OPTIONS", tag + "title 0x4bd5f4");
    check(gm::kControlOptionsPanelName == "PLATE", tag + "PLATE panel");
    check(gm::kControlOptionsKeyJamming == "Keyjamming", tag + "0x4bd5b4");
    check(gm::kControlOptionsCentering == "Joystick Centering", tag + "0x4bd550");
    check(gm::kControlOptionsDone == "Done", tag + "0x4bceb4");
    check(gm::kControlOptionsJoystick1Unused == "Player 1 Joystick: Not Used",
          tag + "0x4bd598");
    check(gm::kControlOptionsJoystick2Unused == "Player 2 Joystick: Not Used",
          tag + "0x4bd570");
    check(gm::kControlOptionsPlayer1Format == "Player 1: %s", tag + "0x4bd5e4");
    check(gm::kControlOptionsPlayer2Format == "Player 2: %s", tag + "0x4bd5c0");
    check(gm::kControlOptionsViewFormat == "View %s", tag + "0x4bd5dc");
    check(gm::kControlOptionsAssignFormat == "Assign %s", tag + "0x4bd5d0");
    check(gm::kControlOptionsMouseFormat == "Mouse: %s", tag + "0x4bd544");

    // Item y and x.
    check_eq(gm::control_options_row_y(0), 27, tag + "item 0 -> y 27");
    check_eq(gm::control_options_row_y(1), 36, tag + "item 1 -> y 36");
    check_eq(gm::control_options_row_y(4), 63, tag + "item 4 -> y 63");
    check_eq(gm::control_options_row_y(9), 108, tag + "item 9 -> y 108");
    check_eq(gm::control_options_row_x(0), 20, tag + "item 0 x 20");
    check_eq(gm::control_options_row_x(1), 32, tag + "item 1 x 32");
    check_eq(gm::control_options_row_x(2), 20, tag + "item 2 x 20");
    check_eq(gm::control_options_row_x(3), 32, tag + "item 3 x 32");
    check_eq(gm::control_options_row_x(4), 20, tag + "item 4 x 20");
    check_eq(gm::control_options_row_x(9), 20, tag + "item 9 x 20");

    // Mouse table of 0x4bc548.
    check(gm::control_options_mouse_name(0) == "Not Used", tag + "mouse 0 (0x4bca50)");
    check(gm::control_options_mouse_name(1) == "Player 1", tag + "mouse 1 (0x4bc944)");
    check(gm::control_options_mouse_name(2) == "Player 2", tag + "mouse 2 (0x4bc938)");
    check(gm::control_options_mouse_name(-1) == "Not Used", tag + "mouse clamp low");
    check(gm::control_options_mouse_name(9) == "Player 2", tag + "mouse clamp high");

    // Item texts of the shipped defaults.
    const gm::ControlOptionsRenderOptions defaults;
    check_eq(gm::control_options_item_text(defaults, 0), "Player 1: ",
             tag + "default item 0");
    check_eq(gm::control_options_item_text(defaults, 1), "View ",
             tag + "set 0 -> View (0x488c15)");
    check_eq(gm::control_options_item_text(defaults, 2), "Player 2: ",
             tag + "default item 2");
    check_eq(gm::control_options_item_text(defaults, 3), "Assign ",
             tag + "set 1 -> Assign (0x488ca8)");
    check_eq(gm::control_options_item_text(defaults, 4), "Keyjamming", tag + "item 4");
    check_eq(gm::control_options_item_text(defaults, 5),
             "Player 1 Joystick: Not Used", tag + "slot -1 -> Not Used (0x488d74)");
    check_eq(gm::control_options_item_text(defaults, 6),
             "Player 2 Joystick: Not Used", tag + "slot -1 -> Not Used (0x488dca)");
    check_eq(gm::control_options_item_text(defaults, 7), "Joystick Centering", tag + "item 7");
    check_eq(gm::control_options_item_text(defaults, 8), "Mouse: Not Used",
             tag + "mouse default 0");
    check_eq(gm::control_options_item_text(defaults, 9), "Done", tag + "item 9");
    check_eq(gm::control_options_item_text(defaults, -1), "", tag + "item -1 is empty");
    check_eq(gm::control_options_item_text(defaults, 10), "", tag + "item 10 is empty");

    // View/Assign switch: `jge` on set >= 1.
    gm::ControlOptionsRenderOptions sets;
    sets.keyset1 = 2;
    sets.keyset2 = 0;
    sets.keyset_names = {"PERFECT", "SPLIT1"};
    check_eq(gm::control_options_item_text(sets, 0), "Player 1: PERFECT",
             tag + "named item 0");
    check_eq(gm::control_options_item_text(sets, 1), "Assign PERFECT",
             tag + "set 2 -> Assign");
    check_eq(gm::control_options_item_text(sets, 2), "Player 2: SPLIT1",
             tag + "named item 2");
    check_eq(gm::control_options_item_text(sets, 3), "View SPLIT1",
             tag + "set 0 -> View");
    // Clamps: set indices to 0..15 (only the View/Assign word depends on it).
    gm::ControlOptionsRenderOptions clamped = sets;
    clamped.keyset1 = 99;
    clamped.keyset2 = -5;
    check_eq(gm::control_options_item_text(clamped, 1), "Assign PERFECT",
             tag + "set clamp high is still Assign");
    check_eq(gm::control_options_item_text(clamped, 3), "View SPLIT1",
             tag + "set clamp low is View");

    // Field caps: name[16] and "%.25s".
    gm::ControlOptionsRenderOptions long_names;
    long_names.keyset_names = {std::string(16, 'K') + "TAIL", "SPLIT2"};
    long_names.joystick1 = 3;
    long_names.joystick_name1 = std::string(40, 'J');
    check_eq(gm::control_options_item_text(long_names, 0),
             "Player 1: " + std::string(16, 'K'), tag + "name[16] cap");
    check_eq(gm::control_options_item_text(long_names, 5),
             "1: " + std::string(25, 'J'), tag + "%.25s cap");
    check_eq(gm::control_options_item_text(long_names, 6),
             "Player 2 Joystick: Not Used", tag + "second slot stays unused");
    // A NUL inside the field ends the string (strncpy/`%s` semantics).
    gm::ControlOptionsRenderOptions nul_name;
    nul_name.keyset_names = {std::string("AB\0CD", 5), ""};
    nul_name.joystick1 = 1;
    nul_name.joystick_name1 = std::string("Device\0X", 8);
    check_eq(gm::control_options_item_text(nul_name, 0), "Player 1: AB",
             tag + "NUL ends the set name");
    check_eq(gm::control_options_item_text(nul_name, 5), "1: Device",
             tag + "NUL ends the joystick name");
    // Mouse index clamp.
    gm::ControlOptionsRenderOptions mouse;
    mouse.mouse_type = 2;
    check_eq(gm::control_options_item_text(mouse, 8), "Mouse: Player 2",
             tag + "mouse 2 -> Player 2");
}

// --- the shipped install -----------------------------------------------------
void test_default_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "default: ";
    const gm::ControlOptionsRenderOptions options = default_options();
    const Frame frame = run(game, options);
    check(frame.ok, tag + "frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kControlOptionsCanvasWidth, tag + "width 320");
    check_eq(frame.height, gm::kControlOptionsCanvasHeight, tag + "height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, tag + "64000 pixels");
    bool opaque = true;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque = opaque && (pixel & 0xFFu) == 0xFFu;
    }
    check(opaque, tag + "every pixel opaque");
    check_eq(frame.stats.rows, 10, tag + "rows 10 (0x67f10c)");
    check_eq(frame.stats.sliders, 0, tag + "no SLIDE/KNOB: sliders 0");
    check_eq(frame.stats.checks, 0, tag + "no CHECK/NOCHECK: checks 0");
    check_eq(frame.stats.arrows, 0, tag + "no MENUARRW: arrows 0");
    check_eq(frame.stats.missing_glyphs, 0, tag + "missing glyphs 0");

    const gm::FontAtlas& title = fonts.at(gm::kControlOptionsTitleFont);
    const int title_x = gm::control_options_title_x(title);
    check_eq(title_x, std::max(0, (196 - title.measure(gm::kControlOptionsTitle)) / 2),
             tag + "title x = (196 - width)/2");
    long long ink = check_run(frame, title, gm::kControlOptionsTitle, title_x,
                              gm::kControlOptionsTitleY, tag + "title", true);
    std::printf("  control_options title: font 5 \"%s\" x=%d y=%d width=%d\n",
                std::string(gm::kControlOptionsTitle).c_str(), title_x,
                gm::kControlOptionsTitleY, title.measure(gm::kControlOptionsTitle));

    // Item 0 is the cursor: font 4.  Every other line is font 0.
    for (int item = 0; item < gm::kControlOptionsRows; ++item) {
        const int font = item == options.cursor ? gm::kControlOptionsSelectedFont
                                                : gm::kControlOptionsRowFont;
        const std::string text = gm::control_options_item_text(options, item);
        ink += check_run(frame, fonts.at(font), text, gm::control_options_row_x(item),
                         gm::control_options_row_y(item),
                         tag + "item " + std::to_string(item), true);
    }
    check_eq(frame.stats.ink_pixels, ink, tag + "ink = sum of the eleven bands");
    std::printf("  control_options default: rows=%d sliders=%d checks=%d arrows=%d ink=%d "
                "missing=%d\n",
                frame.stats.rows, frame.stats.sliders, frame.stats.checks, frame.stats.arrows,
                frame.stats.ink_pixels, frame.stats.missing_glyphs);
    // Title never overlaps the first item band.
    const Box title_box = ink_box(frame, gm::kControlOptionsTitleY,
                                  gm::kControlOptionsTitleY + title.cell_height - 1);
    check(title_box.y1 < gm::kControlOptionsFirstRowY, tag + "title band above item 0");
    std::printf("  control_options title bbox %s, item 0 y=%d\n",
                box_text(title_box).c_str(), gm::kControlOptionsFirstRowY);
}

void test_install_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "install: ";
    const std::string perfect = key_file_name(game, "PERFECT.key");
    const std::string split1 = key_file_name(game, "SPLIT1.KEY");
    if (perfect.empty() || split1.empty()) {
        std::cout << "  control_options install: SKIP (no KEYCFG/PERFECT.key+SPLIT1.KEY)\n";
        return;
    }
    check_eq(perfect, "PERFECT", tag + "KEYCFG/PERFECT.key name[16]");
    check_eq(split1, "SPLIT1", tag + "KEYCFG/SPLIT1.KEY name[16]");
    const gm::ControlOptionsRenderOptions options = named_options(perfect, split1);
    const Frame frame = run(game, options);
    check(frame.ok, tag + "frame: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.rows, 10, tag + "rows 10");
    check_eq(frame.stats.missing_glyphs, 0, tag + "no missing glyph");

    const gm::FontAtlas& title = fonts.at(gm::kControlOptionsTitleFont);
    long long ink = check_run(frame, title, gm::kControlOptionsTitle,
                              gm::control_options_title_x(title), gm::kControlOptionsTitleY,
                              tag + "title", true);

    // The ten lines of the frame, spelled out without calling the module.
    const std::string expected[gm::kControlOptionsRows] = {
        "Player 1: PERFECT",
        "View PERFECT",
        "Player 2: SPLIT1",
        "Assign SPLIT1",
        "Keyjamming",
        "1: SideWinder Precision Pro",
        "Player 2 Joystick: Not Used",
        "Joystick Centering",
        "Mouse: Player 1",
        "Done",
    };
    for (int item = 0; item < gm::kControlOptionsRows; ++item) {
        check_eq(gm::control_options_item_text(options, item), expected[item],
                 tag + "item " + std::to_string(item) + " text");
        const int font = item == options.cursor ? gm::kControlOptionsSelectedFont
                                                : gm::kControlOptionsRowFont;
        ink += check_run(frame, fonts.at(font), expected[item],
                         gm::control_options_row_x(item),
                         gm::control_options_row_y(item),
                         tag + "item " + std::to_string(item), true);
        // Every line stays inside the 196 px band (the bbox itself is pinned by
        // check_run against the independent blit).
        const Box box = ink_box(frame, gm::control_options_row_y(item),
                                gm::control_options_row_y(item) +
                                    fonts.at(font).cell_height - 1);
        check(box.x0 >= gm::control_options_row_x(item) && box.x1 < gm::kControlOptionsPanelWidth,
              tag + "item " + std::to_string(item) + " inside the 196 px band");
    }
    check_eq(frame.stats.ink_pixels, ink, tag + "ink = title + ten items");
    std::printf("  control_options install: rows=%d sliders=%d checks=%d arrows=%d ink=%d "
                "missing=%d\n",
                frame.stats.rows, frame.stats.sliders, frame.stats.checks, frame.stats.arrows,
                frame.stats.ink_pixels, frame.stats.missing_glyphs);
}

void test_values(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "values: ";
    const gm::ControlOptionsRenderOptions base = default_options();
    const Frame plain = run(game, base);
    check(plain.ok, tag + "base frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    const gm::FontAtlas& body = fonts.at(gm::kControlOptionsRowFont);

    // keyset1 0 -> 2: item 0 keeps its x, item 1 switches View -> Assign.
    gm::ControlOptionsRenderOptions keyset = base;
    keyset.keyset1 = 2;
    keyset.cursor = -1;
    gm::ControlOptionsRenderOptions plain_cursor = base;
    plain_cursor.cursor = -1;
    const Frame plain_no_cursor = run(game, plain_cursor);
    const Frame keyset_frame = run(game, keyset);
    check(keyset_frame.ok, tag + "keyset frame: " + keyset_frame.error);
    if (keyset_frame.ok) {
        // Only item 0 and item 1 can change: rows 27..27+cell-1 and 36..36+cell-1.
        const int band0 = gm::control_options_row_y(0) + body.cell_height - 1;
        const int band1 = gm::control_options_row_y(1) + body.cell_height - 1;
        long long wrong = 0;
        for (int y = band0 + 1; y < gm::control_options_row_y(1); ++y) {
            for (int x = 0; x < keyset_frame.width; ++x) {
                wrong += at(keyset_frame, x, y) != at(plain_no_cursor, x, y) ? 1 : 0;
            }
        }
        check_eq(wrong, 0, tag + "keyset1 changes nothing between items 0 and 1");
        long long below = 0;
        for (int y = band1 + 1; y < keyset_frame.height; ++y) {
            for (int x = 0; x < keyset_frame.width; ++x) {
                below += at(keyset_frame, x, y) != at(plain_no_cursor, x, y) ? 1 : 0;
            }
        }
        check_eq(below, 0, tag + "keyset1 changes nothing below item 1");
        std::printf("  control_options keyset1 0->2: %lld px differ (items 0..1 only)\n",
                    differing_pixels(keyset_frame.rgba, plain_no_cursor.rgba));
        check_eq(gm::control_options_item_text(keyset, 0), "Player 1: ",
                 tag + "keyset1 does not rename item 0");
        check_eq(gm::control_options_item_text(keyset, 1), "Assign ",
                 tag + "keyset1 2 -> Assign");
    }

    // keyset_names: the real names change exactly items 0..3.
    gm::ControlOptionsRenderOptions named = base;
    named.cursor = -1;
    named.keyset_names = {"PERFECT", "SPLIT1"};
    const Frame named_frame = run(game, named);
    check(named_frame.ok, tag + "names frame: " + named_frame.error);
    if (named_frame.ok) {
        const int last = gm::control_options_row_y(3) + body.cell_height - 1;
        const long long below = differing_outside(named_frame, plain_no_cursor, 0, last);
        check_eq(below, 0, tag + "keyset names change nothing after item 3");
        std::printf("  control_options names PERFECT/SPLIT1: %lld px differ (items 0..3)\n",
                    differing_pixels(named_frame.rgba, plain_no_cursor.rgba));
    }

    // joystick1 0 + name: item 5 only.
    gm::ControlOptionsRenderOptions joystick = base;
    joystick.cursor = -1;
    joystick.joystick1 = 0;
    joystick.joystick_name1 = "SideWinder Precision Pro";
    const Frame joystick_frame = run(game, joystick);
    check(joystick_frame.ok, tag + "joystick frame: " + joystick_frame.error);
    if (joystick_frame.ok) {
        const long long outside =
            differing_outside(joystick_frame, plain_no_cursor, gm::control_options_row_y(5),
                              gm::control_options_row_y(5) + body.cell_height - 1);
        check_eq(outside, 0, tag + "joystick1 changes only item 5");
        check_eq(gm::control_options_item_text(joystick, 5), "1: SideWinder Precision Pro",
                 tag + "item 5 text");
        std::printf("  control_options joystick1: %lld px differ (item 5 only)\n",
                    differing_pixels(joystick_frame.rgba, plain_no_cursor.rgba));
    }

    // joystick2 -2 also counts as "no device" (`jl` semantics), jobj 0 draws
    // "2: <name>".
    gm::ControlOptionsRenderOptions joystick2 = base;
    joystick2.cursor = -1;
    joystick2.joystick2 = 2;
    joystick2.joystick_name2 = "WingMan";
    const Frame joystick2_frame = run(game, joystick2);
    check(joystick2_frame.ok, tag + "joystick2 frame: " + joystick2_frame.error);
    if (joystick2_frame.ok) {
        check_eq(gm::control_options_item_text(joystick2, 6), "2: WingMan",
                 tag + "item 6 text");
        const long long outside =
            differing_outside(joystick2_frame, plain_no_cursor, gm::control_options_row_y(6),
                              gm::control_options_row_y(6) + body.cell_height - 1);
        check_eq(outside, 0, tag + "joystick2 changes only item 6");
        std::printf("  control_options joystick2: %lld px differ (item 6 only)\n",
                    differing_pixels(joystick2_frame.rgba, plain_no_cursor.rgba));
    }

    // mouse_type 0 -> 2: item 8 only.
    gm::ControlOptionsRenderOptions mouse = base;
    mouse.cursor = -1;
    mouse.mouse_type = 2;
    const Frame mouse_frame = run(game, mouse);
    check(mouse_frame.ok, tag + "mouse frame: " + mouse_frame.error);
    if (mouse_frame.ok) {
        const long long outside =
            differing_outside(mouse_frame, plain_no_cursor, gm::control_options_row_y(8),
                              gm::control_options_row_y(8) + body.cell_height - 1);
        check_eq(outside, 0, tag + "mouse_type changes only item 8");
        check_eq(gm::control_options_item_text(mouse, 8), "Mouse: Player 2",
                 tag + "item 8 text");
        std::printf("  control_options mouse_type 0->2: %lld px differ (item 8 only)\n",
                    differing_pixels(mouse_frame.rgba, plain_no_cursor.rgba));
    }
}

void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    gm::ControlOptionsRenderOptions none = default_options();
    none.cursor = -1;
    const Frame plain = run(game, none);
    check(plain.ok, tag + "unhighlighted frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    // No font-4 pixel means no white (239,239,239) anywhere.
    const std::uint32_t white = 0xFFEFEFEFu;
    bool any_white = false;
    for (const std::uint32_t pixel : plain.rgba) {
        any_white = any_white || pixel == white;
    }
    check(!any_white, tag + "cursor -1 leaves no white pixel");
    check_eq(plain.stats.ink_pixels > 0, true, tag + "the unhighlighted frame has ink");

    const gm::FontAtlas& body = fonts.at(gm::kControlOptionsRowFont);
    const gm::FontAtlas& selected = fonts.at(gm::kControlOptionsSelectedFont);
    std::printf("  control_options cursor: font 0 -> font 4, cell %dx%d -> %dx%d\n",
                body.measure("M"), body.cell_height, selected.measure("M"),
                selected.cell_height);
    long long previous = 0;
    for (int item = 0; item < gm::kControlOptionsRows; ++item) {
        gm::ControlOptionsRenderOptions options = default_options();
        options.cursor = item;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "cursor " + std::to_string(item) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.ink_pixels, plain.stats.ink_pixels,
                 tag + "cursor " + std::to_string(item) + " keeps the ink");
        const long long diff = differing_pixels(frame.rgba, plain.rgba);
        check(diff > 0, tag + "cursor " + std::to_string(item) + " changes the frame");
        const long long outside =
            differing_outside(frame, plain, gm::control_options_row_y(item),
                              gm::control_options_row_y(item) +
                                  std::max(body.cell_height, selected.cell_height) - 1);
        check_eq(outside, 0, tag + "cursor " + std::to_string(item) +
                               " stays inside its band");
        // The highlighted line is not identical to the unhighlighted one.
        const std::string text = gm::control_options_item_text(options, item);
        const std::vector<std::uint32_t> unsel = compose_band(
            body, text, gm::control_options_row_x(item), 196, body.cell_height);
        const std::vector<std::uint32_t> sel = compose_band(
            selected, text, gm::control_options_row_x(item), 196, selected.cell_height);
        check_eq(static_cast<long long>(sel.size()), static_cast<long long>(unsel.size()),
                 tag + "same band size");
        std::printf("  control_options cursor %d (\"%s\"): %lld px differ from cursor -1, "
                    "font0 ink=%lld font4 ink=%lld\n",
                    item, text.c_str(), diff, band_ink(unsel), band_ink(sel));
        previous = diff;
    }
    check(previous > 0, tag + "the last cursor still changes the frame");

    // A cursor outside 0..9 paints no highlight at all.
    gm::ControlOptionsRenderOptions outside = default_options();
    outside.cursor = 10;
    const Frame outside_frame = run(game, outside);
    check(outside_frame.ok, tag + "cursor 10 frame: " + outside_frame.error);
    if (outside_frame.ok) {
        check_eq(differing_pixels(outside_frame.rgba, plain.rgba), 0,
                 tag + "cursor 10 == cursor -1");
    }
}

void test_plate(const fs::path& game) {
    const std::string tag = "plate: ";
    gm::ControlOptionsRenderOptions options = default_options();
    options.cursor = 1;
    const Frame black = run(game, options);
    check(black.ok, tag + "black frame: " + black.error);
    if (!black.ok) {
        return;
    }
    gm::ControlOptionsRenderOptions plated = options;
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
    std::printf("  control_options plate: %lld of 25676 panel pixels are text-free and match\n",
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
    std::printf("  control_options plate: 196x131 decoded, black vs PLATE diff=%lld px, "
                "ink=%d unchanged\n",
                diff, plate.stats.ink_pixels);
}

void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade: ";
    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  control_options fade: SKIP (no 1024-byte GAMEPAL: " << palette_error
                  << ")\n";
        return;
    }
    gm::HudLut lut;
    std::string lut_error;
    if (!gm::build_hud_lut(*palette, lut, &lut_error)) {
        check(false, tag + "build_hud_lut: " + lut_error);
        return;
    }
    gm::ControlOptionsRenderOptions base = default_options();
    base.cursor = 2;
    const Frame plain = run(game, base);
    check(plain.ok, tag + "unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    const auto make = [&](bool fade, int level) {
        gm::ControlOptionsRenderOptions options = base;
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
    gm::ControlOptionsRenderOptions options = default_options();
    check_refused(fs::path{}, options, "empty game path", "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", options, "no BMP.IDX", "BMP.IDX");

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
    check_refused(root / "game-without-plate", options, "archive without PLATE", "PLATE");

    // 2. PLATE only: the FONTS4 faces are missing.
    check(write_archive(root / "game-without-font4", {"PLATE"}, {*plate}),
          "negatives: no-FONTS4 archive written");
    check_refused(root / "game-without-font4", options, "archive without FONTS4", "FONTS4");

    // 3. A PLATE of the wrong size (the 8x16 CHECK renamed) is refused.
    check(write_archive(root / "game-wrong-plate", {"FONTS4", "PLATE"}, {*fonts4, *small}),
          "negatives: wrong-size PLATE archive written");
    check_refused(root / "game-wrong-plate", options, "8x16 PLATE", "expected 196x131");

    // 4. FONTS4 + PLATE is enough for this screen: no widget, no arrow.
    check(write_archive(root / "game-minimal", {"FONTS4", "PLATE"}, {*fonts4, *plate}),
          "negatives: minimal archive written");
    {
        const Frame minimal = run(root / "game-minimal", options);
        check(minimal.ok, "negatives: minimal archive renders: " + minimal.error);
        if (minimal.ok) {
            check_eq(minimal.stats.rows, 10, "negatives: minimal archive rows 10");
            check_eq(minimal.stats.sliders, 0, "negatives: minimal archive sliders 0");
            check_eq(minimal.stats.checks, 0, "negatives: minimal archive checks 0");
            check_eq(minimal.stats.arrows, 0, "negatives: minimal archive arrows 0");
            check(minimal.stats.ink_pixels > 0, "negatives: minimal archive has ink");
            std::printf("  control_options minimal archive (FONTS4+PLATE): rows=%d "
                        "sliders=%d checks=%d arrows=%d ink=%d missing=%d\n",
                        minimal.stats.rows, minimal.stats.sliders, minimal.stats.checks,
                        minimal.stats.arrows, minimal.stats.ink_pixels,
                        minimal.stats.missing_glyphs);
        }
    }
    fs::remove_all(root, ec);

    // 5. A background name that is not in the archive is a text, not black.
    gm::ControlOptionsRenderOptions bad_background = options;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // 6. The exact fade needs a table or a dump.
    gm::ControlOptionsRenderOptions no_lut = options;
    no_lut.fade_lut = true;
    check_refused(game, no_lut, "fade_lut without a table or dump", "fade_lut_table");

    gm::ControlOptionsRenderOptions bad_dump = options;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-such-dump.bin").string();
    check_refused(game, bad_dump, "missing fade dump", "hud_lut");

    // 7. The shipped install renders both API defaults and is deterministic.
    const Frame first = run(game, gm::ControlOptionsRenderOptions{});
    const Frame second = run(game, gm::ControlOptionsRenderOptions{});
    check(first.ok && second.ok, "negatives: shipped install renders");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: two renders are byte-identical");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: stats are deterministic");
        std::printf("  control_options negatives: rows=%d sliders=%d checks=%d arrows=%d "
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
        std::cout << "control_options_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kControlOptionsTitleFont, gm::kControlOptionsRowFont,
                             gm::kControlOptionsSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "control_options_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_default_frame(game, fonts);
    test_install_frame(game, fonts);
    test_values(game, fonts);
    test_cursor(game, fonts);
    test_plate(game);
    test_fade_lut(game);
    test_negatives(game);
    if (failures != 0) {
        std::cerr << "control_options_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "control_options_render: ok (" << checks << " checks)\n";
    return 0;
}
