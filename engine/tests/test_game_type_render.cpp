// One frame of screen 19, `MULTIPLAYER GAME TYPE` (`M8.8` slice 1,
// `ui/game_type_render`, docs/GAME_TYPE_RENDER.md).
//
// The module is stateless: the test owns the cursor `[0x68ccf4]` and the
// availability byte `0x4c1290` (as `GameTypeRenderOptions::cursor/mask`) and
// pins the frame with pixels, not with the code:
//
//   * the shipped install: the title `MULTIPLAYER GAME TYPE` (font 5) centred at
//     y 10, `One Computer` at (18, 18), the ten other modes with step 8 and
//     `Cancel` at (18, 106) with the full mask `0xFF`; `rows == 12`,
//     `modes == 11`, `cancel_row == 106`, no missing glyph, every pixel opaque;
//     every one of the twelve text bands is compared byte for byte with an
//     independent blit of the same atlas;
//   * the ordering proof: `game_type_rows` returns the handler's item ids
//     (0..10) in the VA order of `0x48a5b1`..`0x48a764` plus `Cancel` (11), and
//     the single-bit masks select the documented labels;
//   * the mask `0x4c1290` is a byte: `0x01` -> `Network Server` +
//     `Network No Render Server`, `0x02` -> `Network Client`, `0x04` ->
//     `TCP/IP Server` + `TCP/IP No Render Server`, `0x08` -> `TCP/IP Client`,
//     `0x10`/`0x20` -> `Modem Server`/`Client`, `0x40`/`0x80` -> `Serial Cable
//     Server`/`Client`; disabled modes shift the rest up (no blank rows), and
//     `0x100` behaves like `0` because only the low byte is read;
//   * `mask 0` still paints `One Computer` + `Cancel` (2 rows) — an empty list is
//     not reachable, because both have no bit in the handler;
//   * the cursor is an item id: 0, 1, 10 and 11 use font 4 on exactly that band
//     (pixel counts printed), a cursor on a disabled mode highlights nothing and
//     is byte-identical to `cursor -1`, and out-of-range values highlight
//     nothing;
//   * `background="PLATE"`: the 196x131 panel is decoded here independently, the
//     frame is compared with plate + glyphs, the 38324 pixels outside stay black
//     and `menu_panel_asset(Screen::MultiplayerType) == "PLATE"`;
//   * negatives: an empty game path, a game without `FRONTEND/BMP.IDX`, a valid
//     index without `FONTS4`, an unknown background, and an exact fade without a
//     table/dump — all texts with a cleared buffer and zeroed stats; two renders
//     are identical (determinism);
//   * exact fade: level 11 is byte-identical to the unfaded frame, level 0
//     differs, the ink count is unchanged and the canvas outside the panel stays
//     black.
//
// The installation is only read; the pixel half skips cleanly when it is
// absent.  Numbers are printed and docs/GAME_TYPE_RENDER.md §4 pins them.

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
#include "ui/game_type_render.h"
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

/// One frame (or one refusal) of the renderer.
struct Frame {
    bool ok = false;
    std::string error;
    std::vector<std::uint32_t> rgba;
    int width = 0;
    int height = 0;
    gm::GameTypeRenderStats stats;
};

