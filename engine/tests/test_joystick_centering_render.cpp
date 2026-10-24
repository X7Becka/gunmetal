// One frame of screen 26, `JOYSTICK CENTERING` (`M8.8` slice 10,
// `ui/joystick_centering_render`, docs/JOYSTICK_CENTERING_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there
// is no state object.  The test pins the geometry of docs/UI_SCREENS.md
// §2/§3.8 from the handler `0x488f70` and from pixels, not from the code:
//
//   * the shipped install: `rows == 1` (the handler writes 1 into `0x67f10c`,
//     the five information lines all pass item 0 with step 0), `arrows == 0`
//     (this screen has no `MENUARRW` call), no missing glyph, 320x200 RGBA,
//     every pixel opaque;
//   * `JOYSTICK CENTERING` (font 5) centred at y 14, and the five lines at
//     their proven calls — (36, 25), (36, 35), (36, 45), (41, 86), (41, 97);
//     every band is compared byte for byte with an independent blit and its
//     bounding box is printed; `y`-differences 10/10/41/11 and the x switch
//     36 -> 41 are pinned;
//   * `ink` is the sum of the six independent band inks (the bands do not
//     overlap);
//   * there are no `%s`/`%d` substitutions: every line is a literal of the exe.
//     The frame-changing option is the highlight quirk of `0x485dd0`: all five
//     lines are item 0, so `cursor == 0` whitens the whole body (font 4) and
//     any other value — the API default -1 and the shipped 7 inherited from
//     screen 10 — paints font 0; each frame equals an independent composite;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel under the glyph mask; everything outside stays
//     black, the text ink is unchanged, `menu_panel_asset(Screen::
//     JoystickCentering)` is `PLATE`; the black-vs-plate difference is printed;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames, the canvas outside
//     the PLATE band stays black, and the negative paths are
//     `joystick_centering_render:` texts;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a
//     `BMP.IDX` without the `FONTS4` atlas (the title face is loaded first) and
//     a missing background entry; two renders are deterministic.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/JOYSTICK_CENTERING_RENDER.md §4 pins them.

#include <algorithm>
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
#include "ui/frontend_menu.h"
#include "ui/menu_render.h"
#include "ui/joystick_centering_render.h"

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
    gm::JoystickRenderStats stats;
};

Frame run(const fs::path& game, const gm::JoystickRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_joystick_centering(game, options, frame.rgba, frame.width,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kJoystickBackground; }

/// `#000` with any alpha: the faded palette entries of low levels are black
/// too, but their alpha is the forced 255 and their RGB is what counts here.
bool is_rgb_black(std::uint32_t pixel) { return (pixel >> 8u) == 0u; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Bounding box of the non-black pixels of a band (`y0..y1` rows only).
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

std::string box_text(const Box& box) {
    if (box.empty()) {
        return "empty";
    }
    return "(" + std::to_string(box.x0) + "," + std::to_string(box.y0) + ")-(" +
           std::to_string(box.x1) + "," + std::to_string(box.y1) + ") " +
           std::to_string(box.width()) + "x" + std::to_string(box.height()) + ", " +
           std::to_string(box.count) + " px";
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

/// Sum of the atlas advances of a run: what `0x485dd0` centres with
/// (`0x47fa50`).
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

/// Bounding box of the non-zero pixels of an independent band.
Box band_box(const std::vector<std::uint32_t>& band, int width, int height) {
    Box box;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            if (band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)] == 0) {
                continue;
            }
            ++box.count;
            if (box.x0 < 0 || column < box.x0) {
                box.x0 = column;
            }
            if (box.y0 < 0 || row < box.y0) {
                box.y0 = row;
            }
            if (column > box.x1) {
                box.x1 = column;
            }
            if (row > box.y1) {
                box.y1 = row;
            }
        }
    }
    return box;
}

/// Compare the rows `y..y+height-1` of the band `0..196` of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kJoystickPanelWidth;
    const int height = atlas.cell_height;
    const std::vector<std::uint32_t> band = compose_band(atlas, text, x, width, height);
    int wrong = 0;
    int first_x = -1;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const std::uint32_t want =
                band[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(column)];
            const std::uint32_t got = at(frame, column, y + row);
            const std::uint32_t expected = want == 0 ? gm::kJoystickBackground : want;
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

/// One painted information line of the screen: the handler's call, named.
struct Line {
    int item;
    int x;
    int y;
    std::string_view text;
    std::string name;
};

