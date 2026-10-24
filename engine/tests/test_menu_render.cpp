// Screen rendering of the front-end menu (`M8.2` slice 2, docs/MENU_RENDER.md).
//
// `FrontendMenu` knows the screen, its items and the selection; this module
// paints them into the 320x200 RGBA canvas of the front end (`0x6d8fc0`) using
// the §3 fonts (title 5, item 0, highlight 4 — three FONTS4 faces).  The tests
// pin:
//
//   * screens 1 (MAIN), 10 (CONTROL OPTIONS) and 33 (QUIT GAME?):
//     `items == items().size()`, `drawn_items == items`, `ink_pixels > 0`,
//     `background_pixels > 0`, `ink + background == 320*200`,
//     `missing_glyphs == 0`; on the black background the independent
//     "non-black pixel" count equals `ink_pixels`;
//   * determinism: two renders of the same state are byte-identical;
//   * the highlight: `feed(Down)` changes the picture (differing pixels > 0)
//     while the item count stays;
//   * the panel: `PLATE` (196x131) is the §2 asset of screen 10 and is painted
//     under the text, while MAIN has no panel asset; the game-directory
//     overload paints exactly the same bytes as the caller-supplied atlases
//     and the loaded panel;
//   * negatives: `Screen::None` and `Screen::Exit` are refused with a text, an
//     empty game directory and a missing font provider are refused with a text,
//     and the buffer is reset to 320x200 background on every failure.
//
// The installation is only read and the font/panel runs skip cleanly when it is
// absent (the negatives always run).
//
// Slice 3 adds the options controls (`docs/MENU_RENDER.md` §3.5): the four
// masked sprites `SLIDE` 72x8 / `KNOB` 8x8 / `CHECK` 8x16 / `NOCHECK` 8x8 and
// `MENUARRW` 16x8, wrapped as `MenuWidget`.  Pinned here: the sprite sizes from
// the BMP headers, the proven layouts of SOUND/GRAPHICS/PLAY OPTIONS, a
// synthetic CONTROL OPTIONS (8 sliders + 1 checkbox), ink growth vs the same
// screen without widgets, `drawn_widgets == widgets`, min/mid/max and
// checked/unchecked frames differing, determinism, clamping (out-of-range and
// max == min), the 196x131 clip and the "widgets without sprites" negative.
//
// Slice 8 adds the fade (`docs/MENU_RENDER.md` §7): `MenuRenderOptions` with the
// model (`fade`) and the exact tables of `0x682f90` (`fade_lut`).  Pinned here
// on CONTROL OPTIONS (PLATE, opaque 196x131 panel): level 11 is byte-identical
// to the unfaded frame, level 0 is mostly black and differs from the model,
// 12..15 are four different frames (the model clamps them to 11), the table is
// built in memory from `GAMEPAL` (no dump file), a `fade_lut` call without a
// table and a dump is refused with text, and the model path still produces the
// exact bytes of `render/upscale.h::fade_rgba` on the unfaded frame (what the
// CLI does today).  All numbers are printed.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "render/upscale.h"
#include "res/bmp.h"
#include "ui/frontend_menu.h"
#include "ui/menu_render.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(long long got, long long want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

gm::FontLookup make_lookup(const std::map<int, gm::FontAtlas>& fonts) {
    return [&fonts](int number) -> const gm::FontAtlas* {
        const auto it = fonts.find(number);
        return it == fonts.end() ? nullptr : &it->second;
    };
}

int count_non_background(const std::vector<std::uint32_t>& pixels) {
    int ink = 0;
    for (const std::uint32_t value : pixels) {
        if (value != gm::kMenuBackground) {
            ++ink;
        }
    }
    return ink;
}

int count_diff(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    int diff = 0;
    const std::size_t size = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) {
            ++diff;
        }
    }
    return diff;
}

struct Render {
    std::vector<std::uint32_t> pixels;
    gm::MenuRenderStats stats;
    int width = 0;
    int height = 0;
    std::string error;
    bool ok = false;
};

Render render(const gm::FrontendMenu& menu, const gm::FontLookup& fonts,
              const gm::MenuBackground* panel = nullptr) {
    Render out;
    out.ok = gm::render_screen(menu, fonts, panel, out.pixels, out.width, out.height,
                               &out.stats, &out.error);
    return out;
}

/// The same with the control sprites and widgets; `assets` may be nullptr (the
/// "widgets without sprites" negative).
Render render_widgets(const gm::FrontendMenu& menu, const gm::FontLookup& fonts,
                      const gm::MenuWidgetAssets* assets,
                      const std::vector<gm::MenuWidget>& widgets,
                      const gm::MenuBackground* panel = nullptr) {
    Render out;
    out.ok = gm::render_screen(menu, fonts, panel, assets, widgets, out.pixels, out.width,
                               out.height, &out.stats, &out.error);
    return out;
}

void check_canvas(const Render& result, const std::string& tag) {
    check_eq(result.width, gm::kMenuCanvasWidth, tag + "canvas width");
    check_eq(result.height, gm::kMenuCanvasHeight, tag + "canvas height");
    check_eq(static_cast<long long>(result.pixels.size()),
             static_cast<long long>(gm::kMenuCanvasWidth) * gm::kMenuCanvasHeight,
             tag + "buffer is 320x200");
    check_eq(result.stats.ink_pixels + result.stats.background_pixels,
             static_cast<long long>(gm::kMenuCanvasWidth) * gm::kMenuCanvasHeight,
             tag + "ink + background == w*h");
}

/// One screen of the slice: numbers, determinism and the highlight.
void test_screen(gm::FrontendMenu menu, const std::string& name,
                 const gm::FontLookup& fonts, bool expect_all_drawn) {
    const std::string tag = name + ": ";
    const std::size_t item_count = menu.items().size();
    check(item_count > 0, tag + "screen has items");

    const Render first = render(menu, fonts);
    check(first.ok, tag + "render true");
    check(first.error.empty(), tag + "no error");
    check_canvas(first, tag);

    check_eq(first.stats.items, static_cast<long long>(item_count), tag + "items");
    if (expect_all_drawn) {
        check_eq(first.stats.drawn_items, static_cast<long long>(item_count),
                 tag + "drawn_items == items");
    }
    check(first.stats.drawn_items > 0, tag + "drawn_items > 0");
    check(first.stats.ink_pixels > 0, tag + "ink > 0");
    check(first.stats.background_pixels > 0, tag + "background > 0");
    check(first.stats.missing_glyphs == 0, tag + "missing_glyphs == 0");
    check_eq(count_non_background(first.pixels), first.stats.ink_pixels,
             tag + "independent non-black count == ink");

    // Determinism: the same screen twice, byte for byte.
    const Render again = render(menu, fonts);
    check(again.ok, tag + "second render");
    check(first.pixels == again.pixels, tag + "two renders are byte-identical");
    check_eq(again.stats.ink_pixels, first.stats.ink_pixels, tag + "second ink");
    check_eq(again.stats.drawn_items, first.stats.drawn_items, tag + "second drawn");

    // The highlight: Down moves the selection, the picture must change.
    const bool moved = menu.feed(gm::KeyEvent{gm::KeyEvent::Down, 0});
    check(moved, tag + "feed(Down) moves the selection");
    const Render shifted = render(menu, fonts);
    check(shifted.ok, tag + "render after Down");
    const int diff = count_diff(first.pixels, shifted.pixels);
    check(diff > 0, tag + "highlight changes the picture");
    check_eq(shifted.stats.items, first.stats.items, tag + "items after Down");
    check_eq(shifted.stats.drawn_items, first.stats.drawn_items,
             tag + "drawn after Down");

    std::cout << "  " << name << " (id "
              << gm::FrontendMenu::screen_id(menu.screen()) << "): items="
              << first.stats.items << " drawn=" << first.stats.drawn_items
              << " ink=" << first.stats.ink_pixels
              << " background=" << first.stats.background_pixels
              << " missing=" << first.stats.missing_glyphs
              << " highlight_diff=" << diff << "\n";
}

