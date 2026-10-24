#include "ui/menu_render.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include "render/upscale.h"
#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {
namespace {

/// One palette entry as `0xRRGGBBAA`.  The BMP reserved byte sits in the alpha
/// slot of both `FontAtlas::palette` and `BmpImage::palette`, so alpha is forced
/// to 255 (same as `pager_render`/`bmp_to_rgba`).
std::uint32_t palette_colour(std::span<const std::uint8_t> palette, std::size_t index) {
    const std::size_t base = index * 4u;
    return (static_cast<std::uint32_t>(palette[base + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[base + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[base + 2]) << 8) | 0xFFu;
}

/// Source palette index of one painted pixel, kept only when the exact fade is
/// requested (`MenuRenderOptions::fade_lut`): `0x682f90` is an index -> index
/// table, so the index has to survive until the frame is composed and only then
/// become a colour (same idea as `pager_render.cpp::FadeSource`).  A null
/// `palette` marks a pixel with no index of its own (the black canvas or a
/// hand-built layer without `indices`); the exact path leaves it alone.
struct FadeSource {
    const std::vector<std::uint8_t>* palette = nullptr;
    std::uint8_t index = 0;
};

/// The canvas plus a per-pixel "a glyph wrote here" mask; `ink_pixels` is the
/// number of set bits, so overlapping glyphs are counted once.  `clip_*` is the
/// region `0x485dd0` may write: the 196x131 copy of `PLATE` (§3), so a row
/// below 131 cannot appear on screen even when the canvas has room.  `source`
/// is non-null only on the exact-fade path.
struct Layer {
    std::vector<std::uint32_t>& pixels;
    std::vector<std::uint8_t>& covered;
    int width = 0;
    int height = 0;
    int clip_x0 = 0;
    int clip_y0 = 0;
    int clip_x1 = 0;
    int clip_y1 = 0;
    std::vector<FadeSource>* source = nullptr;
};

/// Width of a label with the atlas advances (`0x47fa50`), used for the centring
/// of the title and of items whose `x` is 0.
int text_width(const FontAtlas& atlas, std::string_view text) {
    int width = 0;
    for (const char raw : text) {
        width += atlas.advance_of(static_cast<unsigned char>(raw));
    }
    return width;
}

/// §2 `step` of a screen (`0x662d58`), from the `step` column of
/// docs/UI_SCREENS.md §2 (0 = the handler writes no immediate).
int style_step(Screen screen) {
    switch (screen) {
    case Screen::Restart: return 16;
    case Screen::ControlOptions: return 9;
    case Screen::SoundOptions: return 10;
    case Screen::GraphicsOptions: return 15;
    case Screen::StartGame: return 10;
    case Screen::SaveGame: return 12;
    case Screen::LevelSelect: return 8;
    case Screen::VideoMode: return 9;
    case Screen::MultiplayerType: return 8;
    case Screen::MouseSetup: return 8;
    case Screen::Difficulty: return 15;
    case Screen::MultiplayerOptions: return 16;
    case Screen::KeySetView: return 8;
    case Screen::PlayOptions: return 15;
    case Screen::Quit: return 16;
    case Screen::KeySetEditorEntry:
    case Screen::KeySetEditor: return 8;
    case Screen::JoystickSetup: return 9;
    case Screen::NewVideoMode: return 16;
    default: return 0;
    }
}

/// Paint one text run at (`x`, `y`); `y` is the top row of the glyph cell.
///
/// `FontGlyph::pixels` row 0 is the *top* cell row: `0x47faf0` writes it at the
/// destination row and goes down (`gm/hud.py::draw_text`, checked against
/// `gm_screen.bin` on 100% of its glyph pixels, docs/HUD_AUDIT.md §2.9).
/// Characters without a glyph are counted in
/// `missing_glyphs` and skipped; `painted` receives the pixels this run covered
/// first (the mask is shared by the whole canvas, so a title over item row 0
/// does not count twice).
bool draw_run(const FontLookup& fonts, int font, std::string_view text, int x, int y,
              Layer& layer, MenuRenderStats& stats, std::string& error, int& painted) {
    painted = 0;
    const FontAtlas* atlas = fonts(font);
    if (atlas == nullptr) {
        error = "menu_render: no atlas for font " + std::to_string(font);
        return false;
    }
    if (!atlas->has_glyphs()) {
        error = "menu_render: font " + std::to_string(font) + " is metrics-only";
        return false;
    }
    const std::size_t palette_entries = atlas->palette.size() / 4u;
    if (palette_entries == 0) {
        error = "menu_render: font " + std::to_string(font) + " has no palette";
        return false;
    }

    int pen = x;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        // A hand-built label cannot paint a control code.
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
                    const std::size_t at =
                        static_cast<std::size_t>(py) * static_cast<std::size_t>(layer.width) +
                        static_cast<std::size_t>(px);
                    layer.pixels[at] = palette_colour(atlas->palette, index);
                    if (layer.source != nullptr) {
                        (*layer.source)[at] = FadeSource{&atlas->palette, index};
                    }
                    if (layer.covered[at] == 0) {
                        layer.covered[at] = 1;
                        ++painted;
                    }
                }
            }
        }
        pen += atlas->advance_of(ch);
    }
    stats.ink_pixels += painted;
    return true;
}

