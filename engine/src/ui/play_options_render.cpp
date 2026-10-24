#include "ui/play_options_render.h"

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

int clamped_item(int item) {
    return std::clamp(item, 0, kPlayOptionsItemCount - 1);
}

} // namespace

int play_options_item_row(int item) {
    return kPlayOptionsItemRow[static_cast<std::size_t>(clamped_item(item))];
}

int play_options_item_y(int item) {
    const int slot = clamped_item(item);
    return kPlayOptionsItemRow[static_cast<std::size_t>(slot)] +
           kPlayOptionsItemStep * slot;
}

int play_options_second_y(int item) {
    if (item < 0 || item >= kPlayOptionsItemCount) {
        return -1;
    }
    const int row = kPlayOptionsSecondRow[static_cast<std::size_t>(item)];
    if (row < 0) {
        return -1;
    }
    // The second line keeps the item id (0x48623d/0x486288/0x4862d5 push the
    // same item), so its y is row + 15*item like the first line.
    return row + kPlayOptionsItemStep * item;
}

int play_options_item_x(int item) {
    if (item < 0 || item >= kPlayOptionsItemCount) {
        return kPlayOptionsItemX;
    }
    return item >= kPlayOptionsCameraItem ? kPlayOptionsTailX : kPlayOptionsItemX;
}

std::uint32_t play_options_flag_bit(int item) {
    switch (item) {
    case kPlayOptionsSkipEmptyItem:
        return kPlayOptionsFlagSkipEmpty;
    case kPlayOptionsSkipDeadlyItem:
        return kPlayOptionsFlagSkipDeadly;
    case kPlayOptionsSwitchAmmoItem:
        return kPlayOptionsFlagSwitchAmmo;
    case kPlayOptionsAutoSwitchItem:
        return kPlayOptionsFlagAutoSwitch;
    case kPlayOptionsAutoTargetItem:
        return kPlayOptionsFlagNoAutoTarget;
    case kPlayOptionsMouseCheatingItem:
        return kPlayOptionsFlagMouseCheating;
    default:
        return 0u;
    }
}

bool play_options_checked(int item, int flags) {
    const std::uint32_t bit = play_options_flag_bit(item);
    if (bit == 0u) {
        return false;
    }
    return (static_cast<std::uint32_t>(flags) & bit) != 0u;
}

int play_options_check_y(int item) {
    if (item < 0 || item >= kPlayOptionsCheckboxCount) {
        return -1;
    }
    return play_options_item_y(item) - kPlayOptionsNoCheckDrop;
}

int play_options_camera(int requested) {
    return std::clamp(requested, kPlayOptionsCameraMin, kPlayOptionsCameraMax);
}

std::string play_options_label(int item, const PlayOptionsRenderOptions& options) {
    switch (item) {
    case kPlayOptionsSkipEmptyItem:
        return std::string(kPlayOptionsSkipEmptyLabel);
    case kPlayOptionsSkipDeadlyItem:
        return std::string(kPlayOptionsSkipDeadlyLabel);
    case kPlayOptionsSwitchAmmoItem:
        return std::string(kPlayOptionsSwitchAmmoLabel);
    case kPlayOptionsAutoSwitchItem:
        return std::string(kPlayOptionsAutoSwitchLabel);
    case kPlayOptionsAutoTargetItem:
        return std::string(kPlayOptionsNoAutoTargetLabel);
    case kPlayOptionsMouseCheatingItem:
        return std::string(kPlayOptionsMouseCheatingLabel);
    case kPlayOptionsCameraItem:
        if (options.in_game) {
            // 0x48632e..0x48633d: `[0x6e89c0] != 0` short-circuits the camera
            // switch before `[0x4aac8c]` is even read.
            return std::string(kPlayOptionsCameraHuman);
        }
        switch (play_options_camera(options.dynamic_camera)) {
        case 0:
            return std::string(kPlayOptionsCameraNone); // 0x486362 branch
        case 1:
            return std::string(kPlayOptionsCameraSome); // 0x48635b branch
        default:
            return std::string(kPlayOptionsCameraFull); // 0x486354 branch
        }
    case kPlayOptionsDoneItem:
    default:
        return std::string(kPlayOptionsDoneLabel);
    }
}

std::string play_options_second_label(int item) {
    switch (item) {
    case kPlayOptionsSwitchAmmoItem:
        return std::string(kPlayOptionsAmmoLine);
    case kPlayOptionsAutoSwitchItem:
        return std::string(kPlayOptionsAmmoDepletedLine);
    case kPlayOptionsAutoTargetItem:
        return std::string(kPlayOptionsLookingLine);
    default:
        return std::string();
    }
}

