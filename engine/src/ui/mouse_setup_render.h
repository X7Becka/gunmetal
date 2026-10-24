#pragma once

// One frame of screen 22, `MOUSE SETUP` (`M8.8` slice 8).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/quit_render` 33,
// `ui/game_type_render` 19, `ui/difficulty_render` 23, `ui/local_players_render`
// 21, `ui/video_mode_render` 37, `ui/restart_render` 6): the caller owns the
// state (the cursor `0x68ccf4`, the "used by" index `0x5a4938`, the axis
// bindings `0x68cd70/74/78`, the sensitivities `0x4aac38/40/48` and the four
// button bindings `0x4aac10..1c`) and this module paints it into the 320x200
// mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads the
// keyboard, `KEYCFG`, `GUNMETAL.CFG`, sounds or saves.
//
// Proven geometry (`0x489a10`, docs/UI_SCREENS.md §2/§3.5)
// -------------------------------------------------------
// The handler is `sub esp,0x3c` .. `ret` at `0x489d57`; it writes `rows = 12`
// (`0x489a13` -> `0x67f10c`) and `step = 8` (`0x489a1d` -> `0x662d58`) before
// copying `PLATE` (`0x489a27` -> `0x486050`, the 196x131 compose buffer
// `0x6865f8`, so 196x131 is the hard text clip).  `0x485dd0` is called 12 times
// for items plus once for the title; every item passes `row = 0x18` (24), so
// `y = 24 + 8*item`:
//
//   item  text (VA)                                          x    y
//   0     `Used By: %s` (`0x4bd8b4`)                         18   24
//   1     `Left/Right: %s` (`0x4bd8a4`)                       9   32
//   2     `Sensitivity:` (`0x4bd894`)                         9   40
//   3     `Forward/Back: %s` (`0x4bd880`)                     9   48
//   4     `Sensitivity:` (`0x4bd894`)                         9   56
//   5     `Up/Down (3D): %s` (`0x4bd86c`)                     9   64
//   6     `Sensitivity:` (`0x4bd894`)                         9   72
//   7     `Button 1: %s` (`0x4bd85c`) / `Button 1: NOT USED` (`0x4bd848`)   9  80
//   8     `Button 2: %s` (`0x4bd838`) / `... NOT USED` (`0x4bd824`)        9  88
//   9     `Button 3: %s` (`0x4bd814`) / `... NOT USED` (`0x4bd800`)        9  96
//   10    `Button 4: %s` (`0x4bd7f0`) / `... NOT USED` (`0x4bd7dc`)        9 104
//   11    `Done` (`0x4bceb4`)                                9  112
//
// The title is `0x485dd0(font=5, text=0x4bd8c0 "MOUSE SETUP", centered=1,
// x=18, row=11, item=-1)` (`0x489a2c`..`0x489a3b`): `centered` replaces `x` by
// `(196 - width)/2` (`0x485de2`), `item == -1` makes the row absolute, so the
// title is centred at y = 11 — two pixels higher than the sibling screens
// (14/10).  The `x = 18` argument is the unused fallback of the centred branch.
//
// Highlight: `0x485dd0` switches the face of the caller to font 4 when
// `item == [0x68ccf4]` (`0x485e0d`..`0x485e19`).  No bar, box or `MENULIT`
// marker is drawn.  The cursor is an **item id** 0..11 (the click handler
// `0x48d502` clamps with `cmp eax,0xb; ja`), so out-of-range values highlight
// nothing.
//
// Sliders (`0x489ac9`, `0x489b4d`, `0x489bd1`)
// --------------------------------------------
// Three masked sprites: `0x486070(SLIDE)` at `0x688501`/`0x689141`/`0x689d81`
// and `0x4860b0(KNOB)` at `0x688504`/`0x689144`/`0x689d84`.  Those are
// destination addresses in the plate copy, so `addr - 0x6865f8` gives
// `y*196 + x`:
//
//   SLIDE  (105, 40) (105, 56) (105, 72)   72x8 track
//   KNOB   x = 108 ... 108, same y         8x8, fully opaque (no index 0)
//
// The knob column is **not** the §3.5 integer model of
// `ui/menu_render.cpp::slider_knob_x` (which would start at the track's x and
// travel 64 px): the engine computes it from a `double`
// (`0x489ad3`..`0x489af5`, identical for all three axes)
//
//     x = 108 - trunc((value - 0.2) * -27.0) = 108 + trunc(27 * (value - 0.2))
//
// with `0x4a2930` = 0.2 (minimum), `0x4a2948` = -27.0 (pixels per unit) and
// `0x40e0e0` = `fistp` with RC = truncate (control word `or ah,0x0c`).  The
// range is proven by the click handlers: decrement clamps at 0.2 (`0x48c37d`
// `fcomp 0x4a2930`), increment clamps at 2.2 (`0x48cc91` `fcomp 0x4a2970`),
// the step is 0.1 (`0x4a2960`/`0x4a2968`), so 0.2..2.2 in 0.1 steps moves the
// knob 108..162 inside the 105..176 track (54 px, never outside).  The API
// carries the value in **tenths** (2..22), because the engine's own step is
// 0.1 and every reachable position is an integer number of tenths.
//
// `slider_knob_x` itself is a `static` helper in an anonymous namespace of
// `ui/menu_render.cpp` (line 305), so it cannot be called from this
// translation unit without editing that module — which this slice forbids.
// `mouse_setup_knob_x` therefore restates the same linear interpolation rule
// with the constants that are proven for this screen; the test pins the
// numbers (2/8/10/22 -> 108/124/129/162) and docs/MOUSE_SETUP_RENDER.md §2
// records the divergence from the §3.5 model (105/169).
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies the `PLATE` 196x131 panel, `SLIDE` 72x8 and
// `KNOB` 8x8 (docs/UI_SCREENS.md §9), plus the `FONTS4` faces 5/0/4.  There is
// no per-screen full-screen art for screen 22; `background` may name any
// `BMP.IDX` entry painted at (0,0) first (empty = opaque black).  As on
// screens 6/13/14/17/19/21/23/33/37 the proven `PLATE` is *not* painted
// implicitly: the original copies it into `0x6865f8` as the compose buffer, and
// where that copy is blitted on screen is not documented (docs/MENU_RENDER.md
// §2), so painting it at (0,0) stays the caller's choice.
//
// Colours come from the atlas palette: title font 5 -> (155,15,15) dark red,
// items font 0 -> (19,15,15) dark grey, highlight font 4 -> (239,239,239)
// white (docs/MENU_RENDER.md §3).  `SLIDE`/`KNOB`/`PLATE` use their own BMP
// palettes; index 0 is the transparent key of the two sprites.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/restart_render` and `ui/menu_render`
// slice 8: every painted pixel's palette index goes through
// `render/hud_lut.h::map(level, index)` **before** its colour is looked up.  The
// table comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  With neither, the call is a text
// refusal prefixed `mouse_setup_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"mouse_setup_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken `SLIDE`/`KNOB` or one of the wrong size, a background entry
// that is missing/broken/not 8 bpp, and an exact fade without a table and a
// dump (or with an unreadable dump).  Out-of-range *values* (a cursor of 99, a
// "used by" of 7, a sensitivity of 100) are clamped, not refused: the original
// wraps its own indices (`idiv 3` at `0x48c31b`, `cmp eax,0x11` at `0x48c536`)
// and the screen stays paintable.  On failure `rgba` is cleared, `width` and
// `height` are 0 (never a stale or half-drawn picture) and `*stats` is zeroed.
// `stats`/`error` may be null.  The call only reads the installation, is
// deterministic and does not depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the click table `0x48e8b0`, the 12 actions, `Esc` -> state 10 at
// `0x48e599`), UI sounds, the actual `GUNMETAL.CFG`/`KEYCFG` round-trip and the
// `double` sensitivities as such (they are modelled in tenths).  There is no
// `MENUARRW` call in `0x489a10`, so `arrows` is always 0; the field keeps the
// stats shape of the sibling renderers.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kMouseSetupCanvasWidth = 320;
inline constexpr int kMouseSetupCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kMouseSetupBackground = 0x000000FFu;