/// One row of the menu before painting.
struct Placement {
    int x = 0;         ///< left offset inside the 196 px band (or centred x)
    int y = 0;         ///< top row of the glyph cell
    bool centred = false; ///< the label is centred in the band
};

/// The 31-row key set lists pass **one** negative base row (-29) for every row,
/// so the literal §3 rule (`y = -row`) would collapse them onto one line; §2/§3.7
/// give `step 8` and 31 rows, so those are stacked from `-row`.  The other
/// screens with negative rows pass a distinct row per item (MultiplayerType,
/// StartGame, ...), which is the documented absolute form.
bool shared_negative_base(const std::vector<Item>& items, int row) {
    int count = 0;
    for (const Item& item : items) {
        if (item.selectable() && item.row == row) {
            ++count;
        }
    }
    return count > 1;
}

Placement place(const Item& item, std::size_t index, int step, bool stack_negative) {
    Placement placement;
    placement.x = item.x;
    if (item.row < 0) {
        // §3 bullet 2 (and `ui_screens.py`): the absolute y is `-row` — for the
        // plain-text rows too (`ESC TO CANCEL` is `-110`, §3.6).
        placement.y = stack_negative ? -item.row + static_cast<int>(index) * step : -item.row;
        return placement;
    }
    if (item.id < 0) {
        // §3 bullet 1: plain text, `row` is the absolute y.
        placement.y = item.row;
        return placement;
    }
    if (step == 0) {
        // MAIN: §2 has no step (the original draws the 7-entry carousel art),
        // so the labels are stacked and centred.  Model, see the header.
        placement.y = kMenuStackedTop + static_cast<int>(index) * kMenuStackedGap;
        placement.centred = true;
        return placement;
    }
    // §3 bullet 3: `y = row + item * step`.
    placement.y = item.row + item.id * step;
    return placement;
}

/// Centre `text` in the 196 px plate band; the label font decides the width.
int centred_x(const FontLookup& fonts, int font, std::string_view text) {
    const FontAtlas* atlas = fonts(font);
    if (atlas == nullptr) {
        return 0;
    }
    const int x = (kMenuPlateWidth - text_width(*atlas, text)) / 2;
    return x < 0 ? 0 : x;
}

void paint_background(const MenuBackground* background, Layer& layer) {
    if (background == nullptr || background->empty()) {
        return;
    }
    const int width = std::min(layer.width, background->width);
    const int height = std::min(layer.height, background->height);
    const bool have_indices =
        background->indices.size() >=
        static_cast<std::size_t>(background->width) * static_cast<std::size_t>(background->height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t source_at =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(background->width) +
                static_cast<std::size_t>(x);
            const std::size_t at =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(layer.width) +
                static_cast<std::size_t>(x);
            layer.pixels[at] = background->rgba[source_at];
            if (layer.source != nullptr && have_indices) {
                (*layer.source)[at] = FadeSource{&background->palette,
                                                 background->indices[source_at]};
            }
        }
    }
}