int play_options_item_font(int item, const PlayOptionsRenderOptions& options) {
    const int base = (item == kPlayOptionsCameraItem && options.in_game)
                         ? kPlayOptionsInGameFont
                         : kPlayOptionsRowFont;
    // 0x485e0d..0x485e19: the highlight only replaces font 0.
    if (item == options.cursor && base == kPlayOptionsRowFont) {
        return kPlayOptionsSelectedFont;
    }
    return base;
}

namespace {

constexpr std::string_view kPrefix = "play_options_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`, the archive from `res/bigfile`) so the caller sees one
/// `"play_options_render: ..."` chain instead of a doubled one.
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
           PlayOptionsRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = PlayOptionsRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so it
/// is forced to 255 (same as `ui/menu_render` and `ui/sound_options_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The minimal index layer of the exact fade (`0x682f90`): table + level,
/// applied to the palette index of a pixel **before** its colour is looked up
/// (docs/HUD_LUT.md §2b).  `table == nullptr` is the unfaded path.
struct PaletteFade {
    const HudLut* table = nullptr;
    int level = kHudLutIdentityLevel;

    bool active() const { return table != nullptr; }
    std::uint8_t index(std::uint8_t raw) const {
        return table == nullptr ? raw : map(*table, level, raw);
    }
};

/// `PaletteFade::index` with a bounds check: a mapped index the source palette
/// cannot hold is a text, never a wrapped or dropped pixel.  With the shipped
/// 256-entry BMP palettes this cannot fire (`map` writes 0..255); it guards
/// short palettes.
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

/// One 8-bpp `FRONTEND/BMP.IDX` entry (the panel or a control sprite), kept in
/// palette indices so the exact fade can run before the palette lookup.
struct Sprite {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> palette; ///< BMP palette, 4 bytes per entry
    std::vector<std::uint8_t> indices; ///< one palette index per pixel
    std::string name;

    bool empty() const { return width <= 0 || height <= 0 || indices.empty(); }
    std::size_t palette_entries() const { return palette.size() / 4u; }
};

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP
/// (read-only, the index is reopened per asset, so no archive object outlives
/// the call).
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

/// Load one sprite of `FRONTEND/BMP.IDX` and refuse a size that is not the one
/// docs/UI_SCREENS.md §9 proves (`PLATE` 196x131, `CHECK` 8x16, `NOCHECK` 8x8).
bool load_sprite(const std::filesystem::path& game, std::string_view name, int want_width,
                 int want_height, Sprite& sprite, std::string& error) {
    BmpImage image;
    std::string load_error;
    if (!read_bmp_entry(game, name, image, load_error)) {
        error = std::string(kPrefix) + std::string(name) + ": " + detail_of(load_error);
        return false;
    }
    if (image.width != want_width || image.height != want_height) {
        error = std::string(kPrefix) + std::string(name) + " is " +
                std::to_string(image.width) + "x" + std::to_string(image.height) +
                ", expected " + std::to_string(want_width) + "x" +
                std::to_string(want_height);
        return false;
    }
    sprite.width = image.width;
    sprite.height = image.height;
    sprite.palette = std::move(image.palette);
    sprite.indices = std::move(image.pixels);
    sprite.name = std::string(name);
    return true;
}

/// The canvas plus the 196x131 write window of `0x485dd0` (the copy of
/// `PLATE`): a pixel outside it cannot reach the screen.
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
/// `FontGlyph::pixels` row 0 is written at the destination row and the blitter
/// `0x47faf0` goes down (`ui/menu_render` and `ui/sound_options_render` use the
/// same order).  Palette index 0 is transparent.  Characters without a glyph
/// increment `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, PlayOptionsRenderStats& stats,
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

/// Masked blit of one sprite (`0x4833e0` skips source index 0), into the
/// 196x131 band.  Counts the newly covered pixels in `stats.ink_pixels`
/// (unique, the same counter the glyphs use).
bool blit_sprite(const Sprite& sprite, int x, int y, const PaletteFade& fade, Layer& layer,
                 PlayOptionsRenderStats& stats, std::string& error) {
    const std::size_t entries = sprite.palette_entries();
    for (int row = 0; row < sprite.height; ++row) {
        const int py = y + row;
        if (py < layer.clip_y0 || py >= layer.clip_y1) {
            continue;
        }
        for (int column = 0; column < sprite.width; ++column) {
            const std::uint8_t index =
                sprite.indices[static_cast<std::size_t>(row) *
                                   static_cast<std::size_t>(sprite.width) +
                               static_cast<std::size_t>(column)];
            if (index == 0) {
                continue; // transparent key of the control sprites
            }
            if (index >= entries) {
                error = std::string(kPrefix) + sprite.name + " uses palette index " +
                        std::to_string(index) + " of " + std::to_string(entries);
                return false;
            }
            const int px = x + column;
            if (px < layer.clip_x0 || px >= layer.clip_x1) {
                continue;
            }
            std::uint8_t mapped = index;
            if (fade.active() &&
                !fade_index(fade, index, entries, sprite.name, mapped, error)) {
                return false;
            }
            const std::size_t at =
                static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                static_cast<std::size_t>(px);
            layer.pixels[at] = palette_colour(sprite.palette, mapped);
            if (layer.covered[at] == 0) {
                layer.covered[at] = 1;
                ++stats.ink_pixels;
            }
        }
    }
    return true;
}

/// Paint `options.background` at (0,0), clipped to the canvas.  A missing,
/// broken or non-8-bpp entry is a text, never a silently black frame.  The
/// backdrop is not counted as `ink_pixels` (the same rule as
/// `ui/menu_render`/`ui/sound_options_render`).
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

/// The `CHECK`/`NOCHECK` pair of `0x4833e0`.
struct CheckboxAssets {
    Sprite check;
    Sprite nocheck;
};

} // namespace

