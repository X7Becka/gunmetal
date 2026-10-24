// One frame of screens 24/25, the local player name entry (`M8.8` slice 22,
// `ui/player_name_render`, docs/PLAYER_NAME_RENDER.md).
//
// The module paints the 320x200 mode-13h canvas from the options alone; there
// is no state object.  The test pins the geometry of docs/UI_SCREENS.md §2
// (rows 1 / step 0) from the handler `0x489020` and from pixels, not from the
// code:
//
//   * the shipped install: `rows == 1`, `chars == strlen(name)`, `arrows == 0`
//     (the handler has no `MENUARRW` call), no missing glyph, 320x200 RGBA,
//     every pixel opaque, deterministic;
//   * both titles (player 0 -> `LOCAL PLAYER ONE NAME` from `0x4bd730`,
//     player 1 -> `LOCAL PLAYER TWO NAME` from `0x4bd748`, font 5, centred in
//     the 196 px band at y 14): the two frames differ **only** inside the
//     title band, every title is compared byte for byte with an independent
//     blit and the bounding boxes are printed;
//   * the name line: font 3 (`FONTS2`, 16x16), centred on x 98 of the plate,
//     y 28; a changed name changes only rows 28..43; `bbox`/`ink` printed;
//   * the 10-character limit of the editor `0x405b20` (`push 0xb` at
//     `0x48f4cd`): a 30-character name yields `chars=10`, `truncated=20` and is
//     pixel-identical to its 10-character prefix; a NUL ends the buffer;
//   * the caret box `0x485d40`: hollow 8x17 at (name end, 27), painted before
//     the text; on/off differs by exactly `caret_pixels`; the width rule of
//     `0x489095`..`0x4890ae` (M/W 16, I 6, else 8) is pinned with a mid-name
//     position; the fill index is the PLATE palette entry;
//   * the six hints (font 0, x 16, y 48/56/67/75/89/102) are compared with an
//     independent blit;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently and
//     compared pixel by pixel where no glyph was painted; outside stays black,
//     the glyph ink is unchanged; `menu_panel_asset(Screen::PlayerOneName)` is
//     `PLATE`;
//   * exact fade (`fade_lut`): level 11 is byte-identical to the unfaded frame
//     (0/64000), levels 12..15 are four different frames, the negative paths
//     are `player_name_render:` texts;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a
//     `BMP.IDX` without `FONTS4` (face 5 is loaded first), one without `FONTS2`
//     (face 3) and one without `PLATE`, plus a missing background entry.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/PLAYER_NAME_RENDER.md §4 pins them.

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
#include "ui/frontend_menu.h"
#include "ui/menu_render.h"
#include "ui/player_name_render.h"

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
    gm::PlayerNameRenderStats stats;
};

