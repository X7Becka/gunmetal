// One frame of screen 30, the KEY SET VIEW (`M8.7` slice 7,
// `ui/keycfg_render`, docs/KEYCFG_RENDER.md).
//
// The module paints the 31 actions of one real `KEYCFG/*.KEY` profile into the
// 320x200 RGBA canvas with the optional `PLATE` band and the `FONTS4` faces 5
// (title), 0 (rows) and 4 (cursor).  The test pins the geometry of
// docs/UI_SCREENS.md §2/§3.7 from the handler `0x489850` and from pixels, not
// from the module's own constants:
//
//   * `PERFECT.key`: `rows == 31` (the proven `0x67f10c = 0x1f`), 29 bound
//     actions, 320x200 RGBA, no missing glyph and every pixel opaque;
//   * the title is the profile's `name[16]` ("PERFECT"), font 5, centred at
//     y 11; the hint `Press Esc to close` (0x4bd7c8) is font 0, centred at
//     y 20; the first band is at y 29 with the action label at x 5, the primary
//     key at x 108 and the second code at x 154;
//   * the ten painted bands are compared byte for byte with an independent
//     blit: row 0 is checked against the literals `Move Forward`/`W`/`NONE`
//     (the `pair.b` column is the loader's second table, 0 in all shipped
//     files), rows 1..9 against the parsed profile and `Controls::key_name`;
//   * `SPLIT1.KEY`/`SPLIT2.KEY`: 19 bound actions each, frames differ
//     (counted pixels); each profile's name is visible — a temporary copy whose
//     `name[16]` is replaced by spaces differs from the real frame only in the
//     title band (y 11..18) and by exactly the title's glyph ink;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently,
//     the pixels outside it stay black, the text ink is unchanged and
//     `menu_panel_asset(Screen::KeySetView) == "PLATE"`;
//   * the down scroll arrow `MENUARRW` at (159, 121) is the only ink outside
//     the text bands (41 sprite pixels), because the default frame is the first
//     window (cursor 0 -> top 0); there is no up arrow;
//   * cursor/window (`M8.7` slice 9, `0x4898a8`..`0x4898d4`): cursor
//     0/5/6/15/20/30 pins `top = 0/0/1/10/15/21`, the visible slots
//     `top..top+9`, the highlighted band (`top + i == cursor`), the arrow count
//     (1/1/2/2/2/1) and the white bbox of the highlight; every window is
//     compared band by band with the independent blit, cursors out of 0..30
//     clamp to the ends (same frame as 0/30, `keycfg_render:`-free);
//   * negatives: a missing/empty profile, 375- and 377-byte copies, an empty
//     game path, a game without `FRONTEND/BMP.IDX`, a `BMP.IDX` without
//     `FONTS4`, an unknown background — all texts with a cleared buffer and
//     zeroed stats; determinism of two renders.
//   * exact fade (`fade_lut`, slice 8, §6 of the doc): level 11 is
//     byte-identical to the unfaded frame (0/64000), level 0 has a printed black
//     count and differs, 12..15 are four different frames while the canvas
//     outside the PLATE band stays black; no table and no dump is a
//     `keycfg_render:` text.  The table is built in memory from GAMEPAL, no dump
//     is read.
//   * editor 34/35 (`M8.7` slice 10, `0x4892d0`): the default is `view` and is
//     the slice 7/9 frame (an explicit view and view+conflict differ by 0
//     pixels); `edit` differs (printed count) only in the hint band and the
//     cursor band, the hint is `Enter changes, Delete clears` (0x4bd7a8,
//     centred, y 20, printed bbox/width), the cursor cell uses font 5 on screen
//     35 and font 4 on the one-frame alias 34 (`options.edit_alias_frame`),
//     `options.edit_cell` moves the face to x 154, the conflict text is one
//     font-5 line at y 109 (`0x489563`, printed bbox/ink) and never appears in
//     `view`; an empty text paints nothing and a text wider than 196 px is
//     clamped to x 0 and clipped (printed width/ink).
//
// The installation and `KEYCFG` are only read; the pixel half skips cleanly
// when the game is absent.  Numbers are printed and docs/KEYCFG_RENDER.md §4
// pins them.

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
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "input/controls.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "ui/keycfg_render.h"
#include "ui/menu_render.h"

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

/// The 31 display names of `0x4bc558`, repeated as literals so the pixel checks
/// do not trust the module's own table (`ui/frontend_menu` carries the same
/// list for screens 30/34/35).
constexpr std::array<std::string_view, 31> kActionLabels = {
    "Move Forward",   "Move Backward",     "Turn Left",         "Turn Right",
    "Look Center",    "Fire",              "Look Up",           "Look Down",
    "Strafe",         "Strafe Left",       "Strafe Right",      "Throttle Up",
    "Throttle Down",  "Next Weapon",       "Previous Weapon",   "Activate",
    "Next Equipment", "Previous Equip.",   "Toggle Equip. Use", "Headlights",
    "Force Field",    "Mine Detector",     "Targeting",         "Warp Device",
    "Radar",          "Mine Layer",        "Combat Block",      "Auto Sentry",
    "Decoy",          "Linear Left",       "Linear Right",
};

/// One frame (or one refusal) of the renderer.
struct Frame {
    bool ok = false;
    std::string error;
    std::vector<std::uint32_t> rgba;
    int width = 0;
    int height = 0;
    gm::KeycfgRenderStats stats;
};

Frame run(const fs::path& game, const fs::path& key_file,
          const gm::KeycfgRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_keycfg(game, key_file, options, frame.rgba, frame.width,
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

/// Differing pixels of two frames outside the row band `y0..y1` (inclusive):
/// used to prove that the editor only touches the hint, the cursor band and
/// the conflict band.
long long differing_pixels_outside(const Frame& a, const Frame& b, int y0, int y1) {
    if (a.rgba.size() != b.rgba.size()) {
        return -1;
    }
    long long count = 0;
    for (int y = 0; y < a.height; ++y) {
        if (y >= y0 && y <= y1) {
            continue;
        }
        for (int x = 0; x < a.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(a.width) +
                                  static_cast<std::size_t>(x);
            count += a.rgba[i] != b.rgba[i] ? 1 : 0;
        }
    }
    return count;
}

bool is_black(std::uint32_t pixel) { return pixel == gm::kKeycfgBackground; }

/// `#000` with any alpha: the faded palette entries of level 0 are black too,
/// but their alpha is the forced 255 and their RGB is what counts here.
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

/// Independent blit of one text run into a `width` x `height` band: glyph row
/// `r` at `y + r` (`0x47faf0`), palette index 0 transparent, the atlas palette
/// forced opaque.  0 marks an untouched pixel.
void blit_into(std::vector<std::uint32_t>& band, int width, int height,
               const gm::FontAtlas& atlas, std::string_view text, int x) {
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
}

std::vector<std::uint32_t> compose_band(const gm::FontAtlas& atlas, std::string_view text,
                                        int x, int width, int height) {
    std::vector<std::uint32_t> band(static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height),
                                    0u);
    blit_into(band, width, height, atlas, text, x);
    return band;
}

/// One row band: action label (font 0, x 5), primary key (font 0/4, x 108) and
/// the second code (font 0, x 154), exactly the three calls of `0x489938`,
/// `0x489975`/`0x4899c3` and `0x4899e8`.
std::vector<std::uint32_t> compose_row(const std::map<int, gm::FontAtlas>& fonts,
                                       std::string_view action, std::string_view primary,
                                       std::string_view second, bool selected, int width) {
    const int height = fonts.at(gm::kKeycfgRowFont).cell_height;
    std::vector<std::uint32_t> band(static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height),
                                    0u);
    blit_into(band, width, height, fonts.at(gm::kKeycfgRowFont), action,
              gm::kKeycfgActionX);
    blit_into(band, width, height,
              fonts.at(selected ? gm::kKeycfgSelectedFont : gm::kKeycfgRowFont), primary,
              gm::kKeycfgKeyX);
    blit_into(band, width, height, fonts.at(gm::kKeycfgRowFont), second,
              gm::kKeycfgSecondKeyX);
    return band;
}

/// The same row with explicit faces for the two code cells: the editor
/// `0x4893bf`..`0x4894c9` picks them per cell (`0x68cd80`) and per screen
/// (`0x4bc440`), so the independent blit must take them as arguments.
std::vector<std::uint32_t> compose_row_faces(const std::map<int, gm::FontAtlas>& fonts,
                                             std::string_view action,
                                             std::string_view primary, int primary_font,
                                             std::string_view second, int second_font,
                                             int width) {
    const int height = fonts.at(gm::kKeycfgRowFont).cell_height;
    std::vector<std::uint32_t> band(static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height),
                                    0u);
    blit_into(band, width, height, fonts.at(gm::kKeycfgRowFont), action,
              gm::kKeycfgActionX);
    blit_into(band, width, height, fonts.at(primary_font), primary, gm::kKeycfgKeyX);
    blit_into(band, width, height, fonts.at(second_font), second,
              gm::kKeycfgSecondKeyX);
    return band;
}

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

