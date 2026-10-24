#pragma once

// One frame of screen 12, `GRAPHICS OPTIONS` (`M8.8` slice 19).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/sound_options_render`
// 11, `ui/control_options_render` 10, `ui/mouse_setup_render` 22): the caller
// owns the state (the cursor `0x68ccf4`, the current video-mode descriptor
// `0x4c05c0`, the border `0x4bc1c8`, the gamma index `0x5a4950`, the
// `Reflections` flag `0x4aac84` and the light-map level `0x4aac88`) and this
// module paints it into the 320x200 mode-13h canvas (`0x6d8fc0`, docs/UI.md
// §6.3).  Nothing here reads the keyboard, `GUNMETAL.CFG`, sounds or saves, and
// nothing is written back.
//
// Proven geometry (`0x485ee0`, docs/UI_SCREENS.md §2/§3.3)
// -------------------------------------------------------
// The handler is the state-12 body itself (`0x485ee0`..`0x48604b`, `sub esp,0x3c`
// for the two text buffers): it sets `[0x67f10c] = 6` (`0x485ee3`) and
// `[0x662d58] = 15` (`0x485eed`), copies `PLATE` into the 196x131 compose buffer
// `0x6865f8` (`0x485ef9` -> `0x486050`), then draws the title with
// `0x485dd0(font=5, "GRAPHICS OPTIONS" `0x4bcf24`, centered=1, x=38, row=11,
// item=-1)` (`0x485efe`..`0x485f0d`).  `centered` replaces `x` by
// `(196 - width)/2` (`0x485de2`) and `item == -1` makes `row` absolute, so the
// title sits at y = 11 (the sibling screens use 10/14).
//
// The six items, `y = row + 15*item`:
//
//   item  text (VA)                                              x    y
//   0     video-mode line, built by `0x4864a0`/`0x486500` from the  38   24
//         5-dword descriptor `0x4c05c0`
//   1     `Border:` (`0x4bcf1c`)                                 38   39
//   2     `%s Gamma Correction` (`0x4bcf08`) with the name of      38   54
//         `[0x4bc6d8 + [0x5a4950]*4]` (No/Little/Some/Max)
//   3     `Reflections` (`0x4bcefc`)                             50   69
//   4     `Light Maps Off`/`Low Quality`/`High Quality`          38   84
//         (`0x4bcebc`/`0x4bcee4`/`0x4bcecc`)
//   5     `Done` (`0x4bceb4`), the only item with row 23          38   98
//
// Item 0 is the only entry to another screen: Enter/Space on it copies the
// 5-dword descriptor, writes state `0x12` (18) and opens `SELECT VIDEO MODE`
// (`0x48d74d`..`0x48d784`); the rest of the frame is unchanged by that.  It is
// also the only item whose text is not a literal: `0x4864a0` looks the
// descriptor up in the runtime mode table `0x4bfd18` (stride 20,
// `[0x4c05bc]` entries) and returns the index (0 when absent), `0x486500(buf,
// index)` formats the entry.  The table is filled by `EnumDisplayModes`/Glide
// at startup (docs/INIT.md §6.1) and cannot be reproduced offline, so the model
// carries the entry (`mode`) and the label comes from the already-proven
// `ui/video_mode_list_render.h::video_mode_list_label` — that formatter *is*
// `0x486500`.
//
// The border slider
// -----------------
// `0x486070(SLIDE)` takes plate-copy destinations, so `addr - 0x6865f8` gives
// `y*196 + x`: `0x68842c - 0x6865f8 = 39*196 + 88`, i.e. the 72x8 `SLIDE` track
// at (88, 39) — the `Border:` label row.  `0x4860b0(KNOB)` is called with
// `0x68842d + 6*[0x4bc1c8]` (`lea edx,[eax+eax*2]; lea eax,[edx*2+0x68842d]` at
// `0x485f79`..`0x485f84`), so the 8x8 knob sits at `x = 89 + 6*border`,
// y = 39; the proven range 0..9 moves it 89..143 inside the 88..159 track.
//
// Ranges (input handlers): border increment clamps at 9 (`cmp eax,0x9; jge`
// `0x48c773`) and decrement at 0 (`0x48bf12`); gamma increment clamps at 3
// (`0x48c7ba`, wraps to 0 from item 0's Enter path `0x48d78e`) and decrement at
// 0 (`0x48bf25`); light maps increment `value+1`, and `> 2` wraps to 0
// (`0x48d7c8`..`0x48d7dc`), decrement stops at 0 (`0x48bf42`); `Reflections` is
// a boolean toggled as `1 - value` (`sub ecx,[0x4aac84]` at `0x48d7b7`,
// `sub ebx,[0x4aac84]` at `0x48c7d3`).  All four are CFG fields
// (docs/CONFIG.md §3: `0x1b4`, `0x1b8`, `0x1c0`, `0x1c4`).
//
// The checkbox
// ------------
// `0x4833e0([0x4aac84], 196, 0x6894d4)` (`0x485fc0`..`0x485fd4`): anchor
// `0x6894d4 - 0x6865f8 = 61*196 + 40`, i.e. (40, 61).  The 16-row `CHECK` is
// copied at the anchor when the flag is non-zero, the 8-row `NOCHECK` **8 rows
// lower** otherwise (`lea ecx,[ecx+edi*8]` in `0x483403`), so both end on the
// `Reflections` label row 69.  Source palette index 0 is transparent
// (`test dl,dl; je` at `0x483413`).
//
// Highlight
// ---------
// `0x485dd0` switches to font 4 when `item == [0x68ccf4]` **and** the caller
// passed font 0 (`0x485e0d`..`0x485e19`), so the title (font 5) is never
// highlighted and every item line is white when its item id equals the cursor.
// There is no bar, box or `MENULIT` marker.  The cursor is an item id, so `-1`
// or 99 highlight nothing.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies the `PLATE` 196x131 panel, `SLIDE` 72x8,
// `KNOB` 8x8, `CHECK` 8x16, `NOCHECK` 8x8 (docs/UI_SCREENS.md §9) and the
// `FONTS4` faces 5/0/4.  There is no per-screen full-screen art for screen 12;
// `background` may name any `BMP.IDX` entry painted at (0,0) first (empty =
// opaque black).  As on the sibling screens the proven `PLATE` is *not* painted
// implicitly: the original copies it into `0x6865f8` as the compose buffer and
// where that copy is blitted on screen is not documented (docs/MENU_RENDER.md
// §2), so painting it at (0,0) stays the caller's choice.
//
// Colours come from the atlas palette: title font 5 -> (155,15,15) dark red,
// items font 0 -> (19,15,15) dark grey, highlight font 4 -> (239,239,239)
// white (docs/MENU_RENDER.md §3).  `SLIDE`/`KNOB`/`CHECK`/`NOCHECK`/`PLATE`
// use their own BMP palettes; index 0 is the transparent key of the control
// sprites.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/sound_options_render` and
// `ui/restart_render`: every painted pixel's palette index goes through
// `render/hud_lut.h::map(level, index)` **before** its colour is looked up.  The
// table comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  With neither, the call is a text
// refusal prefixed `graphics_options_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"graphics_options_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken `PLATE`/`SLIDE`/`KNOB`/`CHECK`/`NOCHECK` or one of the wrong
// size, a background entry that is missing/broken/not 8 bpp, and an exact fade
// without a table and a dump (or with an unreadable dump).  Out-of-range
// *values* (a cursor of 99, a border of -3 or 99, a gamma of 7, a light-map
// level of 5) are clamped, not refused: the original clamps its own ranges and
// the frame stays paintable.  On failure `rgba` is cleared, `width` and
// `height` are 0 (never a stale or half-drawn picture) and `*stats` is zeroed.
// `stats`/`error` may be null.  The call only reads the installation, is
// deterministic and does not depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the `border ±1`, the gamma and light-map steps, the `Reflections`
// toggle, Esc), UI sounds (`updown`/`vleft`, docs/UI_SOUNDS.md §3.2), the
// transitions to state 18 (`0x48d74d`) and back, the gamma application
// `0x4847b0`, the actual `GUNMETAL.CFG` round-trip, the mode switch and the
// screen-37 confirmation.  There is no `MENUARRW` call in `0x485ee0`, so
// `arrows` is always 0; the field keeps the stats shape of the sibling
// renderers.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "ui/video_mode_list_render.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kGraphicsOptionsCanvasWidth = 320;
inline constexpr int kGraphicsOptionsCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kGraphicsOptionsBackground = 0x000000FFu;

