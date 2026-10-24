#include "ui/level_select_render.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/path.h"
#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {
namespace {

constexpr std::string_view kPrefix = "level_select_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`) so the caller sees one `"level_select_render: ..."`
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
           LevelSelectRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = LevelSelectRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so
/// it is forced to 255 (same as `ui/menu_render` and `bmp_to_rgba`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The exact fade of `0x682f90` in the index space of this renderer
/// (docs/LEVEL_SELECT_RENDER.md §4b).  `lut == nullptr` is the plain path, so a
/// caller that never asks for `fade_lut` keeps the slice-3 bytes; otherwise
/// every index that came from a palette (screen bitmap, `MENUARRW`, glyph
/// atlas) goes through `map(level, index)` **before** the palette lookup.
struct Fade {
    const HudLut* lut = nullptr;
    int level = kHudLutIdentityLevel;

    bool enabled() const { return lut != nullptr; }
};

/// Map one palette index for the fade, refusing an index the source palette
/// does not have.  The dump writes 0..255, so a 256-entry palette cannot
/// trigger this; a shortened atlas palette can, and then a silent guard would
/// hide a table that does not fit the asset.
bool fade_index(const Fade& fade, std::uint8_t index, std::size_t palette_entries,
                std::uint8_t& out, std::string& error) {
    out = index;
    if (fade.lut == nullptr) {
        return true;
    }
    out = map(*fade.lut, fade.level, index);
    if (out >= palette_entries) {
        error = std::string(kPrefix) + "fade LUT level " + std::to_string(fade.level) +
                " maps index " + std::to_string(index) + " to " + std::to_string(out) +
                ", outside the " + std::to_string(palette_entries) + "-entry palette";
        return false;
    }
    return true;
}

/// Resolve `options.fade_lut` before anything is painted, so a missing table
/// is an error and not a silently unfaded frame (`owned` backs a dump load).
bool resolve_fade(const LevelSelectRenderOptions& options, HudLut& owned,
                  Fade& fade, std::string& error) {
    if (!options.fade_lut) {
        return true;
    }
    if (options.fade_lut_table != nullptr) {
        fade.lut = options.fade_lut_table;
        fade.level = options.fade_lut_level;
        return true;
    }
    if (options.fade_lut_dump.empty()) {
        error = std::string(kPrefix) +
                "fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)";
        return false;
    }
    std::string lut_error;
    if (!load_hud_lut(options.fade_lut_dump, owned, &lut_error)) {
        error = std::string(kPrefix) + "fade LUT: " + lut_error;
        return false;
    }
    fade.lut = &owned;
    fade.level = options.fade_lut_level;
    return true;
}

/// The canvas plus the 196x131 write window of `0x485dd0` (the copy of `PLATE`
/// at `0x6865f8`): a glyph pixel outside it cannot reach the screen.
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
/// writes it at the destination row and goes down (`gm/hud.py::draw_text`,
/// checked on the HUD screen; `ui/menu_render` uses the same order).  Palette
/// index 0 is transparent.  Characters without a glyph increment
/// `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              Layer& layer, const Fade& fade, LevelSelectRenderStats& stats,
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
                    std::uint8_t colour_index = index;
                    if (!fade_index(fade, index, palette_entries, colour_index, error)) {
                        return false;
                    }
                    const std::size_t at =
                        static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                        static_cast<std::size_t>(px);
                    layer.pixels[at] = palette_colour(atlas->palette, colour_index);
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

/// Paint the `MENUARRW` scroll arrow at (`x`, `y`) of the band.
///
/// The 128 sprite bytes are blitted verbatim by `0x485cc0` (16 bytes per row,
/// eight rows, `lea ecx,[ecx+ebx-0x10]` = stride 196, zero bytes skipped at
/// `0x485ce0`), and `0x485d00` blits the *same* bytes backwards
/// (`mov al,[edx-1]; dec edx` at `0x485d1a`), which rotates the 16x8 image by
/// 180 degrees.  `parse_bmp` returns the rows bottom-up, while the loader
/// (`0x47efe0` -> `fread` of `width * height` bytes at `0x47f100`) copies the
/// file rows as they are stored, so sprite row `r` is `pixels[height-1-r]`.
bool paint_arrow(const BmpImage& sprite, bool up, int x, int y, Layer& layer,
                 const Fade& fade, std::string& error) {
    const std::size_t palette_entries = sprite.palette.size() / 4u;
    for (int row = 0; row < kLevelSelectArrowHeight; ++row) {
        for (int column = 0; column < kLevelSelectArrowWidth; ++column) {
            // 0x485cc0: file row `row`; 0x485d00: file row `7 - row` read from
            // the end, i.e. column `15 - column` of row `7 - row`.
            const int source_row = up ? kLevelSelectArrowHeight - 1 - row : row;
            const int source_column = up ? kLevelSelectArrowWidth - 1 - column : column;
            const std::size_t index =
                static_cast<std::size_t>(sprite.height - 1 - source_row) *
                    static_cast<std::size_t>(sprite.width) +
                static_cast<std::size_t>(source_column);
            const std::uint8_t value = sprite.pixels[index];
            // `0x485ce0`/`0x485d22`: a zero byte leaves the destination alone.
            if (value == 0) {
                continue;
            }
            if (value >= palette_entries) {
                error = std::string(kPrefix) + "arrow " + std::string(kLevelSelectArrowName) +
                        " uses palette index " + std::to_string(value) + " of " +
                        std::to_string(palette_entries);
                return false;
            }
            const int px = x + column;
            const int py = y + row;
            if (px < layer.clip_x0 || px >= layer.clip_x1 || py < layer.clip_y0 ||
                py >= layer.clip_y1) {
                continue;
            }
            std::uint8_t colour_index = value;
            if (!fade_index(fade, value, palette_entries, colour_index, error)) {
                return false;
            }
            layer.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                         static_cast<std::size_t>(px)] =
                palette_colour(sprite.palette, colour_index);
        }
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

/// The `MENUARRW` sprite of `FRONTEND/BMP.IDX` as the blitter expects it: an
/// 8 bpp 16x8 image (`0x48fe59` asks `0x47efe0` for exactly `0x10` x `0x8`).
/// A missing or differently sized entry is a text, like a broken font.
bool load_arrow(const std::filesystem::path& game, BmpImage& image, std::string& error) {
    std::string load_error;
    if (!read_bmp_entry(game, kLevelSelectArrowName, image, load_error)) {
        error = std::string(kPrefix) + "arrow " + std::string(kLevelSelectArrowName) + ": " +
                detail_of(load_error);
        return false;
    }
    if (image.width != kLevelSelectArrowWidth || image.height != kLevelSelectArrowHeight) {
        error = std::string(kPrefix) + "arrow " + std::string(kLevelSelectArrowName) + " is " +
                std::to_string(image.width) + "x" + std::to_string(image.height) + ", want " +
                std::to_string(kLevelSelectArrowWidth) + "x" +
                std::to_string(kLevelSelectArrowHeight);
        return false;
    }
    return true;
}

/// Paint `options.background` at (0,0), clipped to the canvas.  A missing,
/// broken or non-8-bpp entry is a text, never a silently black frame.
bool paint_background(const std::filesystem::path& game, std::string_view name,
                      std::vector<std::uint32_t>& rgba, int width, int height,
                      const Fade& fade, std::string& error) {
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
            std::uint8_t colour_index = index;
            if (!fade_index(fade, index, entries, colour_index, error)) {
                return false;
            }
            rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = palette_colour(image.palette, colour_index);
        }
    }
    return true;
}