/// `0x4bd8c0`, pushed at `0x489a34`.
inline constexpr std::string_view kMouseSetupTitle = "MOUSE SETUP";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x485dd0` writes into its copy, so
/// this is also the text clip.
inline constexpr std::string_view kMouseSetupPanelName = "PLATE";
inline constexpr int kMouseSetupPanelWidth = 196;
inline constexpr int kMouseSetupPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, items 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kMouseSetupTitleFont = 5;
inline constexpr int kMouseSetupRowFont = 0;
inline constexpr int kMouseSetupSelectedFont = 4;

/// Title row (`push 0xb` at `0x489a2e`), centred in the 196-px band; `x = 18`
/// (`push 0x12` at `0x489a30`) is the fallback argument of the centred branch.
inline constexpr int kMouseSetupTitleY = 11;
inline constexpr int kMouseSetupTitleX = 18;

/// `rows = 12` (`0x489a13`), `step = 8` (`0x489a1d`), base row 24
/// (`push 0x18` before every item call): `y = 24 + 8*item`.
inline constexpr int kMouseSetupRows = 12;
inline constexpr int kMouseSetupItemStep = 8;
inline constexpr int kMouseSetupItemFirstY = 24;

/// `x = 18` for item 0 (`0x489a6a`), `x = 9` for items 1..11
/// (`0x489aa3`/`0x489ab6`/...).
inline constexpr int kMouseSetupFirstItemX = 18;
inline constexpr int kMouseSetupItemX = 9;

/// `SLIDE` 72x8 at x 105 (`0x688501`/`0x689141`/`0x689d81` minus `0x6865f8`)
/// and y 40/56/72; `KNOB` 8x8, base column 108 (`0x688504`/...), fully opaque.
inline constexpr int kMouseSetupSlideWidth = 72;
inline constexpr int kMouseSetupSlideHeight = 8;
inline constexpr int kMouseSetupKnobWidth = 8;
inline constexpr int kMouseSetupKnobHeight = 8;
inline constexpr int kMouseSetupSlideX = 105;
inline constexpr int kMouseSetupKnobBaseX = 108;
inline constexpr int kMouseSetupSliderCount = 3;
inline constexpr std::array<int, kMouseSetupSliderCount> kMouseSetupSlideY = {40, 56, 72};

/// Sensitivity range and step of `0x4aac38/40/48`: min `0x4a2930` = 0.2, max
/// `0x4a2970` = 2.2, step `0x4a2960`/`0x4a2968` = 0.1.  Carried in tenths.
inline constexpr int kMouseSetupSensitivityMin = 2;
inline constexpr int kMouseSetupSensitivityMax = 22;
inline constexpr int kMouseSetupSensitivityStep = 1;

/// `.data` initial values of `0x4aac38`/`0x4aac40`/`0x4aac48` (1.0/0.8/0.8);
/// the CFG load (`0x4850fc`..) overwrites them and clamps invalid values to 0.2.
inline constexpr std::array<int, kMouseSetupSliderCount> kMouseSetupSensitivityDefault = {
    10, 8, 8};

/// `0x4a2948` = -27.0 pixels per unit of sensitivity (the knob moves **right**
/// as the value grows because the multiplier is negative and the site uses
/// `sub`); `0x40e0e0` truncates toward zero.
inline constexpr int kMouseSetupKnobPixelsPerUnit = 27;

/// `Used By` names, `0x4bc548` (3 pointers, cycled by `idiv 3` at `0x48c31b`).
inline constexpr std::string_view kMouseSetupUsedByNames[3] = {
    "Not Used",  // 0x4bca50
    "Player 1",  // 0x4bc944
    "Player 2",  // 0x4bc938
};

/// Axis bindings, `0x4bc468` (18 records of 12 bytes; the first dword is the
/// name, the handler reads it at `0x489a85`/`0x489b09`/`0x489b8d`).
inline constexpr std::string_view kMouseSetupAxisNames[18] = {
    "Not Used",         // 0x4bca50
    "Forward/Back",     // 0x4bca40
    "Accel Turn L/R",   // 0x4bca30
    "Accel Turn R/L",   // 0x4bca20
    "Linear Turn L/R",  // 0x4bca10
    "Linear Turn R/L",  // 0x4bca00
    "Strafe L/R",       // 0x4bc9f4
    "Strafe R/L",       // 0x4bc9e8
    "Look Up/Down",     // 0x4bc9d8
    "Look Down/Up",     // 0x4bc9c8
    "Speed +/- Slide",  // 0x4bc9b8
    "Speed -/+ Slide",  // 0x4bc9a8
    "Speed +/- Nudge",  // 0x4bc998
    "Speed -/+ Nudge",  // 0x4bc988
    "Weapons +/-",      // 0x4bc97c
    "Weapons -/+",      // 0x4bc970
    "Equip Cycle +/-",  // 0x4bc960
    "Equip Cycle -/+",  // 0x4bc950
};

/// Button bindings, `0x4bc558` (31 key names, docs/UI_SCREENS.md §12.1); the
/// handler reads it at `0x489c0e`/`0x489c5c`/`0x489caa`/`0x489cf8`.
inline constexpr std::string_view kMouseSetupKeyNames[31] = {
    "Move Forward",        // 0
    "Move Backward",       // 1
    "Turn Left",           // 2
    "Turn Right",          // 3
    "Look Center",         // 4
    "Fire",                // 5
    "Look Up",             // 6
    "Look Down",           // 7
    "Strafe",              // 8
    "Strafe Left",         // 9
    "Strafe Right",        // 10
    "Throttle Up",         // 11
    "Throttle Down",       // 12
    "Next Weapon",         // 13
    "Previous Weapon",     // 14
    "Activate",            // 15
    "Next Equipment",      // 16
    "Previous Equip.",     // 17
    "Toggle Equip. Use",   // 18
    "Headlights",          // 19
    "Force Field",         // 20
    "Mine Detector",       // 21
    "Targeting",           // 22
    "Warp Device",         // 23
    "Radar",               // 24
    "Mine Layer",          // 25
    "Combat Block",        // 26
    "Auto Sentry",         // 27
    "Decoy",               // 28
    "Linear Left",         // 29
    "Linear Right",        // 30
};

/// `.data` initial values of the four button bindings `0x4aac10..0x4aac1c`
/// (0, 1, 5, 15 = `Move Forward`, `Move Backward`, `Fire`, `Activate`); the CFG
/// load overwrites them.  `-1` is the handler's "no binding" sentinel
/// (`test eax,eax; jl` at `0x489c0a`/`0x489c58`/`0x489ca6`/`0x489cf4`).
inline constexpr std::array<int, 4> kMouseSetupButtonsDefault = {0, 1, 5, 15};

/// The `.bss` initial value of the axis bindings `0x68cd70/74/78` and of the
/// "used by" index `0x5a4938` is 0, i.e. the `Not Used` entry of both tables.
inline constexpr int kMouseSetupDefaultUsedBy = 0;
inline constexpr std::array<int, kMouseSetupSliderCount> kMouseSetupAxesDefault = {0, 0, 0};

/// Neutral cursor: `[0x68ccf4]` is not written on entry into state 22
/// (`0x48d292`), so there is no proven "first frame" cursor; 0 is the first row.
inline constexpr int kMouseSetupDefaultCursor = 0;

/// Optional parts of one frame.
struct MouseSetupRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 22, but it is
    /// never painted implicitly (see the header comment).
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** 0..11, not a text line.  The
    /// comparison is the exact one of `0x485e0d`, so values outside the range
    /// highlight nothing (`-1` = a frame without a highlight).
    int cursor = kMouseSetupDefaultCursor;
    /// `[0x5a4938]`, index into `kMouseSetupUsedByNames` (clamped to 0..2).
    int used_by = kMouseSetupDefaultUsedBy;
    /// `[0x68cd70]`/`[0x68cd74]`/`[0x68cd78]`, indices into
    /// `kMouseSetupAxisNames` (clamped to 0..17): items 1, 3 and 5.
    std::array<int, kMouseSetupSliderCount> axes = kMouseSetupAxesDefault;
    /// `[0x4aac38]`/`[0x4aac40]`/`[0x4aac48]` in **tenths**, clamped to 2..22;
    /// the `.data` defaults are 1.0/0.8/0.8.
    std::array<int, kMouseSetupSliderCount> sensitivity = kMouseSetupSensitivityDefault;
    /// `[0x4aac10]`..`[0x4aac1c]`, indices into `kMouseSetupKeyNames` (clamped
    /// to -1..30; `-1` prints `Button N: NOT USED`).
    std::array<int, 4> buttons = kMouseSetupButtonsDefault;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/restart_render`, docs/RESTART_RENDER.md §2).  Needs a
    /// table, either pre-loaded (`fade_lut_table`) or read from `fade_lut_dump`;
    /// without both the call fails with `mouse_setup_render: fade LUT needs ...`.
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
/// `rows` is the fixed `0x67f10c` value 12; `sliders` the three `SLIDE`/`KNOB`
/// pairs; `ink_pixels` the canvas pixels covered by at least one glyph or sprite
/// pixel (unique; the optional backdrop panel is not counted, the same rule as
/// `ui/menu_render`/`ui/restart_render`); `missing_glyphs` the characters with no
/// glyph in the atlas (skipped, never fatal); `arrows` the number of `MENUARRW`
/// sprites painted — screen 22 has no arrow call at `0x489a10`, so it is always 0
/// and the test pins the 0.
struct MouseSetupRenderStats {
    int rows = 0;
    int sliders = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// `y` of item `item` (`kMouseSetupItemFirstY + kMouseSetupItemStep * item`).
int mouse_setup_item_y(int item);

/// `x` of item `item` (18 for item 0, 9 for the rest).
int mouse_setup_item_x(int item);

/// Clamp a sensitivity in tenths to the proven 2..22.
int mouse_setup_sensitivity_tenths(int requested);

/// Left column of the `KNOB` of a sensitivity slider.
///
/// Proven arithmetic of `0x489ad3`..`0x489af5`: `108 + trunc(27*(v-0.2))` with
/// `v` in 0.2..2.2; see the header comment for the constants and for the
/// divergence from the §3.5 integer model.  `sensitivity_tenths` is clamped, so
/// 2/8/10/22 give 108/124/129/162.
int mouse_setup_knob_x(int sensitivity_tenths);

/// The label of item `item` for `options` (the handler's `%s` formatting):
/// `Used By: ...`, `Left/Right: ...`, `Sensitivity:`, `Forward/Back: ...`,
/// `Up/Down (3D): ...`, `Button N: ...`/`Button N: NOT USED`, `Done`.  An
/// out-of-range `item` returns `Done`'s text.
std::string mouse_setup_label(int item, const MouseSetupRenderOptions& options);

/// Paint screen 22 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kMouseSetupTitleY` (font 5); item `item` sits at
/// (`mouse_setup_item_x(item)`, `mouse_setup_item_y(item)`) with
/// `mouse_setup_label(item, options)` (font 0); the item equal to
/// `options.cursor` uses font 4 instead.  The three sensitivity sliders paint
/// `SLIDE` at (`kMouseSetupSlideX`, `kMouseSetupSlideY[i]`) and `KNOB` at
/// (`mouse_setup_knob_x(options.sensitivity[i])`, same y).  Every glyph and
/// sprite is clipped to the 196x131 text band.  With `options.fade_lut` every
/// painted palette index goes through `render/hud_lut.h::map` at the level
/// before its colour is looked up; the untouched black canvas has no index and
/// stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_mouse_setup(const std::filesystem::path& game,
                        const MouseSetupRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        MouseSetupRenderStats* stats = nullptr,
                        std::string* error = nullptr);

} // namespace gm
