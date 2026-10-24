#include "ui/video_mode_list_render.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

constexpr std::string_view kPrefix = "video_mode_list_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`, the archive from `res/bigfile`) so the caller sees one
/// `"video_mode_list_render: ..."` chain instead of a doubled one.
std::string detail_of(std::string_view message) {
    constexpr std::string_view kTextLayout = "text_layout: ";
    constexpr std::string_view kBigFile = "bigfile: ";
    if (message.rfind(kTextLayout, 0) == 0) {
        return std::string(message.substr(kTextLayout.size()));
    }
    if (message.rfind(kBigFile, 0) == 0) {
        return std::string(message.substr(kBigFile.size()));
    }
    return std::string(message);
}

/// Failure contract: no stale pixels and no stale numbers for the caller.
void clear(std::vector<std::uint32_t>& rgba, int& width, int& height,
           VideoModeListRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = VideoModeListRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so
/// it is forced to 255 (same as `ui/menu_render` and `ui/level_select_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The exact fade of `0x682f90` in the index space of this renderer:
/// `table == nullptr` is the plain path, otherwise every index that came from a
/// palette (background, `MENUARRW`, glyph atlas) goes through `map(level,
/// index)` **before** the palette lookup.
struct Fade {
    const HudLut* table = nullptr;
    int level = kHudLutIdentityLevel;

    bool active() const { return table != nullptr; }
};

/// Map one palette index for the fade, refusing an index the source palette
/// does not have (the dump writes 0..255, so a 256-entry palette cannot trigger
/// this; a shortened atlas palette can).
bool fade_index(const Fade& fade, std::uint8_t index, std::size_t palette_entries,
                std::string_view source, std::uint8_t& out, std::string& error) {
    out = index;
    if (!fade.active()) {
        return true;
    }
    out = map(*fade.table, fade.level, index);
    if (out >= palette_entries) {
        error = std::string(kPrefix) + "fade LUT level " + std::to_string(fade.level) +
                " maps index " + std::to_string(index) + " to " + std::to_string(out) +
                ", outside the " + std::to_string(palette_entries) + "-entry palette of " +
                std::string(source);
        return false;
    }
    return true;
}

/// The canvas plus the 196x131 write window of `0x485dd0` (the copy of
/// `PLATE` at `0x6865f8`): a glyph pixel outside it cannot reach the screen.
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
/// centring of the title and the hint (`0x485dd0`'s `centered` branch).
int text_width(const FontAtlas& atlas, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += atlas.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// Paint one run at (`x`, `y`), `y` being the top row of the glyph cell.
///
/// `FontGlyph::pixels` row 0 is the top cell row of the blit: the blitter
/// `0x47faf0` writes it at the destination row and goes down (`ui/menu_render`
/// and `ui/level_select_render` use the same order).  Palette index 0 is
/// transparent.  Characters without a glyph increment `missing_glyphs` and are
/// skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const Fade& fade, Layer& layer, VideoModeListRenderStats& stats,
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
                    if (!fade_index(fade, index, palette_entries,
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

/// Paint the `MENUARRW` scroll arrow at (`x`, `y`) of the band.
///
/// The 128 sprite bytes are blitted verbatim by `0x485cc0` (16 bytes per row,
/// eight rows, stride 196, zero bytes skipped at `0x485ce0`), and `0x485d00`
/// blits the *same* bytes backwards (`mov al,[edx-1]; dec edx` at `0x485d1a`),
/// which rotates the 16x8 image by 180 degrees.  `parse_bmp` returns the rows
/// bottom-up, while the loader (`0x47efe0` -> `fread` of `width * height`
/// bytes) copies the file rows as stored, so sprite row `r` is
/// `pixels[height-1-r]`.
bool paint_arrow(const BmpImage& sprite, bool up, int x, int y, const Fade& fade,
                 Layer& layer, std::string& error) {
    const std::size_t palette_entries = sprite.palette.size() / 4u;
    for (int row = 0; row < kVideoModeListArrowHeight; ++row) {
        for (int column = 0; column < kVideoModeListArrowWidth; ++column) {
            // 0x485cc0: file row `row`; 0x485d00: file row `7 - row` read from
            // the end, i.e. column `15 - column` of row `7 - row`.
            const int source_row = up ? kVideoModeListArrowHeight - 1 - row : row;
            const int source_column = up ? kVideoModeListArrowWidth - 1 - column : column;
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
                error = std::string(kPrefix) + "arrow " +
                        std::string(kVideoModeListArrowName) + " uses palette index " +
                        std::to_string(value) + " of " + std::to_string(palette_entries);
                return false;
            }
            const int px = x + column;
            const int py = y + row;
            if (px < layer.clip_x0 || px >= layer.clip_x1 || py < layer.clip_y0 ||
                py >= layer.clip_y1) {
                continue;
            }
            std::uint8_t mapped = value;
            if (!fade_index(fade, value, palette_entries,
                            "arrow " + std::string(kVideoModeListArrowName), mapped, error)) {
                return false;
            }
            layer.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                         static_cast<std::size_t>(px)] =
                palette_colour(sprite.palette, mapped);
        }
    }
    return true;
}

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP,
/// exactly like `ui/level_select_render::read_bmp_entry` (read-only, index
/// reopened per asset, so no archive object outlives the call).
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

/// The `MENUARRW` sprite as the blitter expects it: an 8 bpp 16x8 image
/// (`0x48fe59` asks `0x47efe0` for exactly `0x10` x `0x8`).  A missing or
/// differently sized entry is a text, like a broken font.
bool load_arrow(const std::filesystem::path& game, BmpImage& image, std::string& error) {
    std::string load_error;
    if (!read_bmp_entry(game, kVideoModeListArrowName, image, load_error)) {
        error = std::string(kPrefix) + "arrow " + std::string(kVideoModeListArrowName) + ": " +
                detail_of(load_error);
        return false;
    }
    if (image.width != kVideoModeListArrowWidth ||
        image.height != kVideoModeListArrowHeight) {
        error = std::string(kPrefix) + "arrow " + std::string(kVideoModeListArrowName) +
                " is " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                ", want " + std::to_string(kVideoModeListArrowWidth) + "x" +
                std::to_string(kVideoModeListArrowHeight);
        return false;
    }
    return true;
}

/// Paint `options.background` at (0,0), clipped to the canvas.  A missing,
/// broken or non-8-bpp entry is a text, never a silently black frame.
bool paint_background(const std::filesystem::path& game, std::string_view name,
                      const Fade& fade, std::vector<std::uint32_t>& rgba, int width,
                      int height, std::string& error) {
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
            if (!fade_index(fade, index, entries, "background " + std::string(name), mapped,
                            error)) {
                return false;
            }
            rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = palette_colour(image.palette, mapped);
        }
    }
    return true;
}

} // namespace

