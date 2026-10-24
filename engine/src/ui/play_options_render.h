#pragma once

// One frame of screen 32, `PLAY OPTIONS` (`M8.8` slice 20).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/sound_options_render`
// 11, `ui/mouse_setup_render` 22, `ui/match_setup_render` 27): the caller owns
// the state (the cursor `0x68ccf4`, the play-option bit word `0x5a46f8`, the
// dynamic-camera index `0x4aac8c` and the in-game flag `0x6e89c0`) and this
// module paints it into the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3).  Nothing here reads the keyboard, `GUNMETAL.CFG`, the
// second flag copy `0x5a46fc`, sounds or saves, and nothing is written back.
//
// Proven geometry (`0x486170`, docs/UI_SCREENS.md §2/§3.4)
// ------------------------------------------------------
// The dispatcher table `0x4883d8` (entry 27, `0x488444`) sends state 32 to the
// stub `0x487ed4`, whose `call 0x486170` is the handler (`ret` at `0x486390`;
// no prologue, no locals, nothing else is drawn).  It writes the immediates
// `rows = 8` (`0x67f10c` at `0x486170`) and `step = 15` (`0x662d58` at
// `0x48617a`), copies `PLATE` (`0x486184` -> `0x486050`: `rep movsd` 0x1913
// dwords = 196x131 bytes from `0x6d2b60` into the compose buffer `0x6865f8`,
// so 196x131 is the hard text clip), then draws the title
// `0x485dd0(font=5, "PLAY OPTIONS" `0x4bd040`, centered=1, x=13, row=11,
// item=-1)` (`0x486189`..`0x486198`): `centered` replaces `x` by
// `(196 - width)/2` (`0x485de2`), and `item == -1` makes `row` absolute, so the
// title sits at y = 11.
//
// Six checkable options, each drawn as `0x4833e0(anchor, 196, bit)` followed by
// its label runs; `y = row + 15*item` (`0x4861bd`..`0x48638d`):
//
//   item  rows (immediates)   label(s) (VA)                                  x
//   0     21                  `Skip Empty Weapons` (`0x4bd02c`)             25
//   1     17                  `Skip Deadly Weapons` (`0x4bd018`)            25
//   2     13 and 21           `Switch Weapon To Picked-Up` (`0x4bcffc`)     25
//                             `Ammo` (`0x4bcff4`)                           25
//   3     15 and 23           `Auto Switch Weapons When` (`0x4bcfd8`)       25
//                             `Ammo Depleted` (`0x4bcfc8`)                  25
//   4     18 and 26           `Don't Auto-Target when` (`0x4bcfb0`)         25
//                             `Looking` (`0x4bcfa8`)                        25
//   5     20                  `Mouse Cheating` (`0x4bcf98`)                 25
//   6     15                  `Dynamic Camera: None/Some/Full` (`0x4bcf38`/
//                             `0x4bcf50`/`0x4bcf68`), or, when the in-game
//                             flag `[0x6e89c0] != 0`, `DYNAMIC CAMERA: HUMAN`
//                             (`0x4bcf80`) with **font 8**                 13
//   7     9                   `Done` (`0x4bceb4`)                         13
//
// so the drawn y values are 21, 32, 43/51, 60/68, 78/86, 95, 105, 114.
//
// Checkboxes
// ----------
// `0x4833e0(anchor, 196, value)` writes into the plate copy, so
// `anchor - 0x6865f8 = y*196 + x` is the box position.  The six anchors are
// `0x686ffc`, `0x687868`, `0x6880d4`, `0x688dd8`, `0x689ba0`, `0x68a8a4`, i.e.
// x = 16 and y = 13, 24, 35, 52, 70, 87 — exactly 8 rows above each label, so
// the bottom of the frame lands on the label row.  A non-zero value copies the
// 16-row `CHECK` 8x16 at the anchor, zero copies the 8-row `NOCHECK` 8x8
// **8 rows lower** (`lea ecx,[ecx+edi*8]` at `0x483403`); source palette index
// 0 is transparent (`test dl,dl; je` at `0x483413`).
//
// Bits of `[0x5a46f8]` (read at `0x48619d`, `0x4861d1`, `0x486207`, `0x486254`,
// `0x48629f`, `0x4862ec`; the click handlers `0x48d816`..`0x48d8e9` XOR the
// same bit in `0x5a46f8` **and** `0x5a46fc`, so both players get one setting):
//
//   item  bit        option
//   0     0x00400    Skip Empty Weapons
//   1     0x04000    Skip Deadly Weapons
//   2     0x08000    Switch Weapon To Picked-Up Ammo
//   3     0x10000    Auto Switch Weapons When Ammo Depleted
//   4     0x01000    Don't Auto-Target when Looking
//   5     0x100000   Mouse Cheating
//
// `0x44d68c` masks exactly these six bits (`and 0x11D400`) when copying the
// word into the player structure, which proves the rest of the word is unused
// (docs/CONFIG.md §3.1).
//
// Highlight
// ---------
// `0x485dd0` switches to font 4 when `item == [0x68ccf4]` **and** the caller
// passed font 0 (`0x485e0d`..`0x485e19`), so the title (font 5) is never
// highlighted, the in-game camera line (font 8) is never highlighted, and both
// lines of a two-line item whiten together (same item id).  There is no bar, box
// or `MENULIT` marker.  The cursor is an item id, so `-1`/99 highlight nothing.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies the `PLATE` 196x131 panel, `CHECK` 8x16, `NOCHECK`
// 8x8 (docs/UI_SCREENS.md §9) and the `FONTS4` faces 5/0/4/8.  There is no
// per-screen full-screen art for screen 32; `background` may name any
// `BMP.IDX` entry painted at (0,0) first (empty = opaque black).  As on the
// sibling screens the proven `PLATE` is *not* painted implicitly: the original
// copies it into `0x6865f8` as the compose buffer and where that copy is
// blitted on screen is not documented (docs/MENU_RENDER.md §2), so painting it
// at (0,0) stays the caller's choice.
//
// Colours come from the atlas palette: title font 5 -> (155,15,15) dark red,
// items font 0 -> (19,15,15) dark grey, highlight font 4 -> (239,239,239)
// white, the in-game camera line font 8 -> (75,75,75) grey
// (docs/MENU_RENDER.md §3).  `CHECK`/`NOCHECK`/`PLATE` use their own BMP
// palettes; index 0 is the transparent key of the control sprites.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/mouse_setup_render` and
// `ui/sound_options_render`: every painted pixel's palette index goes through
// `render/hud_lut.h::map(level, index)` **before** its colour is looked up.  The
// table comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  With neither, the call is a text
// refusal prefixed `play_options_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"play_options_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a missing face 5/0/4 (8 when
// `in_game`), a missing/broken `PLATE`/`CHECK`/`NOCHECK` or one of the wrong
// size, a background entry that is missing/broken/not 8 bpp, and an exact fade
// without a table and a dump (or with an unreadable dump).  Out-of-range
// *values* (a cursor of 99, a camera index of 7, negative flags) are clamped or
// masked, not refused: the original treats the flags as a bit word and cycles
// the camera 0..2, and the frame stays paintable.  On failure `rgba` is
// cleared, `width` and `height` are 0 (never a stale or half-drawn picture) and
// `*stats` is zeroed.  `stats`/`error` may be null.  The call only reads the
// installation, is deterministic and does not depend on the current working
// directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the six bit toggles `0x48d816`..`0x48d8e9`, the camera cycle
// `0x48d8fa` gated on `[0x6e89c0] == 0`, `Esc`, the item dispatch table
// `0x48e8f8`), UI sounds (docs/UI_SOUNDS.md §3.2), the `GUNMETAL.CFG`
// round-trip (`0x1CC`/`0x1D0`/`0x1C8`, docs/CONFIG.md §3), the `0x5a46fc`
// second copy, and the `0x44d68c` copy into the player structure.  There is no
// `MENUARRW`/`MENULIT` call in `0x486170`, so `arrows` is always 0; the field
// keeps the stats shape of the sibling renderers.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kPlayOptionsCanvasWidth = 320;
inline constexpr int kPlayOptionsCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kPlayOptionsBackground = 0x000000FFu;