Frame run(const fs::path& game, const gm::GameTypeRenderOptions& options) {
    Frame frame;
    frame.ok = gm::render_game_type(game, options, frame.rgba, frame.width, frame.height,
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

bool is_black(std::uint32_t pixel) { return pixel == gm::kGameTypeBackground; }

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

int mode_y(int row) { return gm::kGameTypeFirstRow + gm::kGameTypeItemStep * row; }

/// Sum of the atlas advances of a run: what `0x485dd0` centres with
/// (`0x47fa50`); `FontAtlas::measure` adds the trailing tracking pixel.
int pen_width(const gm::FontAtlas& face, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += face.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// Independent blit of one text run at (`x`, `y`) clipped to `width` x
/// `height`: glyph row `r` at `y + r` (`0x47faf0`), palette index 0
/// transparent, the atlas palette forced opaque.  0 marks an untouched pixel.
void blit_run(std::vector<std::uint32_t>& band, int width, int height,
              const gm::FontAtlas& atlas, std::string_view text, int x, int y) {
    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        const gm::FontGlyph* glyph = atlas.glyph(ch);
        if (glyph == nullptr || glyph->width <= 0) {
            pen += atlas.advance_of(ch);
            continue;
        }
        for (int row = 0; row < atlas.cell_height; ++row) {
            const int py = y + row;
            if (py < 0 || py >= height) {
                continue;
            }
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
                const std::size_t base = static_cast<std::size_t>(index) * 4u;
                band[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(px)] =
                    (static_cast<std::uint32_t>(atlas.palette[base + 0]) << 24) |
                    (static_cast<std::uint32_t>(atlas.palette[base + 1]) << 16) |
                    (static_cast<std::uint32_t>(atlas.palette[base + 2]) << 8) | 0xFFu;
            }
        }
        pen += atlas.advance_of(ch);
    }
}

/// Glyph ink of one run inside the 196 px band: the `ink_pixels` contribution.
long long glyph_ink(const gm::FontAtlas& atlas, std::string_view text, int x) {
    std::vector<std::uint32_t> band(
        static_cast<std::size_t>(gm::kGameTypePanelWidth) *
            static_cast<std::size_t>(atlas.cell_height),
        0u);
    blit_run(band, gm::kGameTypePanelWidth, atlas.cell_height, atlas, text, x, 0);
    long long ink = 0;
    for (const std::uint32_t pixel : band) {
        ink += pixel == 0 ? 0 : 1;
    }
    return ink;
}

/// One `FRONTEND/BMP.IDX` entry decoded by the engine's own reader.
struct Asset {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  ///< top-down, as `paint_background` reads it
    std::vector<std::uint32_t> palette;
};

Asset load_asset(const fs::path& game, const std::string& name) {
    Asset asset;
    if (game.empty()) {
        return asset;
    }
    std::string error;
    std::optional<gm::BigFile> archive =
        gm::BigFile::open(game / "FRONTEND" / "BMP.IDX", error);
    if (!archive) {
        return asset;
    }
    const gm::BigEntry* found = nullptr;
    for (const gm::BigEntry& candidate : archive->entries()) {
        std::string upper;
        for (const char c : candidate.name) {
            upper.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        }
        if (upper == name) {
            found = &candidate;
            break;
        }
    }
    if (found == nullptr) {
        return asset;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*found, error);
    if (!bytes) {
        return asset;
    }
    std::optional<gm::BmpImage> bmp = gm::parse_bmp(*bytes, error);
    if (!bmp || bmp->bpp != 8) {
        return asset;
    }
    asset.ok = true;
    asset.width = bmp->width;
    asset.height = bmp->height;
    asset.pixels = std::move(bmp->pixels);
    const std::size_t entries = bmp->palette.size() / 4u;
    for (std::size_t i = 0; i < entries; ++i) {
        asset.palette.push_back(
            (static_cast<std::uint32_t>(bmp->palette[i * 4u + 0]) << 24) |
            (static_cast<std::uint32_t>(bmp->palette[i * 4u + 1]) << 16) |
            (static_cast<std::uint32_t>(bmp->palette[i * 4u + 2]) << 8) | 0xFFu);
    }
    return asset;
}

using FontMap = std::map<int, gm::FontAtlas>;

/// The whole frame painted independently from the proven geometry: the
/// optional `plate` at (0,0), the centred title font 5 at y 10, the rows of
/// `mask` at (18, 18 + 8*k) and `Cancel` below them; the row of `cursor` uses
/// font 4.  Everything is clipped to the 196x131 band, like the plate copy
/// `0x6865f8` of `0x485dd0`.
std::vector<std::uint32_t> expected_frame(const FontMap& fonts, const Asset* plate,
                                          std::uint32_t mask, int cursor) {
    const int width = gm::kGameTypeCanvasWidth;
    const int height = gm::kGameTypeCanvasHeight;
    std::vector<std::uint32_t> canvas(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
        gm::kGameTypeBackground);
    if (plate != nullptr && plate->ok) {
        const int copy_width = std::min(width, plate->width);
        const int copy_height = std::min(height, plate->height);
        for (int y = 0; y < copy_height; ++y) {
            for (int x = 0; x < copy_width; ++x) {
                const std::uint8_t index =
                    plate->pixels[static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(plate->width) +
                                  static_cast<std::size_t>(x)];
                canvas[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)] = plate->palette[index];
            }
        }
    }
    // The title: centred over 196 px, row 10.
    {
        const gm::FontAtlas& title = fonts.at(gm::kGameTypeTitleFont);
        const int x = std::max(
            0, (gm::kGameTypePanelWidth - pen_width(title, gm::kGameTypeTitle)) / 2);
        blit_run(canvas, width, height, title, gm::kGameTypeTitle, x, gm::kGameTypeTitleY);
    }
    const std::vector<gm::GameTypeEntry> rows = gm::game_type_rows(mask);
    int painted = 0;
    for (const gm::GameTypeEntry& entry : rows) {
        const int y = mode_y(painted);
        const int font =
            entry.item == cursor ? gm::kGameTypeSelectedFont : gm::kGameTypeRowFont;
        blit_run(canvas, width, height, fonts.at(font), entry.label, gm::kGameTypeItemX,
                 y);
        ++painted;
    }
    return canvas;
}