Frame run(const fs::path& game, const gm::PlayerNameRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_player_name(game, options, frame.rgba, frame.width,
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

long long differing_pixels(const Frame& a, const Frame& b) {
    return differing_pixels(a.rgba, b.rgba);
}

bool is_black(std::uint32_t pixel) { return pixel == gm::kPlayerNameBackground; }

/// `#000` with any alpha: the faded palette entries of level 0 are black too,
/// but their alpha is the forced 255 and their RGB is what counts here.
bool is_rgb_black(std::uint32_t pixel) { return (pixel >> 8u) == 0u; }

const std::uint32_t& at(const Frame& frame, int x, int y) {
    return frame.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
                      static_cast<std::size_t>(x)];
}

/// Bounding box and count of the differing pixels of two frames.
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

Box diff_box(const Frame& a, const Frame& b) {
    Box box;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (at(a, x, y) == at(b, x, y)) {
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

/// Bounding box of the non-black pixels of a band (`y0..y1`).
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

/// Compare the rows `y..y+height-1` of the band `0..196` of *frame* with the
/// independent blit; every untouched pixel must be the black background.
void check_band(const Frame& frame, const gm::FontAtlas& atlas, std::string_view text,
                int x, int y, const std::string& what) {
    const int width = gm::kPlayerNamePanelWidth;
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
            const std::uint32_t expected = want == 0 ? gm::kPlayerNameBackground : want;
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

/// The glyph ink of one atlas run: independent of the renderer.
long long run_ink(const gm::FontAtlas& atlas, std::string_view text) {
    const std::vector<std::uint32_t> band =
        compose_band(atlas, text, 0, gm::kPlayerNamePanelWidth, atlas.cell_height);
    long long ink = 0;
    for (const std::uint32_t pixel : band) {
        ink += pixel == 0 ? 0 : 1;
    }
    return ink;
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
        return approved / "gm_player_name_render";
    }
    return fs::temp_directory_path() / "gm_player_name_render";
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

/// The raw bytes of one `FRONTEND/BMP.IDX` entry of the installed game (empty
/// when it is missing).
std::vector<std::uint8_t> archive_entry_bytes(const fs::path& game,
                                              const std::string& name) {
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return {};
    }
    for (const gm::BigEntry& candidate : archive->entries()) {
        if (candidate.name == name) {
            std::optional<std::vector<std::uint8_t>> bytes = archive->read(candidate, error);
            return bytes ? *bytes : std::vector<std::uint8_t>{};
        }
    }
    return {};
}

/// Write a `BIGFILE` pair (`BMP.IDX` + `BMP.DAT`) with the given entries, so the
/// refusals of a *single* missing asset can be tested on the real bytes.
bool write_archive(const fs::path& frontend,
                   const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries) {
    std::vector<std::uint8_t> dat;
    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                     static_cast<std::uint8_t>(0x1A)};
    put32(idx, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        std::string name = entry.first;
        name.resize(8, '\0');
        idx.insert(idx.end(), name.begin(), name.end());
        put32(idx, static_cast<std::uint32_t>(dat.size()));
        dat.insert(dat.end(), entry.second.begin(), entry.second.end());
    }
    return write_bytes(frontend / "BMP.IDX", idx) &&
           write_bytes(frontend / "BMP.DAT", dat);
}

// --- refusals ----------------------------------------------------------------
void check_refused(const fs::path& game, const gm::PlayerNameRenderOptions& options,
                   const std::string& what, std::string_view expected) {
    std::vector<std::uint32_t> rgba(64000u, 0xdeadbeefu); // must be cleared
    int width = 320;
    int height = 200;
    gm::PlayerNameRenderStats stats;
    stats.rows = 7;
    stats.chars = 7;
    stats.truncated = 7;
    stats.ink_pixels = 7;
    stats.missing_glyphs = 7;
    stats.arrows = 7;
    stats.caret_pixels = 7;
    std::string error;
    const bool ok =
        gm::render_player_name(game, options, rgba, width, height, &stats, &error);
    check(!ok, what + ": refused");
    check(starts_with(error, "player_name_render:"),
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
    check_eq(stats.caret_pixels, 0, what + ": caret pixels cleared");
}

// --- helpers of the installed frame -----------------------------------------
gm::PlayerNameRenderOptions base_options(int player, std::string name) {
    gm::PlayerNameRenderOptions options;
    options.player = player;
    options.name = std::move(name);
    options.caret = false;
    return options;
}

int title_x(const gm::FontAtlas& face, std::string_view title) {
    return std::max(0, (gm::kPlayerNamePanelWidth - pen_width(face, title)) / 2);
}

[[maybe_unused]] void check_hints(const Frame& frame, const gm::FontAtlas& face) {
    for (int i = 0; i < gm::kPlayerNameHintCount; ++i) {
        check_band(frame, face, gm::kPlayerNameHints[i], gm::kPlayerNameHintX,
                   gm::kPlayerNameHintY[i],
                   "hint " + std::to_string(i) + " \"" +
                       std::string(gm::kPlayerNameHints[i]) + "\"");
    }
}

// --- both titles -------------------------------------------------------------
void test_titles(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "titles: ";
    const gm::FontAtlas& title_face = fonts.at(gm::kPlayerNameTitleFont);

    check_eq(std::string(gm::player_name_title(0)),
             std::string(gm::kPlayerNameTitleOne), tag + "player 0 title");
    check_eq(std::string(gm::player_name_title(1)),
             std::string(gm::kPlayerNameTitleTwo), tag + "player 1 title");
    check_eq(std::string(gm::player_name_title(7)),
             std::string(gm::kPlayerNameTitleTwo), tag + "any non-zero is player 2");

    Frame frames[2];
    for (int player = 0; player < 2; ++player) {
        frames[player] = run(game, base_options(player, "PLAYER ONE"));
        check(frames[player].ok, tag + "player " + std::to_string(player) + ": " +
                                      frames[player].error);
        if (!frames[player].ok) {
            return;
        }
        check_eq(frames[player].width, gm::kPlayerNameCanvasWidth, tag + "width 320");
        check_eq(frames[player].height, gm::kPlayerNameCanvasHeight, tag + "height 200");
        check_eq(static_cast<long long>(frames[player].rgba.size()), 64000,
                 tag + "canvas 64000");
        check_eq(frames[player].stats.rows, gm::kPlayerNameRows, tag + "rows 1");
        check_eq(frames[player].stats.arrows, 0, tag + "no arrows");
        check_eq(frames[player].stats.missing_glyphs, 0, tag + "no missing glyphs");
        check_eq(frames[player].stats.chars, 10, tag + "chars = name length");
        check_eq(frames[player].stats.caret_pixels, 0, tag + "no caret pixels");
        long long opaque = 0;
        for (const std::uint32_t pixel : frames[player].rgba) {
            opaque += (pixel & 0xFFu) == 0xFFu ? 1 : 0;
        }
        check_eq(opaque, 64000, tag + "every pixel opaque");
    }

    const std::string_view titles[2] = {gm::kPlayerNameTitleOne,
                                        gm::kPlayerNameTitleTwo};
    for (int player = 0; player < 2; ++player) {
        const int x = title_x(title_face, titles[player]);
        check_band(frames[player], title_face, titles[player], x, gm::kPlayerNameTitleY,
                   tag + "title " + std::to_string(player) + " band");
        check_eq(x, std::max(0, (gm::kPlayerNamePanelWidth -
                                 pen_width(title_face, titles[player])) /
                                    2),
                 tag + "title x is the (196-width)/2 centre of 0x485dd0");
    }

    const long long title_ink0 = run_ink(title_face, titles[0]);
    const long long title_ink1 = run_ink(title_face, titles[1]);
    const long long frame_ink0 = frames[0].stats.ink_pixels;
    const long long frame_ink1 = frames[1].stats.ink_pixels;
    const Box b0 = ink_box(frames[0], gm::kPlayerNameTitleY,
                           gm::kPlayerNameTitleY + title_face.cell_height - 1);
    const Box b1 = ink_box(frames[1], gm::kPlayerNameTitleY,
                           gm::kPlayerNameTitleY + title_face.cell_height - 1);
    check_eq(b0.count, title_ink0, tag + "title 0 ink is independent");
    check_eq(b1.count, title_ink1, tag + "title 1 ink is independent");

    const Box diff = diff_box(frames[0], frames[1]);
    check(diff.count > 0, tag + "the two titles differ");
    check(diff.y0 >= gm::kPlayerNameTitleY &&
              diff.y1 < gm::kPlayerNameTitleY + title_face.cell_height,
          tag + "differences only in the title band (" + box_text(diff) + ")");
    check_eq(frame_ink1 - frame_ink0, title_ink1 - title_ink0,
             tag + "frame inks differ by the title inks");

    std::printf("  player_name titles: 24 \"%s\" x=%d %s ink=%lld; 25 \"%s\" x=%d %s "
                "ink=%lld; diff=%d px (y %d..%d)\n",
                std::string(titles[0]).c_str(), title_x(title_face, titles[0]),
                box_text(b0).c_str(), title_ink0,
                std::string(titles[1]).c_str(), title_x(title_face, titles[1]),
                box_text(b1).c_str(), title_ink1, diff.count, diff.y0, diff.y1);
}

// --- the name line -----------------------------------------------------------
void test_names(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts) {
    const std::string tag = "names: ";
    const gm::FontAtlas& name_face = fonts.at(gm::kPlayerNameNameFont);

    const Frame empty = run(game, base_options(0, ""));
    check(empty.ok, tag + "empty name: " + empty.error);
    if (!empty.ok) {
        return;
    }
    check_eq(empty.stats.chars, 0, tag + "empty name chars 0");

    const Frame jim = run(game, base_options(0, "JIM BOB"));
    const Frame auto_bot = run(game, base_options(0, "AUTOBOT"));
    check(jim.ok && auto_bot.ok, tag + "two names render");
    if (!jim.ok || !auto_bot.ok) {
        return;
    }
    check_eq(jim.stats.chars, 7, tag + "JIM BOB chars 7");
    const int x_jim = gm::player_name_text_x(pen_width(name_face, "JIM BOB"));
    check_eq(x_jim, gm::player_name_text_x(pen_width(name_face, "JIM BOB")),
             tag + "name x helper");
    check_band(jim, name_face, "JIM BOB", x_jim, gm::kPlayerNameNameY,
               tag + "JIM BOB band");
    const long long jim_ink = jim.stats.ink_pixels;
    const long long auto_ink = auto_bot.stats.ink_pixels;
    const Box name_box = ink_box(jim, gm::kPlayerNameNameY,
                                 gm::kPlayerNameNameY + name_face.cell_height - 1);
    check_eq(name_box.count, run_ink(name_face, "JIM BOB"), tag + "JIM BOB ink");
    const Box name_diff = diff_box(jim, auto_bot);
    check(name_diff.count > 0, tag + "a different name changes the frame");
    check(name_diff.y0 >= gm::kPlayerNameNameY &&
              name_diff.y1 < gm::kPlayerNameNameY + name_face.cell_height,
          tag + "name differences stay in rows 28..43 (" + box_text(name_diff) + ")");

    // The 10-character limit of 0x405b20 (`push 0xb` + `cmp ecx,[ebx-1]`).
    const Frame long_name = run(game, base_options(0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234"));
    const Frame prefix = run(game, base_options(0, "ABCDEFGHIJ"));
    check(long_name.ok && prefix.ok, tag + "long name renders");
    if (long_name.ok && prefix.ok) {
        check_eq(long_name.stats.chars, 10, tag + "long name chars 10");
        check_eq(long_name.stats.truncated, 20, tag + "long name truncated 20");
        check_eq(prefix.stats.truncated, 0, tag + "10-char prefix truncated 0");
        check_eq(differing_pixels(long_name, prefix), 0,
                 tag + "long name equals its 10-char prefix");
    }

    // A NUL ends the buffer (the editor measures with `repnz scas`).
    const Frame nul = run(game, base_options(0, std::string("AB\0CD", 5)));
    check(nul.ok, tag + "NUL name renders: " + nul.error);
    if (nul.ok) {
        check_eq(nul.stats.chars, 2, tag + "NUL ends the name");
        check_eq(nul.stats.truncated, 0, tag + "NUL name not truncated");
        check_eq(differing_pixels(nul, run(game, base_options(0, "AB"))), 0,
                 tag + "NUL name equals \"AB\"");
    }

    std::printf("  player_name names: \"JIM BOB\" x=%d %s (name ink %d, frame ink "
                "%lld) vs \"AUTOBOT\" (frame ink %lld): diff=%d px; empty frame ink=%d; "
                "30 chars -> chars=%d truncated=%d frame ink=%d, prefix diff=%lld; "
                "NUL name chars=%d\n",
                x_jim, box_text(name_box).c_str(), name_box.count, jim_ink, auto_ink,
                name_diff.count, empty.stats.ink_pixels, long_name.stats.chars,
                long_name.stats.truncated, long_name.stats.ink_pixels,
                differing_pixels(long_name, prefix), nul.stats.chars);
}

// --- the caret ---------------------------------------------------------------
void test_caret(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                const SpriteAsset& plate) {
    const std::string tag = "caret: ";
    const gm::FontAtlas& name_face = fonts.at(gm::kPlayerNameNameFont);

    gm::PlayerNameRenderOptions on = base_options(0, "PLAYER ONE");
    on.caret = true;
    const Frame with = run(game, on);
    gm::PlayerNameRenderOptions off = on;
    off.caret = false;
    const Frame without = run(game, off);
    check(with.ok && without.ok, tag + "both frames render");
    if (!with.ok || !without.ok) {
        return;
    }
    const int name_width = pen_width(name_face, "PLAYER ONE");
    const int x = gm::player_name_text_x(name_width);
    const Box box = diff_box(with, without);
    check_eq(with.stats.caret_pixels, gm::kPlayerNameCaretWidth * 2 +
                                          (gm::kPlayerNameCaretHeight - 2) * 2,
             tag + "8x17 hollow box is 46 pixels");
    check_eq(box.count, with.stats.caret_pixels, tag + "on/off diff equals caret pixels");
    check_eq(box.x0, x + name_width, tag + "box starts at the end of the name");
    check_eq(box.y0, gm::kPlayerNameCaretY, tag + "box top y 27");
    check_eq(box.x1, x + name_width + gm::kPlayerNameCaretWidth - 1, tag + "box right");
    check_eq(box.y1, gm::kPlayerNameCaretY + gm::kPlayerNameCaretHeight - 1,
             tag + "box bottom y 43");
    check_eq(with.stats.ink_pixels, without.stats.ink_pixels, tag + "ink unchanged");
    check_eq(without.stats.caret_pixels, 0, tag + "caret off paints no box");

    // The fill index is the PLATE palette entry (the box replaces plate pixels).
    if (plate.ok && plate.palette.size() > static_cast<std::size_t>(gm::kPlayerNameCaretColour)) {
        check_eq(static_cast<long long>(at(with, box.x0, box.y0)),
                 static_cast<long long>(plate.palette[gm::kPlayerNameCaretColour]),
                 tag + "box colour is PLATE palette[15]");
    }

    // The width rule of 0x489095..0x4890ae: M/W -> 16, I -> 6, else 8.
    struct WidthCase {
        std::string name;
        int pos;
        int width;
        char hint;
    };
    const WidthCase cases[3] = {
        {"MIKE", 0, gm::kPlayerNameCaretWideWidth, 'M'},
        {"MIKE", 1, gm::kPlayerNameCaretNarrowWidth, 'I'},
        {"MIKE", 2, gm::kPlayerNameCaretWidth, 'K'},
    };
    std::string case_numbers;
    for (const WidthCase& item : cases) {
        gm::PlayerNameRenderOptions options = base_options(0, item.name);
        options.caret = true;
        options.caret_pos = item.pos;
        const Frame frame = run(game, options);
        gm::PlayerNameRenderOptions plain = options;
        plain.caret = false;
        const Frame no_box = run(game, plain);
        check(frame.ok && no_box.ok, tag + "caret case renders");
        if (!frame.ok || !no_box.ok) {
            return;
        }
        const int prefix = pen_width(name_face, item.name.substr(0, item.pos));
        const int want_x = gm::player_name_text_x(pen_width(name_face, item.name)) + prefix;
        const Box case_box = diff_box(frame, no_box);
        check_eq(case_box.width(), item.width,
                 tag + "'" + std::string(1, item.hint) + "' box width");
        check_eq(case_box.x0, want_x, tag + "'" + std::string(1, item.hint) + "' box x");
        check_eq(gm::player_name_caret_width(static_cast<unsigned char>(item.hint)),
                 item.width, tag + "player_name_caret_width('" +
                                       std::string(1, item.hint) + "')");
        case_numbers += std::string(1, item.hint) + "=" + std::to_string(case_box.width()) +
                        " at x" + std::to_string(case_box.x0) + " ";
    }
    check_eq(gm::player_name_caret_width(static_cast<unsigned char>('\0')),
             gm::kPlayerNameCaretWidth, tag + "NUL caret width 8");

    // The caret position is clamped and -1 means the end.
    gm::PlayerNameRenderOptions clamp = base_options(0, "PLAYER ONE");
    clamp.caret = true;
    clamp.caret_pos = 99;
    const Frame clamped = run(game, clamp);
    check(clamped.ok, tag + "long caret position renders");
    if (clamped.ok) {
        check_eq(differing_pixels(clamped, with), 0, tag + "out-of-range position is the end");
    }
    gm::PlayerNameRenderOptions negative = base_options(0, "PLAYER ONE");
    negative.caret = true;
    negative.caret_pos = -5;
    const Frame negative_frame = run(game, negative);
    if (negative_frame.ok) {
        check_eq(differing_pixels(negative_frame, with), 0, tag + "-5 is the end");
    }

    // An empty name still has the caret at the centre of the plate.
    gm::PlayerNameRenderOptions empty;
    empty.name = "";
    empty.caret = true;
    const Frame empty_caret = run(game, empty);
    check(empty_caret.ok, tag + "empty name with caret renders");
    if (empty_caret.ok) {
        const Box empty_box = ink_box(empty_caret, gm::kPlayerNameCaretY,
                                      gm::kPlayerNameCaretY + gm::kPlayerNameCaretHeight - 1);
        check_eq(empty_box.x0, gm::kPlayerNameNameCenterX, tag + "empty caret at x 98");
        check_eq(empty_box.width(), gm::kPlayerNameCaretWidth,
                 tag + "empty caret box is 8 px wide");
        check_eq(empty_box.count, empty_caret.stats.caret_pixels,
                 tag + "empty caret box pixels");
    }

    // The fill index is an option; a different index changes the frame.
    gm::PlayerNameRenderOptions other_colour = on;
    other_colour.caret_colour = 143;
    const Frame recoloured = run(game, other_colour);
    check(recoloured.ok, tag + "other colour renders");
    if (recoloured.ok) {
        check(differing_pixels(recoloured, with) > 0, tag + "the fill index changes the frame");
        check_eq(recoloured.stats.caret_pixels, with.stats.caret_pixels,
                 tag + "the fill index does not change the pixel count");
    }

    std::printf("  player_name caret: 8x17 at (%d,27) %s, %d px; on/off diff=%d; "
                "widths %s; empty at x=%d; colour 15 vs 143 diff=%lld\n",
                x + name_width, box_text(box).c_str(), with.stats.caret_pixels, box.count,
                case_numbers.c_str(), gm::kPlayerNameNameCenterX,
                recoloured.ok ? differing_pixels(recoloured, with) : -1);
}

// --- PLATE as the background -------------------------------------------------
void test_plate(const fs::path& game, const std::map<int, gm::FontAtlas>& fonts,
                const SpriteAsset& plate) {
    const std::string tag = "plate: ";
    if (!plate.ok) {
        std::cout << "  player_name plate: SKIP (no PLATE in the install)\n";
        return;
    }
    check_eq(plate.width, gm::kPlayerNamePanelWidth, tag + "PLATE width 196");
    check_eq(plate.height, gm::kPlayerNamePanelHeight, tag + "PLATE height 131");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::PlayerOneName)),
             std::string(gm::kPlayerNamePanelName), tag + "menu_panel_asset(24)");
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::PlayerTwoName)),
             std::string(gm::kPlayerNamePanelName), tag + "menu_panel_asset(25)");

    gm::PlayerNameRenderOptions options = base_options(0, "PLAYER ONE");
    options.background = std::string(gm::kPlayerNamePanelName);
    const Frame with = run(game, options);
    const Frame black = run(game, base_options(0, "PLAYER ONE"));
    check(with.ok && black.ok, tag + "both backgrounds render");
    if (!with.ok || !black.ok) {
        return;
    }
    const std::vector<std::uint32_t> panel = plate_pixels(plate);
    check_eq(static_cast<long long>(panel.size()), 25676, tag + "196x131 decoded");
    long long plate_pixels_diff = 0;
    long long glyph_pixels = 0;
    long long outside = 0;
    for (int y = 0; y < with.height; ++y) {
        for (int x = 0; x < with.width; ++x) {
            if (x >= gm::kPlayerNamePanelWidth || y >= gm::kPlayerNamePanelHeight) {
                outside += is_black(at(with, x, y)) ? 0 : 1;
                continue;
            }
            const std::uint32_t painted = at(with, x, y);
            if (painted == at(black, x, y)) {
                glyph_pixels += is_black(painted) ? 0 : 1;
                continue;
            }
            ++plate_pixels_diff;
            const std::uint32_t want =
                panel[static_cast<std::size_t>(y) *
                          static_cast<std::size_t>(gm::kPlayerNamePanelWidth) +
                      static_cast<std::size_t>(x)];
            if (painted != want) {
                ++failures;
                ++checks;
                std::cerr << "FAIL: " << tag << "PLATE pixel (" << x << "," << y
                          << ") differs from the independent decode\n";
                return;
            }
        }
    }
    check_eq(outside, 0, tag + "outside 196x131 stays black");
    check_eq(with.stats.ink_pixels, black.stats.ink_pixels, tag + "ink unchanged");
    check_eq(glyph_pixels, with.stats.ink_pixels, tag + "every ink pixel is painted");
    const long long black_vs_plate = plate_pixels_diff + glyph_pixels;
    check(plate_pixels_diff > 0, tag + "the panel paints pixels");
    check_eq(static_cast<long long>(at(with, 0, 0)),
             static_cast<long long>(panel[0]), tag + "pixel (0,0) is the panel");

    // Determinism: the same call twice gives the same bytes.
    const Frame again = run(game, options);
    check(again.ok, tag + "second render");
    if (again.ok) {
        check_eq(differing_pixels(again, with), 0, tag + "deterministic");
    }

    std::printf("  player_name plate: 196x131 decoded, PLATE vs black diff=%lld px "
                "(glyph pixels %lld + panel-only %lld), ink=%d unchanged\n",
                black_vs_plate, glyph_pixels, plate_pixels_diff, with.stats.ink_pixels);
    (void)fonts;
}

