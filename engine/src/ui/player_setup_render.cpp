#include "ui/player_setup_render.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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

int player_setup_row_y(int row) {
    return kPlayerSetupFirstRowY + kPlayerSetupItemStep * row;
}

int player_setup_scroll(int cursor) {
    return std::max(0, cursor - kPlayerSetupScrollBand);
}

int player_setup_value_x(const FontAtlas& row_face) {
    return kPlayerSetupRowX + row_face.measure(kPlayerSetupValueMeasureLabel);
}

int player_setup_rows(const PlayerSetupRenderOptions& options) {
    if (options.joystick < 0) {
        return kPlayerSetupRowsNoDevice;
    }
    int rows = kPlayerSetupRowsDevice;
    for (int axis = 0; axis < 6; ++axis) {
        if ((options.axis_present & kPlayerSetupAxisPresentBits[axis]) != 0) {
            ++rows; // `0x48a397`/`0x48a3a1` of the row helper
        }
    }
    if (options.hat1) {
        rows += 2; // `0x489f60`
    }
    if (options.hat2) {
        rows += 2; // `0x48a073`
    }
    return rows;
}

int player_setup_content_rows(const PlayerSetupRenderOptions& options) {
    if (options.joystick < 0) {
        return 1;
    }
    int lines = 1; // device name row
    for (int axis = 0; axis < 6; ++axis) {
        if ((options.axis_present & kPlayerSetupAxisPresentBits[axis]) != 0) {
            ++lines;
        }
    }
    if (options.hat1) {
        lines += 2;
    }
    if (options.hat2) {
        lines += 2;
    }
    // Every one of the 32 channels is drawn, assigned or not (the `jl` at
    // `0x48a1b2` only swaps the text for `Button N: None`).
    return lines + kPlayerSetupButtonChannels;
}

std::string_view player_setup_preset_name(int index) {
    const int clamped =
        std::clamp(index, 0, static_cast<int>(std::size(kPlayerSetupPresetNames)) - 1);
    return kPlayerSetupPresetNames[static_cast<std::size_t>(clamped)];
}

std::string_view player_setup_action_name(int index) {
    const int clamped =
        std::clamp(index, 0, static_cast<int>(std::size(kPlayerSetupActionNames)) - 1);
    return kPlayerSetupActionNames[static_cast<std::size_t>(clamped)];
}

std::string player_setup_button_text(int channel, int mask) {
    const int number = std::clamp(channel, 0, kPlayerSetupButtonChannels - 1) + 1;
    std::string label = "Button " + std::to_string(number) + ": ";
    // `0x4863a0` maps `1 << n` to `n` and `-1` to `-1`; the caller draws `None`
    // for the negative value.  A zero or multi-bit mask would make that loop
    // read past the 31-name table in the original, so the model paints `None`.
    const unsigned bits = static_cast<unsigned>(mask);
    if (mask < 0 || bits == 0u || (bits & (bits - 1u)) != 0u) {
        return label + "None";
    }
    const int index = std::countr_zero(bits);
    if (index >= static_cast<int>(std::size(kPlayerSetupActionNames))) {
        return label + "None";
    }
    return label + std::string(kPlayerSetupActionNames[static_cast<std::size_t>(index)]);
}

