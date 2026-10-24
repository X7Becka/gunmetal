#include "ui/game_type_render.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {
namespace {

constexpr std::string_view kPrefix = "game_type_render: ";

constexpr GameTypeEntry kEntries[] = {
    // 0x48a5b1..0x48a5c0: unconditional, item 0.
    {kGameTypeOneComputer, 0x00u, 0},
    // 0x48a5d0..0x48a5f8: `test al,0x2`, item 1.
    {"Network Client", kGameTypeBitNetworkClient, 1},
    // 0x48a5fa..0x48a625: `test al,0x1`, items 2 and 3.
    {"Network Server", kGameTypeBitNetworkServer, 2},
    {"Network No Render Server", kGameTypeBitNetworkServer, 3},
    // 0x48a646..0x48a670: `test al,0x8`, item 4.
    {"TCP/IP Client", kGameTypeBitTcpClient, 4},
    // 0x48a670..0x48a69b: `test al,0x4`, items 5 and 6.
    {"TCP/IP Server", kGameTypeBitTcpServer, 5},
    {"TCP/IP No Render Server", kGameTypeBitTcpServer, 6},
    // 0x48a6bc..0x48a6e6: `test al,0x80`, item 7.
    {"Serial Cable Client", kGameTypeBitSerialClient, 7},
    // 0x48a6e6..0x48a710: `test al,0x40`, item 8.
    {"Serial Cable Server", kGameTypeBitSerialServer, 8},
    // 0x48a710..0x48a73a: `test al,0x20`, item 9.
    {"Modem Client", kGameTypeBitModemClient, 9},
    // 0x48a73a..0x48a764: `test al,0x10`, item 10.
    {"Modem Server", kGameTypeBitModemServer, 10},
};

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`) so the caller sees one `"game_type_render: ..."`
/// chain instead of a doubled one.
std::string detail_of(std::string_view message) {
    constexpr std::string_view kTextLayout = "text_layout: ";
    if (message.rfind(kTextLayout, 0) == 0) {
        return std::string(message.substr(kTextLayout.size()));
    }
    return std::string(message);
}

/// Failure contract: no stale pixels and no stale numbers for the caller.
void clear(std::vector<std::uint32_t>& rgba, int& width, int& height,
           GameTypeRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = GameTypeRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so
/// it is forced to 255 (same as `ui/menu_render` and `ui/start_game_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The minimal index layer of the exact fade (`0x682f90`): table + level,
/// applied to the palette index of a pixel **before** its colour is looked up
/// (docs/HUD_LUT.md §2b, docs/START_GAME_RENDER.md §6).  `table == nullptr` is
/// the unfaded path.  Same private layer as `ui/start_game_render.cpp` and
/// `ui/pager_render.cpp`.
struct PaletteFade {
    const HudLut* table = nullptr;
    int level = kHudLutIdentityLevel;