// --- exact fade --------------------------------------------------------------
void test_fade(const fs::path& game) {
    const std::string tag = "fade LUT: ";
    constexpr long long kCanvas = static_cast<long long>(gm::kPlayerNameCanvasWidth) *
                                  gm::kPlayerNameCanvasHeight;

    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::cout << "  player_name fade: SKIP (no 1024-byte GAMEPAL: " << palette_error
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
    std::printf("  player_name fade: 16x256 built in memory from FRONTEND/GAMEPAL, no dump\n");

    const auto make = [&](bool fade, int level) {
        gm::PlayerNameRenderOptions options;
        options.background = std::string(gm::kPlayerNamePanelName);
        options.name = "PLAYER ONE";
        options.caret = true;
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
                if (x < gm::kPlayerNamePanelWidth && y < gm::kPlayerNamePanelHeight) {
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
    check_eq(static_cast<long long>(plain.rgba.size()), kCanvas, tag + "unfaded canvas 64000");

    const Frame identity = make(true, gm::kHudLutIdentityLevel);
    check(identity.ok, tag + "level 11 frame: " + identity.error);
    if (!identity.ok) {
        return;
    }
    const long long identity_diffs = differing_pixels(identity, plain);
    check_eq(identity_diffs, 0, tag + "level 11 is byte-identical to no fade");
    check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels, tag + "level 11 ink unchanged");
    check_eq(identity.stats.caret_pixels, plain.stats.caret_pixels,
             tag + "level 11 caret pixels unchanged");
    check_eq(outside_panel(identity), 0, tag + "level 11 leaves the canvas black");

    const Frame level0 = make(true, 0);
    check(level0.ok, tag + "level 0 frame: " + level0.error);
    if (!level0.ok) {
        return;
    }
    const long long black0 = black_pixels(level0);
    const long long black_plain = black_pixels(plain);
    const long long diff0 = differing_pixels(level0, plain);
    check(black0 > 0, tag + "level 0 still has black pixels to count");
    check(diff0 > 0, tag + "level 0 differs from the unfaded frame");
    check_eq(outside_panel(level0), 0, tag + "level 0 leaves the canvas black");
    check_eq(level0.stats.ink_pixels, plain.stats.ink_pixels, tag + "level 0 ink unchanged");

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
        check(differing_pixels(exact[static_cast<std::size_t>(i)], plain) > 0,
              tag + "level " + std::to_string(level) + " changes the frame");
        check_eq(outside_panel(exact[static_cast<std::size_t>(i)]), 0,
                 tag + "level " + std::to_string(level) + " leaves the canvas black");
        check_eq(exact[static_cast<std::size_t>(i)].stats.ink_pixels,
                 plain.stats.ink_pixels,
                 tag + "level " + std::to_string(level) + " ink unchanged");
    }
    const long long pair12_13 = differing_pixels(exact[0], exact[1]);
    const long long pair13_14 = differing_pixels(exact[1], exact[2]);
    const long long pair14_15 = differing_pixels(exact[2], exact[3]);
    check(pair12_13 > 0 && pair13_14 > 0 && pair14_15 > 0,
          tag + "levels 12..15 are four different frames");
    const long long to11_12 = differing_pixels(exact[0], identity);
    const long long to11_13 = differing_pixels(exact[1], identity);
    const long long to11_14 = differing_pixels(exact[2], identity);
    const long long to11_15 = differing_pixels(exact[3], identity);
    check(to11_12 > 0 && to11_13 > 0 && to11_14 > 0 && to11_15 > 0,
          tag + "each of 12..15 differs from the identity");

    const Frame clamp99 = make(true, 99);
    check(clamp99.ok, tag + "level 99 frame: " + clamp99.error);
    if (clamp99.ok) {
        check_eq(differing_pixels(clamp99, exact[3]), 0, tag + "level 99 clamps to 15");
    }

    std::printf("  player_name fade: level 11 diffs %lld/%lld; level 0 black %lld "
                "(plain %lld), diffs %lld; adjacent 12/13 %lld, 13/14 %lld, 14/15 %lld; "
                "vs 11 %lld/%lld/%lld/%lld\n",
                identity_diffs, kCanvas, black0, black_plain, diff0, pair12_13,
                pair13_14, pair14_15, to11_12, to11_13, to11_14, to11_15);

    {
        gm::PlayerNameRenderOptions options;
        options.background = std::string(gm::kPlayerNamePanelName);
        options.fade_lut = true;
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "no table and no dump refused");
        check(starts_with(refusal.error, "player_name_render: ") &&
                  contains(refusal.error, "fade LUT needs"),
              tag + "refusal names the missing table: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "refusal clears the buffer");
        check_eq(refusal.width, 0, tag + "refusal zeroes the width");
        check_eq(refusal.height, 0, tag + "refusal zeroes the height");
        check_eq(refusal.stats.ink_pixels, 0, tag + "refusal zeroes the stats");
    }
    {
        gm::PlayerNameRenderOptions options;
        options.background = std::string(gm::kPlayerNamePanelName);
        options.fade_lut = true;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame refusal = run(game, options);
        check(!refusal.ok, tag + "missing dump refused");
        check(starts_with(refusal.error, "player_name_render: ") &&
                  contains(refusal.error, "hud_lut:"),
              tag + "missing dump carries the hud_lut text: " + refusal.error);
        check_eq(static_cast<long long>(refusal.rgba.size()), 0,
                 tag + "missing dump clears the buffer");
    }
    {
        gm::PlayerNameRenderOptions options;
        options.background = std::string(gm::kPlayerNamePanelName);
        options.name = "PLAYER ONE";
        options.caret = true;
        options.fade_lut = true;
        options.fade_lut_level = gm::kHudLutIdentityLevel;
        options.fade_lut_table = &lut;
        options.fade_lut_dump = (temp_root() / "no-lut.bin").string();
        const Frame both = run(game, options);
        check(both.ok, tag + "a table wins over a missing dump: " + both.error);
        if (both.ok) {
            check_eq(differing_pixels(both, plain), 0,
                     tag + "table path equals the unfaded level 11 frame");
        }
    }
}