/// Bytes of `LVLNAMES.SLU` (case-insensitive name, like the original's DOS
/// open).  A missing or unreadable file is an empty string: the caller then
/// shows the stems, which is exactly `0x48a4c0` without its file.
std::string read_level_names(const std::filesystem::path& game) {
    const std::optional<std::filesystem::path> path = resolve_ci(game, kLevelSelectNamesFile);
    if (!path) {
        return {};
    }
    std::ifstream in(*path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

int level_select_scroll(int count, int selected) {
    if (count <= 0) {
        return 0;
    }
    const int cursor = std::clamp(selected, 0, count - 1);
    // 0x48a3f1..0x48a403: `ebx = selected >= 5 ? selected - 5 : 0`.
    int scroll = cursor >= 5 ? cursor - 5 : 0;
    // 0x48a408..0x48a40f: `if (ebx + 10 >= count) ebx = count - 10`.
    if (scroll + LevelSelect::kVisibleRows >= count) {
        scroll = count - LevelSelect::kVisibleRows;
    }
    // 0x48a412..0x48a416: never negative (a list of <= 10 rows starts at 0).
    return scroll < 0 ? 0 : scroll;
}

std::string level_select_name(std::string_view stem, std::string_view level_names) {
    std::string prefix(stem);
    prefix += " - ";
    // 0x48a4cb: without a loaded file the stem comes back unchanged.
    if (stem.empty() || level_names.empty()) {
        return std::string(stem);
    }
    std::size_t line_start = 0;
    while (line_start < level_names.size()) {
        std::size_t line_end = line_start;
        while (line_end < level_names.size() && level_names[line_end] != '\n' &&
               level_names[line_end] != '\0') {
            ++line_end;
        }
        const std::string_view line = level_names.substr(line_start, line_end - line_start);
        // 0x48a505..0x48a518: byte-exact `<stem> ` at the start of the line.
        if (line.size() > stem.size() && line.compare(0, stem.size(), stem) == 0 &&
            line[stem.size()] == ' ') {
            // 0x48a541..0x48a560: copy the rest, at most 30 characters in the
            // buffer (prefix included), stopping at the end of the line.
            std::string out = prefix;
            for (std::size_t i = stem.size() + 1;
                 i < line.size() &&
                 out.size() < static_cast<std::size_t>(kLevelSelectNameMaxChars);
                 ++i) {
                out.push_back(line[i]);
            }
            return out;
        }
        line_start = line_end + 1;
    }
    // 0x48a53b / 0x48a575: no line -> the stem itself.
    return std::string(stem);
}

bool render_level_select(const std::filesystem::path& game, const LevelSelect& select,
                         const LevelSelectRenderOptions& options,
                         std::vector<std::uint32_t>& rgba, int& width, int& height,
                         LevelSelectRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    const std::vector<LevelEntry>& levels = select.levels();
    if (levels.empty()) {
        // Cheap check first: no file is read for a state that cannot be drawn.
        err = std::string(kPrefix) +
              "no levels to select (LevelSelect::build was not called or refused "
              "the campaign state)";
        return false;
    }
    if (game.empty()) {
        err = std::string(kPrefix) + "no game directory (the atlases come from "
                                      "FRONTEND/BMP.IDX)";
        return false;
    }

    // The exact tables are resolved before anything is painted, so a missing
    // or broken dump is an error and not a silently unfaded frame.
    HudLut owned_lut;
    Fade fade;
    if (!resolve_fade(options, owned_lut, fade, err)) {
        return false;
    }

    // §3 faces: title 5, rows 0, cursor 4 (three FONTS4 atlases of BMP.IDX).
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number :
         {kLevelSelectTitleFont, kLevelSelectRowFont, kLevelSelectSelectedFont}) {
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

    width = kLevelSelectCanvasWidth;
    height = kLevelSelectCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kLevelSelectBackground);
    if (!options.background.empty() &&
        !paint_background(game, options.background, rgba, width, height, fade, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    LevelSelectRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kLevelSelectPanelWidth),
                std::min(height, kLevelSelectPanelHeight)};

    // 0x48a3dd..0x48a3ec: font 5, centred in the 196 px band, row 14.
    if (options.title) {
        const FontAtlas* title_atlas = fonts(kLevelSelectTitleFont);
        const int title_x =
            std::max(0, (kLevelSelectPanelWidth - text_width(*title_atlas, kLevelSelectTitle)) / 2);
        if (!draw_run(fonts, kLevelSelectTitleFont, kLevelSelectTitle, title_x,
                      kLevelSelectTitleY, layer, fade, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x48a46a..0x48a49f: `min(count, 10)` rows of the window at x 14, step 8.
    const int count = static_cast<int>(levels.size());
    const int selected = std::clamp(select.selected(), 0, count - 1);
    const int scroll = level_select_scroll(count, selected);
    const std::string names = read_level_names(game);
    const int rows = std::min(LevelSelect::kVisibleRows, count - scroll);

    // 0x48a418..0x48a44d: `MENUARRW` into the plate copy, up when `scroll > 0`
    // and down when `scroll + 10 < count`, at (159, 2) and (159, 121).
    const bool arrow_up = scroll > 0;
    const bool arrow_down = scroll + LevelSelect::kVisibleRows < count;
    if (arrow_up || arrow_down) {
        BmpImage sprite;
        if (!load_arrow(game, sprite, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        if (arrow_up) {
            if (!paint_arrow(sprite, true, kLevelSelectArrowX, kLevelSelectArrowUpY, layer, fade,
                         err)) {
                clear(rgba, width, height, stats);
                return false;
            }
            ++counts.arrows;
        }
        if (arrow_down) {
            if (!paint_arrow(sprite, false, kLevelSelectArrowX, kLevelSelectArrowDownY, layer,
                             fade, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
            ++counts.arrows;
        }
    }

    for (int i = 0; i < rows; ++i) {
        const int index = scroll + i;
        const int font = index == selected ? kLevelSelectSelectedFont : kLevelSelectRowFont;
        const std::string text = level_select_name(levels[static_cast<std::size_t>(index)].name,
                                                   names);
        if (!draw_run(fonts, font, text, kLevelSelectRowX,
                      kLevelSelectFirstRowY + kLevelSelectRowStep * i, layer, fade, counts,
                      err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x48a4a1..0x48a4b0: `ESC TO CANCEL`, font 0, (38, 111), not centred.
    if (options.cancel &&
        !draw_run(fonts, kLevelSelectRowFont, kLevelSelectCancel, kLevelSelectCancelX,
                  kLevelSelectCancelY, layer, fade, counts, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    counts.rows = rows;
    counts.selected_row = selected;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