[[maybe_unused]] std::size_t count_modes(std::uint32_t mask) {
    std::size_t count = 0;
    for (const gm::GameTypeEntry& entry : gm::game_type_rows(mask)) {
        if (entry.item != gm::kGameTypeCancelItem) {
            ++count;
        }
    }
    return count;
}

/// Temporary root: the approved `/tmp/opencode` tree when present, else the
/// platform temp directory.  Only test scratch data is written there.
fs::path temp_root() {
    std::error_code ec;
    const fs::path approved = "/tmp/opencode";
    if (fs::is_directory(approved, ec)) {
        return approved / "gm_game_type_render";
    }
    return fs::temp_directory_path() / "gm_game_type_render";
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

void check_refused(const fs::path& game, const gm::GameTypeRenderOptions& options,
                   const std::string& what, std::string_view needle) {
    const Frame frame = run(game, options);
    check(!frame.ok, what + ": refused");
    check(starts_with(frame.error, "game_type_render: "),
          what + ": prefix (got \"" + frame.error + "\")");
    check(contains(frame.error, needle),
          what + ": text names '" + std::string(needle) + "' (got \"" + frame.error + "\")");
    check_eq(static_cast<long long>(frame.rgba.size()), 0, what + ": buffer cleared");
    check_eq(frame.width, 0, what + ": width zeroed");
    check_eq(frame.height, 0, what + ": height zeroed");
    check_eq(frame.stats.rows, 0, what + ": stats zeroed");
}

// ------------------------------------------------------------------ frame --

void test_install_frame(const fs::path& game, const FontMap& fonts) {
    const Frame frame = run(game, gm::GameTypeRenderOptions{});
    check(frame.ok, "install: rendered: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.width, 320, "install: width 320");
    check_eq(frame.height, 200, "install: height 200");
    check_eq(static_cast<long long>(frame.rgba.size()), 64000, "install: 64000 pixels");
    check_eq(frame.stats.rows, gm::kGameTypeRows, "install: rows 12");
    check_eq(frame.stats.modes, gm::kGameTypeModeItems, "install: modes 11");
    check_eq(frame.stats.first_row, gm::kGameTypeFirstRow, "install: first_row 18");
    check_eq(frame.stats.cancel_row, 106, "install: Cancel at y 106");
    check_eq(frame.stats.arrows, 0, "install: no scroll arrows");
    check_eq(frame.stats.missing_glyphs, 0, "install: no missing glyph");
    check_eq(frame.stats.highlight_item, 0, "install: cursor 0 highlights One Computer");
    check(frame.stats.ink_pixels > 0, "install: glyph ink counted");

    long long not_opaque = 0;
    for (const std::uint32_t pixel : frame.rgba) {
        not_opaque += (pixel & 0xFFu) == 0xFFu ? 0 : 1;
    }
    check_eq(not_opaque, 0, "install: every pixel opaque");

    const std::vector<std::uint32_t> expected = expected_frame(fonts, nullptr, 0xFFu, 0);
    check_eq(differing_pixels(frame.rgba, expected), 0,
             "install: frame equals the independent blit");

    // The twelve bands: title 10, rows 18..98, Cancel 106.
    const gm::FontAtlas& title = fonts.at(gm::kGameTypeTitleFont);
    const int title_x =
        std::max(0, (gm::kGameTypePanelWidth - pen_width(title, gm::kGameTypeTitle)) / 2);
    const Box title_box = ink_box(frame, gm::kGameTypeTitleY,
                                  gm::kGameTypeTitleY + title.cell_height - 1);
    check(!title_box.empty(), "install: title has ink");
    check(title_box.x0 >= title_x, "install: title inside its centred run");
    check(title_box.x1 < title_x + pen_width(title, gm::kGameTypeTitle),
          "install: title ink ends inside the measured run");

    const std::vector<gm::GameTypeEntry> rows = gm::game_type_rows(0xFFu);
    long long ink_sum = glyph_ink(title, gm::kGameTypeTitle, title_x);
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const gm::FontAtlas& face =
            fonts.at(row == 0 ? gm::kGameTypeSelectedFont : gm::kGameTypeRowFont);
        const int y = mode_y(row);
        const Box box = ink_box(frame, y, y + face.cell_height - 1);
        check(!box.empty(), "install: row " + std::to_string(row) + " has ink");
        check(box.x0 >= gm::kGameTypeItemX,
              "install: row " + std::to_string(row) + " starts at x 18");
        check_eq(box.y0, y, "install: row " + std::to_string(row) + " top is its row");
        ink_sum += glyph_ink(face, rows[static_cast<std::size_t>(row)].label,
                            gm::kGameTypeItemX);
    }
    check_eq(frame.stats.ink_pixels, ink_sum, "install: ink equals the sum of the runs");

    const Box first = ink_box(frame, mode_y(0), mode_y(0) + fonts.at(0).cell_height - 1);
    const Box cancel = ink_box(frame, mode_y(11), mode_y(11) + fonts.at(0).cell_height - 1);
    std::printf("  install: title bbox x %d..%d y %d..%d (x0 %d); One Computer bbox "
                "x %d..%d y %d..%d; Cancel bbox x %d..%d y %d..%d; ink %d\n",
                title_box.x0, title_box.x1, title_box.y0, title_box.y1, title_x,
                first.x0, first.x1, first.y0, first.y1, cancel.x0, cancel.x1,
                cancel.y0, cancel.y1, frame.stats.ink_pixels);
}

void test_order() {
    const std::vector<gm::GameTypeEntry> rows = gm::game_type_rows(0xFFu);
    const std::string expected[] = {
        "One Computer",          "Network Client",       "Network Server",
        "Network No Render Server", "TCP/IP Client",     "TCP/IP Server",
        "TCP/IP No Render Server", "Serial Cable Client", "Serial Cable Server",
        "Modem Client",          "Modem Server",         "Cancel",
    };
    check_eq(static_cast<long long>(rows.size()), 12, "order: twelve rows");
    for (int i = 0; i < 12; ++i) {
        check_eq(std::string(rows[static_cast<std::size_t>(i)].label), expected[i],
                 "order: row " + std::to_string(i));
        check_eq(rows[static_cast<std::size_t>(i)].item, i,
                 "order: item id " + std::to_string(i));
    }
    check_eq(static_cast<long long>(gm::game_type_entries().size()), 11,
             "order: eleven modes in the table");
    check_eq(std::string(gm::kGameTypeTitle), "MULTIPLAYER GAME TYPE",
             "order: title text");
}

void test_mask(const fs::path& game, const FontMap& fonts) {
    struct Case {
        std::uint32_t mask;
        int modes;
        std::string labels;
    };
    const Case cases[] = {
        {0x00u, 1, "One Computer"},                                  // no transports
        {0x01u, 3, "Network Server + Network No Render Server"},     // server type 1
        {0x02u, 2, "Network Client"},                                // client type 1
        {0x04u, 3, "TCP/IP Server + TCP/IP No Render Server"},       // server type 2
        {0x08u, 2, "TCP/IP Client"},                                 // client type 2
        {0x10u, 2, "Modem Server"},                                  // server type 3
        {0x20u, 2, "Modem Client"},                                  // client type 3
        {0x40u, 2, "Serial Cable Server"},                           // server type 4
        {0x80u, 2, "Serial Cable Client"},                           // client type 4
        {0xFFu, 11, "all eight bits"},
        {0x100u, 1, "high bits are not read (byte only)"},
        {0x1FFu, 11, "high bits ignored again"},
    };
    for (const Case& item : cases) {
        gm::GameTypeRenderOptions options;
        options.mask = item.mask;
        options.cursor = -1;
        const Frame frame = run(game, options);
        check(frame.ok, "mask " + std::to_string(item.mask) + ": rendered: " +
                            frame.error);
        if (!frame.ok) {
            continue;
        }
        check_eq(frame.stats.modes, item.modes,
                 "mask " + std::to_string(item.mask) + ": modes");
        check_eq(frame.stats.rows, item.modes + 1,
                 "mask " + std::to_string(item.mask) + ": rows = modes + Cancel");
        check_eq(frame.stats.cancel_row, mode_y(item.modes),
                 "mask " + std::to_string(item.mask) + ": Cancel row");
        check_eq(frame.stats.first_row, gm::kGameTypeFirstRow,
                 "mask " + std::to_string(item.mask) + ": first row");
        const std::vector<std::uint32_t> expected =
            expected_frame(fonts, nullptr, item.mask, -1);
        check_eq(differing_pixels(frame.rgba, expected), 0,
                 "mask " + std::to_string(item.mask) + ": independent blit");
        // One row above Cancel is the last mode; one below it is empty canvas.
        const gm::FontAtlas& face = fonts.at(0);
        const Box below = ink_box(frame, frame.stats.cancel_row + face.cell_height,
                                  frame.stats.cancel_row + face.cell_height);
        check(below.empty(), "mask " + std::to_string(item.mask) +
                                 ": nothing below Cancel");
        std::printf("  mask 0x%02x: modes %d rows %d cancel y %d (%s)\n", item.mask,
                    frame.stats.modes, frame.stats.rows, frame.stats.cancel_row,
                    item.labels.c_str());
    }

    // The per-bit label sets of the handler, checked on the model rows.
    struct BitCase {
        std::uint32_t bit;
        const char* first;
        const char* second;  // nullptr when the bit selects one label
    };
    const BitCase bits[] = {
        {0x01u, "Network Server", "Network No Render Server"},
        {0x02u, "Network Client", nullptr},
        {0x04u, "TCP/IP Server", "TCP/IP No Render Server"},
        {0x08u, "TCP/IP Client", nullptr},
        {0x10u, "Modem Server", nullptr},
        {0x20u, "Modem Client", nullptr},
        {0x40u, "Serial Cable Server", nullptr},
        {0x80u, "Serial Cable Client", nullptr},
    };
    for (const BitCase& bit : bits) {
        const std::vector<gm::GameTypeEntry> rows = gm::game_type_rows(bit.bit);
        check_eq(static_cast<long long>(rows.size()),
                 bit.second == nullptr ? 3 : 4,
                 std::string("bit 0x") + std::to_string(bit.bit) + ": row count");
        check_eq(std::string(rows[1].label), bit.first,
                 std::string("bit 0x") + std::to_string(bit.bit) + ": first label");
        if (bit.second != nullptr) {
            check_eq(std::string(rows[2].label), bit.second,
                     std::string("bit 0x") + std::to_string(bit.bit) + ": second label");
        }
        check_eq(std::string(rows.back().label), "Cancel",
                 std::string("bit 0x") + std::to_string(bit.bit) + ": Cancel last");
    }
}

void test_cursor(const fs::path& game, const FontMap& fonts) {
    // Full mask: item ids and visible rows coincide (0..11).
    for (const int cursor : {0, 1, 10, 11}) {
        gm::GameTypeRenderOptions options;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, "cursor " + std::to_string(cursor) + ": rendered: " +
                            frame.error);
        if (!frame.ok) {
            continue;
        }
        check_eq(frame.stats.highlight_item, cursor,
                 "cursor " + std::to_string(cursor) + ": highlighted item");
        const std::vector<gm::GameTypeEntry> rows = gm::game_type_rows(0xFFu);
        const gm::FontAtlas& face = fonts.at(gm::kGameTypeSelectedFont);
        const int y = mode_y(cursor);
        const Box box = ink_box(frame, y, y + face.cell_height - 1);
        long long white = 0;
        long long band_ink = 0;
        const std::string label(rows[static_cast<std::size_t>(cursor)].label);
        for (int row = 0; row < face.cell_height; ++row) {
            for (int x = 0; x < gm::kGameTypePanelWidth; ++x) {
                const std::uint32_t& pixel = at(frame, x, y + row);
                white += pixel == 0xEFEFEFFFu ? 1 : 0;
                band_ink += is_black(pixel) ? 0 : 1;
            }
        }
        check(box.count > 0, "cursor " + std::to_string(cursor) + ": band has ink");
        check(band_ink > 0, "cursor " + std::to_string(cursor) + ": band ink counted");
        // Newer faces of `FONTS4` may use more than one palette index; the
        // independent blit is the exact check, the white count is the number
        // the doc pins.
        const std::vector<std::uint32_t> expected =
            expected_frame(fonts, nullptr, 0xFFu, cursor);
        check_eq(differing_pixels(frame.rgba, expected), 0,
                 "cursor " + std::to_string(cursor) + ": independent blit");
        std::printf("  cursor %d (%s): highlighted item %d, band ink %lld, white "
                    "pixels %lld\n",
                    cursor, label.c_str(), frame.stats.highlight_item, band_ink, white);
    }

    // A disabled mode is skipped by the navigation (`0x48ef00`): the cursor does
    // not highlight a row that is not drawn and the frame equals `cursor -1`.
    {
        gm::GameTypeRenderOptions disabled;
        disabled.mask = gm::kGameTypeBitNetworkClient;  // Network Client only
        disabled.cursor = 2;                            // Network Server: disabled
        const Frame frame = run(game, disabled);
        check(frame.ok, "cursor disabled: rendered: " + frame.error);
        if (frame.ok) {
            check_eq(frame.stats.highlight_item, -1,
                     "cursor disabled: nothing highlighted");
            gm::GameTypeRenderOptions none = disabled;
            none.cursor = -1;
            const Frame plain = run(game, none);
            check(plain.ok, "cursor disabled: plain frame: " + plain.error);
            if (plain.ok) {
                check_eq(differing_pixels(frame.rgba, plain.rgba), 0,
                         "cursor disabled: frame equals cursor -1");
                std::printf("  cursor 2 with mask 0x02 (Network Server disabled): "
                            "highlight none, diffs vs cursor -1 = 0, ink %d\n",
                            frame.stats.ink_pixels);
            }
        }
    }

    // Out-of-range cursors highlight nothing.
    for (const int cursor : {-1, 12, 99}) {
        gm::GameTypeRenderOptions options;
        options.cursor = cursor;
        const Frame frame = run(game, options);
        check(frame.ok, "cursor " + std::to_string(cursor) + ": rendered: " +
                            frame.error);
        if (frame.ok) {
            check_eq(frame.stats.highlight_item, -1,
                     "cursor " + std::to_string(cursor) + ": no highlight");
            const std::vector<std::uint32_t> expected =
                expected_frame(fonts, nullptr, 0xFFu, cursor);
            check_eq(differing_pixels(frame.rgba, expected), 0,
                     "cursor " + std::to_string(cursor) + ": all rows font 0");
        }
    }

    // Cancel is item 11 even with a zero mask (two rows).
    {
        gm::GameTypeRenderOptions options;
        options.mask = 0;
        options.cursor = 11;
        const Frame frame = run(game, options);
        check(frame.ok, "cursor 11 mask 0: rendered: " + frame.error);
        if (frame.ok) {
            check_eq(frame.stats.highlight_item, 11,
                     "cursor 11 mask 0: Cancel highlighted");
            check_eq(frame.stats.cancel_row, mode_y(1), "cursor 11 mask 0: Cancel y 26");
            const std::vector<std::uint32_t> expected =
                expected_frame(fonts, nullptr, 0, 11);
            check_eq(differing_pixels(frame.rgba, expected), 0,
                     "cursor 11 mask 0: independent blit");
        }
    }
}