/// Compare the `height` rows `y..y+height-1` of the band `0..width` of *frame*
/// with the independent blit; every untouched pixel must be the black canvas.
void check_band(const Frame& frame, const std::vector<std::uint32_t>& band, int width,
                int height, int y, const std::string& what) {
    int wrong = 0;
    int first_col = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kKeycfgBackground : want;
            if (got != expected) {
                if (wrong == 0) {
                    first_col = column;
                }
                ++wrong;
            }
        }
    }
    check_eq(wrong, 0, what + ": band matches the independent blit (first x " +
                           std::to_string(first_col) + ")");
}

int band_ink_of(const std::vector<std::uint32_t>& band) {
    int count = 0;
    for (const std::uint32_t pixel : band) {
        count += pixel == 0 ? 0 : 1;
    }
    return count;
}

/// Font 4's main face is (239,239,239) (`0xEFEFEFFF`): the only white on this
/// screen, so counting it locates the cursor row without trusting the module.
bool is_selected_white(std::uint32_t pixel) { return pixel == 0xEFEFEFFFu; }

int white_pixels(const Frame& frame, int y0, int y1) {
    int count = 0;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            count += is_selected_white(at(frame, x, y)) ? 1 : 0;
        }
    }
    return count;
}

Box white_box(const Frame& frame, int y0, int y1) {
    Box box;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (!is_selected_white(at(frame, x, y))) {
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

int count_colour(const Frame& frame, int y0, int y1, std::uint32_t colour) {
    int count = 0;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            count += at(frame, x, y) == colour ? 1 : 0;
        }
    }
    return count;
}