/// Blit a masked sprite into the plate band at (`x`, `y`).
///
/// Source palette index 0 is transparent: the engine helpers copy a byte only
/// when it is non-zero (`0x486070` for `SLIDE`, `0x4860b0` for `KNOB`,
/// `0x4833e0` for `CHECK`/`NOCHECK`, `0x485cc0` for `MENUARRW`).  `rotated`
/// reads the sprite backwards — bottom row first, each row right to left —
/// which is exactly `0x485d00` (the second arrow of a list is `MENUARRW`
/// rotated 180 degrees, not a second asset).
///
/// Returns the pixels this sprite covered first: the mask is shared with the
/// glyphs, so an overlapping widget does not count twice.
int blit_sprite(const MenuSprite& sprite, int x, int y, Layer& layer, bool rotated) {
    int painted = 0;
    const bool have_indices =
        sprite.indices.size() >=
        static_cast<std::size_t>(sprite.width) * static_cast<std::size_t>(sprite.height);
    for (int row = 0; row < sprite.height; ++row) {
        const int py = y + row;
        if (py < layer.clip_y0 || py >= layer.clip_y1) {
            continue;
        }
        const int source_row = rotated ? sprite.height - 1 - row : row;
        for (int column = 0; column < sprite.width; ++column) {
            const int source_column = rotated ? sprite.width - 1 - column : column;
            const std::size_t source_at =
                static_cast<std::size_t>(source_row) * static_cast<std::size_t>(sprite.width) +
                static_cast<std::size_t>(source_column);
            const std::uint32_t colour = sprite.rgba[source_at];
            if (colour == 0) {
                continue;
            }
            const int px = x + column;
            if (px < layer.clip_x0 || px >= layer.clip_x1) {
                continue;
            }
            const std::size_t at = static_cast<std::size_t>(py) *
                                       static_cast<std::size_t>(layer.width) +
                                   static_cast<std::size_t>(px);
            layer.pixels[at] = colour;
            if (layer.source != nullptr && have_indices) {
                (*layer.source)[at] =
                    FadeSource{&sprite.palette, sprite.indices[source_at]};
            }
            if (layer.covered[at] == 0) {
                layer.covered[at] = 1;
                ++painted;
            }
        }
    }
    return painted;
}

/// Left column of the knob: the §3.5 model
/// `x + value*(SLIDE.width-KNOB.width)/(max-min)`, with `value` clamped to
/// `[min,max]` and a degenerate range (max == min) pinned to the left end.
int slider_knob_x(const MenuWidget& widget, int track_width, int knob_width) {
    const int low = std::min(widget.min, widget.max);
    const int high = std::max(widget.min, widget.max);
    const int travel = track_width - knob_width;
    if (travel <= 0 || high == low) {
        return widget.x;
    }
    const int value = std::clamp(widget.value, low, high);
    // 64-bit arithmetic: a hand-built [min,max] can span the whole int range.
    const long long span = static_cast<long long>(high) - static_cast<long long>(low);
    const long long offset = (static_cast<long long>(value) - static_cast<long long>(low)) *
                             static_cast<long long>(travel) / span;
    return widget.x + static_cast<int>(offset);
}

/// Paint every widget after the text: the track first, then the knob / box /
/// arrow on top.  Each widget lands in the same 196x131 band as the glyphs.
bool draw_widgets(const std::vector<MenuWidget>& widgets, const MenuWidgetAssets* assets,
                  Layer& layer, MenuRenderStats& stats, std::string& error) {
    stats.widgets = static_cast<int>(widgets.size());
    if (widgets.empty()) {
        return true;
    }
    if (assets == nullptr || !assets->complete()) {
        error = "menu_render: widgets need the SLIDE/KNOB/CHECK/NOCHECK/MENUARRW sprites "
                "(pass MenuWidgetAssets)";
        return false;
    }
    for (const MenuWidget& widget : widgets) {
        int painted = 0;
        switch (widget.kind) {
        case MenuWidget::Slider:
            painted += blit_sprite(assets->slide, widget.x, widget.y, layer, false);
            painted += blit_sprite(
                assets->knob,
                slider_knob_x(widget, assets->slide.width, assets->knob.width), widget.y,
                layer, false);
            break;
        case MenuWidget::Checkbox:
            if (widget.checked) {
                painted += blit_sprite(assets->check, widget.x, widget.y, layer, false);
            } else {
                // 0x4833e0: the 8x8 NOCHECK is drawn 8 rows below the anchor.
                painted += blit_sprite(assets->nocheck, widget.x,
                                       widget.y + kMenuNoCheckDrop, layer, false);
            }
            break;
        case MenuWidget::Arrow:
            painted += blit_sprite(assets->arrow, widget.x, widget.y, layer, widget.rotated);
            break;
        }
        if (painted > 0) {
            ++stats.drawn_widgets;
        }
        stats.widget_ink += painted;
    }
    stats.ink_pixels += stats.widget_ink;
    return true;
}

