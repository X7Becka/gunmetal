#pragma once

// One frame of screen 11, `SOUND OPTIONS` (`M8.8` slice 18).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/mouse_setup_render`
// 22, `ui/match_setup_render` 27, `ui/game_type_render` 19, `ui/restart_render`
// 6): the caller owns the state (the cursor `0x68ccf4`, the two volumes
// `0x4aac74`/`0x4aac78`, the swap flag `0x5a494c`, the sound-enabled flag
// `0x4aa384` and the "music disabled" flag `0x68cc9c`) and this module paints it
// into the 320x200 mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here
// reads the keyboard, `GUNMETAL.CFG`, `KEYCFG`, sounds or saves, and nothing is
// written back.
//
// Proven geometry (`0x4889f0`, docs/UI_SCREENS.md §2/§3.2)
// -------------------------------------------------------
// The dispatcher table `0x4883d8` sends state 11 to the stub `0x487eb6`, whose
// `call 0x4889f0` is the handler (`ret` at `0x488b28` for the sound-on path,
// `0x488b99` for the sound-off path; no prologue, no locals).  It first copies
// `PLATE` (`0x4889f0` -> `0x486050`, the 196x131 compose buffer `0x6865f8`,
// so 196x131 is the hard text clip), then draws the title
// `0x485dd0(font=5, "SOUND OPTIONS" `0x4bd534`, centered=1, x=38, row=14,
// item=-1)` (`0x4889f5`..`0x488a04`): `centered` replaces `x` by
// `(196 - width)/2` (`0x485de2`), `item == -1` makes `row` absolute, so the
// title sits at y = 14 (the sibling screens use 11/10/14).
//
// `[0x4aa384]` (sound enabled, `.data` = 1) selects two layouts:
//
//   * `[0x4aa384] == 0` (`0x488b29`..`0x488b99`): `rows = 1` (`0x67f10c`),
//     `step = 0` (`0x662d58`), four lines at x = 18 with the row immediates
//     27/37/47/57 and item ids 2/3/4/5:
//
//       item  text (VA)                            x    y
//       2     `Sound is disabled.` (`0x4bd4d0`)     18   27
//       3     `Rerun Gunmetal without the` (`0x4bd4b4`) 18 37
//       4     `N command line option to` (`0x4bd498`)   18 47
//       5     `enjoy sound.` (`0x4bd488`)          18   57
//
//     (the literal bytes at `0x4bd498` start with `N`; the line is the
//     `-nosound` hint of the original and is reproduced verbatim).  No
//     `SLIDE`/`KNOB`/`CHECK` call happens in this branch.
//
//   * `[0x4aa384] != 0` (`0x488a19`..`0x488b28`): `rows = 7`
//     (`0x488a1e`), `step = 10` (`0x488a2a`); the label row arguments are 27
//     for items 0/2/4 and 24 for `Done`, so `y = row + 10*item`:
//
//       item  drawn text                                       x    y
//       0     `Music Volume:` (`0x4bd508`), or
//             `Music Disabled: Low Memory` (`0x4bd518`)        18   27
//       2     `Effects Volume:` (`0x4bd4f8`)                   18   47
//       4     `Switch Left Right` (`0x4bd4e4`)                30   67
//       6     `Done` (`0x4bceb4`)                             18   84
//
//     `[0x68cc9c]` (`.bss`, set to 1 when the music subsystem starts without
//     enough memory: `0x491273` after `cmp [0x4bf290],0xe; jl`) replaces the
//     music slider with the `Music Disabled: Low Memory` line; the effects
//     slider is always drawn.
//
// Sliders (`0x488a45`/`0x488a93`) and the checkbox (`0x488af2`)
// --------------------------------------------------------------
// `0x486070(SLIDE)` and `0x4860b0(KNOB)` take plate-copy destinations, so
// `addr - 0x6865f8` gives `y*196 + x`:
//
//   SLIDE  music   0x688290 -> (68, 37)   72x8 track
//   SLIDE  effects 0x6891e0 -> (68, 57)   72x8 track
//   KNOB   music   0x688291 -> (69, 37)   8x8, fully opaque (no index 0)
//   KNOB   effects 0x6891e1 -> (69, 57)   8x8
//
// The knob column is computed from the volume by `0x488a4f`..`0x488a73`
// (music) and `0x488a9d`..`0x488ac1` (effects), identical arithmetic:
//
//     ecx = 27 * volume
//     edx = (0x66666667 * ecx) >> 32 ; edx >>= 2     ; signed /10 magic
//     x   = 0x688291 - 0x6865f8 + edx = 69 + trunc(27*volume/10)
//
// The range is proven by the input handler: increment clamps at `0x14` = 20
// (`cmp eax,0x14; jge` at `0x48c711` music, `0x48c6da` effects), decrement at 0
// (`cmp eax,0; jle` at `0x48beb9` music, `0x48be83` effects), step 1.  So
// 0..20 (not the 0..32 of the raw CFG field) moves the knob 69..123 inside the
// 68..139 track.  The engine's own audio scale is `5*volume` (`lea eax,
// [eax+eax*4]` before `0x40b860`/`0x40b900`), i.e. the mixer sees 0..100.
//
// The checkbox is `0x4833e0([0x5a494c], 196, 0x689338)`: anchor
// `0x689338 - 0x6865f8 = (20, 59)`; the 16-row `CHECK` is copied at the anchor
// when the flag is non-zero, the 8-row `NOCHECK` **8 rows lower** otherwise
// (`lea ecx,[ecx+edi*8]`), so both end on the label row 67.  Source palette
// index 0 is transparent (`test dl,dl; je` at `0x483413`).
//
// Highlight
// ---------
// `0x485dd0` switches to font 4 when `item == [0x68ccf4]` **and** the caller
// passed font 0 (`0x485e0d`..`0x485e19`), so the title (font 5) is never
// highlighted and every item line is white when its item id equals the cursor.
// There is no bar, box or `MENULIT` marker.  The cursor is an item id, so
// `-1`/99 highlight nothing; the sound-off branch compares against ids 2..5.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies the `PLATE` 196x131 panel, `SLIDE` 72x8,
// `KNOB` 8x8, `CHECK` 8x16, `NOCHECK` 8x8 (docs/UI_SCREENS.md §9) and the
// `FONTS4` faces 5/0/4.  There is no per-screen full-screen art for screen 11;
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
// The same index -> index path as `ui/mouse_setup_render` and
// `ui/restart_render`: every painted pixel's palette index goes through
// `render/hud_lut.h::map(level, index)` **before** its colour is looked up.  The
// table comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  With neither, the call is a text
// refusal prefixed `sound_options_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"sound_options_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken `PLATE`/`SLIDE`/`KNOB`/`CHECK`/`NOCHECK` or one of the wrong
// size, a background entry that is missing/broken/not 8 bpp, and an exact fade
// without a table and a dump (or with an unreadable dump).  Out-of-range
// *values* (a cursor of 99, a volume of -3 or 99, a swap flag of 7) are
// clamped, not refused: the original clamps its volumes (0..20) and treats the
// swap flag as a boolean, and the frame stays paintable.  On failure `rgba` is
// cleared, `width` and `height` are 0 (never a stale or half-drawn picture) and
// `*stats` is zeroed.  `stats`/`error` may be null.  The call only reads the
// installation, is deterministic and does not depend on the current working
// directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the volume `+/-1`, the swap toggle `0x48be13`, `Esc`), UI sounds
// (`updown`/`vleft`, docs/UI_SOUNDS.md §3.2), the actual `GUNMETAL.CFG`
// round-trip, the mixer calls `0x40b860`/`0x40b900` with `5*volume`, and the
// `5*volume` scale itself (the API carries the stored 0..20).  There is no
// `MENUARRW` call in `0x4889f0`, so `arrows` is always 0; the field keeps the
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
inline constexpr int kSoundOptionsCanvasWidth = 320;
inline constexpr int kSoundOptionsCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kSoundOptionsBackground = 0x000000FFu;

