#include "ui/mouse_setup_render.h"

#include <algorithm>
#include <array>
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

int mouse_setup_item_y(int item) {
    return kMouseSetupItemFirstY + kMouseSetupItemStep * item;
}

int mouse_setup_item_x(int item) { return item == 0 ? kMouseSetupFirstItemX : kMouseSetupItemX; }

int mouse_setup_sensitivity_tenths(int requested) {
    return std::clamp(requested, kMouseSetupSensitivityMin, kMouseSetupSensitivityMax);
}

int mouse_setup_knob_x(int sensitivity_tenths) {
    const int tenths = mouse_setup_sensitivity_tenths(sensitivity_tenths);
    // 108 - trunc((v - 0.2) * -27.0) with v = tenths/10: the engine's double
    // path and this integer division agree on all 21 reachable positions
    // (docs/MOUSE_SETUP_RENDER.md §2), because the numerator is non-negative.
    return kMouseSetupKnobBaseX +
           (tenths - kMouseSetupSensitivityMin) * kMouseSetupKnobPixelsPerUnit / 10;
}

std::string mouse_setup_label(int item, const MouseSetupRenderOptions& options) {
    const auto axis_name = [&options](int slot) {
        const int index =
            std::clamp(options.axes[static_cast<std::size_t>(slot)], 0,
                       static_cast<int>(std::size(kMouseSetupAxisNames)) - 1);
        return std::string(kMouseSetupAxisNames[static_cast<std::size_t>(index)]);
    };
    const auto used_by_name = [&options]() {
        const int index = std::clamp(options.used_by, 0,
                                     static_cast<int>(std::size(kMouseSetupUsedByNames)) - 1);
        return std::string(kMouseSetupUsedByNames[static_cast<std::size_t>(index)]);
    };
    const auto button_label = [&options](int button, std::string_view used_text,
                                         std::string_view unused_text) {
        const int index = std::clamp(options.buttons[static_cast<std::size_t>(button)], -1,
                                     static_cast<int>(std::size(kMouseSetupKeyNames)) - 1);
        if (index < 0) {
            return std::string(unused_text);
        }
        return std::string(used_text) + " " +
               std::string(kMouseSetupKeyNames[static_cast<std::size_t>(index)]);
    };

    switch (item) {
    case 0:
        return "Used By: " + used_by_name();                      // 0x4bd8b4
    case 1:
        return "Left/Right: " + axis_name(0);                     // 0x4bd8a4
    case 2:
        return "Sensitivity:";                                    // 0x4bd894
    case 3:
        return "Forward/Back: " + axis_name(1);                   // 0x4bd880
    case 4:
        return "Sensitivity:";                                    // 0x4bd894
    case 5:
        return "Up/Down (3D): " + axis_name(2);                   // 0x4bd86c
    case 6:
        return "Sensitivity:";                                    // 0x4bd894
    case 7:
        return button_label(0, "Button 1:", "Button 1: NOT USED"); // 0x4bd85c/0x4bd848
    case 8:
        return button_label(1, "Button 2:", "Button 2: NOT USED"); // 0x4bd838/0x4bd824
    case 9:
        return button_label(2, "Button 3:", "Button 3: NOT USED"); // 0x4bd814/0x4bd800
    case 10:
        return button_label(3, "Button 4:", "Button 4: NOT USED"); // 0x4bd7f0/0x4bd7dc
    case 11:
    default:
        return "Done";                                            // 0x4bceb4
    }
}

namespace {

constexpr std::string_view kPrefix = "mouse_setup_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`, the archive from `res/bigfile`) so the caller sees one
/// `"mouse_setup_render: ..."` chain instead of a doubled one.
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
           MouseSetupRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = MouseSetupRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so it
/// is forced to 255 (same as `ui/menu_render` and `ui/restart_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The minimal index layer of the exact fade (`0x682f90`): table + level,
/// applied to the palette index of a pixel **before** its colour is looked up
/// (docs/HUD_LUT.md §2b, docs/MOUSE_SETUP_RENDER.md §2).  `table == nullptr` is
/// the unfaded path.
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

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP,
/// exactly like `ui/restart_render::read_bmp_entry` (read-only, the index is
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

/// Load one sprite of `FRONTEND/BMP.IDX` and refuse a size that is not the one
/// docs/UI_SCREENS.md §9 proves (`SLIDE` 72x8, `KNOB` 8x8).
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
/// `0x47faf0` goes down (`ui/menu_render` and `ui/restart_render` use the same
/// order).  Palette index 0 is transparent.  Characters without a glyph
/// increment `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, MouseSetupRenderStats& stats,
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

/// Masked blit of one sprite (`0x486070`/`0x4860b0` skip source index 0), into
/// the 196x131 band.  Counts the newly covered pixels in `stats.ink_pixels`
/// (unique, the same counter the glyphs use) and returns how many it painted.
bool blit_sprite(const Sprite& sprite, int x, int y, const PaletteFade& fade, Layer& layer,
                 MouseSetupRenderStats& stats, std::string& error) {
    const std::size_t entries = sprite.palette_entries();
    int painted = 0;
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
                ++painted;
                ++stats.ink_pixels;
            }
        }
    }
    (void)painted; // the unique-pixel count lives in `stats.ink_pixels`
    return true;
}

