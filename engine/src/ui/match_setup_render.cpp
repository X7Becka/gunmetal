#include "ui/match_setup_render.h"

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

int match_setup_item_y(int item) {
    if (item < 0 || item >= kMatchSetupRows) {
        return kMatchSetupStartGameY;
    }
    return kMatchSetupItemY[static_cast<std::size_t>(item)];
}

int match_setup_item_x(int item) {
    switch (item) {
    case 0:
    case 1:
    case kMatchSetupStartGameItem:
        return kMatchSetupValueX;
    default:
        return kMatchSetupOptionX;
    }
}

int match_setup_match_type(int requested) {
    return std::clamp(requested, 0, kMatchSetupMatchTypeCount - 1);
}

int match_setup_cash_index(int requested) {
    return std::clamp(requested, 0, kMatchSetupCashCount - 1);
}

long long match_setup_starting_cash(int index) {
    return kMatchSetupStartingCash[static_cast<std::size_t>(match_setup_cash_index(index))];
}

bool match_setup_checked(int flags, int item) {
    if (item < kMatchSetupCheckboxFirstItem ||
        item >= kMatchSetupCheckboxFirstItem + kMatchSetupCheckboxCount) {
        return false;
    }
    const int bit = kMatchSetupCheckboxFlag[static_cast<std::size_t>(
        item - kMatchSetupCheckboxFirstItem)];
    return (flags & bit) != 0;
}

int match_setup_checkbox_y(int item) {
    return match_setup_item_y(item) + kMatchSetupCheckboxYOffset;
}

std::string match_setup_label(int item, const MatchSetupRenderOptions& options) {
    switch (item) {
    case 0:
        return std::string(kMatchSetupMatchTypes[static_cast<std::size_t>(
            match_setup_match_type(options.match_type))]); // 0x4bc6e8
    case 1:
        // 0x488623 pushes 0x4bd374 and the *value* of the table (`0x48861c`):
        // `sprintf(buffer, "Starting Cash: %ld", 0x4bc6f8[index])`.
        return "Starting Cash: " + std::to_string(match_setup_starting_cash(options.starting_cash));
    case 2:
        return "Teamplay";                                    // 0x4bd368
    case 3:
        return "No Enemies";                                  // 0x4bd35c
    case 4:
        return "Can't Buy Ammo";                              // 0x4bd34c
    case 5:
        return "Handicap Better Players";                     // 0x4bd334
    case 6:
        return "Allow Mouse Cheating";                        // 0x4bd31c
    case kMatchSetupStartGameItem:
    default:
        return "START GAME!";                                 // 0x4bd310
    }
}