/// The exact fade of `0x682f90`: index -> index through the dumped table, then
/// the palette of the pixel's own source (panel or sprite BMP, glyph atlas).
/// A mapped index outside that palette is refused with text — the dump writes
/// 0..255, so it can only happen with a short palette.  This is the same rule as
/// `pager_render.cpp`; the helper is file-local there, so the ~15 lines are
/// repeated rather than exported from a module this slice may not touch.
bool apply_fade_lut(std::vector<std::uint32_t>& rgba,
                    const std::vector<FadeSource>& source, const HudLut& lut,
                    int level, std::string& error) {
    for (std::size_t at = 0; at < rgba.size(); ++at) {
        const FadeSource& pixel = source[at];
        if (pixel.palette == nullptr) {
            // No palette index of its own: the black canvas (or a hand-built
            // layer without `indices`) has nothing to map and stays as it is.
            continue;
        }
        const std::uint8_t mapped = map(lut, level, pixel.index);
        if (static_cast<std::size_t>(mapped) * 4u >= pixel.palette->size()) {
            error = "menu_render: fade LUT level " + std::to_string(level) +
                    " maps index " + std::to_string(pixel.index) + " to " +
                    std::to_string(mapped) + ", outside the " +
                    std::to_string(pixel.palette->size() / 4u) + "-entry palette";
            return false;
        }
        rgba[at] = palette_colour(*pixel.palette, mapped);
    }
    return true;
}

bool draw_screen(const FrontendMenu& menu, const FontLookup& fonts,
                 const MenuBackground* background, const MenuWidgetAssets* assets,
                 const std::vector<MenuWidget>& widgets, const MenuRenderOptions& options,
                 std::vector<std::uint32_t>& rgba, int width, int height,
                 MenuRenderStats& stats, std::string& error) {
    if (!fonts) {
        error = "menu_render: no font source (pass a FontLookup)";
        return false;
    }

    // The exact tables are resolved before anything is painted, so a missing or
    // broken dump is an error, not a silently unfaded frame (the same contract
    // as `pager_render`).  `fade_lut` wins over the model when both are set.
    HudLut owned_lut;
    const HudLut* lut = nullptr;
    std::vector<FadeSource> source;
    if (options.fade_lut) {
        if (options.fade_lut_table != nullptr) {
            lut = options.fade_lut_table;
        } else if (!options.fade_lut_dump.empty()) {
            std::string lut_error;
            if (!load_hud_lut(options.fade_lut_dump, owned_lut, &lut_error)) {
                error = "menu_render: " + lut_error;
                return false;
            }
            lut = &owned_lut;
        } else {
            error = "menu_render: fade LUT needs a table (fade_lut_table) or a "
                    "dump (fade_lut_dump)";
            return false;
        }
        source.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                      FadeSource{});
    }

    std::vector<std::uint8_t> covered(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height),
                                      0);
    Layer layer{rgba,
                covered,
                width,
                height,
                0,
                0,
                std::min(width, kMenuPlateWidth),
                std::min(height, kMenuPlateHeight),
                options.fade_lut ? &source : nullptr};
    paint_background(background, layer);

    const Screen screen = menu.screen();
    const std::vector<Item>& items = menu.items();
    stats.items = static_cast<int>(items.size());
    const int step = menu_row_step(screen);
    const int selected = menu.selected_id();

    if (menu_has_title(screen)) {
        const std::string_view title = FrontendMenu::screen_name(screen);
        int painted = 0;
        if (!draw_run(fonts, kMenuTitleFont, title,
                      centred_x(fonts, kMenuTitleFont, title), kMenuTitleY, layer, stats,
                      error, painted)) {
            return false;
        }
    }

    for (std::size_t index = 0; index < items.size(); ++index) {
        const Item& item = items[index];
        const bool highlighted = item.selectable() && item.id == selected;
        const int font = highlighted ? kMenuSelectedFont : kMenuItemFont;
        const bool stack_negative =
            item.row < 0 && shared_negative_base(items, item.row);
        const Placement placement = place(item, index, step, stack_negative);

        int x = placement.x;
        if (placement.centred || item.x == 0) {
            x = centred_x(fonts, font, item.label);
        }
        int painted = 0;
        if (!draw_run(fonts, font, item.label, x, placement.y, layer, stats, error, painted)) {
            return false;
        }
        if (painted > 0) {
            ++stats.drawn_items;
        }
    }

    if (!draw_widgets(widgets, assets, layer, stats, error)) {
        return false;
    }

    stats.background_pixels = width * height - stats.ink_pixels;

    if (options.fade_lut) {
        // Exact path: the table is index -> index, so it runs on the composed
        // frame before each source palette is applied (the engine blits the
        // whole 8-bpp buffer through `LUT[k]`).
        if (!apply_fade_lut(rgba, source, *lut, options.fade_lut_level, error)) {
            return false;
        }
    } else if (options.fade) {
        // Model: the same curve the CLI applies today (`render/upscale.h`).
        // It cannot fail with `max_level = 11`, but the text is passed on.
        std::string fade_error;
        if (!fade_rgba(rgba, options.fade_level, kHudLutIdentityLevel, &fade_error)) {
            error = "menu_render: " + fade_error;
            return false;
        }
    }
    return true;
}