/// §2/§9: `PLATE` is the panel of the menu screens, MAIN has none.
void test_panel(const fs::path& game, const gm::FontLookup& fonts) {
    check(gm::menu_panel_asset(gm::Screen::ControlOptions) == gm::kMenuPlateName,
          "panel: CONTROL OPTIONS uses PLATE");
    check(gm::menu_panel_asset(gm::Screen::Quit) == gm::kMenuPlateName,
          "panel: QUIT uses PLATE");
    check(gm::menu_panel_asset(gm::Screen::Main).empty(), "panel: MAIN has no panel");
    check(gm::menu_panel_asset(gm::Screen::SmlPager).empty(), "panel: pager has no panel");

    std::string error;
    std::optional<gm::MenuBackground> panel =
        gm::load_menu_panel(game, gm::Screen::ControlOptions, error);
    check(panel.has_value(), "panel: PLATE loads (" + error + ")");
    if (!panel) {
        return;
    }
    check_eq(panel->width, gm::kMenuPlateWidth, "panel: width 196");
    check_eq(panel->height, gm::kMenuPlateHeight, "panel: height 131");
    check_eq(static_cast<long long>(panel->rgba.size()),
             static_cast<long long>(panel->width) * panel->height, "panel: buffer size");
    check(panel->name == "PLATE" || panel->name == "plate", "panel: entry name");

    gm::FrontendMenu menu;
    menu.reset();
    check(menu.enter(gm::Screen::ControlOptions), "panel: enter CONTROL OPTIONS");
    const Render black = render(menu, fonts);
    const Render with_panel = render(menu, fonts, &*panel);
    check(black.ok && with_panel.ok, "panel: both renders succeed");
    if (!black.ok || !with_panel.ok) {
        return;
    }
    check_canvas(with_panel, "panel: ");
    check_eq(with_panel.stats.items, black.stats.items, "panel: items");
    check_eq(with_panel.stats.drawn_items, black.stats.drawn_items, "panel: drawn");
    check_eq(with_panel.stats.ink_pixels, black.stats.ink_pixels, "panel: ink");
    check_eq(with_panel.stats.missing_glyphs, black.stats.missing_glyphs,
             "panel: missing");
    // (0,0) of the plate is above the first item row (27) -> the panel pixel
    // must survive, and the two pictures must differ.
    check_eq(with_panel.pixels[0], panel->rgba[0], "panel: (0,0) is the plate pixel");
    const int diff = count_diff(black.pixels, with_panel.pixels);
    check(diff > 0, "panel: painted under the text");

    // The game-directory overload (fonts + panel from BMP.IDX) paints the same
    // bytes as the caller-supplied atlases and the loaded panel.
    Render loaded;
    loaded.ok = gm::render_screen(menu, game, loaded.pixels, loaded.width, loaded.height,
                                  &loaded.stats, &loaded.error);
    check(loaded.ok, "panel: game overload renders (" + loaded.error + ")");
    check(loaded.pixels == with_panel.pixels, "panel: game overload paints the same bytes");
    check_eq(loaded.stats.ink_pixels, with_panel.stats.ink_pixels,
             "panel: game overload ink");

    // MAIN has no panel asset: the loader refuses it by text and the game
    // overload still renders (on black).
    std::string main_error;
    check(!gm::load_menu_panel(game, gm::Screen::Main, main_error),
          "panel: MAIN panel refused");
    check(main_error.find("no panel asset") != std::string::npos,
          "panel: MAIN refusal names the asset");
    gm::FrontendMenu main_menu;
    main_menu.reset();
    Render main_loaded;
    main_loaded.ok = gm::render_screen(main_menu, game, main_loaded.pixels, main_loaded.width,
                                       main_loaded.height, &main_loaded.stats,
                                       &main_loaded.error);
    check(main_loaded.ok, "panel: MAIN renders on black (" + main_loaded.error + ")");
    check_eq(count_non_background(main_loaded.pixels), main_loaded.stats.ink_pixels,
             "panel: MAIN stays on black");

    std::cout << "  panel: PLATE " << panel->width << "x" << panel->height
              << ", pixels under text=" << diff
              << ", ink with/without=" << with_panel.stats.ink_pixels << "/"
              << black.stats.ink_pixels << "\n";
}