/// `0x4bcf24`, pushed at `0x485f06`.
inline constexpr std::string_view kGraphicsOptionsTitle = "GRAPHICS OPTIONS";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x485dd0` writes into its copy, so
/// this is also the text clip.
inline constexpr std::string_view kGraphicsOptionsPanelName = "PLATE";
inline constexpr int kGraphicsOptionsPanelWidth = 196;
inline constexpr int kGraphicsOptionsPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, items 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kGraphicsOptionsTitleFont = 5;
inline constexpr int kGraphicsOptionsRowFont = 0;
inline constexpr int kGraphicsOptionsSelectedFont = 4;

/// Title row (`push 0xb` at `0x485f00`), centred in the 196-px band; `x = 38`
/// (`push 0x26` at `0x485f02`) is the ignored fallback of the centred branch.
inline constexpr int kGraphicsOptionsTitleY = 11;
inline constexpr int kGraphicsOptionsTitleX = 38;

/// `rows = 6` (`0x485ee3`), step 15 (`0x485eed`).
inline constexpr int kGraphicsOptionsRows = 6;
inline constexpr int kGraphicsOptionsItemStep = 15;

/// Row immediates: 24 (`push 0x18`) for items 0..4, 23 (`push 0x17` at
/// `0x486031`) for `Done`; item 0 lands on y = 24.
inline constexpr int kGraphicsOptionsLabelRow = 24;
inline constexpr int kGraphicsOptionsDoneRow = 23;

/// Item ids of the six lines.
inline constexpr int kGraphicsOptionsModeItem = 0;
inline constexpr int kGraphicsOptionsBorderItem = 1;
inline constexpr int kGraphicsOptionsGammaItem = 2;
inline constexpr int kGraphicsOptionsReflectionsItem = 3;
inline constexpr int kGraphicsOptionsLightMapsItem = 4;
inline constexpr int kGraphicsOptionsDoneItem = 5;

/// `x = 38` (`push 0x26`) for everything except `Reflections` (`push 0x32` at
/// `0x485fe0`).
inline constexpr int kGraphicsOptionsItemX = 38;
inline constexpr int kGraphicsOptionsReflectionsLabelX = 50;

/// Border: `.data` `0x4bc1c8` = 3, proven 0..9, step 1 (`0x48c773`/`0x48bf12`).
inline constexpr int kGraphicsOptionsBorderMin = 0;
inline constexpr int kGraphicsOptionsBorderMax = 9;
inline constexpr int kGraphicsOptionsBorderStep = 1;
inline constexpr int kGraphicsOptionsBorderDefault = 3;

/// `SLIDE` 72x8 at (88, 39) (`0x68842c` minus `0x6865f8`), `KNOB` 8x8 with base
/// column 89 (`0x68842d`), 6 px per border step.  No horizontal scroll.
inline constexpr int kGraphicsOptionsSlideWidth = 72;
inline constexpr int kGraphicsOptionsSlideHeight = 8;
inline constexpr int kGraphicsOptionsSlideX = 88;
inline constexpr int kGraphicsOptionsSlideY = 39;
inline constexpr int kGraphicsOptionsKnobWidth = 8;
inline constexpr int kGraphicsOptionsKnobHeight = 8;
inline constexpr int kGraphicsOptionsKnobBaseX = 89;
inline constexpr int kGraphicsOptionsKnobPixelsPerStep = 6;

/// Gamma: `.bss` `0x5a4950` = 0 before `GUNMETAL.CFG`, proven 0..3; the names
/// are the four pointers of `0x4bc6d8` (`0x4bc7a4`/`0x4bc79c`/`0x4bc794`/
/// `0x4bc790`).
inline constexpr int kGraphicsOptionsGammaMin = 0;
inline constexpr int kGraphicsOptionsGammaMax = 3;
inline constexpr int kGraphicsOptionsGammaDefault = 0;
inline constexpr int kGraphicsOptionsGammaShippedCfg = 1;
inline constexpr std::string_view kGraphicsOptionsGammaFormat = "%s Gamma Correction";
inline constexpr std::array<std::string_view, 4> kGraphicsOptionsGammaNames = {
    "No",     // 0x4bc7a4
    "Little", // 0x4bc79c
    "Some",   // 0x4bc794
    "Max",    // 0x4bc790
};

/// `.data` `0x4aac84` = 1; a boolean (0 -> `NOCHECK`, non-zero -> `CHECK`).
inline constexpr int kGraphicsOptionsReflectionsDefault = 1;

/// Light maps: `.data` `0x4aac88` = 1, shipped CFG = 2, proven 0..2
/// (`0x48d7c8` wraps above 2).
inline constexpr int kGraphicsOptionsLightMapsMin = 0;
inline constexpr int kGraphicsOptionsLightMapsMax = 2;
inline constexpr int kGraphicsOptionsLightMapsDefault = 1;
inline constexpr int kGraphicsOptionsLightMapsShippedCfg = 2;

/// `CHECK` 8x16 / `NOCHECK` 8x8, anchor (40, 61), the unchecked box 8 rows
/// lower (`0x4833e0`, `0x6894d4`).
inline constexpr int kGraphicsOptionsCheckX = 40;
inline constexpr int kGraphicsOptionsCheckY = 61;
inline constexpr int kGraphicsOptionsCheckWidth = 8;
inline constexpr int kGraphicsOptionsCheckHeight = 16;
inline constexpr int kGraphicsOptionsNoCheckWidth = 8;
inline constexpr int kGraphicsOptionsNoCheckHeight = 8;
inline constexpr int kGraphicsOptionsNoCheckDrop = 8;

/// Model of item 0: the current mode descriptor `0x4c05c0` (5 dwords, only
/// `width`/`height`/`bpp`/`flags` reach `0x486500`).  The shipped
/// `GUNMETAL.CFG` carries 800x600 and the 3dfx build enumerates the Glide set
/// with flags `0x804`, so the default is the first entry of the proven
/// formatter's model list (docs/VIDEO_MODE_LIST_RENDER.md §2); the *table* is
/// runtime data, only the label formatter is proven.
inline constexpr VideoModeListEntry kGraphicsOptionsModeDefault = {800, 600, 32, 0x804};

/// Neutral cursor: `[0x68ccf4]` is 0 in `.bss`; the entry into state 12
/// (`0x48cfa0`) does not write it.
inline constexpr int kGraphicsOptionsDefaultCursor = 0;

/// Labels, verbatim from the VAs named at each constant.
inline constexpr std::string_view kGraphicsOptionsBorderLabel = "Border:";       // 0x4bcf1c
inline constexpr std::string_view kGraphicsOptionsReflectionsLabel = "Reflections"; // 0x4bcefc
inline constexpr std::string_view kGraphicsOptionsDoneLabel = "Done";           // 0x4bceb4
inline constexpr std::array<std::string_view, 3> kGraphicsOptionsLightMapsLabels = {
    "Light Maps Off",           // 0x4bcebc
    "Light Maps Low Quality",   // 0x4bcee4
    "Light Maps High Quality",  // 0x4bcecc
};

/// Optional parts of one frame.
struct GraphicsOptionsRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 12, but it is
    /// never painted implicitly (see the header comment).
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** (0..5).  The comparison is the
    /// exact one of `0x485e0d`, so values outside the drawn ids highlight
    /// nothing (`-1` = a frame without a highlight).
    int cursor = kGraphicsOptionsDefaultCursor;
    /// The 5-dword descriptor `0x4c05c0` as `0x486500` reads it; only
    /// `width`/`height`/`bpp`/`flags` reach the label.  Default = the model
    /// above.
    VideoModeListEntry mode = kGraphicsOptionsModeDefault;
    /// `[0x4bc1c8]` border, clamped to the proven 0..9.
    int border = kGraphicsOptionsBorderDefault;
    /// `[0x5a4950]` gamma index, clamped to the proven 0..3.
    int gamma = kGraphicsOptionsGammaDefault;
    /// `[0x4aac84]` `Reflections`: non-zero paints `CHECK`, zero `NOCHECK`.
    int reflections = kGraphicsOptionsReflectionsDefault;
    /// `[0x4aac88]` light maps, clamped to the proven 0..2.
    int light_maps = kGraphicsOptionsLightMapsDefault;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/sound_options_render`, docs/SOUND_OPTIONS_RENDER.md
    /// §2).  Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `graphics_options_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten).
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read
    /// with `load_hud_lut`.  Used when `fade_lut_table` is null.
    std::string fade_lut_dump;
    /// A table already in memory (e.g. `build_hud_lut` output); wins over
    /// `fade_lut_dump`.  Only read during the call, the caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the handler's `0x67f10c` value (always 6); `checks` the