/// The five lines `0x488fa0`..`0x489010` paint, in call order.  `y` is the
/// `row` immediate itself because the step `0x662d58` is 0.
const Line* lines() {
    static const Line table[] = {
        {0, 36, 25, gm::kJoystickLineOne, "line 1"},
        {0, 36, 35, gm::kJoystickLineTwo, "line 2"},
        {0, 36, 45, gm::kJoystickLineThree, "line 3"},
        {0, 41, 86, gm::kJoystickLineFour, "hint 1"},
        {0, 41, 97, gm::kJoystickLineFive, "hint 2"},
    };
    return table;
}

constexpr int kLineCount = gm::kJoystickLineCount;

/// The centring of `0x485dd0`'s `centered` branch: title x inside 196 px.
int title_x(const gm::FontAtlas& face) {
    return std::max(0, (gm::kJoystickPanelWidth - pen_width(face, gm::kJoystickTitle)) /
                           2);
}

/// One `FRONTEND/BMP.IDX` entry as the blitters see it: the index bytes in
/// *file* order (`0x47efe0` freads `width*height` bytes) plus the palette.
struct SpriteAsset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rows;
    std::vector<std::uint32_t> palette;
};

SpriteAsset load_sprite_asset(const fs::path& game, const std::string& name) {
    SpriteAsset asset;
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
    const std::size_t image_size = static_cast<std::size_t>(image->width) *
                                   static_cast<std::size_t>(image->height);
    if (bytes->size() < 54 || image_size == 0) {
        return asset;
    }
    const std::size_t offset = static_cast<std::size_t>((*bytes)[10]) |
                               (static_cast<std::size_t>((*bytes)[11]) << 8) |
                               (static_cast<std::size_t>((*bytes)[12]) << 16) |
                               (static_cast<std::size_t>((*bytes)[13]) << 24);
    if (offset + image_size > bytes->size()) {
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

/// The `PLATE` panel as `parse_bmp` returns it (top-down rows, RGBA palette).
///
/// `SpriteAsset::rows` are the BMP pixel bytes in *file* order; the standard BMP
/// file stores its rows bottom-up, so `parse_bmp` row `y` is file row
/// `height-1-y` (docs/START_GAME_RENDER.md §1).
std::vector<std::uint32_t> plate_pixels(const SpriteAsset& plate) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(plate.width) *
                                       static_cast<std::size_t>(plate.height),
                                   0u);
    for (int y = 0; y < plate.height; ++y) {
        const int source_row = plate.height - 1 - y;
        for (int x = 0; x < plate.width; ++x) {
            const std::uint8_t index =
                plate.rows[static_cast<std::size_t>(source_row) *
                               static_cast<std::size_t>(plate.width) +
                           static_cast<std::size_t>(x)];
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(plate.width) +
                static_cast<std::size_t>(x)] = plate.palette[index];
        }
    }
    return out;
}

fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_joystick_centering_render";
    }
    return fs::temp_directory_path() / "gm_joystick_centering_render";
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

/// The font of a line for a given cursor (the module's rule: all five lines
/// are item 0, so cursor 0 switches them all to font 4).
const gm::FontAtlas& face_of(const std::map<int, gm::FontAtlas>& fonts, int cursor,
                             int item) {
    return fonts.at(item == cursor ? gm::kJoystickSelectedFont
                                   : gm::kJoystickRowFont);
}

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::JoystickRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::JoystickRenderStats stats;
    stats.rows = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    std::string error;
    const bool ok =
        gm::render_joystick_centering(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "joystick_centering_render:"),
          what + ": error prefix (got \"" + error + "\")");
    check(contains(error, expected),
          what + ": error mentions \"" + std::string(expected) + "\" (got \"" + error + "\")");
    check(rgba.empty(), what + ": buffer cleared");
    check_eq(width, 0, what + ": width cleared");
    check_eq(height, 0, what + ": height cleared");
    check_eq(stats.rows, 0, what + ": rows cleared");
    check_eq(stats.ink_pixels, 0, what + ": ink cleared");
    check_eq(stats.missing_glyphs, 0, what + ": missing cleared");
    check_eq(stats.arrows, 0, what + ": arrows cleared");
}