bool render_play_options(const std::filesystem::path& game,
                         const PlayOptionsRenderOptions& options,
                         std::vector<std::uint32_t>& rgba, int& width, int& height,
                         PlayOptionsRenderStats* stats, std::string* error) {
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

    // §3 faces: title 5, items 0, cursor 4; the in-game camera line adds 8.
    std::vector<FontAtlas> owned;
    owned.reserve(4);
    std::vector<int> wanted = {kPlayOptionsTitleFont, kPlayOptionsRowFont,
                               kPlayOptionsSelectedFont};
    if (options.in_game) {
        wanted.push_back(kPlayOptionsInGameFont);
    }
    for (const int number : wanted) {
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

    // `0x486050` PLATE, `0x4833e0` CHECK/NOCHECK — three 8-bpp entries of
    // BMP.IDX, size-checked.
    Sprite plate;
    CheckboxAssets boxes;
    if (!load_sprite(game, kPlayOptionsPanelName, kPlayOptionsPanelWidth,
                     kPlayOptionsPanelHeight, plate, err) ||
        !load_sprite(game, "CHECK", kPlayOptionsCheckWidth, kPlayOptionsCheckHeight,
                     boxes.check, err) ||
        !load_sprite(game, "NOCHECK", kPlayOptionsNoCheckWidth,
                     kPlayOptionsNoCheckHeight, boxes.nocheck, err)) {
        return false;
    }
    // `0x486050` copies PLATE as the compose buffer, so it is loaded and size
    // checked here; the optional on-screen backdrop is decoded by name in
    // `paint_background` (a different entry is just as valid).
    (void)plate;

    width = kPlayOptionsCanvasWidth;
    height = kPlayOptionsCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kPlayOptionsBackground);
    if (!options.background.empty() &&
        !paint_background(game, options.background, fade, rgba, width, height, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    PlayOptionsRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kPlayOptionsPanelWidth),
                std::min(height, kPlayOptionsPanelHeight)};

    // 0x486189..0x486198: `PLAY OPTIONS`, font 5, `centered=1` over the 196 px
    // band, row 11 (item -1 makes the row absolute).
    {
        const FontAtlas* title = fonts(kPlayOptionsTitleFont);
        if (title == nullptr) {
            clear(rgba, width, height, stats);
            err = std::string(kPrefix) + "no atlas for font " +
                  std::to_string(kPlayOptionsTitleFont);
            return false;
        }
        const int title_x =
            std::max(0, (kPlayOptionsPanelWidth - text_width(*title, kPlayOptionsTitle)) / 2);
        if (!draw_run(fonts, kPlayOptionsTitleFont, kPlayOptionsTitle, title_x,
                      kPlayOptionsTitleY, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // The handler's emission order: for each checkable item the box first, then
    // the label and (for 2/3/4) the second line; then the camera line and
    // `Done`.
    for (int item = 0; item < kPlayOptionsCheckboxCount; ++item) {
        const bool on = play_options_checked(item, options.flags);
        const Sprite& sprite = on ? boxes.check : boxes.nocheck;
        const int box_y = play_options_check_y(item) + (on ? 0 : kPlayOptionsNoCheckDrop);
        if (!blit_sprite(sprite, kPlayOptionsCheckX, box_y, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        ++counts.checks;

        const int font = play_options_item_font(item, options);
        if (!draw_run(fonts, font, play_options_label(item, options),
                      play_options_item_x(item), play_options_item_y(item), fade, layer,
                      counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        const int second_y = play_options_second_y(item);
        if (second_y >= 0 &&
            !draw_run(fonts, font, play_options_second_label(item),
                      play_options_item_x(item), second_y, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }
    for (int item = kPlayOptionsCameraItem; item < kPlayOptionsItemCount; ++item) {
        if (!draw_run(fonts, play_options_item_font(item, options),
                      play_options_label(item, options), play_options_item_x(item),
                      play_options_item_y(item), fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    counts.rows = kPlayOptionsRows;
    // Screen 32 has no MENUARRW/`SLIDE`/`KNOB` call at 0x486170.
    counts.arrows = 0;
    counts.sliders = 0;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
