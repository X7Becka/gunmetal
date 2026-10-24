#include "ui/control_options_render.h"

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

namespace {

/// `"%.Ns"` on a fixed-size field: stop at the first NUL and cap at `chars`
/// bytes (an unterminated field prints exactly `chars` characters).
std::string capped_field(std::string_view raw, int chars) {
    const std::size_t nul = raw.find('\0');
    if (nul != std::string_view::npos) {
        raw = raw.substr(0, nul);
    }
    if (raw.size() > static_cast<std::size_t>(chars)) {
        raw = raw.substr(0, static_cast<std::size_t>(chars));
    }
    return std::string(raw);
}

} // namespace

int control_options_row_y(int item) {
    return kControlOptionsFirstRowY + kControlOptionsRowStep * item;
}

int control_options_row_x(int item) {
    // `push 0x20` at `0x488c57` and `0x488cea` (items 1 and 3), `push 0x14`
    // everywhere else.
    return (item == 1 || item == 3) ? kControlOptionsSubRowX : kControlOptionsRowX;
}

int control_options_title_x(const FontAtlas& title_face) {
    const int width = title_face.measure(kControlOptionsTitle);
    return std::max(0, (kControlOptionsPanelWidth - width) / 2);
}

std::string_view control_options_mouse_name(int index) {
    const int clamped =
        std::clamp(index, 0, static_cast<int>(std::size(kControlOptionsMouseNames)) - 1);
    return kControlOptionsMouseNames[static_cast<std::size_t>(clamped)];
}

std::string control_options_item_text(const ControlOptionsRenderOptions& options, int item) {
    const int set1 = std::clamp(options.keyset1, 0, kControlOptionsKeySets - 1);
    const int set2 = std::clamp(options.keyset2, 0, kControlOptionsKeySets - 1);
    const std::string& name1 = options.keyset_names[0];
    const std::string& name2 = options.keyset_names[1];
    switch (item) {
    case 0:
        // 0x488bd0..0x488c08: sprintf(dst, "Player 1: %s", name).
        return "Player 1: " + capped_field(name1, kControlOptionsKeyNameChars);
    case 1:
        // 0x488c15..0x488c5e: set >= 1 -> "Assign %s", else "View %s".
        return (set1 >= 1 ? "Assign " : "View ") +
               capped_field(name1, kControlOptionsKeyNameChars);
    case 2:
        // 0x488c63..0x488c9b.
        return "Player 2: " + capped_field(name2, kControlOptionsKeyNameChars);
    case 3:
        // 0x488ca8..0x488cf1.
        return (set2 >= 1 ? "Assign " : "View ") +
               capped_field(name2, kControlOptionsKeyNameChars);
    case 4:
        return std::string(kControlOptionsKeyJamming); // 0x4bd5b4
    case 5:
        // 0x488d6f..0x488db8: slot -1 -> 0x4bd598, else "1: %.25s".
        if (options.joystick1 < 0) {
            return std::string(kControlOptionsJoystick1Unused);
        }
        return std::string(kControlOptionsJoystick1Prefix) +
               capped_field(options.joystick_name1, kControlOptionsJoystickNameChars);
    case 6:
        // 0x488dbd..0x488e0e: 0x4bd570 or "2: %.25s".
        if (options.joystick2 < 0) {
            return std::string(kControlOptionsJoystick2Unused);
        }
        return std::string(kControlOptionsJoystick2Prefix) +
               capped_field(options.joystick_name2, kControlOptionsJoystickNameChars);
    case 7:
        return std::string(kControlOptionsCentering); // 0x4bd550
    case 8:
        // 0x488e2a..0x488e5c: "Mouse: %s" of 0x4bc548 + [0x5a4938]*4.
        return "Mouse: " + std::string(control_options_mouse_name(options.mouse_type));
    case 9:
        return std::string(kControlOptionsDone); // 0x4bceb4
    default:
        return {};
    }
}