/// `0x4bd040`, pushed at `0x486191`.
inline constexpr std::string_view kPlayOptionsTitle = "PLAY OPTIONS";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x485dd0` writes into its copy, so
/// this is also the text clip.
inline constexpr std::string_view kPlayOptionsPanelName = "PLATE";
inline constexpr int kPlayOptionsPanelWidth = 196;
inline constexpr int kPlayOptionsPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, items 0, highlight 4.  Font 8 is the fourth
/// `FONTS4` face (`origin_y` 126) used only by the in-game camera line.
inline constexpr int kPlayOptionsTitleFont = 5;
inline constexpr int kPlayOptionsRowFont = 0;
inline constexpr int kPlayOptionsSelectedFont = 4;
inline constexpr int kPlayOptionsInGameFont = 8;

/// Title row (`push 0xb` at `0x48618d`, `item = -1` makes it absolute),
/// centred in the 196-px band; `x = 13` (`push 0xd` at `0x48618b`) is the
/// fallback argument of the centred branch.
inline constexpr int kPlayOptionsTitleY = 11;
inline constexpr int kPlayOptionsTitleX = 13;

/// `rows = 8` (`0x486170`), step 15 (`0x48617a`); labels pass the row
/// immediates below, so `y = row + 15*item`.
inline constexpr int kPlayOptionsRows = 8;
inline constexpr int kPlayOptionsItemStep = 15;

/// Item ids of the handler (0..5 checkable, 6 camera, 7 `Done`).
inline constexpr int kPlayOptionsSkipEmptyItem = 0;
inline constexpr int kPlayOptionsSkipDeadlyItem = 1;
inline constexpr int kPlayOptionsSwitchAmmoItem = 2;
inline constexpr int kPlayOptionsAutoSwitchItem = 3;
inline constexpr int kPlayOptionsAutoTargetItem = 4;
inline constexpr int kPlayOptionsMouseCheatingItem = 5;
inline constexpr int kPlayOptionsCameraItem = 6;
inline constexpr int kPlayOptionsDoneItem = 7;
inline constexpr int kPlayOptionsItemCount = 8;

/// Row immediates of the label calls, per item: 21, 17, 13, 15, 18, 20, 15, 9
/// (`0x4861bf`, `0x4861f5`, `0x48622b`, `0x486276`, `0x4862c3`, `0x486310`,
/// `0x486330`/`0x486364`/`0x48634b`, `0x48637b`).
inline constexpr std::array<int, kPlayOptionsItemCount> kPlayOptionsItemRow = {
    21, 17, 13, 15, 18, 20, 15, 9};

/// Row immediates of the second line of items 2/3/4: 21, 23, 26 (`0x486242`,
/// `0x48628d`, `0x4862da`); `-1` for the items that have a single line.
inline constexpr std::array<int, kPlayOptionsItemCount> kPlayOptionsSecondRow = {
    -1, -1, 21, 23, 26, -1, -1, -1};

/// `x = 25` for items 0..5 (`push 0x19`), `x = 13` for the camera and `Done`
/// (`push 0xd` at `0x486332`, `0x486366`, `0x48634d`, `0x48637d`).
inline constexpr int kPlayOptionsItemX = 25;
inline constexpr int kPlayOptionsTailX = 13;

/// Bit word `[0x5a46f8]` per checkable item (docs/CONFIG.md §3.1).
inline constexpr std::uint32_t kPlayOptionsFlagSkipEmpty = 0x00400u;
inline constexpr std::uint32_t kPlayOptionsFlagSkipDeadly = 0x04000u;
inline constexpr std::uint32_t kPlayOptionsFlagSwitchAmmo = 0x08000u;
inline constexpr std::uint32_t kPlayOptionsFlagAutoSwitch = 0x10000u;
inline constexpr std::uint32_t kPlayOptionsFlagNoAutoTarget = 0x01000u;
inline constexpr std::uint32_t kPlayOptionsFlagMouseCheating = 0x100000u;

/// `and 0x11D400` at `0x44d68c` — the six bits above and nothing else.
inline constexpr std::uint32_t kPlayOptionsFlagMask = 0x11D400u;

/// `.bss` initial value of `0x5a46f8`/`0x5a46fc` is 0; the shipped
/// `GUNMETAL.CFG` carries `0x10400` (Skip Empty Weapons + Auto Switch Weapons
/// When Ammo Depleted, docs/CONFIG.md §3).
inline constexpr int kPlayOptionsFlagsBssDefault = 0;
inline constexpr int kPlayOptionsFlagsShippedCfg = 0x10400;

/// `[0x4aac8c]` dynamic camera: `.data` = 1, cycle 0/1/2 clamped by
/// `0x48d907`..`0x48d91b` (None/Some/Full).
inline constexpr int kPlayOptionsCameraMin = 0;
inline constexpr int kPlayOptionsCameraMax = 2;
inline constexpr int kPlayOptionsCameraDefault = 1;

/// `[0x6e89c0]` — non-zero while a game is running (`0x486322`, `0x48d8fa`).
inline constexpr bool kPlayOptionsInGameDefault = false;

/// `CHECK` 8x16 / `NOCHECK` 8x8; the unchecked box is copied 8 rows below the
/// anchor (`0x4833e0`), so both end on the label row.  x = 16 for all six
/// anchors.
inline constexpr int kPlayOptionsCheckX = 16;
inline constexpr int kPlayOptionsCheckWidth = 8;
inline constexpr int kPlayOptionsCheckHeight = 16;
inline constexpr int kPlayOptionsNoCheckWidth = 8;
inline constexpr int kPlayOptionsNoCheckHeight = 8;
inline constexpr int kPlayOptionsNoCheckDrop = 8;
inline constexpr int kPlayOptionsCheckboxCount = 6;

/// Neutral cursor: `[0x68ccf4]` is 0 in `.bss`; the entry into state 32
/// (`0x48d005` of the main menu) does not write it.
inline constexpr int kPlayOptionsDefaultCursor = 0;

/// Labels and lines, verbatim from the VAs named at each constant.
inline constexpr std::string_view kPlayOptionsSkipEmptyLabel = "Skip Empty Weapons";     // 0x4bd02c
inline constexpr std::string_view kPlayOptionsSkipDeadlyLabel = "Skip Deadly Weapons";   // 0x4bd018
inline constexpr std::string_view kPlayOptionsSwitchAmmoLabel =
    "Switch Weapon To Picked-Up";                                                       // 0x4bcffc
inline constexpr std::string_view kPlayOptionsAmmoLine = "Ammo";                        // 0x4bcff4
inline constexpr std::string_view kPlayOptionsAutoSwitchLabel =
    "Auto Switch Weapons When";                                                         // 0x4bcfd8
inline constexpr std::string_view kPlayOptionsAmmoDepletedLine = "Ammo Depleted";       // 0x4bcfc8
inline constexpr std::string_view kPlayOptionsNoAutoTargetLabel =
    "Don't Auto-Target when";                                                           // 0x4bcfb0
inline constexpr std::string_view kPlayOptionsLookingLine = "Looking";                   // 0x4bcfa8
inline constexpr std::string_view kPlayOptionsMouseCheatingLabel = "Mouse Cheating";    // 0x4bcf98
inline constexpr std::string_view kPlayOptionsCameraNone = "Dynamic Camera: None";      // 0x4bcf38
inline constexpr std::string_view kPlayOptionsCameraSome = "Dynamic Camera: Some";      // 0x4bcf50
inline constexpr std::string_view kPlayOptionsCameraFull = "Dynamic Camera: Full";      // 0x4bcf68
inline constexpr std::string_view kPlayOptionsCameraHuman =
    "DYNAMIC CAMERA: HUMAN";                                                            // 0x4bcf80
inline constexpr std::string_view kPlayOptionsDoneLabel = "Done";                       // 0x4bceb4

/// Optional parts of one frame.
struct PlayOptionsRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 32, but it is
    /// never painted implicitly (see the header comment).
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** 0..7.  The comparison is the
    /// exact one of `0x485e0d`, so values outside the drawn ids highlight
    /// nothing (`-1` = a frame without a highlight).
    int cursor = kPlayOptionsDefaultCursor;
    /// `[0x5a46f8]` play-option bit word (only `kPlayOptionsFlag*` are read;
    /// the proven mask is `kPlayOptionsFlagMask`).  Negative or high bits are
    /// simply masked away by the per-item `and`.
    int flags = kPlayOptionsFlagsBssDefault;
    /// `[0x4aac8c]` dynamic camera, clamped to 0..2 (None/Some/Full).
    int dynamic_camera = kPlayOptionsCameraDefault;
    /// `[0x6e89c0] != 0`: replaces the camera line with `DYNAMIC CAMERA:
    /// HUMAN` in font 8 (`0x48632e`..`0x48633d`).
    bool in_game = kPlayOptionsInGameDefault;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/sound_options_render`, docs/SOUND_OPTIONS_RENDER.md
    /// §2.5).  Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `play_options_render: fade LUT needs ...`.
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
/// `rows` is the handler's `0x67f10c` value (always 8); `checks` the number of
/// drawn `CHECK`/`NOCHECK` boxes (always 6 — one per checkable item, drawn in
/// both states); `sliders` the number of `SLIDE`/`KNOB` pairs (screen 32 has no
/// slider call, so always 0; the field keeps the stats shape); `arrows` the
/// number of `MENUARRW` sprites painted — also always 0, the test pins both;
/// `ink_pixels` the canvas pixels covered by at least one glyph or sprite pixel
/// (unique; the optional backdrop panel is not counted, the same rule as
/// `ui/menu_render`/`ui/sound_options_render`); `missing_glyphs` the characters
/// with no glyph in the atlas (skipped, never fatal).
struct PlayOptionsRenderStats {
    int rows = 0;
    int checks = 0;
    int sliders = 0;
    int arrows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
};

/// Row immediate of item `item` (`kPlayOptionsItemRow`, clamped to 0..7).
int play_options_item_row(int item);

/// `y` of the first line of item `item` (clamped to 0..7):
/// `row + 15*item` → 21, 32, 43, 60, 78, 95, 105, 114.
int play_options_item_y(int item);

/// `y` of the second line of item `item` (items 2/3/4: 51, 68, 86), or `-1`
/// for the single-line items — the handler does not call `0x485dd0` for them.
int play_options_second_y(int item);

/// `x` of item `item`: 25 for 0..5, 13 for the camera and `Done` (and for
/// out-of-range ids, which the handler never passes).
int play_options_item_x(int item);

/// Proven bit of item `item` (`0x400`, `0x4000`, `0x8000`, `0x10000`, `0x1000`,
/// `0x100000`), 0 for the camera and `Done`.
std::uint32_t play_options_flag_bit(int item);

/// `0x4833e0` decision: a non-zero `flags & bit` paints `CHECK`.
bool play_options_checked(int item, int flags);

/// Anchor `y` of the checkbox of item `item`: `play_options_item_y(item) - 8`
/// (13, 24, 35, 52, 70, 87), or `-1` when the item has no checkbox.
int play_options_check_y(int item);

/// Clamp a stored camera index to the proven 0..2
/// (`0x48d907`..`0x48d91b`).
int play_options_camera(int requested);

/// The label of the first line of item `item` for `options` (the handler's
/// literal strings); `Done` for out-of-range ids.
std::string play_options_label(int item, const PlayOptionsRenderOptions& options);

/// The label of the second line of item `item`, empty when the item has one
/// line only.
std::string play_options_second_label(int item);

/// Font of item `item`: 4 when it is the cursor **and** the item would use
/// font 0 (the in-game camera line already uses font 8, so it never turns
/// white), otherwise 0/8 as the handler passes it.
int play_options_item_font(int item, const PlayOptionsRenderOptions& options);

/// Paint screen 32 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kPlayOptionsTitleY` (font 5); the items sit at
/// (`play_options_item_x(item)`, `play_options_item_y(item)`) with
/// `play_options_label(item, options)` and, for items 2/3/4, the second line at
/// `play_options_second_y(item)` (font 0, or 4 for the cursor item).  The six
/// checkable items paint `CHECK`/`NOCHECK` at (`kPlayOptionsCheckX`,
/// `play_options_check_y(item)`, the unchecked sprite 8 rows lower) before their
/// label.  Every glyph and sprite is clipped to the 196x131 text band.  With
/// `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_play_options(const std::filesystem::path& game,
                         const PlayOptionsRenderOptions& options,
                         std::vector<std::uint32_t>& rgba, int& width, int& height,
                         PlayOptionsRenderStats* stats = nullptr,
                         std::string* error = nullptr);

} // namespace gm