/// `0x4bd534`, pushed at `0x4889fd`.
inline constexpr std::string_view kSoundOptionsTitle = "SOUND OPTIONS";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x485dd0` writes into its copy, so
/// this is also the text clip.
inline constexpr std::string_view kSoundOptionsPanelName = "PLATE";
inline constexpr int kSoundOptionsPanelWidth = 196;
inline constexpr int kSoundOptionsPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, items 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kSoundOptionsTitleFont = 5;
inline constexpr int kSoundOptionsRowFont = 0;
inline constexpr int kSoundOptionsSelectedFont = 4;

/// Title row (`push 0xe` at `0x4889f7`), centred in the 196-px band; `x = 38`
/// (`push 0x26` at `0x4889f9`) is the fallback argument of the centred branch.
inline constexpr int kSoundOptionsTitleY = 14;
inline constexpr int kSoundOptionsTitleX = 38;

/// `rows = 7` (`0x488a1e`), step 10 (`0x488a2a`); labels pass row 27, `Done`
/// passes 24 (`0x488b18`), so `y = row + 10*item`.
inline constexpr int kSoundOptionsRows = 7;
inline constexpr int kSoundOptionsItemStep = 10;
inline constexpr int kSoundOptionsLabelRow = 27;
inline constexpr int kSoundOptionsDoneRow = 24;

/// Sound-off layout: `rows = 1` (`0x488b38`), step 0 (`0x488b42`), four lines
/// with the row immediates 27/37/47/57 and item ids 2..5.
inline constexpr int kSoundOptionsDisabledRows = 1;
inline constexpr int kSoundOptionsDisabledItemStep = 0;
inline constexpr std::array<int, 4> kSoundOptionsDisabledItems = {2, 3, 4, 5};
inline constexpr std::array<int, 4> kSoundOptionsDisabledY = {27, 37, 47, 57};

/// Item ids of the sound-on layout (0/2/4/6; odd ids are never drawn).
inline constexpr int kSoundOptionsMusicItem = 0;
inline constexpr int kSoundOptionsEffectsItem = 2;
inline constexpr int kSoundOptionsSwitchItem = 4;
inline constexpr int kSoundOptionsDoneItem = 6;

/// `x = 18` for items 0/2/6, `x = 30` for the checkbox label (item 4).
inline constexpr int kSoundOptionsItemX = 18;
inline constexpr int kSoundOptionsSwitchLabelX = 30;

/// `SLIDE` 72x8 at x 68 (`0x688290`/`0x6891e0` minus `0x6865f8`) and y 37/57;
/// `KNOB` 8x8, base column 69 (`0x688291`/`0x6891e1`), fully opaque.
inline constexpr int kSoundOptionsSlideWidth = 72;
inline constexpr int kSoundOptionsSlideHeight = 8;
inline constexpr int kSoundOptionsSlideX = 68;
inline constexpr int kSoundOptionsKnobWidth = 8;
inline constexpr int kSoundOptionsKnobHeight = 8;
inline constexpr int kSoundOptionsKnobBaseX = 69;
inline constexpr int kSoundOptionsSliderCount = 2;
inline constexpr std::array<int, kSoundOptionsSliderCount> kSoundOptionsSlideY = {37, 57};

/// Volume range and step: clamp 20 (`0x14`) at `0x48c711`/`0x48c6da`, clamp 0
/// at `0x48beb9`/`0x48be83`, step 1.  Carried as the stored engine value.
inline constexpr int kSoundOptionsVolumeMin = 0;
inline constexpr int kSoundOptionsVolumeMax = 20;
inline constexpr int kSoundOptionsVolumeStep = 1;

/// `.data` initial values of `0x4aac74`/`0x4aac78` (10/20); the shipped
/// `GUNMETAL.CFG` carries 16/20 (docs/CONFIG.md §3).
inline constexpr int kSoundOptionsMusicDefault = 10;
inline constexpr int kSoundOptionsEffectsDefault = 20;
inline constexpr int kSoundOptionsMusicShippedCfg = 16;
inline constexpr int kSoundOptionsEffectsShippedCfg = 20;

/// `0x66666667`/`sar 2` divides the `27*volume` numerator by 10; the knob moves
/// 27 px per 10 units, so 0/5/10/20 give 69/82/96/123.
inline constexpr int kSoundOptionsKnobPixelsPerTen = 27;
inline constexpr int kSoundOptionsKnobScaleDivisor = 10;

/// `CHECK` 8x16 / `NOCHECK` 8x8, anchor (20, 59), the unchecked box 8 rows
/// lower (`0x4833e0`, `0x689338`).
inline constexpr int kSoundOptionsCheckX = 20;
inline constexpr int kSoundOptionsCheckY = 59;
inline constexpr int kSoundOptionsCheckWidth = 8;
inline constexpr int kSoundOptionsCheckHeight = 16;
inline constexpr int kSoundOptionsNoCheckWidth = 8;
inline constexpr int kSoundOptionsNoCheckHeight = 8;
inline constexpr int kSoundOptionsNoCheckDrop = 8;

/// `.bss` flag `0x5a494c` (swap left/right), 0 by default, treated as a bool.
inline constexpr int kSoundOptionsSwitchDefault = 0;
/// `.data` flag `0x4aa384` (sound enabled) = 1; `.bss` `0x68cc9c` = 0.
inline constexpr bool kSoundOptionsSoundEnabledDefault = true;
inline constexpr bool kSoundOptionsMusicLowMemoryDefault = false;

/// Neutral cursor: `[0x68ccf4]` is 0 in `.bss`; the entry into state 11
/// (`0x48cf78`) does not write it.
inline constexpr int kSoundOptionsDefaultCursor = 0;

/// Labels and lines, verbatim from the VAs named at each constant.
inline constexpr std::string_view kSoundOptionsMusicLabel = "Music Volume:";        // 0x4bd508
inline constexpr std::string_view kSoundOptionsEffectsLabel = "Effects Volume:";    // 0x4bd4f8
inline constexpr std::string_view kSoundOptionsSwitchLabel = "Switch Left Right";   // 0x4bd4e4
inline constexpr std::string_view kSoundOptionsDoneLabel = "Done";                  // 0x4bceb4
inline constexpr std::string_view kSoundOptionsMusicDisabledLabel =
    "Music Disabled: Low Memory";                                                   // 0x4bd518
inline constexpr std::array<std::string_view, 4> kSoundOptionsDisabledLines = {
    "Sound is disabled.",          // 0x4bd4d0, item 2
    "Rerun Gunmetal without the",  // 0x4bd4b4, item 3
    "N command line option to",    // 0x4bd498, item 4
    "enjoy sound.",                // 0x4bd488, item 5
};

/// Optional parts of one frame.
struct SoundOptionsRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 11, but it is
    /// never painted implicitly (see the header comment).
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** (0/2/4/6 in the sound-on
    /// layout, 2..5 in the sound-off layout).  The comparison is the exact one
    /// of `0x485e0d`, so values outside the drawn ids highlight nothing
    /// (`-1` = a frame without a highlight).
    int cursor = kSoundOptionsDefaultCursor;
    /// `[0x4aac74]` music volume, clamped to the proven 0..20.  Ignored (and
    /// no music slider drawn) when `music_low_memory` is set.
    int music_volume = kSoundOptionsMusicDefault;
    /// `[0x4aac78]` effects volume, clamped to the proven 0..20.
    int effects_volume = kSoundOptionsEffectsDefault;
    /// `[0x5a494c]` swap left/right: non-zero paints `CHECK`, zero `NOCHECK`.
    int switch_left_right = kSoundOptionsSwitchDefault;
    /// `[0x4aa384]`: false selects the `Sound is disabled.` layout
    /// (`0x488b29`), true the four-row/three-widget layout (`0x488a19`).
    bool sound_enabled = kSoundOptionsSoundEnabledDefault;
    /// `[0x68cc9c]`: true replaces the music slider with
    /// `Music Disabled: Low Memory` (`0x488a34`).
    bool music_low_memory = kSoundOptionsMusicLowMemoryDefault;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/mouse_setup_render`, docs/MOUSE_SETUP_RENDER.md §2).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `sound_options_render: fade LUT needs ...`.
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
/// `rows` is the handler's `0x67f10c` value (7 with sound, 1 without); `sliders`
/// the drawn `SLIDE`/`KNOB` pairs (2 with sound and music available, 1 when
/// `music_low_memory`, 0 without sound); `checks` the `CHECK`/`NOCHECK` box
/// (1/0); `arrows` the number of `MENUARRW` sprites painted — screen 11 has no
/// arrow call, so it is always 0 and the test pins the 0; `ink_pixels` the
/// canvas pixels covered by at least one glyph or sprite pixel (unique; the
/// optional backdrop panel is not counted, the same rule as
/// `ui/menu_render`/`ui/restart_render`); `missing_glyphs` the characters with
/// no glyph in the atlas (skipped, never fatal).
struct SoundOptionsRenderStats {
    int rows = 0;
    int sliders = 0;
    int checks = 0;
    int arrows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
};