Box colour_box(const Frame& frame, int y0, int y1, std::uint32_t colour) {
    Box box;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (at(frame, x, y) != colour) {
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

std::set<std::uint32_t> ink_colours(const Frame& frame, int y0, int y1) {
    std::set<std::uint32_t> colours;
    for (int y = std::max(0, y0); y <= std::min(frame.height - 1, y1); ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t pixel = at(frame, x, y);
            if (!is_black(pixel)) {
                colours.insert(pixel);
            }
        }
    }
    return colours;
}

std::string join_colours(const std::set<std::uint32_t>& colours, std::size_t max = 4) {
    std::string out;
    std::size_t shown = 0;
    for (const std::uint32_t colour : colours) {
        if (shown != 0) {
            out += ",";
        }
        char buffer[11];
        std::snprintf(buffer, sizeof(buffer), "%08x", colour);
        out += buffer;
        if (++shown >= max) {
            out += "...";
            break;
        }
    }
    return out;
}

/// A `.key` file parsed here, independently of the module.
struct KeySetData {
    std::string name;
    std::array<std::uint32_t, 31> codes{};
    std::array<std::uint32_t, 31> seconds{};
    int bound = 0;
};

bool parse_key(const fs::path& path, KeySetData& out, std::string& why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        why = "cannot open " + path.string();
        return false;
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    if (bytes.size() != static_cast<std::size_t>(gm::kKeyFileSize)) {
        why = path.string() + " is " + std::to_string(bytes.size()) + " bytes";
        return false;
    }
    std::size_t length = 0;
    while (length < 16 && bytes[length] != 0) {
        ++length;
    }
    while (length > 0 && (bytes[length - 1] == ' ' || bytes[length - 1] == '\t')) {
        --length;
    }
    out.name.assign(reinterpret_cast<const char*>(bytes.data()), length);
    out.bound = 0;
    for (int slot = 0; slot < 31; ++slot) {
        const std::size_t base = 16u + static_cast<std::size_t>(slot) * 8u;
        out.codes[static_cast<std::size_t>(slot)] =
            static_cast<std::uint32_t>(bytes[base]) |
            (static_cast<std::uint32_t>(bytes[base + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[base + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes[base + 3]) << 24);
        out.seconds[static_cast<std::size_t>(slot)] =
            static_cast<std::uint32_t>(bytes[base + 4]) |
            (static_cast<std::uint32_t>(bytes[base + 5]) << 8) |
            (static_cast<std::uint32_t>(bytes[base + 6]) << 16) |
            (static_cast<std::uint32_t>(bytes[base + 7]) << 24);
        out.bound += out.codes[static_cast<std::size_t>(slot)] != 0 ? 1 : 0;
    }
    return true;
}

int title_x(const gm::FontAtlas& atlas, std::string_view name) {
    return std::max(0, (gm::kKeycfgPanelWidth - atlas.measure(name)) / 2);
}

int row_y(int i) { return gm::kKeycfgFirstRowY + gm::kKeycfgRowStep * i; }

/// Temporary root: the approved `/tmp/opencode` tree when present, else the
/// platform temp directory.  Only test scratch data is written there.
fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_keycfg_render";
    }
    return fs::temp_directory_path() / "gm_keycfg_render";
}

bool read_bytes(const fs::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
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

/// All ten painted rows compared with the independent blit.  Rows 1..9 use the
/// parsed profile and `Controls::key_name`; row 0 uses the literals of PERFECT
/// (`W`/`NONE`) so the test does not lean on either table of the module.
void check_rows(const Frame& frame, const std::map<int, gm::FontAtlas>& fonts,
                const KeySetData& set, const std::string& tag) {
    const int height = fonts.at(gm::kKeycfgRowFont).cell_height;
    for (int i = 0; i < gm::kKeycfgVisibleRows; ++i) {
        const std::string action(kActionLabels[static_cast<std::size_t>(i)]);
        std::string primary;
        std::string second;
        if (tag == "PERFECT" && i == 0) {
            primary = "W";
            second = "NONE";
        } else {
            primary = gm::Controls::key_name(static_cast<int>(set.codes[static_cast<std::size_t>(i)]));
            second =
                gm::Controls::key_name(static_cast<int>(set.seconds[static_cast<std::size_t>(i)]));
        }
        const std::vector<std::uint32_t> band =
            compose_row(fonts, action, primary, second, i == 0, gm::kKeycfgPanelWidth);
        check_band(frame, band, gm::kKeycfgPanelWidth, height, row_y(i),
                   tag + ": row " + std::to_string(i) + " at y " +
                       std::to_string(row_y(i)));
        const Box box =
            ink_box(frame, row_y(i), row_y(i) + height - 1);
        check_eq(box.x0, gm::kKeycfgActionX,
                 tag + ": row " + std::to_string(i) + " starts at the action column");
        check_eq(box.y0, row_y(i), tag + ": row " + std::to_string(i) + " y");
        check(box.x1 < gm::kKeycfgPanelWidth,
              tag + ": row " + std::to_string(i) + " fits the 196 px band");
    }
}

// --- the shipped PERFECT frame ---------------------------------------------
void test_install_frame(const fs::path& game, const fs::path& key_file,
                        const std::map<int, gm::FontAtlas>& fonts) {
    KeySetData set;
    std::string why;
    check(parse_key(key_file, set, why), "install: PERFECT parses: " + why);
    check_eq(set.name, "PERFECT", "install: profile name");
    check_eq(set.bound, 29, "install: 29 bound actions in PERFECT");
    check_eq(std::string(gm::keycfg_action_label(0)), "Move Forward",
             "install: action label 0 is the first name of 0x4bc558");
    check_eq(std::string(gm::keycfg_action_label(30)), "Linear Right",
             "install: action label 30 is the last name of 0x4bc558");
    check(gm::keycfg_action_label(-1).empty(), "install: no label for slot -1");
    check(gm::keycfg_action_label(31).empty(), "install: no label for slot 31");
    check_eq(fonts.at(gm::kKeycfgTitleFont).cell_height,
             fonts.at(gm::kKeycfgRowFont).cell_height,
             "install: the title face has the row cell height");
    check_eq(fonts.at(gm::kKeycfgSelectedFont).cell_height,
             fonts.at(gm::kKeycfgRowFont).cell_height,
             "install: the cursor face has the row cell height");

    const Frame frame = run(game, key_file, gm::KeycfgRenderOptions{});
    check(frame.ok, "install: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check(frame.error.empty(), "install: no error on success");
    check_eq(frame.width, 320, "install: width");
    check_eq(frame.height, 200, "install: height");
    check_eq(static_cast<long long>(frame.rgba.size()), 320 * 200, "install: buffer size");
    check_eq(frame.stats.rows, gm::kKeycfgRows, "install: stats.rows (0x67f10c = 31)");
    check_eq(frame.stats.named_actions, 29, "install: stats.named_actions");
    check(frame.stats.ink_pixels > 0, "install: ink > 0");
    check_eq(frame.stats.missing_glyphs, 0, "install: no missing glyphs");
    int opaque = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        opaque += (pixel & 0xFFu) == 0xFFu ? 1 : 0;
    }
    check_eq(opaque, 64000, "install: every pixel is opaque");

    const gm::FontAtlas& title_font = fonts.at(gm::kKeycfgTitleFont);
    const gm::FontAtlas& row_font = fonts.at(gm::kKeycfgRowFont);
    const gm::FontAtlas& selected_font = fonts.at(gm::kKeycfgSelectedFont);

    // Title: the profile name, font 5, centred at y 11 (`0x489872`..).
    const int title_pen = title_x(title_font, set.name);
    const std::vector<std::uint32_t> title_band =
        compose_band(title_font, set.name, title_pen, gm::kKeycfgPanelWidth,
                     title_font.cell_height);
    const Box title_expected =
        band_box(title_band, gm::kKeycfgPanelWidth, title_font.cell_height);
    check(!title_expected.empty(), "install: the title has glyph ink");
    check_band(frame, title_band, gm::kKeycfgPanelWidth, title_font.cell_height,
               gm::kKeycfgTitleY, "install: title");
    const Box title =
        ink_box(frame, gm::kKeycfgTitleY, gm::kKeycfgTitleY + title_font.cell_height - 1);
    check_eq(title.x0, title_expected.x0, "install: title x (centred)");
    check_eq(title.y0, gm::kKeycfgTitleY, "install: title y (row 11)");
    check_eq(title.width(), title_expected.width(), "install: title width");
    check_eq(title.count, title_expected.count, "install: title ink");

    // Hint: `Press Esc to close` (0x4bd7c8), font 0, centred at y 20.
    const int hint_pen = title_x(row_font, gm::kKeycfgHint);
    const std::vector<std::uint32_t> hint_band =
        compose_band(row_font, gm::kKeycfgHint, hint_pen, gm::kKeycfgPanelWidth,
                     row_font.cell_height);
    const Box hint_expected =
        band_box(hint_band, gm::kKeycfgPanelWidth, row_font.cell_height);
    check_band(frame, hint_band, gm::kKeycfgPanelWidth, row_font.cell_height,
               gm::kKeycfgHintY, "install: hint");
    const Box hint =
        ink_box(frame, gm::kKeycfgHintY, gm::kKeycfgHintY + row_font.cell_height - 1);
    check_eq(hint.x0, hint_expected.x0, "install: hint x (centred)");
    check_eq(hint.y0, gm::kKeycfgHintY, "install: hint y (row 20)");
    check_eq(hint.width(), hint_expected.width(), "install: hint width");

    // The ten bands, the first one against the literals `Move Forward`/`W`.
    check_rows(frame, fonts, set, "PERFECT");
    const Box first = ink_box(frame, row_y(0), row_y(0) + row_font.cell_height - 1);
    check_eq(first.y0, row_y(0), "install: the first band is at y 29");
    check_eq(first.x0, gm::kKeycfgActionX, "install: the first band starts at x 5");

    // Faces: title 5 dark red, rows 0 dark grey, cursor 4 white.
    const std::set<std::uint32_t> title_colours =
        ink_colours(frame, gm::kKeycfgTitleY,
                    gm::kKeycfgTitleY + title_font.cell_height - 1);
    check(title_colours.count(0x9B0F0FFFu) != 0, "install: the title uses font 5");
    const std::set<std::uint32_t> cursor_colours =
        ink_colours(frame, row_y(0), row_y(0) + selected_font.cell_height - 1);
    check(cursor_colours.count(0xEFEFEFFFu) != 0, "install: row 0 key uses font 4 (white)");
    const std::set<std::uint32_t> row_colours =
        ink_colours(frame, row_y(1), row_y(1) + row_font.cell_height - 1);
    check(row_colours.count(0x130F0FFFu) != 0, "install: row 1 uses font 0 (dark grey)");
    check(row_colours.count(0xEFEFEFFFu) == 0, "install: row 1 is not font 4");

    // The ink is exactly the ink of the twelve text bands (title, hint, ten
    // rows); the arrow is not glyph ink.  Every non-black pixel outside the
    // bands is the down `MENUARRW` at (159, 121).
    long long total_band_ink = band_ink_of(title_band) + band_ink_of(hint_band);
    for (int i = 0; i < gm::kKeycfgVisibleRows; ++i) {
        const std::string primary = i == 0
                                        ? std::string("W")
                                        : gm::Controls::key_name(
                                              static_cast<int>(set.codes[static_cast<std::size_t>(i)]));
        const std::string second =
            i == 0 ? std::string("NONE")
                   : gm::Controls::key_name(
                         static_cast<int>(set.seconds[static_cast<std::size_t>(i)]));
        total_band_ink += band_ink_of(compose_row(
            fonts, kActionLabels[static_cast<std::size_t>(i)], primary, second, i == 0,
            gm::kKeycfgPanelWidth));
    }
    check_eq(frame.stats.ink_pixels, total_band_ink, "install: ink is the sum of the bands");

    int outside = 0;
    for (int y = 0; y < frame.height; ++y) {
        const bool in_title = y >= gm::kKeycfgTitleY &&
                              y < gm::kKeycfgTitleY + title_font.cell_height;
        const bool in_hint = y >= gm::kKeycfgHintY &&
                             y < gm::kKeycfgHintY + row_font.cell_height;
        bool in_rows = false;
        for (int i = 0; i < gm::kKeycfgVisibleRows; ++i) {
            in_rows = in_rows || (y >= row_y(i) && y < row_y(i) + row_font.cell_height);
        }
        for (int x = 0; x < frame.width; ++x) {
            if (!is_black(at(frame, x, y)) && !in_title && !in_hint && !in_rows) {
                ++outside;
            }
        }
    }
    check_eq(outside, 41, "install: the only ink outside the bands is the 41-pixel arrow");
    const Box arrow = ink_box(frame, gm::kKeycfgArrowDownY,
                              gm::kKeycfgArrowDownY + gm::kKeycfgArrowHeight - 1);
    check_eq(arrow.count, 41, "install: 41 down-arrow pixels");
    check_eq(arrow.x0, 160, "install: down arrow bbox x0");
    check_eq(arrow.y0, 123, "install: down arrow bbox y0");
    check_eq(arrow.x1, 173, "install: down arrow bbox x1");
    check_eq(arrow.y1, 127, "install: down arrow bbox y1");
    check(ink_box(frame, gm::kKeycfgArrowUpY,
                  gm::kKeycfgArrowUpY + gm::kKeycfgArrowHeight - 1)
              .empty(),
          "install: no up arrow in the first window");

    // Determinism.
    const Frame again = run(game, key_file, gm::KeycfgRenderOptions{});
    check(again.ok, "install: second render");
    check_eq(differing_pixels(frame.rgba, again.rgba), 0, "install: deterministic");

    std::cout << "keycfg_render install: profile=" << set.name << " rows=" << frame.stats.rows
              << " named=" << frame.stats.named_actions << " ink=" << frame.stats.ink_pixels
              << " missing=" << frame.stats.missing_glyphs << "; title=(" << title.x0 << ","
              << title.y0 << ") w=" << title.width() << "; row0=(" << first.x0 << ","
              << first.y0 << ") w=" << first.width() << "; hint=(" << hint.x0 << ","
              << hint.y0 << ") w=" << hint.width() << "; outside=" << outside
              << " down-arrow=(" << arrow.x0 << "," << arrow.y0 << ")-(" << arrow.x1 << ","
              << arrow.y1 << ") colours title=[" << join_colours(title_colours)
              << "] cursor=[" << join_colours(cursor_colours) << "] row=["
              << join_colours(row_colours) << "]\n";
}

// --- cursor and the scroll window (M8.7 slice 9) ---------------------------
// `0x4898a8`..`0x4898d4`: `top = max(0, cursor - 5)`, clamped to `0..21`
// (`top + 10 < 31`); `0x48993d`..`0x489948`: the primary key of the band with
// `top + i == cursor` is font 4.  The table below is the literal consequence of
// those sites for the 31-row list, not a recomputation:
//
//   cursor  top  bands   highlight band  arrows
//     0      0   0..9    0               1 (down)
//     5      0   0..9    5               1 (down)
//     6      1   1..10   5               2
//    15     10  10..19   5               2
//    20     15  15..24   5               2
//    30     21  21..30   9               1 (up)
//
// Every band is compared byte for byte with an independent blit of the slot it
// shows, the white (font 4) bbox is checked to sit in the key column of the
// highlight band and nowhere else, and the arrow bands carry the 41-pixel
// sprite only when the listing says so.  Out-of-range cursors clamp and produce
// the frame of the end (0 or 30); the whole 0..30 range is swept against the
// `top`/arrow formula.
void test_cursor(const fs::path& game, const fs::path& key_file,
                 const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    KeySetData set;
    std::string why;
    check(parse_key(key_file, set, why), tag + "PERFECT parses: " + why);
    const int height = fonts.at(gm::kKeycfgRowFont).cell_height;

    struct Window {
        int cursor = 0;
        int first_row = 0;
        int arrows = 0;
    };
    const std::array<Window, 6> windows = {{
        {0, 0, 1}, {5, 0, 1}, {6, 1, 2}, {15, 10, 2}, {20, 15, 2}, {30, 21, 1},
    }};
    std::array<Frame, 6> frames;
    long long window_diffs[5] = {0, 0, 0, 0, 0};
    std::array<int, 6> highlight_white{};
    std::array<Box, 6> highlight_box{};
    std::array<int, 6> highlight_band{};

    for (std::size_t k = 0; k < windows.size(); ++k) {
        const Window want = windows[k];
        gm::KeycfgRenderOptions options;
        options.cursor = want.cursor;
        frames[k] = run(game, key_file, options);
        check(frames[k].ok, tag + "cursor " + std::to_string(want.cursor) +
                                 " renders: " + frames[k].error);
        if (!frames[k].ok) {
            return;
        }
        const Frame& frame = frames[k];
        check_eq(frame.stats.cursor, want.cursor, tag + "stats.cursor");
        check_eq(frame.stats.first_row, want.first_row, tag + "stats.first_row");
        check_eq(frame.stats.arrows, want.arrows, tag + "stats.arrows");
        check_eq(frame.stats.rows, gm::kKeycfgRows, tag + "stats.rows stays 31");
        const int band = want.cursor - want.first_row;
        highlight_band[k] = band;
        check(band >= 0 && band < gm::kKeycfgVisibleRows,
              tag + "the cursor band is on the canvas: band " + std::to_string(band));
        check_eq(frame.stats.first_row + band, want.cursor,
                 tag + "first_row + band == cursor");

        // The ten visible slots, each band against an independent blit.
        for (int i = 0; i < gm::kKeycfgVisibleRows; ++i) {
            const int slot = want.first_row + i;
            const std::size_t at_slot = static_cast<std::size_t>(slot);
            const std::string primary =
                gm::Controls::key_name(static_cast<int>(set.codes[at_slot]));
            const std::string second =
                gm::Controls::key_name(static_cast<int>(set.seconds[at_slot]));
            const std::vector<std::uint32_t> row = compose_row(
                fonts, kActionLabels[at_slot], primary, second, slot == want.cursor,
                gm::kKeycfgPanelWidth);
            check_band(frame, row, gm::kKeycfgPanelWidth, height, row_y(i),
                       tag + "cursor " + std::to_string(want.cursor) + " band " +
                           std::to_string(i) + " shows slot " + std::to_string(slot));
        }

        // Font 4 only on the cursor band, in the key column.
        highlight_white[k] = white_pixels(frame, row_y(band), row_y(band) + height - 1);
        highlight_box[k] = white_box(frame, row_y(band), row_y(band) + height - 1);
        check(highlight_white[k] > 0,
              tag + "cursor " + std::to_string(want.cursor) +
                  " has white ink on band " + std::to_string(band));
        check_eq(highlight_box[k].y0, row_y(band),
                 tag + "white bbox starts on the highlight band");
        check(highlight_box[k].x0 >= gm::kKeycfgKeyX,
              tag + "white bbox is in the key column");
        check(highlight_box[k].x1 < gm::kKeycfgPanelWidth,
              tag + "white bbox fits the 196 px band");
        for (int i = 0; i < gm::kKeycfgVisibleRows; ++i) {
            if (i == band) {
                continue;
            }
            check_eq(white_pixels(frame, row_y(i), row_y(i) + height - 1), 0,
                     tag + "cursor " + std::to_string(want.cursor) + " band " +
                         std::to_string(i) + " is not highlighted");
        }

        // Arrows: the bands are free of text, so their ink is the sprite.
        const Box up = ink_box(frame, gm::kKeycfgArrowUpY,
                               gm::kKeycfgArrowUpY + gm::kKeycfgArrowHeight - 1);
        const Box down = ink_box(frame, gm::kKeycfgArrowDownY,
                                 gm::kKeycfgArrowDownY + gm::kKeycfgArrowHeight - 1);
        const bool want_up = want.arrows == 2 || (want.arrows == 1 && want.first_row > 0);
        const bool want_down = want.arrows >= 1 && want.first_row < 21;
        if (want_up) {
            check_eq(up.count, 41, tag + "up arrow is the 41-pixel MENUARRW");
            check_eq(up.x0, 160, tag + "up arrow bbox x0");
            check_eq(up.y0, 3, tag + "up arrow bbox y0 (rotated at row 2)");
            check_eq(up.x1, 173, tag + "up arrow bbox x1");
            check_eq(up.y1, 7, tag + "up arrow bbox y1");
        } else {
            check(up.empty(), tag + "no up arrow at the top of the list");
        }
        if (want_down) {
            check_eq(down.count, 41, tag + "down arrow is the 41-pixel MENUARRW");
            check_eq(down.x0, 160, tag + "down arrow bbox x0");
            check_eq(down.y0, 123, tag + "down arrow bbox y0");
            check_eq(down.x1, 173, tag + "down arrow bbox x1");
            check_eq(down.y1, 127, tag + "down arrow bbox y1");
        } else {
            check(down.empty(), tag + "no down arrow at the end of the list");
        }

        std::printf("  keycfg_render cursor %2d: top=%2d slots %2d..%2d band %d "
                    "white=%2d bbox=(%d,%d)-(%d,%d) arrows=%d up=%d down=%d\n",
                    want.cursor, want.first_row, want.first_row,
                    want.first_row + gm::kKeycfgVisibleRows - 1, band,
                    highlight_white[k], highlight_box[k].x0, highlight_box[k].y0,
                    highlight_box[k].x1, highlight_box[k].y1, want.arrows, up.count,
                    down.count);
    }

    // Neighbouring cursors are distinguishable frames (window shift and/or the
    // moved white key), not the same picture with a different stats value.
    for (std::size_t k = 0; k + 1 < windows.size(); ++k) {
        window_diffs[k] = differing_pixels(frames[k].rgba, frames[k + 1].rgba);
        check(window_diffs[k] > 0,
              tag + "cursors " + std::to_string(windows[k].cursor) + " and " +
                  std::to_string(windows[k + 1].cursor) + " differ");
    }
    // Cursor 5 and 6 share the first window (top 0 and 1) but keep the cursor
    // on band 5: the white row moves not at all until the window scrolls.
    check_eq(highlight_box[1].y0, highlight_box[2].y0,
             tag + "cursor 5 and 6 highlight band 5 (same y)");

    // The default options are the cursor-0 frame: the additions of slice 9 must
    // not move a byte of the untouched first window.
    {
        const Frame plain = run(game, key_file, gm::KeycfgRenderOptions{});
        check(plain.ok, tag + "default options render: " + plain.error);
        if (plain.ok) {
            check_eq(plain.stats.cursor, 0, tag + "default cursor is 0");
            check_eq(plain.stats.first_row, 0, tag + "default first_row is 0");
            check_eq(plain.stats.arrows, 1, tag + "default frame has one arrow");
            check_eq(differing_pixels(plain.rgba, frames[0].rgba), 0,
                     tag + "default frame equals cursor 0");
        }
    }

    // Whole range 0..30 against the literal formula of `0x4898a8`..`0x4898d4`.
    int range_bad = 0;
    for (int cursor = 0; cursor <= gm::kKeycfgLastRow; ++cursor) {
        int top = cursor < 5 ? 0 : cursor - 5;
        if (top + gm::kKeycfgVisibleRows >= gm::kKeycfgRows) {
            top = 21;
        }
        const int want_arrows = (top > 0 ? 1 : 0) +
                                (top + gm::kKeycfgVisibleRows < gm::kKeycfgRows ? 1 : 0);
        gm::KeycfgRenderOptions options;
        options.cursor = cursor;
        const Frame frame = run(game, key_file, options);
        if (!frame.ok || frame.stats.cursor != cursor || frame.stats.first_row != top ||
            frame.stats.arrows != want_arrows) {
            ++range_bad;
            check(false, tag + "range cursor " + std::to_string(cursor) + ": got top " +
                               std::to_string(frame.stats.first_row) + "/arrows " +
                               std::to_string(frame.stats.arrows) + ", want " +
                               std::to_string(top) + "/" + std::to_string(want_arrows));
        }
    }
    check_eq(range_bad, 0, tag + "0..30 all follow top = clamp(max(0, c-5), 0, 21)");

    // Out-of-range cursors are clamped, not refused.
    {
        gm::KeycfgRenderOptions below;
        below.cursor = -7;
        const Frame frame = run(game, key_file, below);
        check(frame.ok, tag + "cursor -7 is not a refusal: " + frame.error);
        if (frame.ok) {
            check_eq(frame.stats.cursor, 0, tag + "cursor -7 clamps to 0");
            check_eq(frame.stats.first_row, 0, tag + "cursor -7 keeps top 0");
            check_eq(differing_pixels(frame.rgba, frames[0].rgba), 0,
                     tag + "cursor -7 paints the cursor-0 frame");
        }
        gm::KeycfgRenderOptions above;
        above.cursor = 31;
        const Frame frame31 = run(game, key_file, above);
        check(frame31.ok, tag + "cursor 31 is not a refusal: " + frame31.error);
        if (frame31.ok) {
            check_eq(frame31.stats.cursor, 30, tag + "cursor 31 clamps to 30");
            check_eq(frame31.stats.first_row, 21, tag + "cursor 31 keeps top 21");
            check_eq(frame31.stats.arrows, 1, tag + "cursor 31 has the up arrow only");
            check_eq(differing_pixels(frame31.rgba, frames[5].rgba), 0,
                     tag + "cursor 31 paints the cursor-30 frame");
        }
        above.cursor = 99;
        const Frame far = run(game, key_file, above);
        check(far.ok, tag + "cursor 99 renders: " + far.error);
        if (far.ok) {
            check_eq(far.stats.cursor, 30, tag + "cursor 99 clamps to 30");
            check_eq(differing_pixels(far.rgba, frames[5].rgba), 0,
                     tag + "cursor 99 paints the cursor-30 frame");
        }
    }

    // Determinism of a scrolled frame.
    {
        gm::KeycfgRenderOptions options;
        options.cursor = 15;
        const Frame again = run(game, key_file, options);
        check(again.ok, tag + "second cursor-15 render: " + again.error);
        if (again.ok) {
            check_eq(differing_pixels(again.rgba, frames[3].rgba), 0,
                     tag + "cursor 15 is deterministic");
        }
    }

    std::printf("  keycfg_render window: diffs 0/5 %lld, 5/6 %lld, 6/15 %lld, "
                "15/20 %lld, 20/30 %lld; range 0..30 bad=%d; clamps -7->0, "
                "31/99->30; determinism 0\n",
                window_diffs[0], window_diffs[1], window_diffs[2], window_diffs[3],
                window_diffs[4], range_bad);
}

// --- the three shipped profiles --------------------------------------------
void test_profiles(const fs::path& game, const fs::path& keycfg,
                   const std::map<int, gm::FontAtlas>& fonts) {
    const fs::path perfect = keycfg / "PERFECT.key";
    const fs::path split1 = keycfg / "SPLIT1.KEY";
    const fs::path split2 = keycfg / "SPLIT2.KEY";

    KeySetData one;
    KeySetData two;
    std::string why;
    check(parse_key(split1, one, why), "profiles: SPLIT1 parses: " + why);
    check(parse_key(split2, two, why), "profiles: SPLIT2 parses: " + why);
    check_eq(one.name, "SPLIT1", "profiles: SPLIT1 name");
    check_eq(two.name, "SPLIT2", "profiles: SPLIT2 name");
    check_eq(one.bound, 19, "profiles: SPLIT1 has 19 bound actions");
    check_eq(two.bound, 19, "profiles: SPLIT2 has 19 bound actions");

    const Frame f_perfect = run(game, perfect, gm::KeycfgRenderOptions{});
    const Frame f_one = run(game, split1, gm::KeycfgRenderOptions{});
    const Frame f_two = run(game, split2, gm::KeycfgRenderOptions{});
    check(f_perfect.ok && f_one.ok && f_two.ok, "profiles: all three frames render");
    if (!f_perfect.ok || !f_one.ok || !f_two.ok) {
        return;
    }
    check_eq(f_one.stats.rows, 31, "profiles: SPLIT1 rows 31");
    check_eq(f_two.stats.rows, 31, "profiles: SPLIT2 rows 31");
    check_eq(f_one.stats.named_actions, 19, "profiles: SPLIT1 named_actions 19");
    check_eq(f_two.stats.named_actions, 19, "profiles: SPLIT2 named_actions 19");
    check_eq(f_one.stats.missing_glyphs, 0, "profiles: SPLIT1 no missing glyphs");
    check_eq(f_two.stats.missing_glyphs, 0, "profiles: SPLIT2 no missing glyphs");
    check_rows(f_one, fonts, one, "SPLIT1");
    check_rows(f_two, fonts, two, "SPLIT2");

    const long long split_diff = differing_pixels(f_one.rgba, f_two.rgba);
    check(split_diff > 0, "profiles: SPLIT1 and SPLIT2 give different frames");
    const long long perfect_diff = differing_pixels(f_perfect.rgba, f_one.rgba);
    check(perfect_diff > 0, "profiles: PERFECT and SPLIT1 give different frames");

    // A frame where the profile name is 16 spaces must differ only in the
    // title band (y 11..18), by exactly the title's glyph ink.
    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    check(!ec, "profiles: temp root created: " + ec.message());
    long long name_diffs[3] = {0, 0, 0};
    int name_outside[3] = {0, 0, 0};
    long long name_ink[3] = {0, 0, 0};
    const fs::path sources[3] = {perfect, split1, split2};
    const Frame frames[3] = {f_perfect, f_one, f_two};
    for (int i = 0; i < 3; ++i) {
        std::vector<std::uint8_t> bytes;
        check(read_bytes(sources[i], bytes), "profiles: read " + sources[i].filename().string());
        check_eq(static_cast<long long>(bytes.size()), 376,
                 "profiles: " + sources[i].filename().string() + " is 376 bytes");
        if (bytes.size() != 376) {
            continue;
        }
        for (std::size_t k = 0; k < 16; ++k) {
            bytes[k] = ' ';
        }
        const fs::path patched =
            root / ("blank-" + sources[i].filename().string());
        check(write_bytes(patched, bytes), "profiles: write " + patched.filename().string());
        const Frame blank = run(game, patched, gm::KeycfgRenderOptions{});
        check(blank.ok, "profiles: the blank-name frame renders: " + blank.error);
        if (!blank.ok) {
            continue;
        }
        name_diffs[i] = differing_pixels(frames[i].rgba, blank.rgba);
        for (int y = 0; y < frames[i].height; ++y) {
            for (int x = 0; x < frames[i].width; ++x) {
                if (at(frames[i], x, y) == at(blank, x, y)) {
                    continue;
                }
                if (y < gm::kKeycfgTitleY ||
                    y >= gm::kKeycfgTitleY + fonts.at(gm::kKeycfgTitleFont).cell_height) {
                    ++name_outside[i];
                }
            }
        }
        name_ink[i] = ink_box(frames[i], gm::kKeycfgTitleY,
                              gm::kKeycfgTitleY +
                                  fonts.at(gm::kKeycfgTitleFont).cell_height - 1)
                          .count;
        check(name_diffs[i] > 0,
              "profiles: " + sources[i].filename().string() + " name is visible");
        check_eq(name_outside[i], 0,
                 "profiles: " + sources[i].filename().string() +
                     " name differences stay in the title band");
        check_eq(name_diffs[i], name_ink[i],
                 "profiles: " + sources[i].filename().string() +
                     " name differences equal the title ink");
        check(ink_box(blank, gm::kKeycfgTitleY,
                      gm::kKeycfgTitleY +
                          fonts.at(gm::kKeycfgTitleFont).cell_height - 1)
                  .empty(),
              "profiles: the blank-name frame has no title ink");
    }
    fs::remove_all(root, ec);

    std::cout << "keycfg_render profiles: SPLIT1 vs SPLIT2 -> " << split_diff
              << " differing pixels; PERFECT vs SPLIT1 -> " << perfect_diff
              << "; blank-name diffs PERFECT/SPLIT1/SPLIT2 = " << name_diffs[0] << "/"
              << name_diffs[1] << "/" << name_diffs[2] << " (title ink " << name_ink[0]
              << "/" << name_ink[1] << "/" << name_ink[2] << ", outside title band "
              << name_outside[0] << "/" << name_outside[1] << "/" << name_outside[2]
              << ")\n";
}

// --- PLATE as the background ------------------------------------------------
void test_plate(const fs::path& game, const fs::path& key_file) {
    const Frame black = run(game, key_file, gm::KeycfgRenderOptions{});
    check(black.ok, "plate: the black frame renders");
    if (!black.ok) {
        return;
    }
    gm::KeycfgRenderOptions options;
    options.background = std::string(gm::kKeycfgPanelName);
    const Frame frame = run(game, key_file, options);
    check(frame.ok, "plate: renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels, black.stats.ink_pixels,
             "plate: the text ink does not change");
    check_eq(frame.stats.rows, black.stats.rows, "plate: rows do not change");
    check_eq(frame.stats.named_actions, black.stats.named_actions,
             "plate: named_actions do not change");
    check_eq(frame.stats.missing_glyphs, black.stats.missing_glyphs,
             "plate: missing_glyphs do not change");

    // Independent decode of the panel.
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    check(archive.has_value(), "plate: BMP.IDX opens: " + error);
    if (!archive) {
        return;
    }
    const gm::BigEntry* plate_entry = nullptr;
    for (const gm::BigEntry& entry : archive->entries()) {
        if (entry.name == "PLATE") {
            plate_entry = &entry;
            break;
        }
    }
    check(plate_entry != nullptr, "plate: PLATE is in BMP.IDX");
    if (plate_entry == nullptr) {
        return;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*plate_entry, error);
    check(bytes.has_value(), "plate: PLATE reads: " + error);
    if (!bytes) {
        return;
    }
    std::optional<gm::BmpImage> image = gm::parse_bmp(*bytes, error);
    check(image.has_value(), "plate: PLATE parses: " + error);
    if (!image) {
        return;
    }
    check_eq(image->width, gm::kKeycfgPanelWidth, "plate: 196 wide");
    check_eq(image->height, gm::kKeycfgPanelHeight, "plate: 131 high");

    int panel_pixels = 0;
    int band_pixels = 0;
    int wrong = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t got = at(frame, x, y);
            if (x >= image->width || y >= image->height) {
                ++panel_pixels;
                wrong += is_black(got) ? 0 : 1;
                continue;
            }
            if (!is_black(at(black, x, y))) {
                ++band_pixels;
                wrong += got == at(black, x, y) ? 0 : 1;
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(image->width) +
                static_cast<std::size_t>(x);
            const std::uint8_t palette_index = image->pixels[index];
            const std::size_t base = static_cast<std::size_t>(palette_index) * 4u;
            const std::uint32_t want =
                (static_cast<std::uint32_t>(image->palette[base + 0]) << 24) |
                (static_cast<std::uint32_t>(image->palette[base + 1]) << 16) |
                (static_cast<std::uint32_t>(image->palette[base + 2]) << 8) | 0xFFu;
            ++band_pixels;
            wrong += got == want ? 0 : 1;
        }
    }
    check_eq(wrong, 0, "plate: every pixel is the panel or the unchanged text");
    check_eq(panel_pixels, 320 * 200 - 196 * 131,
             "plate: pixels outside the band stay black");
    check_eq(band_pixels, 196 * 131, "plate: the band is covered");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::KeySetView)),
             std::string(gm::kKeycfgPanelName), "plate: screen 30's panel is PLATE");
    const long long changed = differing_pixels(black.rgba, frame.rgba);
    check(changed > 0, "plate: the background changes pixels");
    std::cout << "keycfg_render plate: 196x131=" << 196 * 131 << " panel, outside="
              << 320 * 200 - 196 * 131 << " black, ink=" << frame.stats.ink_pixels
              << " (unchanged), differing pixels vs black=" << changed
              << ", mismatches=0\n";
}

