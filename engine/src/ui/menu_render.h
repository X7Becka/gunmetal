#pragma once

// Rendering of one front-end menu screen into an RGBA canvas (`M8.2` slice 2).
//
// Division of labour
// ------------------
// `ui/frontend_menu` knows the current screen, its visible items (`label`,
// `id`, `x`, `row`) and the highlighted id; this module only *paints* that
// state: the 196x131 menu plate (when the screen has one), the title, every
// item label and the highlight, into the 320x200 front-end buffer (`0x6d8fc0`,
// docs/UI.md §6.3).  It never reads the keyboard, `KEYCFG`, saves or sounds.
//
// Item renderer (`0x485dd0`, docs/UI_SCREENS.md §3)
// -------------------------------------------------
//   * `item == -1`  -> plain text, `row` is the absolute y;
//   * `row < 0`     -> the absolute y is `-row` (docs/UI_SCREENS.md §3 and
//                      `tools/re/ui_screens.py`: "the drawn row is
//                      `base + item*step` when `base >= 0` and `-base`
//                      pixels otherwise");
//   * `row >= 0`    -> `y = row + item * step`, `step` is the §2 immediate of
//                      the handler (`0x662d58`, `menu_row_step`).
// The one deviation is documented below (`shared_negative_base`): the 31-row
// key set lists carry a single negative base for all rows, so they are stacked
// by the §2 step instead of collapsing onto one line.
//
// Text is clipped to the 196x131 copy of `PLATE` — the buffer `0x485dd0` writes
// into (§3) — so a row below 131 (the key-set lists have 31 of them) is not
// painted; the shipped screen scrolls those, and the static snapshot carries no
// scroll state.
//
// Fonts and colours (docs/UI_SCREENS.md §3: "шрифт заголовка — 5, пунктов — 0,
// подсветки — 4") are *proven from the shipped atlas*: `FONTS4` is 512x168 with
// four 8x8 faces at engine y 0/42/84/126 (`font_table_spec`).  Every face has
// the same 1174 ink pixels (identical glyph shapes) but a different palette:
//
//   font 0 -> (19,15,15)  dark grey   (normal item)
//   font 4 -> (239,239,239) white     (highlight)
//   font 5 -> (155,15,15) dark red    (title)
//   font 8 -> (75,75,75)  mid grey    (not used here)
//
// so the highlight is the same glyphs in the white face — no bar, box or
// `MENULIT` marker is drawn (their geometry is not documented).
//
// Assets: screen -> panel
// -----------------------
// There is **no per-screen full-screen art** for the menu screens.  §2 gives
// one shared panel, `PLATE` (196x131), for 28 of the 34 screens; the text is
// composed into a copy of it (`0x486050` copies 196x131 bytes from `0x6d2b60`
// to `0x6865f8`).  `menu_panel_asset()` returns that name or an empty view;
// `load_menu_panel()` loads it read-only from `FRONTEND/BMP.IDX`.  The screen
// origin of the copy is **not documented**, so it is painted at (0,0) — the
// same convention (and the same caveat) as the SML pager page.  Screens
// without a panel get the opaque black background.
//
// Controls (`M8.2` slice 3)
// ------------------------
// The controls of the options screens are four masked sprites of
// `FRONTEND/BMP.IDX`: `SLIDE` 72x8 (track), `KNOB` 8x8, `CHECK` 8x16 /
// `NOCHECK` 8x8 and `MENUARRW` 16x8 (arrow).  A caller hands them in as
// `MenuWidget` (kind, x, y, value/min/max, checked); `menu_widgets(screen)`
// returns the geometry that is **proven from the shipped call sites** (SOUND
// OPTIONS, GRAPHICS OPTIONS, PLAY OPTIONS), and an empty vector for every
// screen whose layout is not proven (CONTROL OPTIONS has text only).
//
// Proven engine rules (docs/MENU_RENDER.md §3.5): all four helpers skip source
// palette index 0 (transparent) and write with a row stride of 196 — they blit
// into the `PLATE` copy `0x6865f8`, so the coordinates below are plate
// coordinates; `0x4833e0` draws `NOCHECK` 8 rows *below* its anchor (the
// 8x16 `CHECK` is top-anchored, the 8x8 `NOCHECK` bottom-aligned), and
// `0x485d00` draws `MENUARRW` rotated 180 degrees (up arrow -> down arrow).
//
// What is *not* drawn (docs/MENU_RENDER.md §5): save-slot strips (`FILESLOT`),
// the `MENULIT` marker, the MAIN carousel art (`SCROLL` is 404x200 and its
// layout is unknown), `F_M_TITL`, mouse cursor, UI sounds and animations.
// `Item::label` is the static snapshot of §3: `%s`/`%ld` stay literal.
//
// Fade (`M8.5` slice 8, docs/MENU_RENDER.md §7)
// --------------------------------------------
// `MenuRenderOptions` has the same two paths as `ui/pager_render` (docs/
// PAGER_RENDER.md §2.5/§2.6):
//
//  * `fade`/`fade_level` is the **model**: the engine curve
//    `(v*k + max/2)/max` (`render/upscale.h::fade_rgba`) applied to the
//    finished RGBA frame.  It is what the CLI does today, so the default keeps
//    those bytes and 12..15 clamp to 11.
//  * `fade_lut`/`fade_lut_level` is the **exact** path: the frame is composed
//    in palette indices (`MenuBackground::indices`, `MenuSprite::indices`, the
//    glyph indices of the atlases), each index goes through
//    `render/hud_lut.h::map(level, index)` and only then becomes a colour from
//    the palette the pixel came from.  Levels 12..15 really brighten.  It needs
//    a preloaded table (`fade_lut_table`) or a 4096-byte dump
//    (`fade_lut_dump`); when both are absent the render fails with text instead
//    of silently drawing an unfaded frame.
//
// Errors are text (`std::string* error`) so the CLI can print `[error] menu:
// ...` and return 2: `Screen::None`/`Screen::Exit`, no font provider, an atlas
// the provider does not have, a panel that cannot be loaded, a `fade_lut` call
// without a table or a dump.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"
#include "ui/frontend_menu.h"