// --- the installed frame -----------------------------------------------------
void test_install_frame(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const Frame frame = run(game, gm::JoystickRenderOptions{});
    check(frame.ok, "install: frame renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, gm::kJoystickCanvasWidth, "install: width 320");
    check_eq(frame.height, gm::kJoystickCanvasHeight, "install: height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, "install: 64000 pixels");
    check_eq(frame.stats.rows, gm::kJoystickRows, "install: rows 1 item");
    check_eq(frame.stats.rows, 1, "install: 0x67f10c = 1");
    check_eq(gm::kJoystickItemStep, 0, "install: 0x662d58 = 0 (no row step)");
    check_eq(frame.stats.arrows, 0, "install: no MENUARRW arrows on screen 26");
    check_eq(frame.stats.missing_glyphs, 0, "install: no missing glyph");
    check(frame.stats.ink_pixels > 0, "install: ink > 0");
    for (const std::uint32_t pixel : frame.rgba) {
        if ((pixel & 0xFFu) != 0xFFu) {
            check(false, "install: every pixel is opaque");
            break;
        }
    }

    // No printf substitutions: the five lines and the title are plain literals.
    for (int i = 0; i < kLineCount; ++i) {
        check(lines()[i].text.find('%') == std::string_view::npos,
              "install: " + lines()[i].name + " has no %-substitution");
    }
    check(gm::kJoystickTitle.find('%') == std::string_view::npos,
          "install: title has no %-substitution");

    const gm::FontAtlas& title_face = fonts.at(gm::kJoystickTitleFont);
    const int tx = title_x(title_face);
    check_band(frame, title_face, gm::kJoystickTitle, tx, gm::kJoystickTitleY,
               "install: title");
    const Box title = ink_box(frame, gm::kJoystickTitleY,
                              gm::kJoystickTitleY + title_face.cell_height - 1);
    check(!title.empty(), "install: title has ink");
    const std::vector<std::uint32_t> title_band =
        compose_band(title_face, gm::kJoystickTitle, tx, gm::kJoystickPanelWidth,
                     title_face.cell_height);
    const Box title_want = band_box(title_band, gm::kJoystickPanelWidth,
                                    title_face.cell_height);
    check_eq(title.x0, title_want.x0, "install: title left is the blit's");
    check_eq(title.y0, gm::kJoystickTitleY + title_want.y0,
             "install: title top is the blit's");
    check_eq(title.count, static_cast<long long>(band_ink(title_band)),
             "install: title ink is the independent blit");
    long long bands_ink = band_ink(title_band);

    std::string boxes;
    for (int i = 0; i < kLineCount; ++i) {
        const Line& line = lines()[i];
        const gm::FontAtlas& face =
            face_of(fonts, gm::kJoystickNoHighlight, line.item);
        check_band(frame, face, line.text, line.x, line.y,
                   "install: " + line.name);
        const std::vector<std::uint32_t> band =
            compose_band(face, line.text, line.x, gm::kJoystickPanelWidth,
                         face.cell_height);
        bands_ink += band_ink(band);
        const Box box = ink_box(frame, line.y, line.y + face.cell_height - 1);
        const Box want = band_box(band, gm::kJoystickPanelWidth, face.cell_height);
        check(!box.empty(), "install: " + line.name + " has ink");
        check_eq(box.x0, want.x0, "install: " + line.name + " left column");
        check_eq(box.y0, line.y + want.y0, "install: " + line.name + " top row " +
                                              std::to_string(line.y));
        check_eq(box.x1, want.x1, "install: " + line.name + " right column");
        check_eq(box.y1, line.y + want.y1, "install: " + line.name + " bottom row");
        check_eq(box.count, static_cast<long long>(band_ink(band)),
                 "install: " + line.name + " ink is the independent blit");
        if (!boxes.empty()) {
            boxes += "; ";
        }
        boxes += line.name + " (" + std::to_string(line.x) + "," +
                 std::to_string(line.y) + ") " + box_text(box);
    }
    check_eq(frame.stats.ink_pixels, bands_ink,
             "install: ink is the sum of the six bands");

    // Constant table vs the handler's immediates, and the spacing the layout
    // proves: paragraphs 10 px apart (25/35/45), hint 9 px (86/97), the gap
    // 45 -> 86 is real, and x switches 36 -> 41 for the hint.
    for (int i = 0; i < kLineCount; ++i) {
        check_eq(gm::kJoystickLines[i].x, lines()[i].x,
                 "install: line " + std::to_string(i + 1) + " constant x");
        check_eq(gm::kJoystickLines[i].y, lines()[i].y,
                 "install: line " + std::to_string(i + 1) + " constant y");
        check_eq(gm::kJoystickLines[i].item, 0,
                 "install: line " + std::to_string(i + 1) + " item 0");
        check_eq(std::string(gm::kJoystickLines[i].text),
                 std::string(lines()[i].text),
                 "install: line " + std::to_string(i + 1) + " text");
    }
    check_eq(lines()[1].y - lines()[0].y, 10, "install: 25 -> 35 is 10 px");
    check_eq(lines()[2].y - lines()[1].y, 10, "install: 35 -> 45 is 10 px");
    check_eq(lines()[3].y - lines()[2].y, 41, "install: 45 -> 86 is 41 px");
    check_eq(lines()[4].y - lines()[3].y, 11, "install: 86 -> 97 is 11 px");
    check_eq(lines()[3].x - lines()[0].x, 5, "install: hint x is 5 px right (36 -> 41)");
    check(gm::kJoystickBodyX == 36 && gm::kJoystickHintX == 41,
          "install: x constants are 36/41");

    // Keep the public labels in step with the exe's VAs (docs/UI_SCREENS.md §2).
    check_eq(std::string(gm::kJoystickTitle), std::string("JOYSTICK CENTERING"),
             "install: title 0x4bd6ac");
    check_eq(std::string(gm::kJoystickLineThree), std::string("it's positions."),
             "install: line 3 0x4bd66c keeps the exe's apostrophe");
    check_eq(std::string(gm::kJoystickLineFive), std::string("to close"),
             "install: hint 2 0x4bd64c");

    // Determinism: two renders are byte-identical.
    const Frame again = run(game, gm::JoystickRenderOptions{});
    check(again.ok, "install: second render");
    check_eq(differing_pixels(frame.rgba, again.rgba), 0, "install: deterministic");

    std::printf("  joystick_centering_render install: rows=%d ink=%d arrows=%d missing=%d; "
                "title y%d x%d %s; %s\n",
                frame.stats.rows, frame.stats.ink_pixels, frame.stats.arrows,
                frame.stats.missing_glyphs, gm::kJoystickTitleY, tx,
                box_text(title).c_str(), boxes.c_str());
}

// --- the highlight quirk -----------------------------------------------------
void test_cursor(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "cursor: ";
    const Frame plain = run(game, gm::JoystickRenderOptions{});
    check(plain.ok, tag + "plain frame");
    if (!plain.ok) {
        return;
    }
    check_eq(gm::kJoystickNoHighlight, -1, tag + "the default is -1 (no match)");

    // All five lines share item 0: cursor 0 switches the whole body to font 4.
    const int cursors[] = {0, gm::kJoystickShippedCursor};
    for (const int cursor : cursors) {
        gm::JoystickRenderOptions options;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "cursor " + std::to_string(cursor) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(frame.stats.rows, gm::kJoystickRows,
                 tag + "cursor " + std::to_string(cursor) + ": rows 1");
        check_eq(frame.stats.arrows, 0,
                 tag + "cursor " + std::to_string(cursor) + ": no arrows");
        check_eq(frame.stats.missing_glyphs, 0,
                 tag + "cursor " + std::to_string(cursor) + ": no missing glyph");
        for (int i = 0; i < kLineCount; ++i) {
            const Line& line = lines()[i];
            check_band(frame, face_of(fonts, cursor, line.item), line.text, line.x,
                       line.y, tag + "cursor " + std::to_string(cursor) + " " +
                                     line.name);
        }

        // Independent composite of the whole canvas.
        Frame expect;
        expect.width = gm::kJoystickCanvasWidth;
        expect.height = gm::kJoystickCanvasHeight;
        expect.rgba.assign(static_cast<std::size_t>(expect.width) *
                               static_cast<std::size_t>(expect.height),
                           gm::kJoystickBackground);
        const auto paint = [&expect](const gm::FontAtlas& face, std::string_view text,
                                     int x, int y) {
            const std::vector<std::uint32_t> band =
                compose_band(face, text, x, gm::kJoystickPanelWidth, face.cell_height);
            for (int row = 0; row < face.cell_height; ++row) {
                for (int column = 0; column < gm::kJoystickPanelWidth; ++column) {
                    const std::uint32_t pixel =
                        band[static_cast<std::size_t>(row) *
                                 static_cast<std::size_t>(gm::kJoystickPanelWidth) +
                             static_cast<std::size_t>(column)];
                    if (pixel != 0) {
                        expect.rgba[static_cast<std::size_t>(y + row) *
                                        static_cast<std::size_t>(expect.width) +
                                    static_cast<std::size_t>(column)] = pixel;
                    }
                }
            }
        };
        const gm::FontAtlas& title_face = fonts.at(gm::kJoystickTitleFont);
        paint(title_face, gm::kJoystickTitle, title_x(title_face),
              gm::kJoystickTitleY);
        for (int i = 0; i < kLineCount; ++i) {
            const Line& line = lines()[i];
            paint(face_of(fonts, cursor, line.item), line.text, line.x, line.y);
        }
        check_eq(differing_pixels(frame.rgba, expect.rgba), 0,
                 tag + "cursor " + std::to_string(cursor) +
                     ": whole canvas matches the independent composite");

        // Which pixels change relative to the plain frame: cursor 0 changes
        // the five body bands and nothing else; the shipped cursor changes
        // nothing at all.
        long long diff_plain = 0;
        long long outside = 0;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                if (at(frame, x, y) == at(plain, x, y)) {
                    continue;
                }
                ++diff_plain;
                bool involved = false;
                for (int i = 0; i < kLineCount; ++i) {
                    const Line& line = lines()[i];
                    if (y >= line.y && y < line.y + title_face.cell_height) {
                        involved = true;
                    }
                }
                outside += involved ? 0 : 1;
            }
        }
        check_eq(outside, 0, tag + "cursor " + std::to_string(cursor) +
                                 ": changes only the five text bands");
        if (cursor == gm::kJoystickShippedCursor) {
            check_eq(diff_plain, 0,
                     tag + "the shipped cursor 7 paints the plain frame");
        } else {
            check(diff_plain > 0, tag + "cursor 0 differs from the plain frame");
        }
        // The ink is the sum of the six bands with the face that cursor picks:
        // the highlight changes the shape of the glyphs, not the background.
        long long want_ink = band_ink(compose_band(
            title_face, gm::kJoystickTitle, title_x(title_face),
            gm::kJoystickPanelWidth, title_face.cell_height));
        for (int i = 0; i < kLineCount; ++i) {
            const Line& line = lines()[i];
            const gm::FontAtlas& face = face_of(fonts, cursor, line.item);
            want_ink += band_ink(compose_band(face, line.text, line.x,
                                              gm::kJoystickPanelWidth,
                                              face.cell_height));
        }
        check_eq(frame.stats.ink_pixels, want_ink,
                 tag + "cursor " + std::to_string(cursor) + ": ink is the band ink");

        std::printf("  joystick_centering_render cursor %d: %lld px vs the default frame, "
                    "ink=%d\n",
                    cursor, diff_plain, frame.stats.ink_pixels);
    }

    // Out-of-range values never match item 0: they paint the plain frame.
    for (const int cursor : {-1, 1, 99}) {
        gm::JoystickRenderOptions options;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, tag + "cursor " + std::to_string(cursor) + ": " + frame.error);
        if (!frame.ok) {
            return;
        }
        check_eq(differing_pixels(frame.rgba, plain.rgba), 0,
                 tag + "cursor " + std::to_string(cursor) + " paints the plain frame");
    }
    std::printf("  joystick_centering_render cursor: 0 whitens all five lines (item 0, "
                "step 0); 7/-1/1/99 paint font 0\n");
}