/// Paint `options.background` at (0,0), clipped to the canvas.  A missing,
/// broken or non-8-bpp entry is a text, never a silently black frame.  The
/// panel is a backdrop, so its pixels are not counted as `ink_pixels` (the same
/// rule as `ui/menu_render`/`ui/restart_render`).
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

bool render_mouse_setup(const std::filesystem::path& game,
                        const MouseSetupRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        MouseSetupRenderStats* stats, std::string* error) {
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

    // §3 faces: title 5, items 0, cursor 4 (three FONTS4 atlases of BMP.IDX).
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number : {kMouseSetupTitleFont, kMouseSetupRowFont,
                             kMouseSetupSelectedFont}) {
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

    // `0x486070`/`0x4860b0`: one `SLIDE` and one `KNOB` sprite, blitted three
    // times each (the three sensitivity rows share the same two assets).
    Sprite slide;
    Sprite knob;
    if (!load_sprite(game, "SLIDE", kMouseSetupSlideWidth, kMouseSetupSlideHeight, slide,
                     err) ||
        !load_sprite(game, "KNOB", kMouseSetupKnobWidth, kMouseSetupKnobHeight, knob,
                     err)) {
        return false;
    }

    width = kMouseSetupCanvasWidth;
    height = kMouseSetupCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kMouseSetupBackground);
    if (!options.background.empty() &&
        !paint_background(game, options.background, fade, rgba, width, height, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    MouseSetupRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kMouseSetupPanelWidth),
                std::min(height, kMouseSetupPanelHeight)};

    // 0x489a2c..0x489a3b: `MOUSE SETUP`, font 5, `centered=1` over the 196 px
    // band, row 11 (item -1 makes the row absolute).
    {
        const FontAtlas* title = fonts(kMouseSetupTitleFont);
        const int title_x =
            std::max(0, (kMouseSetupPanelWidth - text_width(*title, kMouseSetupTitle)) / 2);
        if (!draw_run(fonts, kMouseSetupTitleFont, kMouseSetupTitle, title_x,
                      kMouseSetupTitleY, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // Items in the handler's emission order: rows 0..6, the first slider, rows
    // 7..11 (`0x489a40`..`0x489d51`).  The cursor's item switches to font 4.
    const auto draw_item = [&](int item) {
        const std::string label = mouse_setup_label(item, options);
        const int font = item == options.cursor ? kMouseSetupSelectedFont : kMouseSetupRowFont;
        return draw_run(fonts, font, label, mouse_setup_item_x(item),
                        mouse_setup_item_y(item), fade, layer, counts, err);
    };
    for (int item = 0; item <= 2; ++item) {
        if (!draw_item(item)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }
    for (int i = 0; i < kMouseSetupSliderCount; ++i) {
        const std::size_t slot = static_cast<std::size_t>(i);
        const int y = kMouseSetupSlideY[slot];
        if (!blit_sprite(slide, kMouseSetupSlideX, y, fade, layer, counts, err) ||
            !blit_sprite(knob, mouse_setup_knob_x(options.sensitivity[slot]), y, fade,
                         layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        ++counts.sliders;
        // Rows between the sliders: item 3 after the first, 4 after the first,
        // 5 after the second, 6 after the second, 7..11 after the third.
        for (int item = 3 + 2 * i; item <= 4 + 2 * i && item <= 6; ++item) {
            if (!draw_item(item)) {
                clear(rgba, width, height, stats);
                return false;
            }
        }
        if (i == kMouseSetupSliderCount - 1) {
            for (int item = 7; item < kMouseSetupRows; ++item) {
                if (!draw_item(item)) {
                    clear(rgba, width, height, stats);
                    return false;
                }
            }
        }
    }

    counts.rows = kMouseSetupRows;
    // Screen 22 has no MENUARRW call at 0x489a10.
    counts.arrows = 0;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
