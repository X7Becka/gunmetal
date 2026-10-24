#pragma once

// One frame of screen 36, the per-player joystick setup (`M8.8` slice 15).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/mouse_setup_render`
// 22, `ui/joystick_centering_render` 26, `ui/keycfg_render` 30/34/35,
// `ui/key_jamming_render` 16): the caller owns the state (the list cursor
// `[0x68ccf4]`, the device block `0x4bf7b0 + dev*0x7c`, the axis presets
// `0x68cd00 + dev*6`, the hat bytes `0x68cd30 + dev*8`, the button masks
// `0x4aa490 + dev*0x80`) and this module paints it into the 320x200 mode-13h
// canvas (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads the keyboard, the
// joystick, `GUNMETAL.CFG` or sounds, and nothing is written back.
//
// The handler is `0x489d60`
// -------------------------
// The screen map (docs/UI_SCREENS.md §2) lists `0x489d60` for id 36; the
// dispatcher `0x487e71` computes `eax = [0x4bc440] - 5` and jumps through
// `0x4883d8 + eax*4`, so id 36 is slot index 31, address **`0x488454`**, whose
// stub `0x487f51` is `call 0x489d60; jmp 0x487f64` (no argument pushed — the
// handler reads the globals itself).  Body `0x489d60`..`0x48a2a4` (`ret`), plus
// the private row helper `0x48a2b0` for the six axis rows:
//
//   0x489d60  al = [0x685afc]        player 0/1: picks the title and the device
//   0x489d6b  ebx = [0x4bc540]       joystick slot assigned to that player
//   0x489d76  ebx = [0x4bc544]       (player 2 when [0x685afc] != 0)
//   0x489d7e  [0x662d58] = 9         step
//   0x489d91  [0x67f10c] = 1 or 33   33 when a device is assigned
//   0x489d96  call 0x486050          copy the 196x131 PLATE into 0x6865f8
//   0x489d9b  title   font 5, centred, row 11, item -1
//   0x489dc2  hint    font 0, centred, row 20, item -1
//   0x489dd6  esi = max(0, [0x68ccf4] - 5)     visible window top
//   0x489df9  device == -1 -> one "Joystick Used: None" row, no arrows
//   0x489e41  else: device name row, six axis rows, up to four hat rows,
//                    every assigned button channel, then the two arrows
//
// Everything below was read from that listing (`tools/re/disrange.py`); the
// pool of strings lives at `0x4bd8cc`..`0x4bd9c0` and the tables in `.data`.
//
// Proven geometry
// ---------------
//   * `step = 9` (`0x662d58`, `0x489d7e`) and `row 0` at `y = 28`
//     (`0xffffffe4` = -28 in every row call: `0x489e1c`, `0x489e71`,
//     `0x489f7d`, ...).  A line of content index `c` is at
//     `y = 28 + 9*(c - scroll)`.
//   * `scroll = max(0, [0x68ccf4] - 5)` (the `cmp eax,5` / `lea esi,[eax-5]`
//     pair at `0x489de0`..`0x489dec`), i.e. the same five-row scroll band as
//     screens 30/34/35.
//   * `x = 13` for every label, button line and the device name (`push 0xd` at
//     `0x489e2a`, `0x489e7e`, `0x489f8a`, `0x48a006`, ...).
//   * The value column is `13 + width(font 0, "Front/Back: ")`: the handler
//     measures that one label at `0x489e91`..`0x489e9d` (`0x47fa50(0,
//     0x4bd95c)`) and passes `edi + 13` to every value call.  It is a single
//     fixed column for all six axes and both hats, not a per-label column.
//   * Title `PLAYER 1 JOYSTICK SETUP` (`0x4bd9c0`) / `PLAYER 2 JOYSTICK SETUP`
//     (`0x4bd9a8`), font 5, centred in the 196 px band (`centered = 1`),
//     `row = 11` (`0x489d9b`..`0x489dba`).
//   * Hint `Esc Exit, Button To Highlight` (`0x4bd988`), font 0, centred,
//     `row = 20` (`0x489dc2`..`0x489dd1`).
//
// Proven rows (content index -> text)
// -----------------------------------
//   0                    `Joystick Used: None` (`0x4bd974`, device -1) or the
//                        device name `"%.30s"` (`0x4bd96c`) of
//                        `0x4bf7f0 + dev*0x7c` (no prefix) — `0x489e03` /
//                        `0x489e4e`; skipped when `scroll > 0`
//   1..6                  six axis rows, each only when the device advertises
//                        it (`[0x4bf7e0 + dev*0x7c] & (2 << axis)`, the
//                        literals `2,4,8,0x10,0x20,0x40` pushed at `0x489eaf`,
//                        `0x489ec8`, `0x489ee1`, `0x489efa`, `0x489f13`,
//                        `0x489f2c`): `Left/Right: ` (`0x4bd94c`),
//                        `Front/Back: ` (`0x4bd95c`), `Up/Down: ` (`0x4bd940`),
//                        `Angular X: ` (`0x4bd934`), `Angular Y: ` (`0x4bd928`),
//                        `Angular Z: ` (`0x4bd91c`)
//   next                 `Hat 1 X: ` (`0x4bd910`) / `Hat 1 Y: ` (`0x4bd904`) as
//                        a pair when `[0x4bf7e0 + dev*0x7c] & 0x100`
//                        (`test ch,0x1` at `0x489f51`), values `0x68cd30/31`
//   next                 `Hat 2 X: ` (`0x4bd8f8`) / `Hat 2 Y: ` (`0x4bd8ec`) as
//                        a pair when the same dword has `& 0x200`
//                        (`test ch,0x2` at `0x48a064`), values `0x68cd32/33`
//   next..+31            one row per assigned button channel:
//                        `Button %d: %s` (`0x4bd8dc`) or `Button %d: None`
//                        (`0x4bd8cc`), channel = index + 1, up to 32 channels
//                        (`cmp eax,0x21` at `0x48a25c`)
//
// Axis and hat values are **preset indices** (`0..17`) into the 18-record table
// `0x4bc468` whose first dword is the name (`Not Used`, `Forward/Back`, ...,
// `Equip Cycle -/+`, docs/CONFIG.md §5): axes read `0x68cd00 + dev*6 + axis`
// (`0x48a362`, inside `0x48a2b0`), hats read `0x68cd30 + dev*8 + 0..3`
// (`0x489fbb`, `0x48a035`, `0x48a0cd`, `0x48a147`).
//
// Buttons are **bit masks**: `0x4aa490 + (dev*32 + channel)*4`, `-1` means "no
// assignment" (`test eax,eax; jl` at `0x48a1b2`); anything else goes through
// `0x4863a0`, which returns the bit position (`mask == 1 << n` -> `n`), and
// that index selects one of the 31 action names `0x4bc558` (`Move Forward` …
// `Linear Right`, docs/UI_SCREENS.md §12.1).  `.data` ships
// `{0x20, 0x8000, -1, …}` for all four device slots, i.e. `Fire`, `Activate`
// and 30 empty channels (`0x4aa490`, docs/CONFIG.md §6).
//
// Highlight and live input
// ------------------------
// `0x485dd0` turns a caller's font 0 into font 4 when `item == [0x68ccf4]`
// (`0x485e0d`..`0x485e19`), so exactly the line whose content index equals the
// cursor is white.  On top of that the handler colours a row itself:
//   * an axis **label** becomes font 5 (dark red) while the device reports
//     `|value| >= 0.5`: `0x48a2e8`..`0x48a31b` loads the `double` at
//     `0x4bf7b0 + dev*0x7c + axis*8`, compares it with `0.0` (`0x4a2950`) for
//     the sign and with `0.5` (`0x4a2928`), and picks 5 or 0.  The threshold is
//     inclusive (`test ah,0x41` at `0x48a30d`).
//   * a hat **label** becomes font 5 while `[0x4bf7ec + dev*0x7c]` has the
//     pair of bits of that row: `0xa` (Hat 1 X), `0x5` (Hat 1 Y), `0xa0`
//     (Hat 2 X), `0x50` (Hat 2 Y) — `0x489f82`, `0x489fff`, `0x48a097`,
//     `0x48a111`.
//   * a whole **button line** becomes font 5 while
//     `[0x4bf7e8 + dev*0x7c] & (1 << channel)` (`0x48a1f7`..`0x48a20c`).
// The value of an axis/hat row and the device name are always drawn with font 0,
// so only the `0x485dd0` swap can make them white; a pressed button line stays
// red even when it is the cursor row (its caller font is 5, not 0).
//
// Visible window and arrows
// -------------------------
// Content rows `c` with `c >= scroll` are drawn; the button rows additionally
// stop at `c - scroll >= 10` (`add eax,edi; sub eax,esi; cmp eax,0xa` at
// `0x48a197`), which is the ten-band window of the screen.  Axis and hat rows
// do not need that test: there are at most ten of them and they start at row 1.
// Then, last of all, two `MENUARRW` sprites (16x8) are blitted into the plate
// copy at column 159, rows 2 and 121 (`0x68681f` / `0x68c33b`:
// `0x485d00` = up, `0x485cc0` = down) when `scroll > 0` and when
// `scroll + 10 < [0x67f10c]` (`0x48a269`..`0x48a29a`).
//
// `[0x67f10c]` starts at **1** without a device and at **33** with one
// (`0x489d88`..`0x489d91`), then `+1` per drawn axis (inside `0x48a2b0`,
// `0x48a391`..`0x48a3a1`) and `+2` per present hat (`0x489f60`, `0x48a073`);
// the button loop does not touch it.  With all six axes and both hats that is
// `33 + 6 + 4 = 43` = 1 device row + 6 axes + 4 hats + 32 channels, which is
// why `stats.rows` is that count and not the 10 visible bands.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE` (196x131, copied into `0x6865f8` by
// `0x486050` at `0x489d96`), `FONTS4` faces 5 (title and the red labels), 0
// (all other text) and 4 (the cursor line), and `MENUARRW` (16x8) when an arrow
// is drawn.  All assets are validated even when `background` is empty, because
// the original composes the frame on the plate copy; the canvas is opaque black
// unless `background` names an entry (`"PLATE"` reproduces the panel).  Colours
// come from the atlas palettes: title/label font 5 -> (155,15,15) dark red,
// body font 0 -> (19,15,15) dark grey, highlight font 4 -> (239,239,239) white
// (docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/mouse_setup_render`/`ui/keycfg_render`:
// every painted palette index goes through `render/hud_lut.h::map(level, index)`
// **before** its colour is looked up.  The table comes pre-loaded
// (`fade_lut_table`) or from a 4096-byte dump (`fade_lut_dump`,
// docs/HUD_LUT.md §2); with neither the call refuses, never a silently unfaded
// frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"player_setup_render: "`: an empty game
// directory, a missing `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken/not-196x131 `PLATE`, a missing/broken/not-16x8 `MENUARRW` while
// an arrow is drawn, a background entry that is missing/broken/not 8 bpp, and an
// exact fade without a table and a dump (or with an unreadable dump).
// Out-of-range *values* (cursor, device slot, preset indices, masks, a name
// longer than 30 bytes) are clamped/truncated, not refused: they only move the
// window or change a text, and the screen stays paintable.  On failure `rgba` is
// cleared, `width`/`height` are 0 (never a stale or half-drawn picture) and
// `*stats` is zeroed.  `stats`/`error` may be null.  The call only reads the
// installation, is deterministic and does not depend on the current directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the key poll, the axis/hat preset cycling of `0x48c1b6`/`0x48d32b`, the
// button re-assignment, Esc -> state 10 at `0x48e599`), UI sounds, and the two
// handler side effects `0x664594`/`0x683f94` (the first hat row and the first
// button row, read by that input code).  The sliders (`SLIDE`/`KNOB`) and
// checkboxes (`CHECK`/`NOCHECK`) of the sibling screens do **not** appear on 36:
// `0x489d60` contains no call to `0x486070`, `0x4860b0` or `0x4833e0`, so
// `sliders` and `checks` are always 0.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"
#include "render/text_layout.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kPlayerSetupCanvasWidth = 320;
inline constexpr int kPlayerSetupCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kPlayerSetupBackground = 0x000000FFu;