// --- PLATE -------------------------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "plate: ";
    const Frame black = run(game, gm::JoystickRenderOptions{});
    const SpriteAsset plate =
        load_sprite_asset(game, std::string(gm::kJoystickPanelName));
    if (!black.ok || !plate.ok) {
        std::cout << "  plate: SKIP (no PLATE in BMP.IDX)\n";
        return;
    }
    check_eq(plate.width, gm::kJoystickPanelWidth, tag + "PLATE 196 wide");
    check_eq(plate.height, gm::kJoystickPanelHeight, tag + "PLATE 131 high");

    gm::JoystickRenderOptions options;
    options.background = std::string(gm::kJoystickPanelName);
    const Frame frame = run(game, options);
    check(frame.ok, tag + "renders: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels, black.stats.ink_pixels,
             tag + "text ink does not change");
    check_eq(frame.stats.rows, black.stats.rows, tag + "rows do not change");
    check_eq(frame.stats.arrows, 0, tag + "no arrows");

    // Glyph mask: every pixel any band writes.
    const gm::FontAtlas& title_face = fonts.at(gm::kJoystickTitleFont);
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(frame.width) *
                                       static_cast<std::size_t>(frame.height),
                                   0);
    const auto mark = [&mask, &frame](const gm::FontAtlas& face, std::string_view text,
                                      int x, int y) {
        const std::vector<std::uint32_t> band =
            compose_band(face, text, x, gm::kJoystickPanelWidth, face.cell_height);
        for (int row = 0; row < face.cell_height; ++row) {
            for (int column = 0; column < gm::kJoystickPanelWidth; ++column) {
                if (band[static_cast<std::size_t>(row) *
                             static_cast<std::size_t>(gm::kJoystickPanelWidth) +
                         static_cast<std::size_t>(column)] != 0) {
                    mask[static_cast<std::size_t>(y + row) *
                             static_cast<std::size_t>(frame.width) +
                         static_cast<std::size_t>(column)] = 1;
                }
            }
        }
    };
    mark(title_face, gm::kJoystickTitle, title_x(title_face),
         gm::kJoystickTitleY);
    for (int i = 0; i < kLineCount; ++i) {
        const Line& line = lines()[i];
        mark(face_of(fonts, gm::kJoystickNoHighlight, line.item), line.text, line.x,
             line.y);
    }

    const std::vector<std::uint32_t> panel = plate_pixels(plate);
    long long panel_in = 0;
    long long panel_ink = 0;
    long long outside = 0;
    long long wrong = 0;
    int first_x = -1;
    int first_y = -1;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint32_t got = at(frame, x, y);
            if (x >= plate.width || y >= plate.height) {
                ++outside;
                wrong += is_black(got) ? 0 : 1;
                continue;
            }
            ++panel_in;
            const std::size_t index = static_cast<std::size_t>(y) *
                                          static_cast<std::size_t>(plate.width) +
                                      static_cast<std::size_t>(x);
            if (mask[static_cast<std::size_t>(y) *
                         static_cast<std::size_t>(frame.width) +
                     static_cast<std::size_t>(x)] != 0) {
                panel_ink += 1;
                wrong += got == at(black, x, y) ? 0 : 1;
            } else {
                wrong += got == panel[index] ? 0 : 1;
            }
            if (wrong != 0 && first_x < 0) {
                first_x = x;
                first_y = y;
            }
        }
    }
    check_eq(outside, static_cast<long long>(frame.width) * frame.height -
                          static_cast<long long>(plate.width) * plate.height,
             tag + "38324 pixels outside the panel");
    check_eq(wrong, 0, tag + "panel matches the independent decode under the glyph "
                            "mask (first " + std::to_string(first_x) + "," +
                            std::to_string(first_y) + ")");
    check_eq(panel_ink, black.stats.ink_pixels,
             tag + "the glyph pixels over the panel are the black-frame ink");

    const long long diff = differing_pixels(frame.rgba, black.rgba);
    check(diff > 0, tag + "PLATE changes the frame");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::JoystickCentering)),
             std::string(gm::kJoystickPanelName),
             tag + "menu_panel_asset(Screen::JoystickCentering) is PLATE");

    std::printf("  joystick_centering_render plate: PLATE %dx%d, panel %lld px, %lld under "
                "glyphs, %lld outside black; plate-vs-black diff %lld px\n",
                plate.width, plate.height, panel_in, panel_ink, outside, diff);
}