// --- refusals ---------------------------------------------------------------
void check_refused(const fs::path& game, const fs::path& key_file,
                   const gm::KeycfgRenderOptions& options, const std::string& what,
                   std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::KeycfgRenderStats stats;
    stats.rows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.named_actions = 7;
    stats.cursor = 7;
    stats.first_row = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_keycfg(game, key_file, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "keycfg_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error +
              "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.named_actions, 0, what + ": named_actions cleared");
    check_eq(stats.cursor, 0, what + ": cursor cleared");
    check_eq(stats.first_row, 0, what + ": first_row cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
    std::cout << "keycfg_render negative (" << what << "): " << error << "\n";
}

void test_negatives(const fs::path& game, const fs::path& key_file) {
    check_refused(game, fs::path{}, gm::KeycfgRenderOptions{}, "empty key path",
                  "no key file");
    if (key_file.empty()) {
        // No installation: only the profile-path contract can be exercised.
        return;
    }
    check_refused(game, fs::path("/nonexistent/PERFECT.key"), gm::KeycfgRenderOptions{},
                  "missing key file", "cannot read");
    check_refused(fs::path{}, key_file, gm::KeycfgRenderOptions{}, "empty game path",
                  "no game directory");
    if (game.empty()) {
        return;
    }

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }

    // 375 and 377 bytes: the loader's exact 0x178 check.
    std::vector<std::uint8_t> bytes;
    check(read_bytes(key_file, bytes), "negatives: the real profile reads");
    if (bytes.size() == 376) {
        std::vector<std::uint8_t> short_copy(bytes.begin(), bytes.end() - 1);
        check(write_bytes(root / "short.key", short_copy), "negatives: 375-byte copy written");
        check_refused(game, root / "short.key", gm::KeycfgRenderOptions{}, "375-byte profile",
                      "want 376");
        std::vector<std::uint8_t> long_copy = bytes;
        long_copy.push_back(0);
        check(write_bytes(root / "long.key", long_copy), "negatives: 377-byte copy written");
        check_refused(game, root / "long.key", gm::KeycfgRenderOptions{}, "377-byte profile",
                      "want 376");
    }

    // No FRONTEND/BMP.IDX: the atlases cannot be built.
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: BMP-less tree created");
    check_refused(root / "game-without-bmp", key_file, gm::KeycfgRenderOptions{},
                  "no BMP.IDX", "BMP.IDX");

    // A valid BIGFILE index whose only entry is FONTS1: the three faces of this
    // screen all come from FONTS4 (fonts 0/4/5), and the title face 5 is loaded
    // first, so that entry is the missing one.
    const fs::path fontless = root / "game-without-font4" / "FRONTEND";
    fs::create_directories(fontless, ec);
    check(!ec, "negatives: fontless tree created");
    if (!ec) {
        std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                         static_cast<std::uint8_t>(0x1A)};
        put32(idx, 1);
        std::string name = "FONTS1";
        name.resize(8, '\0');
        idx.insert(idx.end(), name.begin(), name.end());
        put32(idx, 0);
        check(write_bytes(fontless / "BMP.IDX", idx),
              "negatives: synthetic BMP.IDX written");
        check(write_bytes(fontless / "BMP.DAT", {}),
              "negatives: synthetic BMP.DAT written");
        check_refused(root / "game-without-font4", key_file, gm::KeycfgRenderOptions{},
                      "missing FONTS4", "FONTS4");
    }

    fs::remove_all(root, ec);

    // A background name that is not in the archive is a text, not black.
    gm::KeycfgRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, key_file, bad_background, "unknown background", "NOSUCH");
}