void test_plate(const fs::path& game, const FontMap& fonts) {
    check_eq(std::string(gm::menu_panel_asset(gm::Screen::MultiplayerType)),
             std::string(gm::kGameTypePanelName),
             "plate: menu_panel_asset(MultiplayerType) is PLATE");
    const Asset plate = load_asset(game, std::string(gm::kGameTypePanelName));
    check(plate.ok, "plate: PLATE decoded");
    if (!plate.ok) {
        return;
    }
    check_eq(plate.width, gm::kGameTypePanelWidth, "plate: width 196");
    check_eq(plate.height, gm::kGameTypePanelHeight, "plate: height 131");

    gm::GameTypeRenderOptions options;
    options.background = std::string(gm::kGameTypePanelName);
    const Frame frame = run(game, options);
    check(frame.ok, "plate: rendered: " + frame.error);
    if (!frame.ok) {
        return;
    }
    check_eq(frame.stats.ink_pixels,
             run(game, gm::GameTypeRenderOptions{}).stats.ink_pixels,
             "plate: text ink unchanged");
    const std::vector<std::uint32_t> expected = expected_frame(fonts, &plate, 0xFFu, 0);
    const long long diffs = differing_pixels(frame.rgba, expected);
    check_eq(diffs, 0, "plate: frame equals PLATE + glyphs");

    long long outside = 0;
    long long panel_ink = 0;
    long long panel_black = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const bool inside =
                x < gm::kGameTypePanelWidth && y < gm::kGameTypePanelHeight;
            if (!inside) {
                outside += is_black(at(frame, x, y)) ? 0 : 1;
            } else if (!is_black(at(frame, x, y))) {
                ++panel_ink;
            } else {
                ++panel_black;
            }
        }
    }
    check_eq(outside, 0, "plate: outside the 196x131 band stays black");
    check_eq(panel_ink + panel_black, 196 * 131,
             "plate: the panel covers exactly 196x131 pixels");
    const Frame black = run(game, gm::GameTypeRenderOptions{});
    check(black.ok, "plate: black frame: " + black.error);
    const long long changed = black.ok ? differing_pixels(frame.rgba, black.rgba) : -1;
    check(changed > 0, "plate: PLATE changes the frame");
    std::printf("  plate: 196x131, pixels differing from black %lld, canvas outside "
                "the panel %d, ink (glyphs) %d\n",
                changed, 320 * 200 - 196 * 131, frame.stats.ink_pixels);
}