    bool active() const { return table != nullptr; }
    std::uint8_t index(std::uint8_t raw) const {
        return table == nullptr ? raw : map(*table, level, raw);
    }
};

/// `PaletteFade::index` with a bounds check: a mapped index the source palette
/// cannot hold is a text, never a wrapped or dropped pixel.
bool fade_index(const PaletteFade& fade, std::uint8_t raw, std::size_t palette_entries,
                std::string_view source, std::uint8_t& out, std::string& error) {
    const std::uint8_t mapped = fade.index(raw);
    if (mapped >= palette_entries) {
        error = std::string(kPrefix) + "fade LUT level " + std::to_string(fade.level) +
                " maps index " + std::to_string(raw) + " to " + std::to_string(mapped) +
                ", outside the " + std::to_string(palette_entries) + "-entry palette of " +
                std::string(source);
        return false;
    }
    out = mapped;
    return true;
}

/// The canvas plus the 196x131 write window of `0x485dd0` (the copy of
/// `PLATE`): a glyph pixel outside it cannot reach the screen.
struct Layer {
    std::vector<std::uint32_t>& pixels;
    std::vector<std::uint8_t>& covered;
    int width = 0;
    int height = 0;
    int clip_x0 = 0;
    int clip_y0 = 0;
    int clip_x1 = 0;
    int clip_y1 = 0;
};

/// Width of *text* with the atlas advances (`0x47fa50`), needed for the
/// centring of the title (`0x485dd0`'s `centered` branch).
int text_width(const FontAtlas& atlas, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += atlas.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// Paint one run at (`x`, `y`), `y` being the top row of the glyph cell.
///
/// `FontGlyph::pixels` row 0 is the top cell row: the blitter `0x47faf0`
/// writes it at the destination row and goes down (`ui/menu_render` and
/// `ui/start_game_render` use the same order).  Palette index 0 is transparent.
/// Characters without a glyph increment `missing_glyphs` and are skipped
/// instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, GameTypeRenderStats& stats,
              std::string& error) {
    const FontAtlas* atlas = fonts(font);
    if (atlas == nullptr) {
        error = std::string(kPrefix) + "no atlas for font " + std::to_string(font);
        return false;
    }
    if (!atlas->has_glyphs()) {
        error = std::string(kPrefix) + "font " + std::to_string(font) + " is metrics-only";
        return false;
    }
    const std::size_t palette_entries = atlas->palette.size() / 4u;
    if (palette_entries == 0) {
        error = std::string(kPrefix) + "font " + std::to_string(font) + " has no palette";
        return false;
    }

    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        const FontGlyph* glyph = atlas->glyph(ch);
        if (glyph == nullptr) {
            ++stats.missing_glyphs;
            continue;
        }
        if (glyph->width > 0 && !glyph->pixels.empty()) {
            for (int row = 0; row < atlas->cell_height; ++row) {
                const int py = y + row;
                if (py < layer.clip_y0 || py >= layer.clip_y1) {
                    continue;
                }
                for (int column = 0; column < glyph->width; ++column) {
                    const std::uint8_t index =
                        glyph->pixels[static_cast<std::size_t>(row) * glyph->width + column];
                    if (index == 0 || index >= palette_entries) {
                        continue;
                    }
                    const int px = pen + column;
                    if (px < layer.clip_x0 || px >= layer.clip_x1) {
                        continue;
                    }
                    std::uint8_t mapped = index;
                    if (fade.active() &&
                        !fade_index(fade, index, palette_entries,
                                    "font " + std::to_string(font), mapped, error)) {
                        return false;
                    }
                    const std::size_t at =
                        static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                        static_cast<std::size_t>(px);
                    layer.pixels[at] = palette_colour(atlas->palette, mapped);
                    if (layer.covered[at] == 0) {
                        layer.covered[at] = 1;
                        ++stats.ink_pixels;
                    }
                }
            }
        }
        pen += atlas->advance_of(ch);
    }
    return true;
}

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP,
/// exactly like `ui/menu_render::read_bmp_entry` (read-only, index reopened per
/// asset).
bool read_bmp_entry(const std::filesystem::path& game, std::string_view name,
                    BmpImage& image, std::string& error) {
    if (game.empty()) {
        error = "no game directory for FRONTEND/BMP.IDX";
        return false;
    }
    const std::filesystem::path idx = game / "FRONTEND" / "BMP.IDX";
    std::optional<BigFile> archive = BigFile::open(idx, error);
    if (!archive) {
        return false;
    }
    const BigEntry* entry = nullptr;
    for (const BigEntry& candidate : archive->entries()) {
        if (candidate.name.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto upper = [](char c) {
                return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
            };
            if (upper(candidate.name[i]) != upper(name[i])) {
                same = false;
                break;
            }
        }
        if (same) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        error = std::string(name) + " is not in " + idx.string();
        return false;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return false;
    }
    std::optional<BmpImage> parsed = parse_bmp(*bytes, error);
    if (!parsed) {
        return false;
    }
    if (parsed->bpp != 8) {
        error = std::string(name) + " is not 8 bpp";
        return false;
    }
    if (parsed->palette.size() / 4u == 0) {
        error = std::string(name) + " has no palette";
        return false;
    }
    image = std::move(*parsed);
    return true;
}

/// Paint `options.background` at (0,0), clipped to the canvas.  A missing,
/// broken or non-8-bpp entry is a text, never a silently black frame.
bool paint_background(const std::filesystem::path& game, std::string_view name,
                      const PaletteFade& fade, std::vector<std::uint32_t>& rgba,
                      int width, int height, std::string& error) {
    BmpImage image;
    std::string load_error;
    if (!read_bmp_entry(game, name, image, load_error)) {
        error = std::string(kPrefix) + "background " + std::string(name) + ": " +
                detail_of(load_error);
        return false;
    }
    const std::size_t entries = image.palette.size() / 4u;
    const int copy_width = std::min(width, image.width);
    const int copy_height = std::min(height, image.height);
    for (int y = 0; y < copy_height; ++y) {
        for (int x = 0; x < copy_width; ++x) {
            const std::uint8_t index =
                image.pixels[static_cast<std::size_t>(y) *
                                 static_cast<std::size_t>(image.width) +
                             static_cast<std::size_t>(x)];
            if (index >= entries) {
                error = std::string(kPrefix) + "background " + std::string(name) +
                        " uses palette index " + std::to_string(index) + " of " +
                        std::to_string(entries);
                return false;
            }
            std::uint8_t mapped = index;
            if (fade.active() &&
                !fade_index(fade, index, entries, "background " + std::string(name),
                            mapped, error)) {
                return false;
            }
            rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = palette_colour(image.palette, mapped);
        }
    }
    return true;
}

} // namespace

const std::vector<GameTypeEntry>& game_type_entries() {
    static const std::vector<GameTypeEntry> entries(std::begin(kEntries),
                                                    std::end(kEntries));
    return entries;
}

bool game_type_allowed(const GameTypeEntry& entry, std::uint32_t mask) {
    return entry.bit == 0u || (mask & entry.bit) != 0u;
}