/// `CHECK`/`NOCHECK` box (always 1); `sliders` the drawn `SLIDE`/`KNOB` pair
/// (always 1, the border track); `arrows` the number of `MENUARRW` sprites
/// painted — screen 12 has no arrow call, so it is always 0 and the test pins
/// the 0; `ink_pixels` the canvas pixels covered by at least one glyph or
/// sprite pixel (unique; the optional backdrop panel is not counted, the same
/// rule as `ui/menu_render`/`ui/sound_options_render`); `missing_glyphs` the
/// characters with no glyph in the atlas (skipped, never fatal).
struct GraphicsOptionsRenderStats {
    int rows = 0;
    int checks = 0;
    int sliders = 0;
    int arrows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
};

/// `y` of item `item`: `kGraphicsOptionsLabelRow + 15*item`, except `Done`
/// (item 5), which passes the row immediate 23: 0 -> 24, 1 -> 39, 2 -> 54,
/// 3 -> 69, 4 -> 84, 5 -> 98.
int graphics_options_item_y(int item);

/// `x` of item `item`: 50 for `Reflections` (item 3), 38 otherwise.
int graphics_options_item_x(int item);

/// Clamp a stored border to the proven 0..9 (`0x48c773`/`0x48bf12`).
int graphics_options_border(int requested);