namespace gm {

/// The 320x200 mode-13h front-end buffer (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kMenuCanvasWidth = 320;
inline constexpr int kMenuCanvasHeight = 200;

/// Opaque `#000`: the canvas before the panel and any glyph.
inline constexpr std::uint32_t kMenuBackground = 0x000000FFu;

/// §3: title 5, item 0, highlight 4 (all `FONTS4` faces, see the header).
inline constexpr int kMenuTitleFont = 5;
inline constexpr int kMenuItemFont = 0;
inline constexpr int kMenuSelectedFont = 4;

/// `PLATE` in `FRONTEND/BMP.IDX` (§9) — the only menu panel of the shipped
/// installation; text is composed into a copy of it.
inline constexpr int kMenuPlateWidth = 196;
inline constexpr int kMenuPlateHeight = 131;
inline constexpr std::string_view kMenuPlateName = "PLATE";

/// y of the title inside the 196 px band.  The §2 table gives the title
/// *string*, not its row, and the plate has 27 px above the first item row of
/// the dense screens (27), so 10 is a model, not a byte-proven number.
inline constexpr int kMenuTitleY = 10;

/// Top row and line gap for a screen whose §2 `step` is 0 but whose items are
/// selectable (only `MAIN`, the 7-entry carousel): the carousel geometry is not
/// documented, so the labels are stacked in the plate band.  Model.
inline constexpr int kMenuStackedTop = 20;
inline constexpr int kMenuStackedGap = 10;

/// Sizes read from the BMP headers of `FRONTEND/BMP.IDX` (§9): `SLIDE` 72x8,
/// `KNOB` 8x8, `CHECK` 8x16, `NOCHECK` 8x8, `MENUARRW` 16x8.  A loaded sprite
/// of another size is refused, so the layout constants cannot silently drift.
inline constexpr int kMenuSlideWidth = 72;
inline constexpr int kMenuSlideHeight = 8;
inline constexpr int kMenuKnobWidth = 8;
inline constexpr int kMenuKnobHeight = 8;
inline constexpr int kMenuCheckWidth = 8;
inline constexpr int kMenuCheckHeight = 16;
inline constexpr int kMenuNoCheckWidth = 8;
inline constexpr int kMenuNoCheckHeight = 8;
inline constexpr int kMenuArrowWidth = 16;
inline constexpr int kMenuArrowHeight = 8;

/// `0x4833e0` draws the unchecked box `CHECK.height - NOCHECK.height = 8` rows
/// below the anchor, so the 8x8 box is bottom-aligned with the 8x16 tick.
inline constexpr int kMenuNoCheckDrop = kMenuCheckHeight - kMenuNoCheckHeight;

/// One RGB canvas layer (the panel), packed `0xRRGGBBAA` like the output.
///
/// `palette`/`indices` keep the source bytes of the BMP (palette RGBA quads and
/// one palette index per pixel) so the exact fade (`MenuRenderOptions::fade_lut`)
/// can map the index *before* the palette lookup, exactly like
/// `ui/pager_render`.  A hand-built layer may leave them empty: its pixels have
/// no index of their own and the exact path leaves them as they are.
struct MenuBackground {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> rgba;
    std::vector<std::uint8_t> palette; ///< BMP palette, 4 bytes per entry
    std::vector<std::uint8_t> indices; ///< one palette index per pixel
    std::string name;  ///< `FRONTEND/BMP.IDX` entry the layer came from

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

/// One masked sprite of `FRONTEND/BMP.IDX` (`SLIDE`, `KNOB`, `CHECK`,
/// `NOCHECK`, `MENUARRW`).  `rgba` is `0xRRGGBBAA`; **0 means transparent**
/// (source palette index 0), the same key the engine helpers use (`0x486070`,
/// `0x4833e0`, `0x485cc0`).  `palette`/`indices` are the source bytes for the
/// exact fade (see `MenuBackground`).
struct MenuSprite {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> rgba;
    std::vector<std::uint8_t> palette;
    std::vector<std::uint8_t> indices;
    std::string name;

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

/// The five sprites every control needs (`load_menu_widget_assets`).
struct MenuWidgetAssets {
    MenuSprite slide;
    MenuSprite knob;
    MenuSprite check;
    MenuSprite nocheck;
    MenuSprite arrow;