/// Sprite geometry (docs/UI_SCREENS.md §9): the four control sprites of
/// `FRONTEND/BMP.IDX` have the sizes the layout constants assume.
void test_widget_assets(const fs::path& game) {
    std::string error;
    const std::optional<gm::MenuWidgetAssets> assets =
        gm::load_menu_widget_assets(game, error);
    check(assets.has_value(), "widget assets: load (" + error + ")");
    if (!assets) {
        return;
    }
    check(assets->complete(), "widget assets: complete");
    check_eq(assets->slide.width, gm::kMenuSlideWidth, "widget assets: SLIDE width");
    check_eq(assets->slide.height, gm::kMenuSlideHeight, "widget assets: SLIDE height");
    check_eq(assets->knob.width, gm::kMenuKnobWidth, "widget assets: KNOB width");
    check_eq(assets->knob.height, gm::kMenuKnobHeight, "widget assets: KNOB height");
    check_eq(assets->check.width, gm::kMenuCheckWidth, "widget assets: CHECK width");
    check_eq(assets->check.height, gm::kMenuCheckHeight, "widget assets: CHECK height");
    check_eq(assets->nocheck.width, gm::kMenuNoCheckWidth, "widget assets: NOCHECK width");
    check_eq(assets->nocheck.height, gm::kMenuNoCheckHeight,
             "widget assets: NOCHECK height");
    check_eq(assets->arrow.width, gm::kMenuArrowWidth, "widget assets: MENUARRW width");
    check_eq(assets->arrow.height, gm::kMenuArrowHeight, "widget assets: MENUARRW height");

    // Index 0 is the transparent key: the 8x8 KNOB and NOCHECK are fully
    // opaque, the track, the tick and the arrow have transparent pixels
    // (checked against the BMPs: 71 / 37 / 87 key pixels).
    const auto transparent = [](const gm::MenuSprite& sprite) {
        int count = 0;
        for (const std::uint32_t pixel : sprite.rgba) {
            if (pixel == 0) {
                ++count;
            }
        }
        return count;
    };
    check_eq(transparent(assets->knob), 0, "widget assets: KNOB is opaque");
    check_eq(transparent(assets->slide), 71, "widget assets: SLIDE key pixels");
    check_eq(transparent(assets->check), 37, "widget assets: CHECK key pixels");
    check_eq(transparent(assets->nocheck), 0, "widget assets: NOCHECK is opaque");
    check_eq(transparent(assets->arrow), 87, "widget assets: MENUARRW key pixels");

    std::string empty_error;
    check(!gm::load_menu_widget_assets(fs::path{}, empty_error),
          "widget assets: empty game refused");
    check(empty_error.find("no game directory") != std::string::npos,
          "widget assets: empty game error text");

    std::cout << "  widget assets: SLIDE " << assets->slide.width << "x"
              << assets->slide.height << " key=" << transparent(assets->slide)
              << ", KNOB " << assets->knob.width << "x" << assets->knob.height
              << " key=" << transparent(assets->knob) << ", CHECK "
              << assets->check.width << "x" << assets->check.height << " key="
              << transparent(assets->check) << ", NOCHECK " << assets->nocheck.width
              << "x" << assets->nocheck.height << " key=" << transparent(assets->nocheck)
              << ", MENUARRW " << assets->arrow.width << "x" << assets->arrow.height
              << " key=" << transparent(assets->arrow) << "\n";
}

/// One screen with widgets: the counters, the ink growth, determinism and the
/// picture change against the same screen without widgets.
void check_widgets_render(const std::string& tag, const gm::FrontendMenu& menu,
                          const gm::FontLookup& fonts,
                          const gm::MenuWidgetAssets& assets,
                          const std::vector<gm::MenuWidget>& widgets) {
    const Render plain = render(menu, fonts);
    const Render with = render_widgets(menu, fonts, &assets, widgets);
    check(plain.ok, tag + "plain render");
    check(with.ok, tag + "widget render (" + with.error + ")");
    if (!plain.ok || !with.ok) {
        return;
    }
    check_canvas(with, tag);
    check_eq(with.stats.items, plain.stats.items, tag + "items unchanged");
    check_eq(with.stats.drawn_items, plain.stats.drawn_items, tag + "drawn items unchanged");
    check_eq(with.stats.widgets, static_cast<long long>(widgets.size()), tag + "widgets");
    check_eq(with.stats.drawn_widgets, static_cast<long long>(widgets.size()),
             tag + "drawn_widgets == widgets");
    check(with.stats.widget_ink > 0, tag + "widget ink > 0");
    check(with.stats.ink_pixels > plain.stats.ink_pixels, tag + "ink grows with widgets");
    check(with.stats.ink_pixels <= plain.stats.ink_pixels + with.stats.widget_ink,
          tag + "ink is not double counted");
    const int diff = count_diff(plain.pixels, with.pixels);
    check(diff > 0, tag + "widgets change the picture");

    const Render again = render_widgets(menu, fonts, &assets, widgets);
    check(again.ok && again.pixels == with.pixels, tag + "widget render deterministic");
    check_eq(again.stats.widget_ink, with.stats.widget_ink, tag + "second widget ink");

    std::cout << "  " << tag << "widgets=" << with.stats.widgets
              << " drawn=" << with.stats.drawn_widgets
              << " ink=" << with.stats.ink_pixels << " (text "
              << plain.stats.ink_pixels << " + widget " << with.stats.widget_ink
              << ") diff=" << diff << "\n";
}

/// Slider values: min/mid/max paint different frames, the position is clamped
/// (out-of-range values are not a crash and not a different picture) and a
/// degenerate range stays at the left end.
void test_slider_values(const gm::FrontendMenu& menu, const gm::FontLookup& fonts,
                        const gm::MenuWidgetAssets& assets) {
    const auto slider = [](int value, int max_value) {
        std::vector<gm::MenuWidget> widgets;
        widgets.emplace_back(gm::MenuWidget::Slider, 100, 16, value, 0, max_value);
        return widgets;
    };
    const Render zero = render_widgets(menu, fonts, &assets, slider(0, 100));
    const Render mid = render_widgets(menu, fonts, &assets, slider(50, 100));
    const Render full = render_widgets(menu, fonts, &assets, slider(100, 100));
    check(zero.ok && mid.ok && full.ok, "slider: three values render");
    if (!zero.ok || !mid.ok || !full.ok) {
        return;
    }
    const int zero_mid = count_diff(zero.pixels, mid.pixels);
    const int mid_full = count_diff(mid.pixels, full.pixels);
    const int zero_full = count_diff(zero.pixels, full.pixels);
    check(zero_mid > 0, "slider: value 0 != middle");
    check(mid_full > 0, "slider: middle != max");
    check(zero_full > 0, "slider: value 0 != max");

    // Clamping: -1000 is the same picture as min, 10000 as max, no crash.
    const Render low = render_widgets(menu, fonts, &assets, slider(-1000, 100));
    const Render high = render_widgets(menu, fonts, &assets, slider(10000, 100));
    check(low.ok && high.ok, "slider: out-of-range values render");
    check(low.pixels == zero.pixels, "slider: -1000 clamps to min");
    check(high.pixels == full.pixels, "slider: 10000 clamps to max");
    check_eq(low.stats.ink_pixels, zero.stats.ink_pixels, "slider: clamped ink");
    check_eq(high.stats.ink_pixels, full.stats.ink_pixels, "slider: clamped max ink");

    // A zero-width range (max == min) must not divide by zero.
    std::vector<gm::MenuWidget> degenerate;
    degenerate.emplace_back(gm::MenuWidget::Slider, 100, 16, 7, 5, 5);
    const Render flat = render_widgets(menu, fonts, &assets, degenerate);
    check(flat.ok, "slider: max == min renders");
    check_eq(flat.stats.drawn_widgets, 1, "slider: degenerate draws one widget");

    std::cout << "  slider values: 0/mid/max diffs=" << zero_mid << "/" << mid_full << "/"
              << zero_full << ", clamp identical (-1000==0: " << (low.pixels == zero.pixels)
              << ", 10000==100: " << (high.pixels == full.pixels)
              << "), degenerate ink=" << flat.stats.ink_pixels << "\n";
}