// --- exact fade --------------------------------------------------------------
void test_fade_lut(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kJoystickCanvasWidth) *
                                  gm::kJoystickCanvasHeight;

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
        gm::JoystickRenderOptions options;
        options.background = std::string(gm::kJoystickPanelName);
        options.cursor = gm::kJoystickNoHighlight;
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, options);
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
                if (x < gm::kJoystickPanelWidth && y < gm::kJoystickPanelHeight) {
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
    check_eq(outside_panel(identity), 0, tag + "level 11 leaves the canvas black");

    // 2. Level 0: a different frame with the same ink.
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

    std::printf("  joystick_centering_render fade: level 11 diffs %lld/%lld; level 0 black "
                "%lld (plain %lld), diffs %lld; adjacent 12/13 %lld, 13/14 %lld, "
                "14/15 %lld; vs 11 %lld/%lld/%lld/%lld\n",
                identity_diffs, kCanvas, black0, black_plain, diff0, pair12_13,
                pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);

    // 4. Negatives: no table and no dump, an unreadable dump, and the table
    // winning over the dump.
    {
        gm::JoystickRenderOptions options;
        options.background = std::string(gm::kJoystickPanelName);
        options.fade_lut = true;
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "no table and no dump refused");
        check(starts_with(refusal.error, "joystick_centering_render: ") &&
                  contains(refusal.error, "fade LUT needs"),
              tag + "refusal names the missing table: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "refusal clears the buffer");
        check_eq(refusal.width, 0, tag + "refusal zeroes the width");
        check_eq(refusal.height, 0, tag + "refusal zeroes the height");
        check_eq(refusal.stats.ink_pixels, 0, tag + "refusal zeroes the stats");
    }
    {
        gm::JoystickRenderOptions options;
        options.background = std::string(gm::kJoystickPanelName);
        options.fade_lut = true;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "missing dump refused");
        check(starts_with(refusal.error, "joystick_centering_render: ") &&
                  contains(refusal.error, "hud_lut:"),
              tag + "missing dump carries the hud_lut text: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "missing dump clears the buffer");
    }
    {
        gm::JoystickRenderOptions options;
        options.background = std::string(gm::kJoystickPanelName);
        options.cursor = gm::kJoystickNoHighlight;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame both = run(game, options);
        check(both.ok, tag + "a table wins over a missing dump: " + both.error);
        if (both.ok) {
            check_eq(differing_pixels(both.rgba, plain.rgba), 0,
                     tag + "table path equals the unfaded level 11 frame");
        }
    }
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::JoystickRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }

    // No FRONTEND/BMP.IDX: the atlases cannot be built.
    check_refused(root / "game-without-bmp", gm::JoystickRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    // A valid BIGFILE index whose only entry is FONTS1: all three faces of this
    // screen come from the `FONTS4` atlas (fonts 0/4/5), so that entry is the
    // missing one — the title face 5 is loaded first.
    const fs::path fontless = root / "game-without-font4" / "FRONTEND";
    fs::create_directories(fontless, ec);
    check(!ec, "negatives: fontless tree created");
    if (ec) {
        fs::remove_all(root, ec);
        return;
    }
    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                     static_cast<std::uint8_t>(0x1A)};
    put32(idx, 1);
    std::string name = "FONTS1";
    name.resize(8, '\0');
    idx.insert(idx.end(), name.begin(), name.end());
    put32(idx, 0);
    check(write_bytes(fontless / "BMP.IDX", idx), "negatives: synthetic BMP.IDX written");
    check(write_bytes(fontless / "BMP.DAT", {}), "negatives: synthetic BMP.DAT written");
    check_refused(root / "game-without-font4", gm::JoystickRenderOptions{},
                  "missing FONTS4 (the face atlas)", "FONTS4");

    fs::remove_all(root, ec);

    if (game.empty()) {
        return;
    }

    // A background name that is not in the archive is a text, not black.
    gm::JoystickRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // The shipped install renders one item, no arrows.
    const Frame frame = run(game, gm::JoystickRenderOptions{});
    check(frame.ok, "negatives: shipped install renders: " + frame.error);
    if (frame.ok) {
        check_eq(frame.stats.rows, gm::kJoystickRows, "negatives: rows 1");
        check_eq(frame.stats.arrows, 0, "negatives: no arrows");
        std::printf("  joystick_centering_render negatives: rows=%d ink=%d arrows=%d "
                    "missing=%d\n",
                    frame.stats.rows, frame.stats.ink_pixels, frame.stats.arrows,
                    frame.stats.missing_glyphs);
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
    if (game.empty()) {
        std::cout << "joystick_centering_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kJoystickTitleFont, gm::kJoystickRowFont,
                             gm::kJoystickSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "joystick_centering_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_install_frame(game, fonts);
    test_cursor(game, fonts);
    test_plate(game, fonts);
    test_negatives(game);
    test_fade_lut(game);
    if (failures != 0) {
        std::cerr << "joystick_centering_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "joystick_centering_render: ok (" << checks
              << " checks) screen 26 -> 320x200 RGBA (title y 14, five lines "
                 "36/36/36/41/41 at y 25/35/45/86/97, no substitutions, PLATE, "
                 "exact fade, negatives)\n";
    return 0;
}