void test_negatives(const fs::path& game) {
    check_refused(fs::path{}, gm::GameTypeRenderOptions{}, "empty game path",
                  "no game directory");

    const fs::path root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "game-without-bmp", ec);
    check(!ec, "negatives: temp tree created");
    if (ec) {
        return;
    }
    check_refused(root / "game-without-bmp", gm::GameTypeRenderOptions{},
                  "no BMP.IDX", "BMP.IDX");

    // A valid BIGFILE index whose only entry is FONTS1: the three faces of this
    // screen come from `FONTS4` (fonts 0/4/5), which is loaded first as font 5.
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
    check_refused(root / "game-without-font4", gm::GameTypeRenderOptions{},
                  "missing FONTS4 (the face atlas)", "FONTS4");

    fs::remove_all(root, ec);

    if (game.empty()) {
        return;
    }

    gm::GameTypeRenderOptions bad_background;
    bad_background.background = "NOSUCH";
    check_refused(game, bad_background, "unknown background", "NOSUCH");

    gm::GameTypeRenderOptions bad_fade;
    bad_fade.fade_lut = true;
    check_refused(game, bad_fade, "fade without a table", "fade LUT needs");
    gm::GameTypeRenderOptions bad_dump;
    bad_dump.fade_lut = true;
    bad_dump.fade_lut_dump = (temp_root() / "no-lut.bin").string();
    check_refused(game, bad_dump, "fade with a missing dump", "hud_lut:");

    // Determinism: two calls, byte-identical frames and numbers.
    const Frame first = run(game, gm::GameTypeRenderOptions{});
    const Frame second = run(game, gm::GameTypeRenderOptions{});
    check(first.ok && second.ok, "negatives: two renders succeed");
    if (first.ok && second.ok) {
        check_eq(differing_pixels(first.rgba, second.rgba), 0,
                 "negatives: determinism (0 differing pixels)");
        check_eq(first.stats.ink_pixels, second.stats.ink_pixels,
                 "negatives: determinism of ink");
    }
}