std::vector<GameTypeEntry> game_type_rows(std::uint32_t mask) {
    std::vector<GameTypeEntry> rows;
    for (const GameTypeEntry& entry : game_type_entries()) {
        if (game_type_allowed(entry, mask)) {
            rows.push_back(entry);
        }
    }
    rows.push_back(GameTypeEntry{kGameTypeCancel, 0x00u, kGameTypeCancelItem});
    return rows;
}

bool render_game_type(const std::filesystem::path& game,
                      const GameTypeRenderOptions& options,
                      std::vector<std::uint32_t>& rgba, int& width, int& height,
                      GameTypeRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    if (game.empty()) {
        err = std::string(kPrefix) + "no game directory (the atlases come from "
                                      "FRONTEND/BMP.IDX)";
        return false;
    }

    // The row set is materialised before anything is painted, so a list with
    // no modes is a refusal instead of an empty frame.  Unreachable with the
    // shipped handler (`One Computer` and `Cancel` have no bit); kept as the
    // contract `ui/start_game_render` uses for its `slots.empty()` guard.
    const std::vector<GameTypeEntry> rows = game_type_rows(options.mask);
    if (rows.empty()) {
        err = std::string(kPrefix) + "no game types to draw (the entry table is empty)";
        return false;
    }

    // The exact tables are resolved before anything is painted, so a missing or
    // broken dump/table is a refusal and not a silently unfaded frame.
    HudLut owned_lut;
    const HudLut* fade_table = nullptr;
    if (options.fade_lut) {
        if (options.fade_lut_table != nullptr) {
            fade_table = options.fade_lut_table;
        } else if (!options.fade_lut_dump.empty()) {
            std::string lut_error;
            if (!load_hud_lut(options.fade_lut_dump, owned_lut, &lut_error)) {
                err = std::string(kPrefix) + lut_error;
                return false;
            }
            fade_table = &owned_lut;
        } else {
            err = std::string(kPrefix) +
                  "fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)";
            return false;
        }
    }
    const PaletteFade fade{fade_table, options.fade_lut_level};

    // §3 faces: title 5, rows 0, cursor 4 (three FONTS4 atlases of BMP.IDX).
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number : {kGameTypeTitleFont, kGameTypeRowFont,
                             kGameTypeSelectedFont}) {
        std::string load_error;
        std::optional<FontAtlas> atlas = load_font(game, number, load_error);
        if (!atlas) {
            err = std::string(kPrefix) + "font " + std::to_string(number) + ": " +
                  detail_of(load_error);
            return false;
        }
        owned.push_back(std::move(*atlas));
    }
    const FontLookup fonts = [&owned](int number) -> const FontAtlas* {
        for (const FontAtlas& atlas : owned) {
            if (atlas.number == number) {
                return &atlas;
            }
        }
        return nullptr;
    };

    width = kGameTypeCanvasWidth;
    height = kGameTypeCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kGameTypeBackground);
    if (!options.background.empty() &&
        !paint_background(game, options.background, fade, rgba, width, height, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    GameTypeRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kGameTypePanelWidth),
                std::min(height, kGameTypePanelHeight)};

    // 0x48a59a..0x48a5ae: the title, font 5, `centered=1` over the 196 px band,
    // row 10 (item -1 makes the row absolute).
    {
        const FontAtlas* title = fonts(kGameTypeTitleFont);
        const int title_x =
            std::max(0, (kGameTypePanelWidth - text_width(*title, kGameTypeTitle)) / 2);
        if (!draw_run(fonts, kGameTypeTitleFont, kGameTypeTitle, title_x,
                      kGameTypeTitleY, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x48a5b1..0x48a764: one row per allowed mode, y = 18 + 8*k, x = 18, font
    // 0, and the cursor's item in font 4 (0x485e0d..0x485e19).
    const int cursor = options.cursor;
    int painted = 0;
    for (const GameTypeEntry& entry : rows) {
        if (entry.item == kGameTypeCancelItem) {
            continue;
        }
        const int y = kGameTypeFirstRow + kGameTypeItemStep * painted;
        const int font = entry.item == cursor ? kGameTypeSelectedFont : kGameTypeRowFont;
        if (entry.item == cursor) {
            counts.highlight_item = entry.item;
        }
        if (!draw_run(fonts, font, entry.label, kGameTypeItemX, y, fade, layer, counts,
                      err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        ++painted;
    }

    // 0x48a764..0x48a774: `Cancel` (0x4bda0c) at y = 18 + 8*modes, item 11 —
    // unconditional, and it takes part in the highlight.
    {
        const int y = kGameTypeFirstRow + kGameTypeItemStep * painted;
        const int font =
            kGameTypeCancelItem == cursor ? kGameTypeSelectedFont : kGameTypeRowFont;
        if (kGameTypeCancelItem == cursor) {
            counts.highlight_item = kGameTypeCancelItem;
        }
        if (!draw_run(fonts, font, kGameTypeCancel, kGameTypeItemX, y, fade, layer,
                      counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        counts.cancel_row = y;
    }

    counts.modes = painted;
    counts.rows = painted + 1;
    counts.first_row = kGameTypeFirstRow;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