namespace {

constexpr std::string_view kPrefix = "match_setup_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`, the archive from `res/bigfile`) so the caller sees one
/// `"match_setup_render: ..."` chain instead of a doubled one.
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
           MatchSetupRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = MatchSetupRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so it
/// is forced to 255 (same as `ui/menu_render` and `ui/mouse_setup_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The minimal index layer of the exact fade (`0x682f90`): table + level,
/// applied to the palette index of a pixel **before** its colour is looked up
/// (docs/HUD_LUT.md §2b, docs/MATCH_SETUP_RENDER.md §2).  `table == nullptr` is
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

/// One 8-bpp `FRONTEND/BMP.IDX` entry (the panel or a checkbox sprite), kept in
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
/// exactly like `ui/mouse_setup_render::read_bmp_entry` (read-only, the index is
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

/// Decode one `FRONTEND/BMP.IDX` entry into a palette-index sprite.  The error
/// has no module prefix; the callers add theirs.
bool load_entry(const std::filesystem::path& game, std::string_view name, Sprite& sprite,
                std::string& error) {
    BmpImage image;
    if (!read_bmp_entry(game, name, image, error)) {
        return false;
    }
    sprite.width = image.width;
    sprite.height = image.height;
    sprite.palette = std::move(image.palette);
    sprite.indices = std::move(image.pixels);
    sprite.name = std::string(name);
    return true;
}

/// Load one sprite of `FRONTEND/BMP.IDX` and refuse a size that is not the one
/// docs/UI_SCREENS.md §9 proves (`PLATE` 196x131, `CHECK` 8x16, `NOCHECK` 8x8).
bool load_sprite(const std::filesystem::path& game, std::string_view name, int want_width,
                 int want_height, Sprite& sprite, std::string& error) {
    std::string load_error;
    if (!load_entry(game, name, sprite, load_error)) {
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
/// `0x47faf0` goes down (`ui/menu_render` and `ui/mouse_setup_render` use the
/// same order).  Palette index 0 is transparent.  Characters without a glyph
/// increment `missing_glyphs` and are skipped instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, MatchSetupRenderStats& stats,
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
                 MatchSetupRenderStats& stats, std::string& error) {
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
                continue; // transparent key of the checkbox sprites
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

/// `0x4833e0`: `CHECK` 8x16 at the anchor when the bit is set, `NOCHECK` 8x8
/// eight rows lower when it is clear (both masked, both bottom-aligned).
bool draw_checkbox(const Sprite& check, const Sprite& nocheck, bool checked, int x, int y,
                   const PaletteFade& fade, Layer& layer, MatchSetupRenderStats& stats,
                   std::string& error) {
    if (checked) {
        return blit_sprite(check, x, y, fade, layer, stats, error);
    }
    return blit_sprite(nocheck, x, y + kMatchSetupNoCheckDrop, fade, layer, stats, error);
}

/// Paint one already-decoded sprite at (0,0), clipped to the canvas.  The
/// panel is a backdrop, so its pixels are not counted as `ink_pixels` (the same
/// rule as `ui/menu_render`/`ui/mouse_setup_render`).
bool paint_backdrop(const Sprite& sprite, const PaletteFade& fade,
                    std::vector<std::uint32_t>& rgba, int width, int height,
                    std::string& error) {
    const std::size_t entries = sprite.palette_entries();
    const int copy_width = std::min(width, sprite.width);
    const int copy_height = std::min(height, sprite.height);
    for (int y = 0; y < copy_height; ++y) {
        for (int x = 0; x < copy_width; ++x) {
            const std::uint8_t index =
                sprite.indices[static_cast<std::size_t>(y) *
                                   static_cast<std::size_t>(sprite.width) +
                               static_cast<std::size_t>(x)];
            if (index >= entries) {
                error = std::string(kPrefix) + sprite.name + " uses palette index " +
                        std::to_string(index) + " of " + std::to_string(entries);
                return false;
            }
            std::uint8_t mapped = index;
            if (fade.active() &&
                !fade_index(fade, index, entries, sprite.name, mapped, error)) {
                return false;
            }
            rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = palette_colour(sprite.palette, mapped);
        }
    }
    return true;
}

/// The font of `item` for this frame: the caller's face (0 for the rows, 5 for
/// `START GAME!`) until `0x485dd0` swaps in font 4 for the cursor item — and
/// that swap only happens when the caller passed font 0 (`0x485e15`), which is
/// why the `START GAME!` call selects its font by `[0x68ccf4] == 7`.
int item_font(int item, int cursor) {
    if (item == cursor) {
        return kMatchSetupSelectedFont;
    }
    return item == kMatchSetupStartGameItem ? kMatchSetupTitleFont : kMatchSetupRowFont;
}

} // namespace

bool render_match_setup(const std::filesystem::path& game,
                        const MatchSetupRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        MatchSetupRenderStats* stats, std::string* error) {
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
    for (const int number : {kMatchSetupTitleFont, kMatchSetupRowFont,
                             kMatchSetupSelectedFont}) {
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

    // `0x4885d7` -> `0x486050`: the handler copies the preloaded 196x131 PLATE
    // into `0x6865f8`; without the asset the frame buffer has no content.
    Sprite plate;
    if (!load_sprite(game, kMatchSetupPanelName, kMatchSetupPanelWidth,
                     kMatchSetupPanelHeight, plate, err)) {
        return false;
    }
    // `0x4833e0` reads `CHECK` (16 rows, `0x67fc10`) or `NOCHECK` (8 rows,
    // `0x683fb8`).
    Sprite check;
    Sprite nocheck;
    if (!load_sprite(game, "CHECK", kMatchSetupCheckWidth, kMatchSetupCheckHeight, check,
                     err) ||
        !load_sprite(game, "NOCHECK", kMatchSetupNoCheckWidth, kMatchSetupNoCheckHeight,
                     nocheck, err)) {
        return false;
    }

    width = kMatchSetupCanvasWidth;
    height = kMatchSetupCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kMatchSetupBackground);
    if (!options.background.empty()) {
        // `"PLATE"` reuses the copy that was validated above; any other name is
        // a separate BMP.IDX entry (read-only).
        const bool is_plate = same_name_ci(options.background, kMatchSetupPanelName);
        Sprite backdrop;
        const Sprite* layer = &plate;
        if (!is_plate) {
            std::string load_error;
            if (!load_entry(game, options.background, backdrop, load_error)) {
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

    MatchSetupRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer canvas{rgba,
                 covered,
                 width,
                 height,
                 0,
                 0,
                 std::min(width, kMatchSetupPanelWidth),
                 std::min(height, kMatchSetupPanelHeight)};

    // 0x4885dc..0x4885eb: `MULTIPLAY GAME OPTIONS`, font 5, `centered=1` over
    // the 196 px band, row 11 (item -1 makes the row absolute).
    {
        const FontAtlas* title = fonts(kMatchSetupTitleFont);
        const int title_x =
            std::max(0, (kMatchSetupPanelWidth - text_width(*title, kMatchSetupTitle)) / 2);
        if (!draw_run(fonts, kMatchSetupTitleFont, kMatchSetupTitle, title_x,
                      kMatchSetupTitleY, fade, canvas, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // The handler's emission order: item 0, item 1, checkbox 1 (Teamplay),
    // item 2, checkbox 2, ..., item 6, then `START GAME!` (0x4885f0..0x488772).
    const auto draw_item = [&](int item) {
        const std::string label = match_setup_label(item, options);
        const int font = item_font(item, options.cursor);
        return draw_run(fonts, font, label, match_setup_item_x(item),
                        match_setup_item_y(item), fade, canvas, counts, err);
    };
    for (int item = 0; item < kMatchSetupRows; ++item) {
        if (!draw_item(item)) {
            clear(rgba, width, height, stats);
            return false;
        }
        if (item >= kMatchSetupCheckboxFirstItem &&
            item < kMatchSetupCheckboxFirstItem + kMatchSetupCheckboxCount) {
            const bool checked = match_setup_checked(options.flags, item);
            if (!draw_checkbox(check, nocheck, checked, kMatchSetupCheckboxX,
                               match_setup_checkbox_y(item), fade, canvas, counts, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
            ++counts.checks;
        }
    }

    counts.rows = kMatchSetupRows;
    // Screen 27 has no SLIDE/KNOB call at 0x4885c0 and no MENUARRW call.
    counts.sliders = 0;
    counts.arrows = 0;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