/// `y` of item `item` in the sound-on layout:
/// `kSoundOptionsLabelRow + 10*item`, except `Done` (item 6), which passes the
/// row immediate 24: 0 -> 27, 2 -> 47, 4 -> 67, 6 -> 84.
int sound_options_item_y(int item);

/// `x` of item `item`: 30 for the checkbox label (item 4), 18 otherwise.
int sound_options_item_x(int item);

/// `y` of line `index` (0..3) of the sound-off layout: 27/37/47/57.
int sound_options_disabled_y(int index);

/// Clamp a stored volume to the proven 0..20 (`0x48c711`/`0x48c6da`).
int sound_options_volume(int requested);

/// `CHECK`/`NOCHECK` decision of `0x4833e0`: non-zero paints `CHECK`.
bool sound_options_switch_on(int switch_left_right);

/// Left column of the volume `KNOB`.
///
/// Proven arithmetic of `0x488a4f`..`0x488a73`: `69 + trunc(27*volume/10)` with
/// `volume` in 0..20; 0/5/10/15/20 give 69/82/96/109/123.
int sound_options_knob_x(int volume);

/// The label of item `item` for `options` (the handler's literal strings).
///
/// With `options.sound_enabled` false the four ids 2/3/4/5 return the
/// `Sound is disabled.` block and any other id returns `Done`; with sound on
/// item 0 is `Music Volume:` unless `music_low_memory` picks
/// `Music Disabled: Low Memory`, items 2/4/6 are `Effects Volume:`/
/// `Switch Left Right`/`Done`, and an out-of-range id returns `Done`.
std::string sound_options_label(int item, const SoundOptionsRenderOptions& options);

/// Paint screen 11 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kSoundOptionsTitleY` (font 5); the items sit at
/// (`sound_options_item_x(item)`, `sound_options_item_y(item)`) with
/// `sound_options_label(item, options)` (font 0), the item equal to
/// `options.cursor` using font 4 instead.  The two volumes paint `SLIDE` at
/// (`kSoundOptionsSlideX`, `kSoundOptionsSlideY[i]`) and `KNOB` at
/// (`sound_options_knob_x(volume)`, same y); the swap flag paints
/// `CHECK`/`NOCHECK` at (`kSoundOptionsCheckX`, `kSoundOptionsCheckY`, the
/// unchecked sprite 8 rows lower).  Every glyph and sprite is clipped to the
/// 196x131 text band.  With `options.fade_lut` every painted palette index goes
/// through `render/hud_lut.h::map` at the level before its colour is looked up;
/// the untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_sound_options(const std::filesystem::path& game,
                          const SoundOptionsRenderOptions& options,
                          std::vector<std::uint32_t>& rgba, int& width, int& height,
                          SoundOptionsRenderStats* stats = nullptr,
                          std::string* error = nullptr);

} // namespace gm