void test_fade(const fs::path& game) {
    std::string palette_error;
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, palette_error);
    if (!palette || palette->size() != gm::kHudLutPaletteBytes) {
        std::printf("  fade: SKIP (no 1024-byte GAMEPAL: %s)\n",
                    palette_error.c_str());
        return;
    }
    gm::HudLut lut;
    std::string error;
    if (!gm::build_hud_lut(*palette, lut, &error)) {
        check(false, "fade: build_hud_lut: " + error);
        return;
    }
    const auto make = [&](bool fade, int level) {
        gm::GameTypeRenderOptions options;
        options.cursor = 0;
        options.fade_lut = fade;
        options.fade_lut_level = level;
        if (fade) {
            options.fade_lut_table = &lut;
        }
        return run(game, options);
    };
    const Frame plain = make(false, gm::kHudLutIdentityLevel);
    check(plain.ok, "fade: unfaded frame: " + plain.error);
    if (!plain.ok) {
        return;
    }
    const Frame identity = make(true, gm::kHudLutIdentityLevel);
    check(identity.ok, "fade: identity frame: " + identity.error);
    if (identity.ok) {
        check_eq(differing_pixels(identity.rgba, plain.rgba), 0,
                 "fade: level 11 is byte-identical");
        check_eq(identity.stats.ink_pixels, plain.stats.ink_pixels,
                 "fade: level 11 ink unchanged");
    }
    const Frame zero = make(true, 0);
    check(zero.ok, "fade: level 0 frame: " + zero.error);
    if (zero.ok) {
        check(differing_pixels(zero.rgba, plain.rgba) > 0,
              "fade: level 0 changes the frame");
        check_eq(zero.stats.ink_pixels, plain.stats.ink_pixels,
                 "fade: level 0 ink unchanged");
        long long outside = 0;
        for (int y = 0; y < zero.height; ++y) {
            for (int x = 0; x < zero.width; ++x) {
                if ((x >= gm::kGameTypePanelWidth || y >= gm::kGameTypePanelHeight) &&
                    !is_black(at(zero, x, y))) {
                    ++outside;
                }
            }
        }
        check_eq(outside, 0, "fade: outside the panel stays black");
        std::printf("  fade: level 11 diffs 0/64000; level 0 diffs %lld, ink %d, "
                    "outside-panel non-black %lld\n",
                    differing_pixels(zero.rgba, plain.rgba), zero.stats.ink_pixels,
                    outside);
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
        std::cout << "game_type_render: game not installed, pixel runs skipped\n";
        test_negatives(fs::path{});
        return failures == 0 ? 0 : 1;
    }

    FontMap fonts;
    std::string error;
    for (const int number : {gm::kGameTypeTitleFont, gm::kGameTypeRowFont,
                             gm::kGameTypeSelectedFont}) {
        std::optional<gm::FontAtlas> atlas = gm::load_font(game, number, error);
        check(atlas.has_value(), "font " + std::to_string(number) + ": " + error);
        if (!atlas) {
            std::cerr << "game_type_render: cannot load the atlases\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }

    test_order();
    test_install_frame(game, fonts);
    test_mask(game, fonts);
    test_cursor(game, fonts);
    test_plate(game, fonts);
    test_negatives(game);
    test_fade(game);
    if (failures != 0) {
        std::cerr << "game_type_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "game_type_render: ok (" << checks
              << " checks) screen 19 -> 320x200 RGBA (title y 10, modes at "
                 "(18, 18 + 8*k), mask 0x4c1290, Cancel last, cursor font 4, "
                 "PLATE, negatives)\n";
    return 0;
}