/// Checkboxes: `checked` draws `CHECK` (8x16), unchecked `NOCHECK` (8x8, eight
/// rows lower) — the two frames differ.
void test_checkbox_state(const gm::FrontendMenu& menu, const gm::FontLookup& fonts,
                         const gm::MenuWidgetAssets& assets) {
    std::vector<gm::MenuWidget> unchecked;
    unchecked.emplace_back(gm::MenuWidget::Checkbox, 16, 40);
    std::vector<gm::MenuWidget> checked;
    checked.emplace_back(gm::MenuWidget::Checkbox, 16, 40, 0, 0, 0, true);
    const Render off = render_widgets(menu, fonts, &assets, unchecked);
    const Render on = render_widgets(menu, fonts, &assets, checked);
    check(off.ok && on.ok, "checkbox: both states render");
    if (!off.ok || !on.ok) {
        return;
    }
    check_eq(on.stats.widgets, 1, "checkbox: one widget");
    check_eq(on.stats.drawn_widgets, 1, "checkbox: checked drawn");
    check_eq(off.stats.drawn_widgets, 1, "checkbox: unchecked drawn");
    const int diff = count_diff(off.pixels, on.pixels);
    check(diff > 0, "checkbox: checked != unchecked");
    // The 8x16 tick paints more than the 8x8 box (both are masked and the
    // shared mask subtracts the label pixels they overlap).
    check(on.stats.widget_ink > off.stats.widget_ink,
          "checkbox: CHECK paints more than NOCHECK");

    std::cout << "  checkbox: checked/unchecked diff=" << diff
              << ", widget ink " << off.stats.widget_ink << " -> " << on.stats.widget_ink
              << "\n";
}

/// Arrows: `MENUARRW` as-is and rotated 180 degrees (`0x485d00`).
void test_arrow(const gm::FrontendMenu& menu, const gm::FontLookup& fonts,
                const gm::MenuWidgetAssets& assets) {
    std::vector<gm::MenuWidget> up;
    up.emplace_back(gm::MenuWidget::Arrow, 170, 20);
    std::vector<gm::MenuWidget> down;
    down.emplace_back(gm::MenuWidget::Arrow, 170, 20, 0, 0, 0, false, true);
    const Render a = render_widgets(menu, fonts, &assets, up);
    const Render b = render_widgets(menu, fonts, &assets, down);
    check(a.ok && b.ok, "arrow: both orientations render");
    if (!a.ok || !b.ok) {
        return;
    }
    check_eq(a.stats.drawn_widgets, 1, "arrow: one widget");
    const int diff = count_diff(a.pixels, b.pixels);
    check(diff > 0, "arrow: rotated 180 != as-is");
    std::cout << "  arrow: 16x8, rotated diff=" << diff << ", ink=" << a.stats.widget_ink
              << "\n";
}