namespace {

constexpr std::string_view kPrefix = "control_options_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`, the archive from `res/bigfile`) so the caller sees one
/// `"control_options_render: ..."` chain instead of a doubled one.
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
           ControlOptionsRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = ControlOptionsRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so it
/// is forced to 255 (same as `ui/menu_render` and `ui/keycfg_render`).
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

/// Paint one run at (`x`, `y`), `y` being the top row of the glyph cell.
///
/// `FontGlyph::pixels` row 0 is the top cell row: the blitter `0x47faf0` writes
/// it at the destination row and goes down (`ui/menu_render`, `ui/keycfg_render`
/// use the same order).  Palette index 0 is transparent.  Characters without a
/// glyph increment `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, ControlOptionsRenderStats& stats,
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

/// Paint one already-decoded sprite at (0,0), clipped to the canvas.  The panel
/// is a backdrop, so its pixels are not counted as `ink_pixels` (the same rule
/// as `ui/menu_render`/`ui/keycfg_render`).
bool paint_backdrop(const BmpImage& sprite, const PaletteFade& fade,
                    std::vector<std::uint32_t>& rgba, int width, int height,
                    std::string& error) {
    const std::size_t entries = sprite.palette.size() / 4u;
    const int copy_width = std::min(width, sprite.width);
    const int copy_height = std::min(height, sprite.height);
    for (int y = 0; y < copy_height; ++y) {
        for (int x = 0; x < copy_width; ++x) {
            const std::uint8_t index =
                sprite.pixels[static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(sprite.width) +
                              static_cast<std::size_t>(x)];
            if (index >= entries) {
                error = std::string(kPrefix) + "background uses palette index " +
                        std::to_string(index) + " of " + std::to_string(entries);
                return false;
            }
            std::uint8_t mapped = index;
            if (fade.active() &&
                !fade_index(fade, index, entries, "background", mapped, error)) {
                return false;
            }
            rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = palette_colour(sprite.palette, mapped);
        }
    }
    return true;
}

/// Case-insensitive comparison of an archive entry name with a wanted name.
bool same_name_ci(std::string_view have, std::string_view want) {
    if (have.size() != want.size()) {
        return false;
    }
    const auto upper = [](char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    };
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (upper(have[i]) != upper(want[i])) {
            return false;
        }
    }
    return true;
}

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP,
/// exactly like `ui/keycfg_render::read_bmp_entry` (read-only, the index is
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
        if (same_name_ci(candidate.name, name)) {
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
/// docs/UI_SCREENS.md §9 proves.
bool load_sprite(const std::filesystem::path& game, std::string_view name, int want_width,
                 int want_height, BmpImage& sprite, std::string& error) {
    std::string load_error;
    if (!read_bmp_entry(game, name, sprite, load_error)) {
        error = std::string(kPrefix) + std::string(name) + ": " + detail_of(load_error);
        return false;
    }
    if (sprite.width != want_width || sprite.height != want_height) {
        error = std::string(kPrefix) + std::string(name) + " is " +
                std::to_string(sprite.width) + "x" + std::to_string(sprite.height) +
                ", expected " + std::to_string(want_width) + "x" +
                std::to_string(want_height);
        return false;
    }
    return true;
}

} // namespace

bool render_control_options(const std::filesystem::path& game,
                            const ControlOptionsRenderOptions& options,
                            std::vector<std::uint32_t>& rgba, int& width, int& height,
                            ControlOptionsRenderStats* stats, std::string* error) {
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

    // Faces 5 (title), 0 (every item) and 4 (the cursor item) — three rows of
    // the same FONTS4 atlas.
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number : {kControlOptionsTitleFont, kControlOptionsRowFont,
                             kControlOptionsSelectedFont}) {
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
    const FontAtlas& title_face = *fonts(kControlOptionsTitleFont);

    // `0x488bb7` -> `0x486050`: the handler copies the preloaded 196x131 PLATE
    // into `0x6865f8`; without the asset the frame buffer has no content.
    BmpImage plate;
    if (!load_sprite(game, kControlOptionsPanelName, kControlOptionsPanelWidth,
                     kControlOptionsPanelHeight, plate, err)) {
        return false;
    }

    width = kControlOptionsCanvasWidth;
    height = kControlOptionsCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kControlOptionsBackground);
    if (!options.background.empty()) {
        // `"PLATE"` reuses the copy that was validated above; any other name is
        // a separate BMP.IDX entry (read-only).
        BmpImage backdrop;
        const BmpImage* layer = &plate;
        if (!same_name_ci(options.background, kControlOptionsPanelName)) {
            std::string load_error;
            if (!read_bmp_entry(game, options.background, backdrop, load_error)) {
                err = std::string(kPrefix) + "background " + options.background + ": " +
                      detail_of(load_error);
                clear(rgba, width, height, stats);
                return false;
            }
            layer = &backdrop;
        }
        if (!paint_backdrop(*layer, fade, rgba, width, height, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    ControlOptionsRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer canvas{rgba,
                 covered,
                 width,
                 height,
                 0,
                 0,
                 std::min(width, kControlOptionsPanelWidth),
                 std::min(height, kControlOptionsPanelHeight)};

    // `0x485dd0` force-swaps font 0 for font 4 on the cursor item
    // (`0x485e0d`..`0x485e19`); a caller font of 5 is left alone (the title is
    // item -1, so it never matches the cursor).
    const int cursor = options.cursor;
    const auto item_font = [cursor](int item) {
        return item == cursor ? kControlOptionsSelectedFont : kControlOptionsRowFont;
    };

    // 0x488bbc..0x488bcb: title 0x4bd5f4, font 5, centred in the 196 px band,
    // row 14, item -1.
    if (!draw_run(fonts, kControlOptionsTitleFont, kControlOptionsTitle,
                  control_options_title_x(title_face), kControlOptionsTitleY, fade, canvas,
                  counts, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    // 0x488bd0..0x488e78: the ten items, top to bottom, `row = 27` each and the
    // handler's item index as the last `0x485dd0` argument.
    for (int item = 0; item < kControlOptionsRows; ++item) {
        if (!draw_run(fonts, item_font(item), control_options_item_text(options, item),
                      control_options_row_x(item), control_options_row_y(item), fade, canvas,
                      counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    counts.rows = kControlOptionsRows;
    // 0x488ba0 has no SLIDE/KNOB (0x486070/0x4860b0), no CHECK/NOCHECK
    // (0x4833e0) and no MENUARRW (0x485cc0/0x485d00) call; all three stay 0.
    counts.sliders = 0;
    counts.checks = 0;
    counts.arrows = 0;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