/// Clamp a stored gamma index to the proven 0..3 (`0x48c7ba`/`0x48bf25`).
int graphics_options_gamma(int requested);

/// Clamp a stored light-map level to the proven 0..2 (`0x48d7c8`).
int graphics_options_light_maps(int requested);

/// `CHECK`/`NOCHECK` decision of `0x4833e0`: non-zero paints `CHECK`.
bool graphics_options_reflections_on(int reflections);

/// Name of gamma index `index` (clamped to 0..3) from `0x4bc6d8`.
std::string_view graphics_options_gamma_name(int index);

/// Label of light-map level `value` (clamped to 0..2) from
/// `0x4bcebc`/`0x4bcee4`/`0x4bcecc`.
std::string_view graphics_options_light_maps_label(int value);

/// Left column of the border `KNOB`: `89 + 6*border` (proven arithmetic of
/// `0x485f79`..`0x485f84`); 0/3/9 give 89/107/143.
int graphics_options_knob_x(int border);

/// The whole line of item `item` (0..5) as the handler builds it: the
/// `0x486500` mode label, `Border:`, `<gamma> Gamma Correction`,
/// `Reflections`, the light-map label, `Done`.  Out-of-range ids return
/// `Done` (the handler's fall-through).
std::string graphics_options_item_text(const GraphicsOptionsRenderOptions& options,
                                       int item);

/// Paint screen 12 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The frame is painted in the handler's emission order: title (font 5,
/// centred, y 11), then items 0..5 at `y = 24 + 15*item` (`Done` at 98, x 38
/// except `Reflections` at 50) with the cursor item in font 4; the border
/// `SLIDE` at (88, 39) and `KNOB` at (`graphics_options_knob_x(border)`, 39);
/// the `CHECK`/`NOCHECK` box at (40, 61), the unchecked sprite 8 rows lower.
/// Every glyph and sprite is clipped to the 196x131 text band.  With
/// `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.  Returns false +
/// `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_graphics_options(const std::filesystem::path& game,
                             const GraphicsOptionsRenderOptions& options,
                             std::vector<std::uint32_t>& rgba, int& width, int& height,
                             GraphicsOptionsRenderStats* stats = nullptr,
                             std::string* error = nullptr);

} // namespace gm