void reset_canvas(std::vector<std::uint32_t>& rgba, int& width, int& height) {
    width = kMenuCanvasWidth;
    height = kMenuCanvasHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                kMenuBackground);
}

} // namespace

int menu_row_step(Screen screen) { return style_step(screen); }

std::string_view menu_panel_asset(Screen screen) {
    switch (screen) {
    // §2 asset table / `ui_screens.py --json`: the shared 196x131 PLATE.
    case Screen::Error:
    case Screen::Restart:
    case Screen::ControlOptions:
    case Screen::SoundOptions:
    case Screen::GraphicsOptions:
    case Screen::StartGame:
    case Screen::SaveGame:
    case Screen::SaveNameEntry:
    case Screen::KeyJamming:
    case Screen::LevelSelect:
    case Screen::VideoMode:
    case Screen::MultiplayerType:
    case Screen::Shareware:
    case Screen::LocalPlayers:
    case Screen::MouseSetup:
    case Screen::Difficulty:
    case Screen::PlayerOneName:
    case Screen::PlayerTwoName:
    case Screen::JoystickCentering:
    case Screen::MultiplayerOptions:
    case Screen::KeySetView:
    case Screen::NewKeySetName:
    case Screen::PlayOptions:
    case Screen::Quit:
    case Screen::KeySetEditorEntry:
    case Screen::KeySetEditor:
    case Screen::JoystickSetup:
    case Screen::NewVideoMode:
        return kMenuPlateName;
    // No full-screen menu art: INTRO/SML PAGER/briefing transitions are other
    // renderers, the in-game menu uses DEMOTION/DISBAND, MAIN uses SCROLL
    // (404x200 carousel background) + entry art.
    case Screen::None:
    case Screen::Exit:
    case Screen::Intro:
    case Screen::Main:
    case Screen::SmlPager:
    case Screen::BriefingTransitionA:
    case Screen::BriefingTransitionB:
    case Screen::InGameMenu:
        return {};
    }
    return {};
}

bool menu_has_title(Screen screen) {
    switch (screen) {
    // §2 `title VA` is present for these screens.
    case Screen::Error:
    case Screen::Restart:
    case Screen::ControlOptions:
    case Screen::SoundOptions:
    case Screen::GraphicsOptions:
    case Screen::StartGame:
    case Screen::SaveGame:
    case Screen::KeyJamming:
    case Screen::LevelSelect:
    case Screen::VideoMode:
    case Screen::MultiplayerType:
    case Screen::Shareware:
    case Screen::LocalPlayers:
    case Screen::MouseSetup:
    case Screen::Difficulty:
    case Screen::PlayerOneName:
    case Screen::PlayerTwoName:
    case Screen::JoystickCentering:
    case Screen::MultiplayerOptions:
    case Screen::NewKeySetName:
    case Screen::PlayOptions:
    case Screen::Quit:
    case Screen::JoystickSetup:
    case Screen::NewVideoMode:
        return true;
    // 30/34/35: the title is the key set name (`0x6c2058 + set*0x108`), which
    // the snapshot does not hold; MAIN uses the F_M_TITL art; 9/15 have none.
    default:
        return false;
    }
}