namespace {

constexpr std::string_view kPrefix = "player_setup_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`, the archive from `res/bigfile`) so the caller sees one
/// `"player_setup_render: ..."` chain instead of a doubled one.
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
           PlayerSetupRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = PlayerSetupRenderStats{};
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

/// The canvas plus the 196x131 write window of `0x485dd0` / the arrow blits
/// (the copy of `PLATE`): a pixel outside it cannot reach the screen.
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

/// Width of a text run with the atlas advances (`0x47fa50`), needed for the
/// centring of the title and the hint (`0x485dd0`'s `centered` branch).
int text_width(const FontAtlas& atlas, std::string_view text) {
    return atlas.measure(text);
}

/// Paint one run at (`x`, `y`), `y` being the top row of the glyph cell.
///
/// `FontGlyph::pixels` row 0 is the top cell row: the blitter `0x47faf0` writes
/// it at the destination row and goes down (`ui/menu_render`, `ui/keycfg_render`
/// use the same order).  Palette index 0 is transparent.  Characters without a
/// glyph increment `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, PlayerSetupRenderStats& stats,
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

/// Paint the `MENUARRW` scroll arrow at (`x`, `y`) of the band.
///
/// `0x485cc0` blits the 128 sprite bytes verbatim (16 per row, eight rows,
/// `lea ecx,[ecx+ebx-0x10]` = stride 196, zero bytes skipped at `0x485ce0`);
/// `0x485d00` blits the same bytes backwards (`mov al,[edx-1]; dec edx` at
/// `0x485d1a`), i.e. the up arrow is the down arrow rotated by 180 degrees.
/// The loader `0x47efe0` stores the file rows as they are (`fread` of
/// `width * height` bytes), while `parse_bmp` returns them top-down, so sprite
/// row `r` of the engine is `pixels[height-1-r]`.
bool paint_arrow(const BmpImage& sprite, bool up, int x, int y, const PaletteFade& fade,
                 Layer& layer, std::string& error) {
    const std::size_t palette_entries = sprite.palette.size() / 4u;
    for (int row = 0; row < kPlayerSetupArrowHeight; ++row) {
        for (int column = 0; column < kPlayerSetupArrowWidth; ++column) {
            const int source_row = up ? kPlayerSetupArrowHeight - 1 - row : row;
            const int source_column = up ? kPlayerSetupArrowWidth - 1 - column : column;
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
                error = std::string(kPrefix) + "arrow " + std::string(kPlayerSetupArrowName) +
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
            std::uint8_t mapped = value;
            if (fade.active() &&
                !fade_index(fade, value, palette_entries,
                            "arrow " + std::string(kPlayerSetupArrowName), mapped, error)) {
                return false;
            }
            layer.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                         static_cast<std::size_t>(px)] =
                palette_colour(sprite.palette, mapped);
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

/// `"%.30s"` of the device-name field: the string up to the first NUL, capped
/// at `kPlayerSetupNameMaxChars` bytes.
std::string name_field(std::string_view raw) {
    const std::size_t nul = raw.find('\0');
    if (nul != std::string_view::npos) {
        raw = raw.substr(0, nul);
    }
    if (raw.size() > static_cast<std::size_t>(kPlayerSetupNameMaxChars)) {
        raw = raw.substr(0, static_cast<std::size_t>(kPlayerSetupNameMaxChars));
    }
    return std::string(raw);
}

} // namespace

bool render_player_setup(const std::filesystem::path& game,
                         const PlayerSetupRenderOptions& options,
                         std::vector<std::uint32_t>& rgba, int& width, int& height,
                         PlayerSetupRenderStats* stats, std::string* error) {
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

    // Faces 5 (title and the red labels), 0 (every other run) and 4 (the cursor
    // line) — three rows of the same FONTS4 atlas.
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number : {kPlayerSetupTitleFont, kPlayerSetupRowFont,
                             kPlayerSetupSelectedFont}) {
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
    const FontAtlas& row_face = *fonts(kPlayerSetupRowFont);

    // `0x489d96` -> `0x486050`: the handler copies the preloaded 196x131 PLATE
    // into `0x6865f8`; without the asset the frame buffer has no content.
    BmpImage plate;
    if (!load_sprite(game, kPlayerSetupPanelName, kPlayerSetupPanelWidth,
                     kPlayerSetupPanelHeight, plate, err)) {
        return false;
    }

    // The window and the counter, all derived from the options (§ header).
    const int cursor = options.cursor;
    const int scroll = player_setup_scroll(cursor);
    const int rows = player_setup_rows(options);
    const int value_x = player_setup_value_x(row_face);

    width = kPlayerSetupCanvasWidth;
    height = kPlayerSetupCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kPlayerSetupBackground);
    if (!options.background.empty()) {
        // `"PLATE"` reuses the copy that was validated above; any other name is
        // a separate BMP.IDX entry (read-only).
        BmpImage backdrop;
        const BmpImage* layer = &plate;
        if (!same_name_ci(options.background, kPlayerSetupPanelName)) {
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

    PlayerSetupRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer canvas{rgba,
                 covered,
                 width,
                 height,
                 0,
                 0,
                 std::min(width, kPlayerSetupPanelWidth),
                 std::min(height, kPlayerSetupPanelHeight)};

    // `0x485dd0` force-swaps font 0 for font 4 on the cursor line
    // (`0x485e0d`..`0x485e19`); a caller font of 5 is left alone.
    const auto line_font = [cursor](int content, int base) {
        return (content == cursor && base == kPlayerSetupRowFont) ? kPlayerSetupSelectedFont
                                                                  : base;
    };
    const auto centred = [&](int font, std::string_view text, int y) {
        const FontAtlas* atlas = fonts(font);
        const int x = std::max(0, (kPlayerSetupPanelWidth - text_width(*atlas, text)) / 2);
        return draw_run(fonts, font, text, x, y, fade, canvas, counts, err);
    };

    // 0x489d9b..0x489dba / 0x489dc2..0x489dd1: title (0x4bd9c0 or 0x4bd9a8),
    // hint 0x4bd988, both centred, rows 11 and 20.
    if (!centred(kPlayerSetupTitleFont,
                 options.player == 0 ? kPlayerSetupTitlePlayer1 : kPlayerSetupTitlePlayer2,
                 kPlayerSetupTitleY) ||
        !centred(kPlayerSetupRowFont, kPlayerSetupHint, kPlayerSetupHintY)) {
        clear(rgba, width, height, stats);
        return false;
    }

    // Content rows.  `counter` is the handler's `[esp+0x10]`, i.e. the content
    // index that also feeds `item` and the `0x485dd0` highlight.
    if (options.joystick < 0) {
        // 0x489df9..0x489e39: one row and an early return; no arrows.
        if (scroll <= 0) {
            if (!draw_run(fonts, line_font(0, kPlayerSetupRowFont), kPlayerSetupNoDevice,
                          kPlayerSetupRowX, player_setup_row_y(0), fade, canvas, counts,
                          err)) {
                clear(rgba, width, height, stats);
                return false;
            }
        }
    } else {
        // 0x489e41..0x489e8e: `%.30s` of the device-name field, no prefix.
        if (scroll <= 0) {
            const std::string name = name_field(options.joystick_name);
            if (!draw_run(fonts, line_font(0, kPlayerSetupRowFont), name,
                          kPlayerSetupRowX, player_setup_row_y(0), fade, canvas, counts,
                          err)) {
                clear(rgba, width, height, stats);
                return false;
            }
        }

        int counter = 1;

        // 0x489eb3..0x489f30 through the helper 0x48a2b0: six axis rows, each
        // only when its presence bit is set.  The label turns font 5 on a
        // deflection of 0.5 or more; the value keeps font 0.
        for (int axis = 0; axis < 6; ++axis) {
            if ((options.axis_present & kPlayerSetupAxisPresentBits[axis]) == 0) {
                continue;
            }
            const int y = player_setup_row_y(counter - scroll);
            if (counter >= scroll) {
                const bool deflected =
                    std::fabs(options.axis_values[static_cast<std::size_t>(axis)]) >=
                    kPlayerSetupDeflectionThreshold;
                const int label_font =
                    line_font(counter, deflected ? kPlayerSetupTitleFont : kPlayerSetupRowFont);
                if (!draw_run(fonts, label_font,
                              kPlayerSetupAxisLabels[static_cast<std::size_t>(axis)],
                              kPlayerSetupRowX, y, fade, canvas, counts, err) ||
                    !draw_run(fonts, line_font(counter, kPlayerSetupRowFont),
                              player_setup_preset_name(
                                  options.axes[static_cast<std::size_t>(axis)]),
                              value_x, y, fade, canvas, counts, err)) {
                    clear(rgba, width, height, stats);
                    return false;
                }
            }
            ++counter;
        }

        // 0x489f54..0x48a16b: the two hat pairs.  Only the presence bit turns a
        // row on; its label is font 5 while the live state has the row's bits.
        const auto hat_pair = [&](bool present, unsigned presence_bit,
                                  std::size_t first) -> bool {
            if (!present) {
                return true;
            }
            for (std::size_t k = 0; k < 2; ++k) {
                const int slot = static_cast<int>(first + k);
                const int y = player_setup_row_y(counter - scroll);
                if (counter >= scroll) {
                    const bool active =
                        (options.hat_bits &
                         kPlayerSetupHatActiveBits[static_cast<std::size_t>(slot)]) != 0;
                    // `0x48a136` adds 13 to the value column before the Hat 2 Y
                    // blit: that one value sits one column further right.
                    const bool shifted = (presence_bit == kPlayerSetupHat2Bit && k == 1);
                    const int x = shifted ? value_x + kPlayerSetupRowX : value_x;
                    if (!draw_run(fonts,
                                  line_font(counter, active ? kPlayerSetupTitleFont
                                                            : kPlayerSetupRowFont),
                                  kPlayerSetupHatLabels[static_cast<std::size_t>(slot)],
                                  kPlayerSetupRowX, y, fade, canvas, counts, err) ||
                        !draw_run(fonts, line_font(counter, kPlayerSetupRowFont),
                                  player_setup_preset_name(
                                      options.hats[static_cast<std::size_t>(slot)]),
                                  x, y, fade, canvas, counts, err)) {
                        return false;
                    }
                }
                ++counter;
            }
            return true;
        };
        if (!hat_pair(options.hat1, kPlayerSetupHat1Bit, 0) ||
            !hat_pair(options.hat2, kPlayerSetupHat2Bit, 2)) {
            clear(rgba, width, height, stats);
            return false;
        }

        // 0x48a170..0x48a263: all 32 channels; the row of channel `i` is
        // `counter + i`, a pressed mask bit paints it font 5, and the ten-band
        // window stops the loop (`cmp eax,0xa`).
        for (int channel = 0; channel < kPlayerSetupButtonChannels; ++channel) {
            const int row = counter + channel;
            if (row < scroll) {
                continue;
            }
            if (row - scroll >= kPlayerSetupVisibleRows) {
                break; // `0x48a19b..0x48a19e`
            }
            const unsigned mask = 1u << static_cast<unsigned>(channel);
            const bool pressed = (options.pressed_buttons & mask) != 0;
            const int font =
                line_font(row, pressed ? kPlayerSetupTitleFont : kPlayerSetupRowFont);
            if (!draw_run(fonts, font,
                          player_setup_button_text(
                              channel, options.buttons[static_cast<std::size_t>(channel)]),
                          kPlayerSetupRowX, player_setup_row_y(row - scroll), fade, canvas,
                          counts, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
        }

        // 0x48a269..0x48a29a: the arrows come last, `scroll > 0` for the up one
        // and `scroll + 10 < [0x67f10c]` for the down one.
        const bool arrow_up = scroll > 0;
        const bool arrow_down = scroll + kPlayerSetupVisibleRows < rows;
        counts.arrows = (arrow_up ? 1 : 0) + (arrow_down ? 1 : 0);
        if (arrow_up || arrow_down) {
            BmpImage arrow;
            if (!load_sprite(game, kPlayerSetupArrowName, kPlayerSetupArrowWidth,
                             kPlayerSetupArrowHeight, arrow, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
            if ((arrow_up && !paint_arrow(arrow, true, kPlayerSetupArrowX,
                                          kPlayerSetupArrowUpY, fade, canvas, err)) ||
                (arrow_down && !paint_arrow(arrow, false, kPlayerSetupArrowX,
                                            kPlayerSetupArrowDownY, fade, canvas, err))) {
                clear(rgba, width, height, stats);
                return false;
            }
        }
    }

    counts.rows = rows;
    // 0x489d60 has no SLIDE/KNOB (0x486070/0x4860b0) and no CHECK/NOCHECK
    // (0x4833e0) call; both stay 0.
    counts.sliders = 0;
    counts.checks = 0;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