// M8.7 slice 8: the exact fade tables of `0x682f90` in the palettised path
// (docs/KEYCFG_RENDER.md §6, the same layer as `ui/pager_render.cpp`).
//
// The palette index of every painted pixel (PLATE, MENUARRW, glyphs) goes
// through `render/hud_lut.h::map` **before** the palette lookup, so the table
// sees the index the engine sees, not a colour.  The table is built in memory
// from the installed `FRONTEND/GAMEPAL` by `build_hud_lut` - no dump is read.
// Pinned here on the shipped `PERFECT.key` frame:
//
//   * level 11 (the identity of `0x483290(dst, 11/11, 1)`) is byte-identical to
//     the unfaded frame: 0 differing pixels out of 64000;
//   * level 0 is *not* an all-black frame: the table leaves the slots outside
//     the palette ramps alone, so the black count is printed and the frame
//     differs from the unfaded one;
//   * levels 12..15 are four different frames (this screen has no RGBA model
//     path, so there is nothing to clamp them to 11);
//   * the canvas outside the 196x131 PLATE has no palette index and stays black
//     at every level;
//   * no table and no dump is a `keycfg_render:` text, an unreadable dump too,
//     and a pre-loaded table wins over the dump; level 99 clamps to 15.
void test_fade_lut(const fs::path& game, const fs::path& key_file) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas =
        static_cast<long long>(gm::kKeycfgCanvasWidth) * gm::kKeycfgCanvasHeight;

    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  fade LUT: SKIP (no 1024-byte GAMEPAL: " << palette_error
                  << ")\n";
        return;
    }
    gm::HudLut lut;
    std::string error;
    if (!gm::build_hud_lut(*palette, lut, &error)) {
        check(false, tag + "build_hud_lut: " + error);
        return;
    }
    check(error.empty(), tag + "build_hud_lut leaves no error");
    std::printf("  fade LUT: 16x256 built in memory from FRONTEND/GAMEPAL, no dump\n");

    const auto make = [&](bool fade, int level) {
        gm::KeycfgRenderOptions options;
        options.background = std::string(gm::kKeycfgPanelName);
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, key_file, options);
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
                if (x < gm::kKeycfgPanelWidth && y < gm::kKeycfgPanelHeight) {
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
    check_eq(static_cast<long long>(plain.rgba.size()), kCanvas,
             tag + "unfaded canvas 64000");

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
    check_eq(identity.stats.named_actions, plain.stats.named_actions,
             tag + "level 11 bound actions unchanged");
    check_eq(outside_panel(identity), 0, tag + "level 11 leaves the canvas black");

    // 2. Level 0: counted black pixels and a different frame.
    const Frame level0 = make(true, 0);
    check(level0.ok, tag + "level 0 frame: " + level0.error);
    if (!level0.ok) {
        return;
    }
    const long long black0 = black_pixels(level0);
    const long long black_plain = black_pixels(plain);
    const long long diff0 = differing_pixels(level0.rgba, plain.rgba);
    check(black0 > 0, tag + "level 0 still has black pixels to count");
    check(diff0 > 0, tag + "level 0 differs from the unfaded frame");
    check_eq(outside_panel(level0), 0, tag + "level 0 leaves the canvas black");
    check_eq(level0.stats.ink_pixels, plain.stats.ink_pixels,
             tag + "level 0 ink unchanged");

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
        check_eq(differing_pixels(clamp99.rgba, exact[3].rgba), 0,
                 tag + "level 99 clamps to 15");
    }

    std::printf("  keycfg_render fade: level 11 diffs %lld/%lld; level 0 black "
                "%lld (plain %lld), diffs %lld; adjacent 12/13 %lld, 13/14 %lld, "
                "14/15 %lld; vs 11 %lld/%lld/%lld/%lld\n",
                identity_diffs, kCanvas, black0, black_plain, diff0, pair12_13,
                pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);

    // 4. Negatives: no table and no dump, an unreadable dump, and the table
    // winning over the dump (the dump path is the CLI's option).
    {
        gm::KeycfgRenderOptions options;
        options.background = std::string(gm::kKeycfgPanelName);
        options.fade_lut = true;
        const Frame refusal = run(game, key_file, options);
        check(!refusal.ok, tag + "no table and no dump refused");
        check(starts_with(refusal.error, "keycfg_render: ") &&
                  contains(refusal.error, "fade LUT needs"),
              tag + "refusal names the missing table: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "refusal clears the buffer");
        check_eq(refusal.width, 0, tag + "refusal zeroes the width");
        check_eq(refusal.height, 0, tag + "refusal zeroes the height");
        check_eq(refusal.stats.ink_pixels, 0, tag + "refusal zeroes the stats");
    }
    {
        gm::KeycfgRenderOptions options;
        options.background = std::string(gm::kKeycfgPanelName);
        options.fade_lut = true;
        options.fade_lut_dump =
            (temp_root() / "no-lut.bin").string();
        const Frame refusal = run(game, key_file, options);
        check(!refusal.ok, tag + "missing dump refused");
        check(starts_with(refusal.error, "keycfg_render: ") &&
                  contains(refusal.error, "hud_lut:"),
              tag + "missing dump carries the hud_lut text: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "missing dump clears the buffer");
    }
    {
        gm::KeycfgRenderOptions options;
        options.background = std::string(gm::kKeycfgPanelName);
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        options.fade_lut_dump =
            (temp_root() / "no-lut.bin").string();
        const Frame both = run(game, key_file, options);
        check(both.ok, tag + "a table wins over a missing dump: " + both.error);
        if (both.ok) {
            check_eq(differing_pixels(both.rgba, plain.rgba), 0,
                     tag + "table path equals the unfaded level 11 frame");
        }
    }
}

// --- editor screens 34/35 (M8.7 slice 10, `0x4892d0`) ----------------------
// The editor shares the whole frame of screen 30 (same 31/8 geometry, title at
// y 11, ten bands at y 29) and differs in exactly three things, all from the
// listing: the hint `Enter changes, Delete clears` (0x4bd7a8, row 20), the
// highlight of the cursor row (one cell, face from `[0x4bc440]`: 35 -> font 5,
// the one-frame alias 34 -> font 4, cell from `[0x68cd80]`) and the conflict
// line `%s has been used twice` (0x4bd790) / `%s is used by other player`
// (0x4bd774) at y 109, font 5.
void test_editor(const fs::path& game, const fs::path& key_file,
                 const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "editor: ";
    const int height = fonts.at(gm::kKeycfgRowFont).cell_height;
    const std::uint32_t white = 0xEFEFEFFFu; ///< font 4 main face
    const std::uint32_t red = 0x9B0F0FFFu;   ///< font 5 main face
    const int hint_top = gm::kKeycfgHintY;
    const int hint_bottom = gm::kKeycfgHintY + height - 1;
    const int row0 = gm::kKeycfgFirstRowY;
    const int row0_bottom = row0 + height - 1;
    const int last_row =
        gm::kKeycfgFirstRowY + gm::kKeycfgRowStep * (gm::kKeycfgVisibleRows - 1);
    const int conflict_top = gm::kKeycfgConflictY;
    const int conflict_bottom = conflict_top + height - 1;

    const auto band_diffs = [&](const Frame& a, const Frame& b, int y0, int y1) {
        long long count = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = 0; x < a.width; ++x) {
                count += at(a, x, y) != at(b, x, y) ? 1 : 0;
            }
        }
        return count;
    };
    const auto outside_ink = [](const Frame& frame) {
        int count = 0;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                if (x < gm::kKeycfgPanelWidth && y < gm::kKeycfgPanelHeight) {
                    continue;
                }
                count += is_black(at(frame, x, y)) ? 0 : 1;
            }
        }
        return count;
    };

    KeySetData set;
    std::string why;
    check(parse_key(key_file, set, why), tag + "PERFECT parses: " + why);

    // 1. `view` is the default and stays the slice 7/9 frame byte for byte.
    gm::KeycfgRenderOptions view_options;
    const Frame view = run(game, key_file, view_options);
    check(view.ok, tag + "view frame: " + view.error);
    if (!view.ok) {
        return;
    }
    gm::KeycfgRenderOptions explicit_view = view_options;
    explicit_view.mode = gm::KeycfgMode::view;
    const Frame view_again = run(game, key_file, explicit_view);
    check(view_again.ok, tag + "explicit view frame: " + view_again.error);
    check_eq(differing_pixels(view.rgba, view_again.rgba), 0,
             tag + "an explicit view is the default frame (0 differing pixels)");
    check(view.stats.mode == gm::KeycfgMode::view, tag + "default stats.mode is view");
    check_eq(view.stats.conflict_rows, 0, tag + "view reports no conflict row");
    const gm::FontAtlas& hint_face = fonts.at(gm::kKeycfgRowFont);
    const int view_hint_x = title_x(hint_face, gm::kKeycfgHint);
    check_band(view, compose_band(hint_face, gm::kKeycfgHint, view_hint_x,
                                 gm::kKeycfgPanelWidth, height),
               gm::kKeycfgPanelWidth, height, hint_top, tag + "view hint band");

    // The conflict text is a property of the editor; in `view` it is ignored.
    gm::KeycfgRenderOptions view_conflict = view_options;
    view_conflict.conflict = "MOVE FORWARD has been used twice";
    const Frame view_with_conflict = run(game, key_file, view_conflict);
    check(view_with_conflict.ok, tag + "view + conflict: " + view_with_conflict.error);
    check_eq(differing_pixels(view.rgba, view_with_conflict.rgba), 0,
             tag + "a conflict never changes the view frame");
    check_eq(view_with_conflict.stats.conflict_rows, 0,
             tag + "view still reports conflict_rows 0");

    // 2. `edit` (screen 35, primary cell): hint, one red cell, no conflict yet.
    gm::KeycfgRenderOptions edit_options;
    edit_options.mode = gm::KeycfgMode::edit;
    const Frame edit = run(game, key_file, edit_options);
    check(edit.ok, tag + "edit frame: " + edit.error);
    if (!edit.ok) {
        return;
    }
    check(edit.stats.mode == gm::KeycfgMode::edit, tag + "stats.mode is edit");
    check_eq(edit.stats.conflict_rows, 0, tag + "edit without a text reports 0 rows");
    const long long view_edit = differing_pixels(view.rgba, edit.rgba);
    check(view_edit > 0, tag + "edit differs from view");
    check_eq(differing_pixels_outside(view, edit, hint_top, last_row + height - 1), 0,
             tag + "edit vs view: nothing changes outside hint+rows");

    // 3. The hint: `Enter changes, Delete clears` (0x4bd7a8), font 0, centred.
    const Box view_hint = ink_box(view, hint_top, hint_bottom);
    const Box edit_hint = ink_box(edit, hint_top, hint_bottom);
    check(!view_hint.empty() && !edit_hint.empty(), tag + "both hints have ink");
    check(view_hint.width() != edit_hint.width(), tag + "the hint text changed");
    const int edit_hint_x = title_x(hint_face, gm::kKeycfgHintEdit);
    check_eq(edit_hint.x0, edit_hint_x, tag + "the edit hint is centred in 196 px");
    check_band(edit, compose_band(hint_face, gm::kKeycfgHintEdit, edit_hint_x,
                                 gm::kKeycfgPanelWidth, height),
               gm::kKeycfgPanelWidth, height, hint_top, tag + "edit hint band");
    const long long hint_diffs = band_diffs(view, edit, hint_top, hint_bottom);
    check(hint_diffs > 0, tag + "the hint band differs from view");

    // 4. The cursor cell of screen 35 uses font 5 (red), not font 4 (white).
    const std::vector<std::uint32_t> edit_row0 =
        compose_row_faces(fonts, kActionLabels[0], "W", gm::kKeycfgEditCursorFont,
                          "NONE", gm::kKeycfgRowFont, gm::kKeycfgPanelWidth);
    check_band(edit, edit_row0, gm::kKeycfgPanelWidth, height, row0,
               tag + "edit row 0 (font-5 cell, font-0 second code)");
    check_eq(count_colour(edit, row0, row0_bottom, white), 0,
             tag + "edit row 0 has no font-4 face on screen 35");
    const Box edit_red = colour_box(edit, row0, row0_bottom, red);
    check(edit_red.count > 0, tag + "edit row 0 highlights with font 5");
    check(edit_red.x0 >= gm::kKeycfgKeyX && edit_red.x1 < gm::kKeycfgSecondKeyX,
          tag + "the red cell is the x 108 column");
    const long long row0_diffs = band_diffs(view, edit, row0, row0_bottom);
    check(row0_diffs > 0, tag + "the cursor cell differs from view");

    // 5. The one-frame alias (screen 34, `0x48e212`) keeps font 4.
    gm::KeycfgRenderOptions alias_options = edit_options;
    alias_options.edit_alias_frame = true;
    const Frame alias = run(game, key_file, alias_options);
    check(alias.ok, tag + "alias frame: " + alias.error);
    if (!alias.ok) {
        return;
    }
    check_eq(band_diffs(alias, view, row0, row0_bottom), 0,
             tag + "screen 34 keeps the font-4 cell of view");
    check_eq(count_colour(alias, row0, row0_bottom, red), 0,
             tag + "screen 34 has no font-5 cell");
    check(count_colour(alias, row0, row0_bottom, white) > 0,
          tag + "screen 34 keeps the white cell");
    check_eq(band_diffs(alias, edit, hint_top, hint_bottom), 0,
             tag + "the alias keeps the edit hint");
    const long long alias_edit = differing_pixels(alias.rgba, edit.rgba);
    check(alias_edit > 0, tag + "the alias differs from the steady editor");
    check_eq(differing_pixels_outside(alias, edit, row0, row0_bottom), 0,
             tag + "alias vs edit: only the cursor band differs");

    // 6. The second cell (`[0x68cd80] != 0`) moves the red face to x 154.
    gm::KeycfgRenderOptions second_options = edit_options;
    second_options.edit_cell = gm::KeycfgCell::second;
    const Frame second_cell = run(game, key_file, second_options);
    check(second_cell.ok, tag + "second-cell frame: " + second_cell.error);
    if (!second_cell.ok) {
        return;
    }
    const std::vector<std::uint32_t> second_row0 =
        compose_row_faces(fonts, kActionLabels[0], "W", gm::kKeycfgRowFont, "NONE",
                          gm::kKeycfgEditCursorFont, gm::kKeycfgPanelWidth);
    check_band(second_cell, second_row0, gm::kKeycfgPanelWidth, height, row0,
               tag + "second-cell row 0 (font-5 second code)");
    const Box second_red = colour_box(second_cell, row0, row0_bottom, red);
    check(second_red.count > 0, tag + "the second cell uses font 5");
    check(second_red.x0 >= gm::kKeycfgSecondKeyX, tag + "the red cell is the x 154 column");
    check_eq(count_colour(second_cell, row0, row0_bottom, white), 0,
             tag + "the primary cell of the second cell frame is font 0");
    const long long cell_diffs = differing_pixels(edit.rgba, second_cell.rgba);
    check(cell_diffs > 0, tag + "the two cells give different frames");
    check_eq(differing_pixels_outside(second_cell, edit, row0, row0_bottom), 0,
             tag + "cell switch only changes the cursor band");

    // 7. The conflict line: font 5, centred, y 109 (`0x489563`/`0x489573`).
    const std::string conflict = "MOVE FORWARD has been used twice";
    const gm::FontAtlas& red_face = fonts.at(gm::kKeycfgConflictFont);
    gm::KeycfgRenderOptions conflict_options = edit_options;
    conflict_options.conflict = conflict;
    const Frame with_conflict = run(game, key_file, conflict_options);
    check(with_conflict.ok, tag + "conflict frame: " + with_conflict.error);
    if (!with_conflict.ok) {
        return;
    }
    check_eq(with_conflict.stats.conflict_rows, 1, tag + "the conflict line is one row");
    const long long conflict_diffs = differing_pixels(edit.rgba, with_conflict.rgba);
    check(conflict_diffs > 0, tag + "the conflict changes the frame");
    check_eq(differing_pixels_outside(with_conflict, edit, conflict_top, conflict_bottom), 0,
             tag + "the conflict only adds the y 109 band");
    const Box conflict_box = ink_box(with_conflict, conflict_top, conflict_bottom);
    check_eq(conflict_box.y0, conflict_top, tag + "the conflict line starts at y 109");
    check(conflict_box.y1 <= conflict_bottom, tag + "the conflict line fits its band");
    check_eq(static_cast<long long>(conflict_box.count), conflict_diffs,
             tag + "the added pixels are exactly the conflict ink");
    check_eq(with_conflict.stats.ink_pixels,
             edit.stats.ink_pixels + static_cast<int>(conflict_diffs),
             tag + "ink_pixels grows by the conflict ink");
    const int conflict_x = title_x(red_face, conflict);
    check_eq(conflict_box.x0, conflict_x, tag + "the conflict line is centred");
    check(conflict_box.x1 < gm::kKeycfgPanelWidth, tag + "the conflict fits 196 px");
    check(count_colour(with_conflict, conflict_top, conflict_bottom, red) > 0,
          tag + "the conflict line uses font 5");
    check_band(with_conflict,
               compose_band(red_face, conflict, conflict_x, gm::kKeycfgPanelWidth, height),
               gm::kKeycfgPanelWidth, height, conflict_top, tag + "conflict band");
    check_eq(outside_ink(with_conflict), 0, tag + "the conflict stays in the 196x131 band");

    // 8. Negatives: an empty text paints nothing, a too-wide text is clamped to
    //    x 0 and clipped at 196 (`0x485de2` centres `(196-width)/2`, which is
    //    negative here; the original would wrap into the previous scanline of
    //    the linear band buffer — docs/KEYCFG_RENDER.md §7).
    gm::KeycfgRenderOptions empty_options = edit_options;
    empty_options.conflict = "";
    const Frame empty_conflict = run(game, key_file, empty_options);
    check(empty_conflict.ok, tag + "empty conflict frame: " + empty_conflict.error);
    check_eq(empty_conflict.stats.conflict_rows, 0, tag + "an empty text is no row");
    check_eq(differing_pixels(empty_conflict.rgba, edit.rgba), 0,
             tag + "an empty text paints nothing");

    const std::string too_long = conflict + " and " + conflict;
    gm::KeycfgRenderOptions long_options = edit_options;
    long_options.conflict = too_long;
    const Frame long_frame = run(game, key_file, long_options);
    check(long_frame.ok, tag + "a too-wide conflict is not a refusal: " + long_frame.error);
    if (!long_frame.ok) {
        return;
    }
    const int long_width = red_face.measure(too_long);
    check(long_width > gm::kKeycfgPanelWidth, tag + "the long text is wider than the band");
    check_eq(long_frame.stats.conflict_rows, 1, tag + "the long text is still one row");
    const Box long_box = ink_box(long_frame, conflict_top, conflict_bottom);
    check(long_box.x0 < conflict_box.x0, tag + "a wider-than-band line starts left of the "
                                             "centred one (clamped to x 0)");
    check(long_box.x1 < gm::kKeycfgPanelWidth, tag + "the long line is clipped at 196 px");
    check_eq(outside_ink(long_frame), 0, tag + "the long line stays in the 196x131 band");
    check_band(long_frame, compose_band(red_face, too_long, 0, gm::kKeycfgPanelWidth, height),
               gm::kKeycfgPanelWidth, height, conflict_top,
               tag + "the long line blits from x 0, clipped to 196");

    // 9. Determinism of the editor frame.
    const Frame again = run(game, key_file, conflict_options);
    check(again.ok, tag + "second conflict frame: " + again.error);
    check_eq(differing_pixels(again.rgba, with_conflict.rgba), 0,
             tag + "editor frames are deterministic");

    std::printf("  keycfg_render editor: view/edit diffs %lld (hint %lld, cursor band %lld); "
                "hint view=(%d,%d) w=%d edit=(%d,%d) w=%d x=%d; "
                "cell font5 red=%d bbox=(%d,%d)-(%d,%d) white=%d; alias/edit %lld; "
                "second cell bbox=(%d,%d) diffs %lld; "
                "conflict bbox=(%d,%d)-(%d,%d) ink=%d diffs %lld; "
                "long w=%d ink=%d bbox=(%d,%d)-(%d,%d)\n",
                view_edit, hint_diffs, row0_diffs, view_hint.x0, view_hint.y0,
                view_hint.width(), edit_hint.x0, edit_hint.y0, edit_hint.width(),
                edit_hint_x, edit_red.count, edit_red.x0, edit_red.y0, edit_red.x1,
                edit_red.y1, count_colour(edit, row0, row0_bottom, white), alias_edit,
                second_red.x0, second_red.x1, cell_diffs, conflict_box.x0, conflict_box.y0,
                conflict_box.x1, conflict_box.y1, conflict_box.count, conflict_diffs,
                long_width, long_box.count, long_box.x0, long_box.y0, long_box.x1,
                long_box.y1);
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
        if (fs::exists(root / "FRONTEND" / "BMP.IDX", ec) &&
            fs::exists(root / "KEYCFG" / "PERFECT.key", ec)) {
            game = root;
            break;
        }
    }
    if (game.empty()) {
        std::cout << "keycfg_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{}, fs::path{});
        return failures == 0 ? 0 : 1;
    }
    const fs::path keycfg = game / "KEYCFG";
    const fs::path perfect = keycfg / "PERFECT.key";

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number :
         {gm::kKeycfgTitleFont, gm::kKeycfgRowFont, gm::kKeycfgSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "keycfg_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_install_frame(game, perfect, fonts);
    test_cursor(game, perfect, fonts);
    test_profiles(game, keycfg, fonts);
    test_plate(game, perfect);
    test_negatives(game, perfect);
    test_fade_lut(game, perfect);
    test_editor(game, perfect, fonts);

    if (failures != 0) {
        std::cerr << "keycfg_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "keycfg_render: ok (" << checks
              << " checks) screen 30 -> 320x200 RGBA (title y 11, hint y 20, rows 31/step 8/"
                 "x 5, ten-band window at y 29, key x 108, second code x 154, PLATE, "
                 "editor 34/35 at 0x4892d0 with hint 0x4bd7a8 and the conflict line at "
                 "y 109, negatives)\n";
    return 0;
}