std::string video_mode_list_label(const VideoModeListEntry& entry) {
    const std::string size =
        std::to_string(entry.width) + "x" + std::to_string(entry.height);
    // 0x486510: `test ch,0x2` — Direct 3D.
    if ((entry.flags & 0x200u) != 0u) {
        return size + " Direct 3D";
    }
    // 0x486536: `test ch,0x4` — Open GL.
    if ((entry.flags & 0x400u) != 0u) {
        return size + " Open GL";
    }
    // 0x48655c: `test ch,0x8` — Glide 3Dfx.
    if ((entry.flags & 0x800u) != 0u) {
        return size + " Glide 3Dfx";
    }
    // 0x486582: `test cl,0x2` — TV Mode.
    if ((entry.flags & 0x002u) != 0u) {
        return size + " TV Mode";
    }
    // 0x4865a8: `test cl,0x1` — the 256-colour Mode X pair; `0x100` picks the
    // "Skip X" variant (`0x4865b3`).
    if ((entry.flags & 0x001u) != 0u) {
        return size + (((entry.flags & 0x100u) != 0u) ? " 256 Skip X" : " 256 Mode X");
    }
    // 0x4865e6: `test ch,0x1` — the "Skip" (double-scan) family, by bpp.
    if ((entry.flags & 0x100u) != 0u) {
        switch (entry.bpp) {
        case 8: return size + " 256 Skip";
        case 15: return size + " 32k Skip";
        case 16: return size + " 64k Skip";
        default: return size + " Skip";
        }
    }
    // 0x486678: the plain family, by bpp (`0x4866b9`/`0x4866da`/`0x4866a6`).
    switch (entry.bpp) {
    case 8: return size + " 256 Colours";
    case 15: return size + " 32k Colours";
    case 16: return size + " 64k Colours";
    default: return size;
    }
}

int video_mode_list_first_row(int count, int cursor) {
    if (count <= 0) {
        return 0;
    }
    // 0x48a902..0x48a909: `first = cursor >= 5 ? cursor - 5 : 0`.
    int first = cursor >= 5 ? cursor - 5 : 0;
    // 0x48a911..0x48a918: `if (first + 10 >= count) first = count - 10`.
    if (first + kVideoModeListVisibleRows >= count) {
        first = count - kVideoModeListVisibleRows;
    }
    // 0x48a91b..0x48a91f: never negative (a list of <= 10 entries starts at 0).
    return first < 0 ? 0 : first;
}