/// The panel and the text band: `0x489d96` -> `0x486050` copies the 196x131
/// `PLATE` into `0x6865f8`, so this is also the glyph clip.
inline constexpr std::string_view kPlayerSetupPanelName = "PLATE";
inline constexpr int kPlayerSetupPanelWidth = 196;
inline constexpr int kPlayerSetupPanelHeight = 131;

/// `0x4bd9c0` (player 1) / `0x4bd9a8` (player 2), font 5, centred, y 11.
inline constexpr std::string_view kPlayerSetupTitlePlayer1 = "PLAYER 1 JOYSTICK SETUP";
inline constexpr std::string_view kPlayerSetupTitlePlayer2 = "PLAYER 2 JOYSTICK SETUP";

/// `0x4bd988`, font 0, centred, y 20.
inline constexpr std::string_view kPlayerSetupHint = "Esc Exit, Button To Highlight";

/// `0x4bd974`: the only content row when no device is assigned to the player.
inline constexpr std::string_view kPlayerSetupNoDevice = "Joystick Used: None";

/// docs/UI_SCREENS.md §3: title/label 5, body 0, the highlight face 4 that
/// `0x485dd0` swaps in for the cursor line (`0x485e15`/`0x485e19`).
inline constexpr int kPlayerSetupTitleFont = 5;
inline constexpr int kPlayerSetupRowFont = 0;
inline constexpr int kPlayerSetupSelectedFont = 4;