    bool complete() const {
        return !slide.empty() && !knob.empty() && !check.empty() && !nocheck.empty() &&
               !arrow.empty();
    }
};

/// One control of an options screen (`docs/UI_SCREENS.md` §3.1-3.4).
///
/// `x`/`y` are plate coordinates (the same space as `Item::x`, §3): `y` is the
/// top row of the 8-px control and `x` the left column.  Slider: the `SLIDE`
/// track is drawn at (`x`, `y`) and `KNOB` at
/// `x + value*(SLIDE.width-KNOB.width)/(max-min)` (clamped; a zero or inverted
/// range puts the knob at `x`).  Checkbox: `CHECK` at (`x`, `y`) when
/// `checked`, `NOCHECK` at (`x`, `y + 8`) when not (`0x4833e0`).  Arrow:
/// `MENUARRW` at (`x`, `y`), rotated 180 degrees when `rotated` (`0x485d00`
/// needs no second asset: it reads the same 16x8 sprite backwards).
struct MenuWidget {
    enum Kind { Slider, Checkbox, Arrow };

    Kind kind = Slider;
    int x = 0;
    int y = 0;
    int value = 0;
    int min = 0;
    int max = 0;
    bool checked = false;
    bool rotated = false; ///< Arrow: `MENUARRW` rotated 180 degrees (`0x485d00`)