/// The four option screens whose controls are proven (SOUND/GRAPHICS/PLAY),
/// plus a synthetic CONTROL OPTIONS layout (8 sliders + 1 checkbox), the
/// clipping rule and the negatives.
void test_widgets(const fs::path& game, const gm::FontLookup& fonts) {
    std::string error;
    const std::optional<gm::MenuWidgetAssets> loaded =
        gm::load_menu_widget_assets(game, error);
    check(loaded.has_value(), "widgets: assets load (" + error + ")");
    if (!loaded) {
        return;
    }
    const gm::MenuWidgetAssets& assets = *loaded;

    // Proven layouts: counts and coordinates come from the call sites.
    const std::vector<gm::MenuWidget> sound = gm::menu_widgets(gm::Screen::SoundOptions);
    check_eq(sound.size(), 3, "layout: SOUND OPTIONS 2 sliders + 1 checkbox");
    check_eq(sound[0].kind, gm::MenuWidget::Slider, "layout: SOUND[0] slider");
    check_eq(sound[0].x, 68, "layout: SOUND music x");
    check_eq(sound[0].y, 37, "layout: SOUND music y");
    check_eq(sound[0].max, 20, "layout: SOUND volume 0..20");
    check_eq(sound[1].y, 57, "layout: SOUND effects y");
    check_eq(sound[2].kind, gm::MenuWidget::Checkbox, "layout: SOUND[2] checkbox");
    check_eq(sound[2].x, 20, "layout: SOUND switch x");
    check_eq(sound[2].y, 59, "layout: SOUND switch y");

    const std::vector<gm::MenuWidget> graphics =
        gm::menu_widgets(gm::Screen::GraphicsOptions);
    check_eq(graphics.size(), 2, "layout: GRAPHICS slider + checkbox");
    check_eq(graphics[0].kind, gm::MenuWidget::Slider, "layout: GRAPHICS[0] slider");
    check_eq(graphics[0].x, 88, "layout: GRAPHICS border x");
    check_eq(graphics[0].y, 39, "layout: GRAPHICS border y");
    check_eq(graphics[0].max, 9, "layout: GRAPHICS border 0..9");
    check_eq(graphics[1].kind, gm::MenuWidget::Checkbox, "layout: GRAPHICS[1] checkbox");
    check_eq(graphics[1].x, 40, "layout: GRAPHICS reflections x");
    check_eq(graphics[1].y, 61, "layout: GRAPHICS reflections y");

    const std::vector<gm::MenuWidget> play = gm::menu_widgets(gm::Screen::PlayOptions);
    check_eq(play.size(), 6, "layout: PLAY OPTIONS 6 checkboxes");
    for (const gm::MenuWidget& widget : play) {
        check_eq(widget.kind, gm::MenuWidget::Checkbox, "layout: PLAY widget is a checkbox");
        check_eq(widget.x, 16, "layout: PLAY checkbox x");
    }
    check_eq(play[0].y, 13, "layout: PLAY first anchor");
    check_eq(play[5].y, 87, "layout: PLAY last anchor");

    check(gm::menu_widgets(gm::Screen::ControlOptions).empty(),
          "layout: CONTROL OPTIONS has no proven controls");
    check(gm::menu_widgets(gm::Screen::Main).empty(), "layout: MAIN has no controls");

    // CONTROL OPTIONS: the requested synthetic layout — 8 sliders (0/mid/max)
    // and one checkbox, all inside the 196x131 band.
    gm::FrontendMenu control;
    control.reset();
    check(control.enter(gm::Screen::ControlOptions), "widgets: enter CONTROL OPTIONS");
    std::vector<gm::MenuWidget> control_widgets;
    for (int i = 0; i < 8; ++i) {
        const int value = i % 3 == 0 ? 0 : (i % 3 == 1 ? 50 : 100);
        control_widgets.emplace_back(gm::MenuWidget::Slider, 100, 16 + i * 13, value, 0,
                                     100);
    }
    control_widgets.emplace_back(gm::MenuWidget::Checkbox, 16, 112, 0, 0, 0, true);
    check_widgets_render("CONTROL OPTIONS: ", control, fonts, assets, control_widgets);
    test_slider_values(control, fonts, assets);
    test_checkbox_state(control, fonts, assets);
    test_arrow(control, fonts, assets);

    // GRAPHICS OPTIONS: one slider and the Reflections box; the box must change
    // the picture when toggled.
    {
        gm::FrontendMenu menu;
        menu.reset();
        check(menu.enter(gm::Screen::GraphicsOptions), "widgets: enter GRAPHICS OPTIONS");
        check_widgets_render("GRAPHICS OPTIONS: ", menu, fonts, assets, graphics);
        std::vector<gm::MenuWidget> toggled = graphics;
        toggled[1].checked = true;
        const Render off = render_widgets(menu, fonts, &assets, graphics);
        const Render on = render_widgets(menu, fonts, &assets, toggled);
        check(off.ok && on.ok, "graphics: toggled renders");
        const int diff = count_diff(off.pixels, on.pixels);
        check(diff > 0, "graphics: Reflections checked changes the picture");
        std::cout << "  GRAPHICS OPTIONS toggle: diff=" << diff
                  << ", widget ink " << off.stats.widget_ink << " -> "
                  << on.stats.widget_ink << "\n";
    }

    // SOUND OPTIONS: the two volume sliders at min vs max.
    {
        gm::FrontendMenu menu;
        menu.reset();
        check(menu.enter(gm::Screen::SoundOptions), "widgets: enter SOUND OPTIONS");
        std::vector<gm::MenuWidget> low = sound;
        std::vector<gm::MenuWidget> high = sound;
        for (gm::MenuWidget& widget : high) {
            if (widget.kind == gm::MenuWidget::Slider) {
                widget.value = widget.max;
            }
        }
        low[2].checked = true;
        high[2].checked = true;
        check_widgets_render("SOUND OPTIONS: ", menu, fonts, assets, low);
        const Render a = render_widgets(menu, fonts, &assets, low);
        const Render b = render_widgets(menu, fonts, &assets, high);
        check(a.ok && b.ok, "sound: both volume states render");
        const int diff = count_diff(a.pixels, b.pixels);
        check(diff > 0, "sound: volume 0 != volume 20");
        std::cout << "  SOUND OPTIONS volume: 0 vs 20 diff=" << diff << "\n";
    }

    // PLAY OPTIONS: six checkboxes, all off vs all on.
    {
        gm::FrontendMenu menu;
        menu.reset();
        check(menu.enter(gm::Screen::PlayOptions), "widgets: enter PLAY OPTIONS");
        std::vector<gm::MenuWidget> on = play;
        for (gm::MenuWidget& widget : on) {
            widget.checked = true;
        }
        check_widgets_render("PLAY OPTIONS: ", menu, fonts, assets, on);
        const Render off = render_widgets(menu, fonts, &assets, play);
        const Render both = render_widgets(menu, fonts, &assets, on);
        check(off.ok && both.ok, "play: both checkbox states render");
        const int diff = count_diff(off.pixels, both.pixels);
        check(diff > 0, "play: six checkboxes change the picture");
        std::cout << "  PLAY OPTIONS checkboxes: off/on diff=" << diff << "\n";
    }

    // Clipping: a widget below the 196x131 band paints nothing and is not
    // counted, the render still succeeds.
    {
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(gm::Screen::ControlOptions);
        std::vector<gm::MenuWidget> outside;
        outside.emplace_back(gm::MenuWidget::Slider, 100, 140, 0, 0, 100);
        outside.emplace_back(gm::MenuWidget::Arrow, 10, 200);
        const Render clipped = render_widgets(menu, fonts, &assets, outside);
        check(clipped.ok, "clip: widget below the band renders");
        check_eq(clipped.stats.widgets, 2, "clip: two widgets passed");
        check_eq(clipped.stats.drawn_widgets, 0, "clip: nothing drawn below the band");
        check_eq(clipped.stats.widget_ink, 0, "clip: no widget ink below the band");
    }

    // Negatives: widgets without the sprites are refused with a text and the
    // buffer is reset; an empty game directory is refused by the loader.
    {
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(gm::Screen::SoundOptions);
        std::vector<gm::MenuWidget> widgets;
        widgets.emplace_back(gm::MenuWidget::Slider, 68, 37, 10, 0, 20);
        Render missing = render_widgets(menu, fonts, nullptr, widgets);
        check(!missing.ok, "negative: widgets without sprites fail");
        check(missing.error.find("MenuWidgetAssets") != std::string::npos,
              "negative: error names MenuWidgetAssets");
        check_eq(count_non_background(missing.pixels), 0, "negative: buffer reset");
        check_eq(missing.stats.widgets, 0, "negative: stats zeroed");
    }

    // The game-directory overload loads the sprites itself and paints the same
    // bytes as the caller-supplied assets + panel.
    {
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(gm::Screen::SoundOptions);
        Render loaded_render;
        loaded_render.ok =
            gm::render_screen(menu, game, sound, loaded_render.pixels, loaded_render.width,
                              loaded_render.height, &loaded_render.stats,
                              &loaded_render.error);
        check(loaded_render.ok, "game overload: widgets render (" + loaded_render.error + ")");
        std::string panel_error;
        const std::optional<gm::MenuBackground> panel =
            gm::load_menu_panel(game, gm::Screen::SoundOptions, panel_error);
        check(panel.has_value(), "game overload: panel loads");
        if (panel && loaded_render.ok) {
            const Render manual = render_widgets(menu, fonts, &assets, sound, &*panel);
            check(manual.ok, "game overload: manual render");
            check(manual.pixels == loaded_render.pixels,
                  "game overload: same bytes as manual assets + panel");
            check_eq(loaded_render.stats.widget_ink, manual.stats.widget_ink,
                     "game overload: widget ink");
        }
    }
}

/// The step table of §2 and the title rule of §2.
void test_style() {
    check_eq(gm::menu_row_step(gm::Screen::ControlOptions), 9, "style: 10 step 9");
    check_eq(gm::menu_row_step(gm::Screen::Quit), 16, "style: 33 step 16");
    check_eq(gm::menu_row_step(gm::Screen::PlayOptions), 15, "style: 32 step 15");
    check_eq(gm::menu_row_step(gm::Screen::Main), 0, "style: MAIN has no step");
    check(gm::menu_has_title(gm::Screen::ControlOptions), "style: 10 has a title");
    check(gm::menu_has_title(gm::Screen::Quit), "style: 33 has a title");
    check(!gm::menu_has_title(gm::Screen::Main), "style: MAIN title is art");
    check(!gm::menu_has_title(gm::Screen::KeySetView),
          "style: 30 title is the key set name");
}

