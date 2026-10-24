#include "ui/start_game_render.h"

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

constexpr std::string_view kPrefix = "start_game_render: ";

/// Strip the module prefix of a nested error (the atlases come from
/// `render/text_layout`) so the caller sees one `"start_game_render: ..."`
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
           StartGameRenderStats* stats) {
    rgba.clear();
    width = 0;
    height = 0;
    if (stats != nullptr) {
        *stats = StartGameRenderStats{};
    }
}

/// One palette entry as `0xRRGGBBAA`; the BMP reserved byte is not alpha, so
/// it is forced to 255 (same as `ui/menu_render` and `ui/save_menu_render`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// The minimal index layer of the exact fade (`0x682f90`): table + level,
/// applied to the palette index of a pixel **before** its colour is looked up
/// (docs/HUD_LUT.md §2b, docs/START_GAME_RENDER.md §6).  `table == nullptr` is
/// the unfaded path, so the default options keep every byte of slices 5..7.
/// `ui/pager_render.cpp` has the same layer as `FadeSource` (private there), so
/// it is repeated here instead of reaching into another module's canvas.
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
/// 256-entry BMP palettes this cannot fire (`map` writes 0..255), it is a guard
/// for short palettes.
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
/// `ui/save_menu_render` use the same order).  Palette index 0 is transparent.
/// Characters without a glyph increment `missing_glyphs` and are skipped
/// instead of failing.
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              const PaletteFade& fade, Layer& layer, StartGameRenderStats& stats,
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