/// `[0x662d58] = 9` at `0x489d7e`.
inline constexpr int kPlayerSetupItemStep = 9;
/// `[0x67f10c]` of `0x489d88`..`0x489d91`: 1 without a device, 33 with one.
inline constexpr int kPlayerSetupRowsNoDevice = 1;
inline constexpr int kPlayerSetupRowsDevice = 33;

/// Ten visible bands, the five-row scroll band of `0x489de0`..`0x489dec` and
/// the `0xa` clip of the button loop (`0x48a19b`).
inline constexpr int kPlayerSetupVisibleRows = 10;
inline constexpr int kPlayerSetupScrollBand = 5;

/// Title anchors: `push 0xb` (`row`) and `push 0x26` (`x`, ignored by
/// `centered = 1`).
inline constexpr int kPlayerSetupTitleY = 11;
inline constexpr int kPlayerSetupTitleX = 38;
/// Hint anchors: `push 0x14` for both `row` and `x`.
inline constexpr int kPlayerSetupHintY = 20;
inline constexpr int kPlayerSetupHintX = 20;

/// First content row (`row = -0x1c`) and the `x` of every label / button line /
/// the device name (`push 0xd`).
inline constexpr int kPlayerSetupFirstRowY = 28;
inline constexpr int kPlayerSetupRowX = 13;