// --- negatives ---------------------------------------------------------------
void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::PlayerNameRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }

    check_refused(root / "game-without-bmp", gm::PlayerNameRenderOptions{}, "no BMP.IDX",
                  "BMP.IDX");

    if (game.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    // Real entry bytes, so exactly one asset is missing at a time.
    const std::vector<std::uint8_t> fonts4 = archive_entry_bytes(game, "FONTS4");
    const std::vector<std::uint8_t> fonts2 = archive_entry_bytes(game, "FONTS2");
    const std::vector<std::uint8_t> plate = archive_entry_bytes(game, "PLATE");
    const std::vector<std::uint8_t> fonts1 = archive_entry_bytes(game, "FONTS1");
    check(!fonts4.empty() && !fonts2.empty() && !plate.empty() && !fonts1.empty(),
          "negatives: FONTS1/FONTS2/FONTS4/PLATE extracted from the install");
    if (fonts4.empty() || fonts2.empty() || plate.empty() || fonts1.empty()) {
        fs::remove_all(root, ec);
        return;
    }

    const fs::path font4less = root / "no-fonts4" / "FRONTEND";
    fs::create_directories(font4less, ec);
    check(write_archive(font4less, {{"FONTS1", fonts1}, {"FONTS2", fonts2}, {"PLATE", plate}}),
          "negatives: archive without FONTS4 written");
    check_refused(root / "no-fonts4", gm::PlayerNameRenderOptions{}, "missing FONTS4",
                  "font 5");

    const fs::path font2less = root / "no-fonts2" / "FRONTEND";
    fs::create_directories(font2less, ec);
    check(write_archive(font2less, {{"FONTS4", fonts4}, {"PLATE", plate}}),
          "negatives: archive without FONTS2 written");
    check_refused(root / "no-fonts2", gm::PlayerNameRenderOptions{}, "missing FONTS2",
                  "font 3");

    const fs::path plateless = root / "no-plate" / "FRONTEND";
    fs::create_directories(plateless, ec);
    check(write_archive(plateless, {{"FONTS4", fonts4}, {"FONTS2", fonts2}}),
          "negatives: archive without PLATE written");
    check_refused(root / "no-plate", gm::PlayerNameRenderOptions{}, "missing PLATE",
                  "PLATE");

    fs::remove_all(root, ec);

    gm::PlayerNameRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    // The minimal archive (both atlases + PLATE) renders.
    const fs::path minimal = root / "minimal" / "FRONTEND";
    fs::create_directories(minimal, ec);
    if (write_archive(minimal, {{"FONTS4", fonts4}, {"FONTS2", fonts2}, {"PLATE", plate}})) {
        gm::PlayerNameRenderOptions options;
        options.name = "ZED";
        options.caret = true;
        const Frame frame = run(root / "minimal", options);
        check(frame.ok, "negatives: minimal archive renders: " + frame.error);
        if (frame.ok) {
            check_eq(frame.stats.chars, 3, "negatives: minimal chars 3");
            check_eq(frame.stats.rows, 1, "negatives: minimal rows 1");
            std::printf("  player_name minimal archive (FONTS4+FONTS2+PLATE): rows=%d "
                        "chars=%d ink=%d missing=%d caret=%d\n",
                        frame.stats.rows, frame.stats.chars, frame.stats.ink_pixels,
                        frame.stats.missing_glyphs, frame.stats.caret_pixels);
        }
    }
    fs::remove_all(root, ec);

    const Frame frame = run(game, gm::PlayerNameRenderOptions{});
    check(frame.ok, "negatives: shipped install renders: " + frame.error);
    if (frame.ok) {
        check_eq(frame.stats.rows, 1, "negatives: rows 1");
        check_eq(frame.stats.chars, 0, "negatives: default name empty");
        check_eq(frame.stats.arrows, 0, "negatives: no arrows");
        check_eq(frame.stats.caret_pixels, 46, "negatives: default caret box");
        std::printf("  player_name_render negatives: rows=%d chars=%d ink=%d arrows=%d "
                    "missing=%d caret=%d\n",
                    frame.stats.rows, frame.stats.chars, frame.stats.ink_pixels,
                    frame.stats.arrows, frame.stats.missing_glyphs,
                    frame.stats.caret_pixels);
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
        std::cout << "player_name_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kPlayerNameTitleFont, gm::kPlayerNameHintFont,
                             gm::kPlayerNameNameFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "player_name_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }
    check(fonts.at(gm::kPlayerNameNameFont).cell_height == 16,
          "font 3 is the 16 px FONTS2 display face");

    const SpriteAsset plate = load_sprite_asset(game, std::string(gm::kPlayerNamePanelName));

    test_titles(game, fonts);
    test_names(game, fonts);
    test_caret(game, fonts, plate);
    test_plate(game, fonts, plate);
    test_negatives(game);
    test_fade(game);
    if (failures != 0) {
        std::cerr << "player_name_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "player_name_render: ok (" << checks
              << " checks) screens 24/25 -> 320x200 RGBA (rows 1/step 0, title font 5 "
                 "y 14, name font 3 centred at x 98 y 28, 10-char limit, hollow 8x17 "
                 "caret at y 27, six hints x 16, PLATE, exact fade, negatives)\n";
    return 0;
}