/// Negatives: no screen, the terminal state, no provider, no game directory.
void test_negatives(const fs::path& game) {
    {
        gm::FrontendMenu menu; // not reset: Screen::None
        std::vector<std::uint32_t> pixels;
        int width = 0;
        int height = 0;
        gm::MenuRenderStats stats;
        std::string error;
        check(!gm::render_screen(menu, gm::FontLookup{}, pixels, width, height, &stats,
                                 &error),
              "none: render false");
        check(error.find("no screen") != std::string::npos, "none: error names the screen");
        check_eq(width, gm::kMenuCanvasWidth, "none: canvas width");
        check_eq(height, gm::kMenuCanvasHeight, "none: canvas height");
        check_eq(static_cast<long long>(pixels.size()),
                 static_cast<long long>(width) * height, "none: buffer is 320x200");
        check_eq(count_non_background(pixels), 0, "none: buffer is background only");
        check_eq(stats.items, 0, "none: no items");
        check_eq(stats.ink_pixels, 0, "none: no ink");
    }
    {
        gm::FrontendMenu menu;
        menu.reset();
        check(menu.enter(gm::Screen::Exit), "exit: enter(Exit) allowed by the host");
        std::vector<std::uint32_t> pixels;
        int width = 0;
        int height = 0;
        gm::MenuRenderStats stats;
        std::string error;
        check(!gm::render_screen(menu, gm::FontLookup{}, pixels, width, height, &stats,
                                 &error),
              "exit: render false");
        check(error.find("Exit") != std::string::npos, "exit: error names Screen::Exit");
        check_eq(count_non_background(pixels), 0, "exit: buffer is background only");
        check_eq(stats.ink_pixels, 0, "exit: no ink");

        // The same through the game-directory overload: the refusal happens
        // before any file is read.
        std::string path_error;
        check(!gm::render_screen(menu, game, pixels, width, height, &stats, &path_error),
              "exit: game overload false");
        check(path_error.find("Exit") != std::string::npos,
              "exit: game overload names Screen::Exit");
    }
    {
        gm::FrontendMenu menu;
        menu.reset();
        menu.enter(gm::Screen::ControlOptions);
        std::vector<std::uint32_t> pixels;
        int width = 0;
        int height = 0;
        gm::MenuRenderStats stats;
        std::string error;
        check(!gm::render_screen(menu, gm::FontLookup{}, pixels, width, height, &stats,
                                 &error),
              "no provider: render false");
        check(error.find("no font source") != std::string::npos,
              "no provider: error text");
        check_eq(count_non_background(pixels), 0, "no provider: buffer reset");

        std::string path_error;
        check(!gm::render_screen(menu, fs::path{}, pixels, width, height, &stats,
                                 &path_error),
              "no game dir: render false");
        check(path_error.find("no game directory") != std::string::npos,
              "no game dir: error text");
        check_eq(count_non_background(pixels), 0, "no game dir: buffer reset");
    }
}

/// Every §2 screen renders without an error or a missing glyph; the only
/// screens whose list does not fit the 196x131 plate are the 31-row key set
/// lists (they scroll in the original, the snapshot has no scroll state).
void test_all_screens(const gm::FontLookup& fonts) {
    int screens = 0;
    int with_items = 0;
    int ink_total = 0;
    int clipped = 0;
    for (const gm::Screen screen : gm::FrontendMenu::screens()) {
        gm::FrontendMenu menu;
        menu.reset();
        check(menu.enter(screen), "sweep: enter screen");
        const std::string name(static_cast<std::string_view>(
            gm::FrontendMenu::screen_name(screen)));
        const Render result = render(menu, fonts);
        check(result.ok, "sweep: " + name + " renders (" + result.error + ")");
        check(result.error.empty(), "sweep: " + name + " no error");
        check_canvas(result, "sweep: " + name + ": ");
        check_eq(result.stats.missing_glyphs, 0, "sweep: " + name + " missing_glyphs");
        check_eq(result.stats.items, static_cast<long long>(menu.items().size()),
                 "sweep: " + name + " items");
        if (!menu.items().empty()) {
            ++with_items;
            check(result.stats.ink_pixels > 0, "sweep: " + name + " ink > 0");
        }
        if (result.stats.drawn_items != result.stats.items) {
            ++clipped;
            const bool key_list = screen == gm::Screen::KeySetView ||
                                  screen == gm::Screen::KeySetEditorEntry ||
                                  screen == gm::Screen::KeySetEditor;
            check(key_list, "sweep: only the 31-row key lists are clipped");
        }
        ink_total += result.stats.ink_pixels;
        ++screens;
    }
    check_eq(screens, 34, "sweep: 34 §2 screens");
    check_eq(clipped, 3, "sweep: 3 clipped screens (the key set lists)");
    std::cout << "  sweep: " << screens << "/34 screens render, " << with_items
              << " with items, " << clipped << " clipped (key set lists), ink total "
              << ink_total << "\n";
}