/// The label the handler measures once for the value column (`0x489e91`).
inline constexpr std::string_view kPlayerSetupValueMeasureLabel = "Front/Back: ";

/// The six axis label strings, in handler order (`0x4bd94c`, `0x4bd95c`,
/// `0x4bd940`, `0x4bd934`, `0x4bd928`, `0x4bd91c`).
inline constexpr std::string_view kPlayerSetupAxisLabels[6] = {
    "Left/Right: ",  // 0x4bd94c, mask 0x02
    "Front/Back: ",  // 0x4bd95c, mask 0x04
    "Up/Down: ",     // 0x4bd940, mask 0x08
    "Angular X: ",   // 0x4bd934, mask 0x10
    "Angular Y: ",   // 0x4bd928, mask 0x20
    "Angular Z: ",   // 0x4bd91c, mask 0x40
};

/// Presence bits of those rows in `[0x4bf7e0 + dev*0x7c]` (the literals of the
/// six `push` instructions listed above).
inline constexpr unsigned kPlayerSetupAxisPresentBits[6] = {
    0x002u, 0x004u, 0x008u, 0x010u, 0x020u, 0x040u,
};
inline constexpr unsigned kPlayerSetupAllAxes = 0x07Eu;

/// The four hat labels and their presence bits (`test ch,0x1`/`ch,0x2`).
inline constexpr std::string_view kPlayerSetupHatLabels[4] = {
    "Hat 1 X: ",  // 0x4bd910, 0x68cd30
    "Hat 1 Y: ",  // 0x4bd904, 0x68cd31
    "Hat 2 X: ",  // 0x4bd8f8, 0x68cd32
    "Hat 2 Y: ",  // 0x4bd8ec, 0x68cd33
};
inline constexpr unsigned kPlayerSetupHat1Bit = 0x100u;
inline constexpr unsigned kPlayerSetupHat2Bit = 0x200u;