    MenuWidget() = default;
    MenuWidget(Kind widget_kind, int widget_x, int widget_y, int widget_value = 0,
               int widget_min = 0, int widget_max = 0, bool is_checked = false,
               bool is_rotated = false)
        : kind(widget_kind), x(widget_x), y(widget_y), value(widget_value),
          min(widget_min), max(widget_max), checked(is_checked), rotated(is_rotated) {}
};

/// Numbers a render produced.
struct MenuRenderStats {
    int items = 0;             ///< `menu.items().size()`
    int drawn_items = 0;       ///< items that painted at least one glyph pixel
    int ink_pixels = 0;        ///< canvas pixels covered by a glyph or a widget (unique)
    int background_pixels = 0; ///< canvas pixels with no glyph and no widget (`w*h - ink`)
    int missing_glyphs = 0;    ///< label bytes with no glyph in the atlas
    int widgets = 0;           ///< widgets handed to the renderer
    int drawn_widgets = 0;     ///< widgets that painted at least one pixel
    int widget_ink = 0;        ///< canvas pixels covered by a widget (unique)
};

/// Optional fade of one frame (`M8.5` slice 8, docs/MENU_RENDER.md §7).  The
/// defaults reproduce the pre-slice bytes: no fade at all.
///
/// `fade_lut` wins over `fade` when both are set, exactly like
/// `PageRenderOptions`: the exact tables are index -> index, so they cannot be
/// combined with the RGBA model.  `fade_lut_level`/`fade_level` are clamped to
/// `0..15` by the table/model; level 11 is the identity and 12..15 are real
/// tables on the exact path (the model clamps them to 11).
struct MenuRenderOptions {
    bool fade = false;            ///< channel model `(v*k + max/2)/max` (RGB frame)
    int fade_level = 11;          ///< 0..15, 0 = black, 11 = identity (model clamps)
    bool fade_lut = false;        ///< exact path: index -> index before the palette
    int fade_lut_level = 11;      ///< 0..15, 12..15 are not aliases of 11
    std::string fade_lut_dump;    ///< 4096-byte dump for `load_hud_lut`
    const HudLut* fade_lut_table = nullptr; ///< preloaded table, wins over the dump
};

/// §2 `step` (`0x662d58`) of a screen, 0 when the handler does not set one.
int menu_row_step(Screen screen);

/// `FRONTEND/BMP.IDX` panel of a screen: `kMenuPlateName` for the screens whose
/// §2 asset list contains `plate`, an empty view otherwise (MAIN's `scroll` is
/// a 404x200 carousel background, not a menu plate).
std::string_view menu_panel_asset(Screen screen);

/// True when §2 gives an engine title *string* for the screen (`title VA` in
/// docs/UI_SCREENS.md §2).  30/34/35 use the key set name (not in the
/// snapshot), MAIN uses the `F_M_TITL` art, 9/15 have no title.
bool menu_has_title(Screen screen);

/// Load one BMP entry of `<game>/FRONTEND/BMP.IDX` as an RGBA layer.
///
/// `screen` selects the name (`menu_panel_asset`); an empty name is refused
/// (`"menu_render: screen N has no panel asset"`).  The installation is only
/// read.
std::optional<MenuBackground> load_menu_panel(const std::filesystem::path& game,
                                              Screen screen, std::string& error);

/// Load `SLIDE`/`KNOB`/`CHECK`/`NOCHECK`/`MENUARRW` from
/// `<game>/FRONTEND/BMP.IDX` as masked sprites (read-only).  A sprite of the
/// wrong size or without a palette fails with a text naming the entry, e.g.
/// `menu_render: SLIDE is 8x8, expected 72x8`.
std::optional<MenuWidgetAssets> load_menu_widget_assets(const std::filesystem::path& game,
                                                        std::string& error);

/// Controls of a screen with the geometry that is proven from the shipped
/// call sites (docs/MENU_RENDER.md §3.5); values are neutral (`value = min`,
/// unchecked), because they live in `GUNMETAL.CFG`/KEYCFG, which this module
/// does not read.  Screens without proven controls (including CONTROL OPTIONS,
/// which is text only) return an empty vector.
///
/// * 11 SOUND OPTIONS: slider (68,37) music (0..20), slider (68,57) effects
///   (0..20) — tracks `0x688290`/`0x6891e0` (`0x488a45`/`0x488a93`), clamps
///   `0x48c711`/`0x48c6da`; checkbox (20,59) *Switch Left Right* —
///   `0x689338` (`0x488af2`).
/// * 12 GRAPHICS OPTIONS: slider (88,39) *Border* (0..9) — track `0x68842c`
///   (`0x485f67`), clamp `0x48c773`; checkbox (40,61) *Reflections* —
///   `0x6894d4` (`0x485fd4`).
/// * 32 PLAY OPTIONS: six checkboxes x=16, y=13/24/35/52/70/87 —
///   `0x4861b5`..`0x486306`.
std::vector<MenuWidget> menu_widgets(Screen screen);

/// Draw the current screen into `rgba` (320x200, `0xRRGGBBAA`), black
/// background.  `width`/`height` are set to the canvas size and `rgba` is
/// resized even on failure.
bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, with the fade of `MenuRenderOptions` (docs/MENU_RENDER.md §7).
bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuRenderOptions& options, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats = nullptr,
                   std::string* error = nullptr);

/// Same, with `background` painted first (nullptr = black).  The layer is
/// clipped to the canvas and does not have to be 320x200.
bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, with the fade options.
bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background, const MenuRenderOptions& options,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, plus the controls `widgets` painted after the text.  `assets` must
/// hold `SLIDE`/`KNOB`/`CHECK`/`NOCHECK`/`MENUARRW` when `widgets` is not empty
/// (otherwise the render fails with a text and resets the buffer).  Both are
/// clipped to the 196x131 `PLATE` band, exactly like the text.
bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background, const MenuWidgetAssets* assets,
                   const std::vector<MenuWidget>& widgets,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, with the fade options (`M8.5` slice 8): the exact path composes the
/// frame in palette indices, so the table runs before any palette lookup.
bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuBackground* background, const MenuWidgetAssets* assets,
                   const std::vector<MenuWidget>& widgets,
                   const MenuRenderOptions& options, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats = nullptr,
                   std::string* error = nullptr);

/// Same, on the black background.
bool render_screen(const FrontendMenu& menu, const FontLookup& fonts,
                   const MenuWidgetAssets* assets, const std::vector<MenuWidget>& widgets,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, loading the atlases (fonts 0/4/5), the screen's panel and — when
/// `widgets` is not empty — the five control sprites from
/// `<game>/FRONTEND/BMP.IDX` (the CLI path; read-only).  A screen without a
/// panel (`menu_panel_asset` empty) renders on black — that is not an error.
bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, with the fade options.
bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   const MenuRenderOptions& options, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats = nullptr,
                   std::string* error = nullptr);

/// Same, with the controls `widgets` (the game-directory path).
bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   const std::vector<MenuWidget>& widgets,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   MenuRenderStats* stats = nullptr, std::string* error = nullptr);

/// Same, with the controls and the fade options.
bool render_screen(const FrontendMenu& menu, const std::filesystem::path& game,
                   const std::vector<MenuWidget>& widgets,
                   const MenuRenderOptions& options, std::vector<std::uint32_t>& rgba,
                   int& width, int& height, MenuRenderStats* stats = nullptr,
                   std::string* error = nullptr);

} // namespace gm