/// FNV-1a 64 of the 4096 table bytes — the dump's fingerprint
/// (`docs/HUD_LUT.md` §2: `0x6330406254771f3d`), so the in-memory build is the
/// same table the exact path would load from a file.
std::uint64_t fnv1a64(const gm::HudLut& lut) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto& level : lut.levels) {
        for (const std::uint8_t value : level) {
            hash ^= value;
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

int count_black(const std::vector<std::uint32_t>& pixels) {
    int dark = 0;
    for (const std::uint32_t value : pixels) {
        if ((value >> 8u) == 0u) {
            ++dark;
        }
    }
    return dark;
}

/// Rendering with the fade options on the real SOUND OPTIONS screen (PLATE plus
/// the three control sprites, so both the panel palette and a sprite palette are
/// on screen).  The `Switch Left Right` box is checked: `CHECK` is the one
/// shipped asset whose pixels use the HUD LUT skip slots (243/244, dark red),
/// which is what makes level 0 differ from the RGBA model.
Render render_fade(const gm::FrontendMenu& menu, const gm::FontLookup& fonts,
                   const gm::MenuBackground* panel, const gm::MenuWidgetAssets* assets,
                   const std::vector<gm::MenuWidget>& widgets,
                   const gm::MenuRenderOptions& options, std::string* error = nullptr) {
    Render out;
    out.ok = gm::render_screen(menu, fonts, panel, assets, widgets, options, out.pixels,
                               out.width, out.height, &out.stats, &out.error);
    if (error != nullptr) {
        *error = out.error;
    }
    return out;
}

/// Fade (`M8.5` slice 8): exact tables on palette indices vs the channel model.
void test_fade(const fs::path& game, const gm::FontLookup& fonts) {
    gm::FrontendMenu menu;
    menu.reset();
    check(menu.enter(gm::Screen::SoundOptions), "fade: enter SOUND OPTIONS");

    std::string error;
    const std::optional<gm::MenuBackground> panel =
        gm::load_menu_panel(game, gm::Screen::SoundOptions, error);
    check(panel.has_value(), "fade: PLATE loads (" + error + ")");
    const std::optional<gm::MenuWidgetAssets> loaded_assets =
        gm::load_menu_widget_assets(game, error);
    check(loaded_assets.has_value(), "fade: control sprites load (" + error + ")");
    const std::optional<std::vector<std::uint8_t>> palette =
        gm::installed_palette(game, error);
    check(palette.has_value(), "fade: GAMEPAL loads (" + error + ")");
    if (!panel || !loaded_assets || !palette) {
        return;
    }
    const gm::MenuWidgetAssets& assets = *loaded_assets;
    std::vector<gm::MenuWidget> widgets = gm::menu_widgets(gm::Screen::SoundOptions);
    check(!widgets.empty(), "fade: SOUND OPTIONS has widgets");
    widgets[2].checked = true;
    check_eq(static_cast<long long>(panel->indices.size()),
             static_cast<long long>(panel->width) * panel->height,
             "fade: PLATE keeps its palette indices");
    check_eq(static_cast<long long>(panel->palette.size()), 1024L,
             "fade: PLATE palette is 256 RGBA quads");

    // The table is built from the installed palette: no dump file involved.
    gm::HudLut built;
    std::string build_error;
    check(gm::build_hud_lut(*palette, built, &build_error),
          "fade: build_hud_lut from GAMEPAL (" + build_error + ")");
    const std::uint64_t built_fnv = fnv1a64(built);
    check(built_fnv == 0x6330406254771f3dull,
          "fade: built table FNV 0x6330406254771f3d");

    const Render plain =
        render_fade(menu, fonts, &*panel, &assets, widgets, gm::MenuRenderOptions{});
    check(plain.ok, "fade: plain render (" + plain.error + ")");
    if (!plain.ok) {
        return;
    }

    // The same bytes through the game-directory overload (which loads PLATE, the
    // atlases and the sprites itself) — the options are forwarded, not dropped.
    Render game_plain;
    game_plain.ok = gm::render_screen(menu, game, widgets, gm::MenuRenderOptions{},
                                      game_plain.pixels, game_plain.width,
                                      game_plain.height, &game_plain.stats,
                                      &game_plain.error);
    check(game_plain.ok, "fade: game overload renders (" + game_plain.error + ")");
    check(game_plain.pixels == plain.pixels,
          "fade: game overload equals the manual panel render");

    // Level 11: the identity table -> the unfaded frame, byte for byte.
    gm::MenuRenderOptions at11;
    at11.fade_lut = true;
    at11.fade_lut_table = &built;
    at11.fade_lut_level = gm::kHudLutIdentityLevel;
    const Render l11 = render_fade(menu, fonts, &*panel, &assets, widgets, at11);
    check(l11.ok, "fade: level 11 renders (" + l11.error + ")");
    const int diff11 = count_diff(plain.pixels, l11.pixels);
    check_eq(diff11, 0, "fade: level 11 is byte-identical to no fade");

    // Level 0: black almost everywhere, but the skip slots are untouched, so it
    // differs from the channel model (which blacks out every channel value).
    gm::MenuRenderOptions at0 = at11;
    at0.fade_lut_level = 0;
    const Render l0 = render_fade(menu, fonts, &*panel, &assets, widgets, at0);
    check(l0.ok, "fade: level 0 renders (" + l0.error + ")");
    gm::MenuRenderOptions model0;
    model0.fade = true;
    model0.fade_level = 0;
    const Render m0 = render_fade(menu, fonts, &*panel, &assets, widgets, model0);
    check(m0.ok, "fade: model level 0 renders (" + m0.error + ")");
    const int black_lut0 = count_black(l0.pixels);
    const int black_model0 = count_black(m0.pixels);
    const int lut_model0 = count_diff(l0.pixels, m0.pixels);
    check_eq(black_model0, gm::kMenuCanvasWidth * gm::kMenuCanvasHeight,
             "fade: model level 0 blacks out the whole frame");
    check(black_lut0 > 0, "fade: LUT level 0 is mostly black");
    check(black_lut0 < gm::kMenuCanvasWidth * gm::kMenuCanvasHeight,
          "fade: LUT level 0 leaves the skip slots untouched");
    check(lut_model0 > 0, "fade: LUT level 0 differs from the model");

    // 12..15: four different frames; the model clamps them all to 11.
    Render levels[gm::kHudLutLevels];
    levels[gm::kHudLutIdentityLevel] = l11;
    for (int level = gm::kHudLutIdentityLevel + 1; level < gm::kHudLutLevels; ++level) {
        gm::MenuRenderOptions options = at11;
        options.fade_lut_level = level;
        levels[level] = render_fade(menu, fonts, &*panel, &assets, widgets, options);
        check(levels[level].ok,
              "fade: level " + std::to_string(level) + " renders");
    }
    int adjacent[4] = {0, 0, 0, 0};
    int against11[4] = {0, 0, 0, 0};
    for (int level = 12; level <= 15; ++level) {
        adjacent[level - 12] =
            count_diff(levels[level - 1].pixels, levels[level].pixels);
        against11[level - 12] =
            count_diff(levels[gm::kHudLutIdentityLevel].pixels, levels[level].pixels);
        check(adjacent[level - 12] > 0,
              "fade: level " + std::to_string(level) + " differs from the previous one");
        check(against11[level - 12] > 0,
              "fade: level " + std::to_string(level) + " differs from level 11");
    }
    for (int a = 12; a <= 15; ++a) {
        for (int b = a + 1; b <= 15; ++b) {
            check(count_diff(levels[a].pixels, levels[b].pixels) > 0,
                  "fade: levels " + std::to_string(a) + "/" + std::to_string(b) +
                      " are different frames");
        }
    }

    // The model path is unchanged: `fade_level` on the finished frame is exactly
    // `fade_rgba` (what the CLI applies today), and 12..15 clamp to 11.
    gm::MenuRenderOptions model5;
    model5.fade = true;
    model5.fade_level = 5;
    const Render m5 = render_fade(menu, fonts, &*panel, &assets, widgets, model5);
    std::vector<std::uint32_t> expected = plain.pixels;
    std::string fade_error;
    check(gm::fade_rgba(expected, 5, gm::kHudLutIdentityLevel, &fade_error),
          "fade: fade_rgba reference (" + fade_error + ")");
    check(m5.ok, "fade: model level 5 renders (" + m5.error + ")");
    check(m5.pixels == expected, "fade: model level 5 == fade_rgba on the unfaded frame");
    gm::MenuRenderOptions model12 = model5;
    model12.fade_level = 12;
    const Render m12 = render_fade(menu, fonts, &*panel, &assets, widgets, model12);
    check(m12.ok, "fade: model level 12 renders");
    check(m12.pixels == plain.pixels, "fade: model clamps level 12 to 11");

    // `fade_lut` wins over `fade`, exactly like `PageRenderOptions`: level 0 of
    // the model plus the identity table is the unfaded frame.
    gm::MenuRenderOptions both;
    both.fade = true;
    both.fade_level = 0;
    both.fade_lut = true;
    both.fade_lut_table = &built;
    both.fade_lut_level = gm::kHudLutIdentityLevel;
    const Render precedence = render_fade(menu, fonts, &*panel, &assets, widgets, both);
    check(precedence.ok, "fade: both paths render");
    check_eq(count_diff(plain.pixels, precedence.pixels), 0,
             "fade: fade_lut wins over the model");

    // Negatives: a `fade_lut` call with neither a table nor a dump must fail
    // with text (never silently unfaded), and the buffer is reset.
    {
        gm::MenuRenderOptions missing;
        missing.fade_lut = true;
        std::string refusal;
        const Render failed =
            render_fade(menu, fonts, &*panel, &assets, widgets, missing, &refusal);
        check(!failed.ok, "fade: no table and no dump fails");
        check(refusal.find("fade_lut_table") != std::string::npos &&
                  refusal.find("fade_lut_dump") != std::string::npos,
              "fade: refusal names the table and the dump");
        check(refusal.find("menu_render:") == 0, "fade: refusal has the menu_render prefix");
        check_eq(count_black(failed.pixels), gm::kMenuCanvasWidth * gm::kMenuCanvasHeight,
                 "fade: refused frame is reset");
        check_eq(failed.stats.ink_pixels, 0, "fade: refused stats are zeroed");

        // A named dump that does not exist is a `hud_lut` error, not a silent
        // unfaded frame.
        gm::MenuRenderOptions bad_dump;
        bad_dump.fade_lut = true;
        bad_dump.fade_lut_dump = (fs::temp_directory_path() / "gm-no-such-hud-lut.bin").string();
        std::string dump_refusal;
        const Render dump_failed =
            render_fade(menu, fonts, &*panel, &assets, widgets, bad_dump, &dump_refusal);
        check(!dump_failed.ok, "fade: missing dump fails");
        check(dump_refusal.find("hud_lut") != std::string::npos,
              "fade: dump refusal carries the hud_lut text");
        check(dump_refusal.find("menu_render:") == 0,
              "fade: dump refusal has the menu_render prefix");
    }

    // The game-directory overload forwards the options for widgets too: level
    // 13 through it is the same frame as the manual panel + sprites, and it
    // differs from level 11.
    int game13_diff = 0;
    {
        gm::MenuRenderOptions at13 = at11;
        at13.fade_lut_level = 13;
        const Render manual13 = render_fade(menu, fonts, &*panel, &assets, widgets, at13);
        Render game13;
        game13.ok = gm::render_screen(menu, game, widgets, at13, game13.pixels,
                                      game13.width, game13.height, &game13.stats,
                                      &game13.error);
        check(manual13.ok, "fade: manual level 13 renders");
        check(game13.ok, "fade: game overload level 13 renders (" + game13.error + ")");
        check(manual13.pixels == game13.pixels,
              "fade: game overload level 13 == manual render");
        game13_diff = count_diff(l11.pixels, manual13.pixels);
        check(game13_diff > 0, "fade: level 13 differs from level 11 with widgets");
    }

    std::cout << "fade: SOUND OPTIONS + 3 widgets (PLATE + sprite palettes)\n";
    std::cout << "fade: LUT level 11 vs no fade: " << diff11 << "/"
              << gm::kMenuCanvasWidth * gm::kMenuCanvasHeight << " px differ\n";
    std::cout << "fade: LUT level 0: black " << black_lut0 << "/"
              << gm::kMenuCanvasWidth * gm::kMenuCanvasHeight << " (model "
              << black_model0 << "), " << lut_model0
              << " px differ from the model\n";
    std::cout << "fade: levels 12..15 adjacent diffs " << adjacent[0] << "/"
              << adjacent[1] << "/" << adjacent[2] << "/" << adjacent[3]
              << ", vs level 11 " << against11[0] << "/" << against11[1] << "/"
              << against11[2] << "/" << against11[3] << "\n";
    std::cout << "fade: model level 5 == fade_rgba on the unfaded frame, model level 12"
                 " == level 11, fade_lut wins over fade\n";
    std::cout << "fade: built table FNV-1a64 0x" << std::hex << built_fnv << std::dec
              << " (dump 0x6330406254771f3d), no dump file needed\n";
    std::cout << "fade: game overload level 13 vs 11 diff=" << game13_diff << "\n";
}

} // namespace

int main() {
    // The negatives never touch the installation.
    test_negatives(fs::path{});
    test_style();

    const char* env = std::getenv("GUNMETAL_DIR");
    const fs::path game = env != nullptr ? fs::path(env) : fs::path("/mnt/d/games/Gunmetal 3dfx");
    std::error_code ec;
    if (!fs::exists(game / "FRONTEND" / "BMP.IDX", ec)) {
        std::cout << "menu_render: game not installed, font/panel runs skipped\n";
        return failures == 0 ? 0 : 1;
    }

    std::map<int, gm::FontAtlas> fonts;
    std::string error;
    for (const int number : {gm::kMenuItemFont, gm::kMenuSelectedFont, gm::kMenuTitleFont}) {
        auto atlas = gm::load_font(game, number, error);
        if (!atlas) {
            std::cerr << "FAIL: font " << number << ": " << error << "\n";
            return 1;
        }
        fonts[number] = std::move(*atlas);
    }
    const gm::FontLookup lookup = make_lookup(fonts);

    // The three screens of the slice.
    {
        gm::FrontendMenu menu;
        menu.reset(); // MAIN
        test_screen(menu, "MAIN MENU", lookup, true);
    }
    {
        gm::FrontendMenu menu;
        menu.reset();
        check(menu.enter(gm::Screen::ControlOptions), "enter CONTROL OPTIONS");
        test_screen(menu, "CONTROL OPTIONS", lookup, true);
    }
    {
        gm::FrontendMenu menu;
        menu.reset();
        check(menu.enter(gm::Screen::Quit), "enter QUIT");
        test_screen(menu, "QUIT GAME?", lookup, true);
    }

    test_panel(game, lookup);
    test_widget_assets(game);
    test_widgets(game, lookup);
    test_all_screens(lookup);
    test_fade(game, lookup);
    // The path overload and the negatives with the real game directory.
    test_negatives(game);

    if (failures != 0) {
        std::cerr << "menu_render: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "menu_render: 320x200 RGBA of the menu screen (plate + §3 fonts, "
                 "highlight, deterministic); OK\n";
    return 0;
}