/// Paint the `MENUARRW` scroll arrow at (`x`, `y`) of the band.
///
/// `0x485cc0` blits the 128 sprite bytes verbatim (16 per row, eight rows,
/// `lea ecx,[ecx+ebx-0x10]` = stride 196, zero bytes skipped at `0x485ce0`);
/// `0x485d00` blits the same bytes backwards (`mov al,[edx-1]; dec edx` at
/// `0x485d1a`), i.e. the up arrow is the down arrow rotated by 180 degrees.
/// The loader `0x47efe0` stores the file rows as they are (`fread` of
/// `width * height` bytes at `0x47f100`), while `parse_bmp` returns them
/// bottom-up, so sprite row `r` is `pixels[height-1-r]`.
bool paint_arrow(const BmpImage& sprite, bool up, int x, int y, const PaletteFade& fade,
                 Layer& layer, std::string& error) {
    const std::size_t palette_entries = sprite.palette.size() / 4u;
    for (int row = 0; row < kStartGameArrowHeight; ++row) {
        for (int column = 0; column < kStartGameArrowWidth; ++column) {
            const int source_row = up ? kStartGameArrowHeight - 1 - row : row;
            const int source_column = up ? kStartGameArrowWidth - 1 - column : column;
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
                error = std::string(kPrefix) + "arrow " + std::string(kStartGameArrowName) +
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
                            "arrow " + std::string(kStartGameArrowName), mapped, error)) {
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
/// 8 bpp 16x8 image.  A missing or differently sized entry is a text.
bool load_arrow(const std::filesystem::path& game, BmpImage& image, std::string& error) {
    std::string load_error;
    if (!read_bmp_entry(game, kStartGameArrowName, image, load_error)) {
        error = std::string(kPrefix) + "arrow " + std::string(kStartGameArrowName) + ": " +
                detail_of(load_error);
        return false;
    }
    if (image.width != kStartGameArrowWidth || image.height != kStartGameArrowHeight) {
        error = std::string(kPrefix) + "arrow " + std::string(kStartGameArrowName) + " is " +
                std::to_string(image.width) + "x" + std::to_string(image.height) + ", want " +
                std::to_string(kStartGameArrowWidth) + "x" +
                std::to_string(kStartGameArrowHeight);
        return false;
    }
    return true;
}

/// The `FILESLOT` band of `FRONTEND/BMP.IDX` as `0x48ab90` copies it: an 8 bpp
/// 124x10 image.  A missing or differently sized entry is a text.
bool load_fileslot(const std::filesystem::path& game, BmpImage& image, std::string& error) {
    std::string load_error;
    if (!read_bmp_entry(game, kStartGameFileslotName, image, load_error)) {
        error = std::string(kPrefix) + "fileslot " + std::string(kStartGameFileslotName) +
                ": " + detail_of(load_error);
        return false;
    }
    if (image.width != kStartGameFileslotWidth ||
        image.height != kStartGameFileslotHeight) {
        error = std::string(kPrefix) + "fileslot " + std::string(kStartGameFileslotName) +
                " is " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                ", want " + std::to_string(kStartGameFileslotWidth) + "x" +
                std::to_string(kStartGameFileslotHeight);
        return false;
    }
    return true;
}

/// Copy the `FILESLOT` bar to (`x`, `y`) of the band, as `0x484cc0` does from
/// `0x48ab90`: ten rows of 124 bytes, no transparency (every index is written,
/// including 0 — the shipped art has none), destination stride 196.
///
/// `0x47efe0` reads the BMP pixel bytes verbatim (`fread` of `width*height` at
/// `0x47f100`), so file row `r` is buffer row `r` and the forward `rep movs`
/// puts it on screen row `y + r`; `parse_bmp` returns top-down rows, so the
/// visible row is `height - 1 - r` (the `flipV(parse_bmp)` of the pager,
/// docs/PAGER_RENDER.md §2.4).
bool paint_fileslot(const BmpImage& band, int x, int y, const PaletteFade& fade,
                    Layer& layer, std::string& error) {
    const std::size_t palette_entries = band.palette.size() / 4u;
    for (int row = 0; row < kStartGameFileslotHeight; ++row) {
        const int source_row = band.height - 1 - row;
        for (int column = 0; column < kStartGameFileslotWidth; ++column) {
            const std::uint8_t value =
                band.pixels[static_cast<std::size_t>(source_row) *
                                static_cast<std::size_t>(band.width) +
                            static_cast<std::size_t>(column)];
            const int px = x + column;
            const int py = y + row;
            if (px < layer.clip_x0 || px >= layer.clip_x1 || py < layer.clip_y0 ||
                py >= layer.clip_y1) {
                continue;
            }
            if (value >= palette_entries) {
                error = std::string(kPrefix) + "fileslot " +
                        std::string(kStartGameFileslotName) + " uses palette index " +
                        std::to_string(value) + " of " + std::to_string(palette_entries);
                return false;
            }
            std::uint8_t mapped = value;
            if (fade.active() &&
                !fade_index(fade, value, palette_entries,
                            "fileslot " + std::string(kStartGameFileslotName), mapped, error)) {
                return false;
            }
            layer.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                         static_cast<std::size_t>(px)] =
                palette_colour(band.palette, mapped);
        }
    }
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

/// The text of one slot band: the table entry of `0x6c2fe0 + slot*0x13`, i.e.
/// the save title of an occupied slot or `::: empty slot :::` (`0x4bc3b0`).
std::string_view slot_text(const SaveMenu& menu, int slot) {
    const SaveSlotView& view = menu.at(slot);
    if (!view.occupied) {
        return SaveMenu::kEmptyLabel;
    }
    return std::string_view(view.name);
}

} // namespace

bool render_start_game(const std::filesystem::path& game, const SaveMenu& menu,
                       const StartGameRenderOptions& options,
                       std::vector<std::uint32_t>& rgba, int& width, int& height,
                       StartGameRenderStats* stats, std::string* error) {
    clear(rgba, width, height, stats);
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    const std::vector<SaveSlotView>& slots = menu.slots();
    if (slots.empty()) {
        // Defensive only: `SaveMenu` always exposes the 20-entry table.  A
        // list-backed screen must not paint a frame without rows.
        err = std::string(kPrefix) + "no save slots to draw (the slot table is empty)";
        return false;
    }
    if (game.empty()) {
        err = std::string(kPrefix) + "no game directory (the atlases come from "
                                      "FRONTEND/BMP.IDX)";
        return false;
    }

    // The exact tables are resolved before anything is painted, so a missing or
    // broken dump/table is a refusal and not a silently unfaded frame
    // (docs/START_GAME_RENDER.md §6).
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
    for (const int number : {kStartGameTitleFont, kStartGameRowFont,
                             kStartGameSelectedFont}) {
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

    width = kStartGameCanvasWidth;
    height = kStartGameCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kStartGameBackground);
    if (!options.background.empty() &&
        !paint_background(game, options.background, fade, rgba, width, height, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    StartGameRenderStats counts;
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kStartGamePanelWidth),
                std::min(height, kStartGamePanelHeight)};

    // §5.1: the window of `scroll 0x68ccdc` ∈ 0..13, six visible slots.
    const int count = static_cast<int>(slots.size());
    const int max_first = std::max(0, std::min(kStartGameMaxFirst, count - kStartGameVisible));
    const int first = std::clamp(options.first_slot, 0, max_first);
    const int slot_rows = std::max(0, std::min(kStartGameVisible, count - first));

    // 0x48aa7c..0x48aab3: `MENUARRW` into the plate copy, up when
    // `scroll > 0` (0x48aa83) and down when `scroll + 6 < 20` (0x48aa9f, a
    // literal 0x14 = 20, hence `SaveMenu::kSlots`), at (159, 2) and (159, 121).
    // The two calls come before the title and the rows.
    const bool arrow_up = first > 0;
    const bool arrow_down = first + kStartGameVisible < SaveMenu::kSlots;
    if (arrow_up || arrow_down) {
        BmpImage sprite;
        if (!load_arrow(game, sprite, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
        if (arrow_up) {
            if (!paint_arrow(sprite, true, kStartGameArrowX, kStartGameArrowUpY, fade,
                             layer, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
            ++counts.arrows;
        }
        if (arrow_down) {
            if (!paint_arrow(sprite, false, kStartGameArrowX, kStartGameArrowDownY, fade,
                             layer, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
            ++counts.arrows;
        }
    }

    // 0x48aab6..0x48aaca: `START GAME` (0x4bdc20), font 5, centred at y = 12.
    {
        const FontAtlas* title = fonts(kStartGameTitleFont);
        const int title_x =
            std::max(0, (kStartGamePanelWidth - text_width(*title, kStartGameTitle)) / 2);
        if (!draw_run(fonts, kStartGameTitleFont, kStartGameTitle, title_x,
                      kStartGameTitleY, fade, layer, counts, err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x48aacd..0x48aae1 item 0 (row 23, item 0) and 0x48aae4..0x48aaf3 item 1
    // (row -33, item 1, i.e. the absolute y = 33).
    {
        const std::string_view items[kStartGameItems] = {kStartGameSinglePlayer,
                                                         kStartGameMultiplayer};
        for (int item = 0; item < kStartGameItems; ++item) {
            const int y = kStartGameItemFirstY + kStartGameItemStep * item;
            if (!draw_run(fonts, kStartGameRowFont, items[item], kStartGameItemX, y, fade,
                          layer, counts, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
        }
    }

    // 0x48aafb..0x48ab5d: six slot rows, name `0x6c2fe0 + (scroll + i)*0x13`,
    // x = 39, y = 44 + 11*i, item = i + 2; the highlight is the cursor's font 4
    // (0x485e0d..0x485e19).  Each row first gets its `FILESLOT` bar
    // (0x48ab21 -> 0x48ab90: 124x10 at (35, 43 + 11*i) of the plate copy).
    BmpImage fileslot;
    if (options.fileslot && slot_rows > 0 && !load_fileslot(game, fileslot, err)) {
        clear(rgba, width, height, stats);
        return false;
    }
    const int selected = std::clamp(menu.selected(), 0, count - 1);
    for (int i = 0; i < slot_rows; ++i) {
        if (options.fileslot) {
            if (!paint_fileslot(fileslot, kStartGameFileslotX,
                                kStartGameFileslotFirstY + kStartGameSlotStep * i, fade,
                                layer, err)) {
                clear(rgba, width, height, stats);
                return false;
            }
            ++counts.fileslot_bands;
        }
        const int index = first + i;
        const int font = index == selected ? kStartGameSelectedFont : kStartGameRowFont;
        if (!draw_run(fonts, font, slot_text(menu, index), kStartGameSlotX,
                      kStartGameSlotFirstY + kStartGameSlotStep * i, fade, layer, counts,
                      err)) {
            clear(rgba, width, height, stats);
            return false;
        }
    }

    // 0x48ab5f..0x48ab7a: `ESC TO CANCEL` (0x4bd9d8) at (35, 110), font 0.
    if (!draw_run(fonts, kStartGameRowFont, kStartGameCancel, kStartGameCancelX,
                  kStartGameCancelY, fade, layer, counts, err)) {
        clear(rgba, width, height, stats);
        return false;
    }

    counts.rows = kStartGameItems + slot_rows;
    counts.first_slot = first;
    counts.slot_rows = slot_rows;
    if (stats != nullptr) {
        *stats = counts;
    }
    err.clear();
    return true;
}

} // namespace gm