/// Bits of `[0x4bf7ec + dev*0x7c]` that paint a hat label with font 5.
inline constexpr unsigned kPlayerSetupHatActiveBits[4] = {
    0x0Au,  // Hat 1 X: 0x489f82
    0x05u,  // Hat 1 Y: 0x489fff
    0xA0u,  // Hat 2 X: 0x48a097
    0x50u,  // Hat 2 Y: 0x48a111
};

/// Axis deflection threshold: `0.5` at `0x4a2928` (sign probe `0.0` at
/// `0x4a2950`); `|value| >= 0.5` paints the label with font 5.
inline constexpr double kPlayerSetupDeflectionThreshold = 0.5;

/// The 18 axis presets of `0x4bc468` (12-byte records, the first dword is the
/// name; docs/CONFIG.md §5).  Indices of the axis and hat values index this
/// table.
inline constexpr std::string_view kPlayerSetupPresetNames[18] = {
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

/// The 31 action names of `0x4bc558`, in button-mask bit order (docs/
/// UI_SCREENS.md §12.1); `0x4863a0` turns a mask into one of these indices.
inline constexpr std::string_view kPlayerSetupActionNames[31] = {
    "Move Forward",      // 0
    "Move Backward",     // 1
    "Turn Left",         // 2
    "Turn Right",        // 3
    "Look Center",       // 4
    "Fire",              // 5
    "Look Up",           // 6
    "Look Down",         // 7
    "Strafe",            // 8
    "Strafe Left",       // 9
    "Strafe Right",      // 10
    "Throttle Up",       // 11
    "Throttle Down",     // 12
    "Next Weapon",       // 13
    "Previous Weapon",   // 14
    "Activate",          // 15
    "Next Equipment",    // 16
    "Previous Equip.",   // 17
    "Toggle Equip. Use", // 18
    "Headlights",        // 19
    "Force Field",       // 20
    "Mine Detector",     // 21
    "Targeting",         // 22
    "Warp Device",       // 23
    "Radar",             // 24
    "Mine Layer",        // 25
    "Combat Block",      // 26
    "Auto Sentry",       // 27
    "Decoy",             // 28
    "Linear Left",       // 29
    "Linear Right",      // 30
};

/// Button channels per device (`cmp eax,0x21`, 32 iterations at `0x48a25c`).
inline constexpr int kPlayerSetupButtonChannels = 32;

/// `.data` defaults of `0x4aa490 + dev*0x80` (all four device slots):
/// `0x20` = `1 << 5` = `Fire`, `0x8000` = `1 << 15` = `Activate`, rest `-1`
/// (no assignment).
inline constexpr int kPlayerSetupUnassigned = -1;
inline constexpr std::array<int, kPlayerSetupButtonChannels>
    kPlayerSetupDefaultButtons = [] {
        std::array<int, kPlayerSetupButtonChannels> values{};
        values.fill(kPlayerSetupUnassigned);
        values[0] = 0x20;   // Fire
        values[1] = 0x8000; // Activate
        return values;
    }();

/// `0x4bf7f0 + dev*0x7c` is a 60-byte DirectInput name; the handler prints it
/// through `"%.30s"` (`0x4bd96c`).
inline constexpr int kPlayerSetupNameMaxChars = 30;

/// Scroll arrows: `MENUARRW` (16x8, 8 bpp) at column 159, rows 2 and 121 of the
/// plate copy (`0x68681f`/`0x68c33b`, `0x48a26d`..`0x48a295`).
inline constexpr std::string_view kPlayerSetupArrowName = "MENUARRW";
inline constexpr int kPlayerSetupArrowWidth = 16;
inline constexpr int kPlayerSetupArrowHeight = 8;
inline constexpr int kPlayerSetupArrowX = 159;
inline constexpr int kPlayerSetupArrowUpY = 2;
inline constexpr int kPlayerSetupArrowDownY = 121;

/// Optional parts of one frame.
struct PlayerSetupRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of the screen and is never
    /// painted implicitly.
    std::string background;

    /// The cursor `[0x68ccf4]`: the **content row index** the highlight is on.
    /// Its value also sets the window top `max(0, cursor - 5)` of
    /// `0x489dd6`..`0x489dec`, exactly like screens 30/34/35.  Values outside
    /// `0..rows-1` simply highlight nothing; `-1` is the documented way to ask
    /// for a frame without a white line.
    int cursor = 0;

    /// `[0x685afc]`: 0 = player 1 (`PLAYER 1 JOYSTICK SETUP`), non-zero =
    /// player 2.  It only selects the title here; the device slot itself is
    /// `joystick` below (the handler reads `[0x4bc540]` for player 1 and
    /// `[0x4bc544]` for player 2).
    int player = 0;

    /// The assigned joystick slot `[0x4bc540]`/`[0x4bc544]`: `-1` = `Joystick
    /// Used: None` (.data initial value), otherwise the DirectInput device
    /// index `0..7` that `0x404880` accepts (`cmp edi,8`).
    int joystick = kPlayerSetupUnassigned;

    /// The device name of `0x4bf7f0 + joystick*0x7c` as `"%.30s"` prints it;
    /// empty by default (that field is `.bss`, filled by device enumeration).
    /// Only drawn when a device is assigned and the window top is 0.
    std::string joystick_name;

    /// `0x68cd00 + joystick*6 + axis`, indices into `kPlayerSetupPresetNames`
    /// (clamped to 0..17).  These are the texts of the six axis rows.
    std::array<int, 6> axes = {0, 0, 0, 0, 0, 0};

    /// Presence bits of the six axis rows (`[0x4bf7e0 + dev*0x7c]`, bits
    /// 0x02..0x40).  All six by default: the bits come from DirectInput
    /// enumeration (`0x404e78`..`0x40524e`), so "a full six-axis device" is a
    /// model, not a shipped byte.
    unsigned axis_present = kPlayerSetupAllAxes;

    /// The live axis deflections `0x4bf7b0 + dev*0x7c + axis*8`.  A label is
    /// painted with font 5 while `|value| >= 0.5` (`0x48a2e8`..`0x48a31b`);
    /// zero (the `.bss` value) is neutral.
    std::array<double, 6> axis_values = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    /// `0x68cd30 + joystick*8 + 0..3` = Hat 1 X/Y and Hat 2 X/Y, preset
    /// indices clamped to 0..17 (`.bss`: all 0 = `Not Used`).
    std::array<int, 4> hats = {0, 0, 0, 0};

    /// Presence of the two hat pairs (`[0x4bf7e0 + dev*0x7c] & 0x100/0x200`).
    /// Both on by default (same enumeration caveat as `axis_present`).
    bool hat1 = true;
    bool hat2 = true;

    /// `[0x4bf7ec + dev*0x7c]`: the live hat/axis state bits that paint a hat
    /// label with font 5 (0x0A/0x05/0xA0/0x50, see the header comment).  The
    /// raw mask is kept because that is what the handler tests.
    unsigned hat_bits = 0;

    /// The 32 button masks of `0x4aa490 + joystick*0x80`; `-1` = no assignment
    /// (draws `Button N: None`), otherwise a single bit `1 << n` selects
    /// `kPlayerSetupActionNames[n]` through `0x4863a0`.  Defaults are the
    /// shipped `.data` values (`Fire`, `Activate`).
    std::array<int, kPlayerSetupButtonChannels> buttons =
        kPlayerSetupDefaultButtons;

    /// `[0x4bf7e8 + dev*0x7c]`: a set bit paints that button line with font 5
    /// (`0x48a1f7`..`0x48a20c`).  Zero (the `.bss` value) is neutral.
    unsigned pressed_buttons = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level.
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `player_setup_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten).
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read with
    /// `load_hud_lut`.  Used when `fade_lut_table` is null.
    std::string fade_lut_dump;
    /// A table already in memory (e.g. `build_hud_lut` output); wins over
    /// `fade_lut_dump`.  Only read during the call, the caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the handler's own counter `[0x67f10c]` (1 without a device,
/// `33 + present axes + 2 per present hat` with one — it counts 32 button
/// channels whether they are assigned or not); `sliders` and `checks` are
/// always 0 because screen 36 paints no `SLIDE`/`KNOB` and no
/// `CHECK`/`NOCHECK`; `arrows` the number of `MENUARRW` sprites (0..2);
/// `ink_pixels` the canvas pixels covered by at least one glyph pixel (unique,
/// independent of the background; the arrow sprite is not counted — the same
/// rule as the sibling modules); `missing_glyphs` the characters with no glyph
/// in the atlas (skipped, never fatal).
struct PlayerSetupRenderStats {
    int rows = 0;
    int sliders = 0;
    int checks = 0;
    int arrows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
};

/// `y` of content row `row`: `kPlayerSetupFirstRowY + kPlayerSetupItemStep *
/// row`.  The value is the unwindowed position; the renderer subtracts
/// `player_setup_scroll(cursor)` before painting.
int player_setup_row_y(int row);

/// Window top of `0x489dd6`..`0x489dec`: `max(0, cursor - 5)`.
int player_setup_scroll(int cursor);

/// `x` of the value column: `kPlayerSetupRowX + width(font 0,
/// kPlayerSetupValueMeasureLabel)` (`0x489e91`..`0x489eaa`).  The measured
/// value on the shipped `FONTS4` is 54, i.e. x = 67 (printed by the test).
int player_setup_value_x(const FontAtlas& row_face);

/// `[0x67f10c]` after the handler: `kPlayerSetupRowsNoDevice` when
/// `options.joystick < 0`, else `kPlayerSetupRowsDevice` + one per present axis
/// + two per present hat.  This is what the arrows scroll against.
int player_setup_rows(const PlayerSetupRenderOptions& options);

/// Number of content lines the frame has: 1 device row, the present axis rows,
/// two rows per present hat and all 32 button channels (an unassigned channel
/// still draws `Button N: None`), i.e. the same number `player_setup_rows`
/// returns — the handler starts `[0x67f10c]` at 33 to count those 32 channels
/// before it knows how many are assigned.
int player_setup_content_rows(const PlayerSetupRenderOptions& options);

/// Name of preset `index` (clamped to 0..17) from `0x4bc468`.
std::string_view player_setup_preset_name(int index);

/// Name of action `index` (clamped to 0..30) from `0x4bc558`.
std::string_view player_setup_action_name(int index);

/// The whole line of button channel `channel` (0..31): `Button <n>: <action>`
/// (`0x4bd8dc`) or `Button <n>: None` (`0x4bd8cc`).  A mask that is not `-1`
/// and not a single bit has no name in the original (`0x4863a0` would not
/// terminate); this model paints `None` for it.
std::string player_setup_button_text(int channel, int mask);

/// Paint screen 36 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The frame is painted in the handler's emission order: title (font 5,
/// centred, y 11), hint (font 0, centred, y 20), then the content rows of
/// `0x489d60` from the window top `max(0, cursor-5)` — the device row at y 28,
/// the present axis rows, the present hat rows, every assigned button channel —
/// with the cursor row in font 4, and last the two `MENUARRW` arrows at
/// (159,2)/(159,121).  Every glyph is clipped to the 196x131 text band.  With
/// `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.  Returns false +
/// `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_player_setup(const std::filesystem::path& game,
                         const PlayerSetupRenderOptions& options,
                         std::vector<std::uint32_t>& rgba, int& width, int& height,
                         PlayerSetupRenderStats* stats = nullptr,
                         std::string* error = nullptr);

} // namespace gm