/// Case-insensitive lookup of one `FRONTEND/BMP.IDX` entry and its parsed BMP.
/// Every caller is read-only; the index is tiny, so it is reopened per asset.
namespace {

std::optional<BmpImage> read_bmp_entry(const std::filesystem::path& game,
                                       std::string_view name, std::string& error) {
    if (game.empty()) {
        error = "menu_render: no game directory for FRONTEND/BMP.IDX";
        return std::nullopt;
    }
    const std::filesystem::path idx = game / "FRONTEND" / "BMP.IDX";
    std::optional<BigFile> archive = BigFile::open(idx, error);
    if (!archive) {
        return std::nullopt;
    }
    const BigEntry* entry = nullptr;
    for (const BigEntry& candidate : archive->entries()) {
        if (candidate.name.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto lower = [](char c) {
                return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
            };
            if (lower(candidate.name[i]) != lower(name[i])) {
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
        error = "menu_render: " + std::string(name) + " is not in " + idx.string();
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> bytes = archive->read(*entry, error);
    if (!bytes) {
        return std::nullopt;
    }
    std::optional<BmpImage> image = parse_bmp(*bytes, error);
    if (!image) {
        return std::nullopt;
    }
    if (image->bpp != 8) {
        error = "menu_render: " + std::string(name) + " is not 8 bpp";
        return std::nullopt;
    }
    if (image->palette.size() / 4u == 0) {
        error = "menu_render: " + std::string(name) + " has no palette";
        return std::nullopt;
    }
    return image;
}

/// Palette-index 0 is the transparent key of the control sprites
/// (`0x4833e0`/`0x486070`/`0x485cc0` copy a byte only when it is non-zero), so
/// it becomes 0x00000000 instead of a palette colour.
std::optional<MenuSprite> load_sprite(const std::filesystem::path& game,
                                      std::string_view name, int want_width, int want_height,
                                      std::string& error) {
    std::optional<BmpImage> image = read_bmp_entry(game, name, error);
    if (!image) {
        return std::nullopt;
    }
    if (image->width != want_width || image->height != want_height) {
        error = "menu_render: " + std::string(name) + " is " +
                std::to_string(image->width) + "x" + std::to_string(image->height) +
                ", expected " + std::to_string(want_width) + "x" +
                std::to_string(want_height);
        return std::nullopt;
    }
    const std::size_t entries = image->palette.size() / 4u;
    MenuSprite sprite;
    sprite.name = std::string(name);
    sprite.width = image->width;
    sprite.height = image->height;
    sprite.rgba.assign(static_cast<std::size_t>(image->width) *
                           static_cast<std::size_t>(image->height),
                       0u);
    sprite.palette = image->palette;
    sprite.indices = image->pixels;
    for (std::size_t i = 0; i < sprite.rgba.size(); ++i) {
        const std::uint8_t value = image->pixels[i];
        if (value >= entries) {
            error = "menu_render: " + std::string(name) + " uses palette index " +
                    std::to_string(value) + " of " + std::to_string(entries);
            return std::nullopt;
        }
        sprite.rgba[i] = value == 0 ? 0u : palette_colour(image->palette, value);
    }
    return sprite;
}

} // namespace

std::optional<MenuBackground> load_menu_panel(const std::filesystem::path& game,
                                              Screen screen, std::string& error) {
    error.clear();
    const std::string_view name = menu_panel_asset(screen);
    if (name.empty()) {
        error = "menu_render: screen " + std::to_string(FrontendMenu::screen_id(screen)) +
                " has no panel asset";
        return std::nullopt;
    }
    std::optional<BmpImage> image = read_bmp_entry(game, name, error);
    if (!image) {
        return std::nullopt;
    }
    const std::size_t entries = image->palette.size() / 4u;

    MenuBackground panel;
    panel.name = std::string(name);
    panel.width = image->width;
    panel.height = image->height;
    panel.rgba.assign(static_cast<std::size_t>(image->width) *
                          static_cast<std::size_t>(image->height),
                      kMenuBackground);
    panel.palette = image->palette;
    panel.indices = image->pixels;
    for (std::size_t i = 0; i < panel.rgba.size(); ++i) {
        const std::uint8_t value = image->pixels[i];
        if (value >= entries) {
            error = "menu_render: " + std::string(name) + " uses palette index " +
                    std::to_string(value) + " of " + std::to_string(entries);
            return std::nullopt;
        }
        panel.rgba[i] = palette_colour(image->palette, value);
    }
    return panel;
}

std::optional<MenuWidgetAssets> load_menu_widget_assets(const std::filesystem::path& game,
                                                        std::string& error) {
    error.clear();
    MenuWidgetAssets assets;
    std::optional<MenuSprite> slide =
        load_sprite(game, "SLIDE", kMenuSlideWidth, kMenuSlideHeight, error);
    if (!slide) {
        return std::nullopt;
    }
    assets.slide = std::move(*slide);
    std::optional<MenuSprite> knob =
        load_sprite(game, "KNOB", kMenuKnobWidth, kMenuKnobHeight, error);
    if (!knob) {
        return std::nullopt;
    }
    assets.knob = std::move(*knob);
    std::optional<MenuSprite> check =
        load_sprite(game, "CHECK", kMenuCheckWidth, kMenuCheckHeight, error);
    if (!check) {
        return std::nullopt;
    }
    assets.check = std::move(*check);
    std::optional<MenuSprite> nocheck =
        load_sprite(game, "NOCHECK", kMenuNoCheckWidth, kMenuNoCheckHeight, error);
    if (!nocheck) {
        return std::nullopt;
    }
    assets.nocheck = std::move(*nocheck);
    std::optional<MenuSprite> arrow =
        load_sprite(game, "MENUARRW", kMenuArrowWidth, kMenuArrowHeight, error);
    if (!arrow) {
        return std::nullopt;
    }
    assets.arrow = std::move(*arrow);
    return assets;
}

std::vector<MenuWidget> menu_widgets(Screen screen) {
    std::vector<MenuWidget> widgets;
    switch (screen) {
    case Screen::SoundOptions:
        // Tracks 0x688290 = (68,37) and 0x6891e0 = (68,57), pushed at
        // 0x488a45 / 0x488a93: a track sits 10 px below its label (rows 27/47).
        // Volume range 0..20: the right-key clamp is `cmp eax,0x14` at
        // 0x48c711 (music) / 0x48c6da (effects); the knob anchors are
        // 0x688291/0x6891e1 (x = track + 1).
        widgets.emplace_back(MenuWidget::Slider, 68, 37, 0, 0, 20);
        widgets.emplace_back(MenuWidget::Slider, 68, 57, 0, 0, 20);
        // 0x488af2: `Switch Left Right`, checkbox anchor 0x689338 = (20,59);
        // item 4 has row 27 + 4*10 = 67, and the 8x16 CHECK is top-anchored
        // 8 rows above the label row.
        widgets.emplace_back(MenuWidget::Checkbox, 20, 59);
        break;
    case Screen::GraphicsOptions:
        // 0x485f67 pushes the Border track 0x68842c = (88,39) = the item 1
        // label row (24 + 15); clamp 0x48c773 (`cmp eax,0x9`), knob at
        // 0x68842d (x+1).
        widgets.emplace_back(MenuWidget::Slider, 88, 39, 0, 0, 9);
        // 0x485fd4: Reflections checkbox 0x6894d4 = (40,61); item 3 label row
        // 24 + 3*15 = 69, anchor = row - 8.
        widgets.emplace_back(MenuWidget::Checkbox, 40, 61);
        break;
    case Screen::PlayOptions:
        // 0x4861b5, 0x4861eb, 0x486221, 0x48626c, 0x4862b9, 0x486306: six
        // checkboxes, x = 16, anchor y = label row - 8.  The label rows are
        // 21, 32, 43, 60, 78, 95 (rows 21/17/13+15*item, §3.4), so the anchors
        // are 13, 24, 35, 52, 70, 87.
        for (const int y : {13, 24, 35, 52, 70, 87}) {
            widgets.emplace_back(MenuWidget::Checkbox, 16, y);
        }
        break;
    default:
        // CONTROL OPTIONS (10) is text only (§3.1); MOUSE SETUP (22) has three
        // proven sliders (105,40/56/72, 0x489ac9/0x489b4d/0x489bd1) but its
        // knob is anchored at x+3 and moves right-to-left (0x489af2 `sub`),
        // which the left-to-right model below does not express yet.
        break;
    }
    return widgets;
}

bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, fonts, nullptr, nullptr, {}, MenuRenderOptions{}, rgba, width,
                         height, stats, error);
}

bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuRenderOptions& options, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, fonts, nullptr, nullptr, {}, options, rgba, width, height,
                         stats, error);
}

bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, fonts, background, nullptr, {}, MenuRenderOptions{}, rgba,
                         width, height, stats, error);
}

bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background, const MenuRenderOptions& options,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, fonts, background, nullptr, {}, options, rgba, width, height,
                         stats, error);
}

bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuWidgetAssets* assets, const std::vector<MenuWidget>& widgets,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, fonts, nullptr, assets, widgets, MenuRenderOptions{}, rgba,
                         width, height, stats, error);
}

bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background, const MenuWidgetAssets* assets,
                   const std::vector<MenuWidget>& widgets, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, fonts, background, assets, widgets, MenuRenderOptions{},
                         rgba, width, height, stats, error);
}

bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background, const MenuWidgetAssets* assets,
                   const std::vector<MenuWidget>& widgets, const MenuRenderOptions& options,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats, std::string* error) {
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    MenuRenderStats counts;
    reset_canvas(rgba, width, height);

    const Screen screen = menu.screen();
    if (screen == Screen::None) {
        err = "menu_render: no screen (FrontendMenu::reset was not called)";
        if (stats != nullptr) {
            *stats = counts;
        }
        return false;
    }
    if (screen == Screen::Exit) {
        err = "menu_render: Screen::Exit has no picture (the front-end has left)";
        if (stats != nullptr) {
            *stats = counts;
        }
        return false;
    }

    if (!draw_screen(menu, fonts, background, assets, widgets, options, rgba, width, height,
                     counts, err)) {
        // A named failure leaves the buffer as background, never a half-drawn
        // screen the caller could mistake for a result.
        reset_canvas(rgba, width, height);
        if (stats != nullptr) {
            *stats = MenuRenderStats{};
        }
        return false;
    }
    if (stats != nullptr) {
        *stats = counts;
    }
    return true;
}

bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, game, {}, MenuRenderOptions{}, rgba, width, height, stats,
                         error);
}

bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   const MenuRenderOptions& options, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, game, {}, options, rgba, width, height, stats, error);
}

bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   const std::vector<MenuWidget>& widgets, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats, std::string* error) {
    return render_screen(menu, game, widgets, MenuRenderOptions{}, rgba, width, height,
                         stats, error);
}

bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   const std::vector<MenuWidget>& widgets, const MenuRenderOptions& options,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats, std::string* error) {
    std::string local_error;
    std::string& err = error != nullptr ? *error : local_error;
    err.clear();

    // `None`/`Exit` fail before any file is read (and reset the buffer), so both
    // overloads report the same text.
    const Screen screen = menu.screen();
    if (screen == Screen::None || screen == Screen::Exit) {
        return render_screen(menu, FontLookup{}, nullptr, nullptr, widgets, options, rgba,
                             width, height, stats, &err);
    }
    if (game.empty()) {
        reset_canvas(rgba, width, height);
        err = "menu_render: no game directory for the font atlases and the panel";
        if (stats != nullptr) {
            *stats = MenuRenderStats{};
        }
        return false;
    }

    // §3: items use font 0, the highlight 4, the title 5 (three FONTS4 faces).
    std::vector<FontAtlas> owned;
    owned.reserve(3);
    for (const int number : {kMenuItemFont, kMenuSelectedFont, kMenuTitleFont}) {
        std::string load_error;
        std::optional<FontAtlas> atlas = load_font(game, number, load_error);
        if (!atlas) {
            reset_canvas(rgba, width, height);
            err = "menu_render: font " + std::to_string(number) + ": " + load_error;
            if (stats != nullptr) {
                *stats = MenuRenderStats{};
            }
            return false;
        }
        owned.push_back(std::move(*atlas));
    }
    const FontLookup lookup = [&owned](int number) -> const FontAtlas* {
        for (const FontAtlas& atlas : owned) {
            if (atlas.number == number) {
                return &atlas;
            }
        }
        return nullptr;
    };

    std::optional<MenuBackground> panel;
    if (!menu_panel_asset(screen).empty()) {
        std::string load_error;
        panel = load_menu_panel(game, screen, load_error);
        if (!panel) {
            reset_canvas(rgba, width, height);
            err = "menu_render: panel " + std::string(menu_panel_asset(screen)) + ": " +
                  load_error;
            if (stats != nullptr) {
                *stats = MenuRenderStats{};
            }
            return false;
        }
    }

    // `SLIDE`/`KNOB`/`CHECK`/`NOCHECK`/`MENUARRW` are only read when the caller
    // asks for controls, so the text-only path keeps working on installs whose
    // FRONTEND archive is incomplete.
    std::optional<MenuWidgetAssets> assets;
    if (!widgets.empty()) {
        std::string load_error;
        assets = load_menu_widget_assets(game, load_error);
        if (!assets) {
            reset_canvas(rgba, width, height);
            err = "menu_render: control sprites: " + load_error;
            if (stats != nullptr) {
                *stats = MenuRenderStats{};
            }
            return false;
        }
    }
    return render_screen(menu, lookup, panel ? &*panel : nullptr,
                         assets ? &*assets : nullptr, widgets, options, rgba, width, height,
                         stats, &err);
}

} // namespace gm