bool render_video_mode_list(const std::filesystem::path& game,
                            const VideoModeListRenderOptions& options,
                            std::vector<std::uint32_t>& rgba, int& width, int& height,
                            VideoModeListRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    if (game.empty()) {
        err = std::string(kPrefix) + "no game directory (the atlases come from "
                                      "FRONTEND/BMP.IDX)";
        return false;
    }

    // The exact tables are resolved before anything is painted, so a missing or
    // broken dump/table is a refusal and not a silently unfaded frame.
    HudLut owned_lut;
    Fade fade;
    if (options.fade_lut) {
        if (options.fade_lut_table != nullptr) {
            fade.table = options.fade_lut_table;
        } else if (!options.fade_lut_dump.empty()) {
            std::string lut_error;
            if (!load_hud_lut(options.fade_lut_dump, owned_lut, &lut_error)) {
                err = std::string(kPrefix) + lut_error;
                return false;
            }
            fade.table = &owned_lut;
        } else {
            err = std::string(kPrefix) +
                  "fade LUT needs a table (fade_lut_table) or a dump (fade_lut_dump)";
            return false;
        }
        fade.level = options.fade_lut_level;
    }

    // §3 faces: title 5, rows 0, cursor 4 (three FONTS4 atlases of BMP.IDX).
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number :
         {kVideoModeListTitleFont, kVideoModeListRowFont, kVideoModeListSelectedFont}) {
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

    width = kVideoModeListCanvasWidth;
    height = kVideoModeListCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kVideoModeListBackground);
    if (!options.background.empty() &&
        !paint_background(game, options.background, fade, rgba, width, height, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    VideoModeListRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kVideoModeListPanelWidth),
                std::min(height, kVideoModeListPanelHeight)};

    // 0x48a8cf..0x48a8de: `SELECT VIDEO MODE`, font 5, centred, row 11.
    {
        const FontAtlas* title = fonts(kVideoModeListTitleFont);
        const int title_x = std::max(
            0, (kVideoModeListPanelWidth - text_width(*title, kVideoModeListTitle)) / 2);
        if (!draw_run(fonts, kVideoModeListTitleFont, kVideoModeListTitle, title_x,
                      kVideoModeListTitleY, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x48a8e6..0x48a8f5: `Press Esc to close`, font 0, centred, row 20.
    {
        const FontAtlas* hint = fonts(kVideoModeListRowFont);
        const int hint_x = std::max(
            0, (kVideoModeListPanelWidth - text_width(*hint, kVideoModeListHint)) / 2);
        if (!draw_run(fonts, kVideoModeListRowFont, kVideoModeListHint, hint_x,
                      kVideoModeListHintY, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x48a8fa..0x48a921: the ten-band window of the list.
    const int count = kVideoModeListModeCount;
    const int first = video_mode_list_first_row(count, options.cursor);

    // 0x48a923..0x48a952: `MENUARRW` into the plate copy, up when `first > 0`
    // and down when `first + 10 < count`, at (159, 2) and (159, 121).
    const bool arrow_up = first > 0;
    const bool arrow_down = first + kVideoModeListVisibleRows < count;
    if (arrow_up || arrow_down) {
        BmpImage sprite;
        if (!load_arrow(game, sprite, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        if (arrow_up &&
            !paint_arrow(sprite, true, kVideoModeListArrowX, kVideoModeListArrowUpY, fade,
                         layer, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        if (arrow_down &&
            !paint_arrow(sprite, false, kVideoModeListArrowX, kVideoModeListArrowDownY, fade,
                         layer, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        counts.arrows = (arrow_up ? 1 : 0) + (arrow_down ? 1 : 0);
    }

    // 0x48a955..0x48a991: `min(10, count - first)` real bands at x 38, step 9,
    // item = first + i; the cursor's item switches to font 4.  (The original
    // runs the loop ten times unconditionally and paints the zeroed table slots
    // beyond `count` as `0x0`; with a list longer than the window — always the
    // case for the model — the two are identical.)
    const int rows = std::min(kVideoModeListVisibleRows, count - first);
    for (int i = 0; i < rows; ++i) {
        const int index = first + i;
        const int font = index == options.cursor ? kVideoModeListSelectedFont
                                                 : kVideoModeListRowFont;
        const std::string text =
            video_mode_list_label(kVideoModeListModes[static_cast<std::size_t>(index)]);
        if (!draw_run(fonts, font, text, kVideoModeListRowX,
                      kVideoModeListFirstRowY + kVideoModeListRowStep * i, fade, layer,
                      counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    counts.rows = count;
    counts.first_row = first;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
