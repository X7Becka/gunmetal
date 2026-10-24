#pragma once

// One frame of screen 27, `MULTIPLAY GAME OPTIONS` (`M8.8` slice 9).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/mouse_setup_render`
// 22, `ui/game_type_render` 19, `ui/difficulty_render` 23,
// `ui/local_players_render` 21, `ui/video_mode_render` 37, `ui/restart_render`
// 6): the caller owns the state (the cursor `0x68ccf4`, the match type
// `0x68cd84`, the starting-cash index `0x4bc728` and the option bits
// `0x68cd88`) and this module paints it into the 320x200 mode-13h canvas
// (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads the keyboard,
// `GUNMETAL.CFG`, sounds or saves, and nothing is written back.
//
// Proven geometry (`0x4885c0`, docs/UI_SCREENS.md §2/§3.8)
// -------------------------------------------------------
// The handler is `sub esp,0x3c` .. `ret` at `0x488778`; it writes `rows = 8`
// (`0x4885c3` -> `0x67f10c`) and `step = 16` (`0x4885cd` -> `0x662d58`) before
// copying `PLATE` (`0x4885d7` -> `0x486050`, the 196x131 compose buffer
// `0x6865f8`, so 196x131 is the hard text clip).  `0x485dd0` computes
// `y = row + step*item` for `row >= 0` and `y = -row` for `row < 0`
// (`0x485e4b`/`0x485e2b`), so the `row` immediates of the calls give:
//
//   item  drawn text                                            x    row   y
//   0     `[0x4bc6e8 + [0x68cd84]*4]` (match type `%s`)         23   22    22
//   1     `sprintf("Starting Cash: %ld", 0x4bc6f8[[0x4bc728]])` 23   16    32
//   2     `Teamplay` (`0x4bd368`)                               35   13    45
//   3     `No Enemies` (`0x4bd35c`)                             35   12    60
//   4     `Can't Buy Ammo` (`0x4bd34c`)                         35   11    75
//   5     `Handicap Better Players` (`0x4bd334`)                35   10    90
//   6     `Allow Mouse Cheating` (`0x4bd31c`)                   35    7   103
//   7     `START GAME!` (`0x4bd310`)                            23  -113  113
//
// The title is `0x485dd0(font=5, text=0x4bd388 "MULTIPLAY GAME OPTIONS",
// centered=1, x=38, row=11, item=-1)` (`0x4885dc`..`0x4885eb`): `centered`
// replaces `x` by `(196 - width)/2` (`0x485de2`), `item == -1` makes the row
// absolute, so the title sits at y = 11 and the `x = 38` argument is the unused
// fallback of the centred branch.
//
// Highlight: `0x485dd0` switches the face of the caller to font 4 when
// `item == [0x68ccf4]` **and** the caller passed font 0 (`0x485e0d`..
// `0x485e19`).  Rows 0..6 pass font 0, so the cursor turns them white; row 7
// passes font 5 (`0x48875d`) or font 0 when `[0x68ccf4] == 7` (`0x48874b`,
// `0x48876b`), so `START GAME!` is dark red normally and white when selected.
// There is no bar, box or `MENULIT` marker.  The cursor is an item id 0..7 (the
// input handler clamps with `cmp eax,7; ja` at `0x48dba9`), so out-of-range
// values highlight nothing.
//
// Checkboxes (`0x4833e0`)
// -----------------------
// Five calls, each with a plate destination address, stride 196 and the masked
// bit of `[0x68cd88]`:
//
//   object        offset-0x6865f8   (x, y)  bit   row (text y)
//   `0x688264`    0x1c6c            (24,37) 0x01  item 2 `Teamplay`
//   `0x688de0`    0x27e8            (24,52) 0x04  item 3 `No Enemies`
//   `0x68995c`    0x3364            (24,67) 0x10  item 4 `Can't Buy Ammo`
//   `0x68a4d8`    0x3ee0            (24,82) 0x20  item 5 `Handicap ...`
//   `0x68aecc`    0x48d4            (24,95) 0x40  item 6 `Allow Mouse ...`
//
// `0x4833e0` copies the 16-row `CHECK` (8x16) from `0x67fc10` when the bit is
// set and the 8-row `NOCHECK` (8x8) from `0x683fb8` **8 rows lower** when it is
// clear, so both boxes end on the text row: the anchor is `(24, text_y - 8)`.
// Source palette index 0 is transparent (`test dl,dl; je`, `0x483413`).  The
// shipped `CHECK` art fills all 16 rows (its lower rows are the only ones that
// share the 8-row text line), so a checked box reaches 8 rows above the text;
// the test pins that geometry.
//
// There is **no `SLIDE`/`KNOB` call** in `0x4885c0` (unlike 11/12/22/32), so
// screen 27 has no sliders; `stats.sliders` stays 0 and the module does not
// load those two assets.  The match type and the starting cash are value rows
// changed with the arrow keys, not sliders.
//
// Proven values and ranges (listing + `.data`/`.bss`)
// ---------------------------------------------------
// * match type `0x68cd84` (`0x4885f0`): index 0..2 into the three pointers at
//   `0x4bc6e8` (`Normal Deathmatch` `0x4bc77c`, `Scavenger Hunt Deathmatch`
//   `0x4bc760`, `Deathmatch Classic` `0x4bc74c`).  `.bss`, so 0 unless the
//   config loader (`0x484fb7`) overwrites it; the input handler wraps with
//   `idiv 3` (`0x48dbc6`).
// * starting cash `0x4bc728` (`0x48860f`): index 0..11 into the twelve integers
//   at `0x4bc6f8` (36000, 50000, 75000, 100000, 150000, 200000, 350000,
//   500000, 750000, 1000000, 2000000, 5000000); the handler prints the *value*
//   (`Starting Cash: %ld`) and the input wraps with `idiv 12` (`0x48dbdd`).
//   `.data` initial value 4 (150000); the shipped `GUNMETAL.CFG` carries 7
//   (500000, docs/CONFIG.md §3).
// * option bits `0x68cd88` (`0x488646`..`0x488710`): bit 0 Teamplay (mask 0x01),
//   bit 2 No Enemies (0x04), bit 4 Can't Buy Ammo (0x10), bit 5 Handicap
//   Better Players (0x20), bit 6 Allow Mouse Cheating (0x40) — the bit order of
//   docs/CONFIG.md §3 (`deathmatch_flags`, `0x1dc`).  `.bss`, so 0.
// * the entry cursor is 7: the two transitions out of screen 19 write
//   `[0x68ccf4] = 7` right before calling the builder (`0x48dcee`, `0x48dd81`).
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE`, `CHECK` and `NOCHECK`
// (docs/UI_SCREENS.md §9) plus the `FONTS4` faces 5/0/4.  `PLATE` is loaded and
// validated because `0x4885d7` copies it as the frame buffer; the frame itself
// is painted on opaque black unless `background` names an entry (`"PLATE"`
// reproduces the panel), because where the engine blits the 196x131 compose
// buffer on the 320x200 canvas is not documented (docs/MENU_RENDER.md §2).
//
// Colours come from the atlas palette: title font 5 -> (155,15,15) dark red,
// items font 0 -> (19,15,15) dark grey, highlight font 4 -> (239,239,239)
// white (docs/MENU_RENDER.md §3).  `PLATE`/`CHECK`/`NOCHECK` use their own BMP
// palettes; index 0 is the transparent key of the checkbox sprites.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/mouse_setup_render` and
// `ui/restart_render`: every painted pixel's palette index goes through
// `render/hud_lut.h::map(level, index)` **before** its colour is looked up.  The
// table comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  With neither, the call is a text
// refusal prefixed `match_setup_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"match_setup_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken/not-196x131 `PLATE`, a missing/broken/not-8x16 `CHECK`, one
// not 8x8 `NOCHECK`, a background entry that is missing/broken/not 8 bpp, and
// an exact fade without a table and a dump (or with an unreadable dump).
// Out-of-range *values* (a cursor of 99, a match type of -3, a cash index of
// 40, unknown flag bits) are clamped or ignored, not refused: the original
// keeps its own indices in range (`idiv 3`, `idiv 12`) and the screen stays
// paintable.  On failure `rgba` is cleared, `width`/`height` are 0 (never a
// stale or half-drawn picture) and `*stats` is zeroed.  `stats`/`error` may be
// null.  The call only reads the installation, is deterministic and does not
// depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the cursor/arrow/click handling of `0x48db9c`, `Esc` -> state 5 at
// `0x48db8c`, `START GAME!` -> `0x48b0c0`), UI sounds, the actual
// `GUNMETAL.CFG` round-trip and the match-type -> `game_mode` conversion
// (`0x48b0c0`, docs/DM_RULES.md §2.2).  There is no `MENUARRW` call in
// `0x4885c0`, so `arrows` is always 0; the field keeps the stats shape of the
// sibling renderers.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kMatchSetupCanvasWidth = 320;
inline constexpr int kMatchSetupCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kMatchSetupBackground = 0x000000FFu;

/// `0x4bd388`, pushed at `0x4885e4`.
inline constexpr std::string_view kMatchSetupTitle = "MULTIPLAY GAME OPTIONS";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x4885d7` copies it into
/// `0x6865f8` (`0x486050`), so this is also the text clip.
inline constexpr std::string_view kMatchSetupPanelName = "PLATE";
inline constexpr int kMatchSetupPanelWidth = 196;
inline constexpr int kMatchSetupPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, items 0, highlight 4 (three `FONTS4`
/// faces).  `START GAME!` is passed font 5 and joins the highlight only when
/// the cursor is 7 (`0x48874b`/`0x48876b`).
inline constexpr int kMatchSetupTitleFont = 5;
inline constexpr int kMatchSetupRowFont = 0;
inline constexpr int kMatchSetupSelectedFont = 4;

/// Title row (`push 0xb` at `0x4885de`), centred in the 196-px band; `x = 38`
/// (`push 0x26` at `0x4885e0`) is the fallback argument of the centred branch.
inline constexpr int kMatchSetupTitleY = 11;
inline constexpr int kMatchSetupTitleX = 38;

/// `rows = 8` (`0x4885c3`), `step = 16` (`0x4885cd`); the row immediates of
/// the nine `0x485dd0` calls give these absolute rows (item 7 uses the negative
/// row `-113`).
inline constexpr int kMatchSetupRows = 8;
inline constexpr int kMatchSetupItemStep = 16;
inline constexpr std::array<int, kMatchSetupRows> kMatchSetupItemY = {
    22, 32, 45, 60, 75, 90, 103, 113};
inline constexpr std::array<int, kMatchSetupRows> kMatchSetupItemRowArg = {
    22, 16, 13, 12, 11, 10, 7, -113};

/// `x = 23` for items 0/1/7 (`push 0x17`), `x = 35` for items 2..6
/// (`push 0x23`).
inline constexpr int kMatchSetupValueX = 23;
inline constexpr int kMatchSetupOptionX = 35;

/// `START GAME!` (item 7) and the item id the input handler clamps to
/// (`cmp eax,7; ja` at `0x48dba9`).
inline constexpr int kMatchSetupStartGameItem = 7;
inline constexpr int kMatchSetupStartGameY = 113;

/// Checkbox sprites of `FRONTEND/BMP.IDX` (§9): `CHECK` 8x16, `NOCHECK` 8x8.
/// `0x4833e0` draws `NOCHECK` `CHECK.height - NOCHECK.height = 8` rows below
/// its anchor.
inline constexpr int kMatchSetupCheckWidth = 8;
inline constexpr int kMatchSetupCheckHeight = 16;
inline constexpr int kMatchSetupNoCheckWidth = 8;
inline constexpr int kMatchSetupNoCheckHeight = 8;
inline constexpr int kMatchSetupNoCheckDrop = kMatchSetupCheckHeight - kMatchSetupNoCheckHeight;

/// Five checkbox rows: items 2..6, anchors `(24, text_y - 8)` = 37/52/67/82/95
/// (`0x688264`/`0x688de0`/`0x68995c`/`0x68a4d8`/`0x68aecc` minus `0x6865f8`).
inline constexpr int kMatchSetupCheckboxCount = 5;
inline constexpr int kMatchSetupCheckboxFirstItem = 2;
inline constexpr int kMatchSetupCheckboxX = 24;
inline constexpr int kMatchSetupCheckboxYOffset = -8;

/// `[0x68cd88]` bits, in row order (docs/CONFIG.md §3, `deathmatch_flags`).
inline constexpr int kMatchSetupFlagTeamplay = 0x01;
inline constexpr int kMatchSetupFlagNoEnemies = 0x04;
inline constexpr int kMatchSetupFlagCantBuyAmmo = 0x10;
inline constexpr int kMatchSetupFlagHandicap = 0x20;
inline constexpr int kMatchSetupFlagMouseCheating = 0x40;
inline constexpr std::array<int, kMatchSetupCheckboxCount> kMatchSetupCheckboxFlag = {
    kMatchSetupFlagTeamplay, kMatchSetupFlagNoEnemies, kMatchSetupFlagCantBuyAmmo,
    kMatchSetupFlagHandicap, kMatchSetupFlagMouseCheating};
/// Bits the screen actually reads; everything else in `0x68cd88` is ignored.
inline constexpr int kMatchSetupFlagMask =
    kMatchSetupFlagTeamplay | kMatchSetupFlagNoEnemies | kMatchSetupFlagCantBuyAmmo |
    kMatchSetupFlagHandicap | kMatchSetupFlagMouseCheating;

/// Match-type pointers at `0x4bc6e8` (`0x4885f8`), 3 entries (`idiv 3`).
inline constexpr int kMatchSetupMatchTypeCount = 3;
inline constexpr std::array<std::string_view, kMatchSetupMatchTypeCount>
    kMatchSetupMatchTypes = {"Normal Deathmatch",         // 0x4bc77c
                             "Scavenger Hunt Deathmatch", // 0x4bc760
                             "Deathmatch Classic"};       // 0x4bc74c

/// The twelve `long` values at `0x4bc6f8` (`0x48861c`, `idiv 12` at
/// `0x48dbdd`); the menu stores the index `0x4bc728` and prints the value.
inline constexpr int kMatchSetupCashCount = 12;
inline constexpr std::array<long long, kMatchSetupCashCount> kMatchSetupStartingCash = {
    36000, 50000, 75000, 100000, 150000, 200000,
    350000, 500000, 750000, 1000000, 2000000, 5000000};

/// `.data`/`.bss` initial values of `0x68cd84`/`0x4bc728`/`0x68cd88`: 0, 4
/// (150000) and 0.  The shipped `GUNMETAL.CFG` carries 0/7/4 and the loader
/// (`0x484fb7`..`0x484fcc`) overwrites them, so these are defaults only.
inline constexpr int kMatchSetupMatchTypeDefault = 0;
inline constexpr int kMatchSetupCashDefault = 4;
inline constexpr int kMatchSetupFlagsDefault = 0;

/// `[0x68ccf4]` is written to 7 by both transitions into this screen
/// (`0x48dcee`, `0x48dd81`), so 7 is the entry cursor.  `0` is the neutral
/// default of this API (first row), like the sibling renderers.
inline constexpr int kMatchSetupEntryCursor = kMatchSetupStartGameItem;
inline constexpr int kMatchSetupDefaultCursor = 0;

/// Optional parts of one frame.
struct MatchSetupRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 27 and is painted
    /// from the copy that is validated anyway; it is never painted implicitly
    /// (see the header comment).
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** 0..7, not a text line.  The
    /// comparison is the exact one of `0x485e0d`, so values outside the range
    /// highlight nothing (`-1` = a frame without a highlight).
    int cursor = kMatchSetupDefaultCursor;
    /// `[0x68cd84]`, index into `kMatchSetupMatchTypes` (clamped to 0..2).
    int match_type = kMatchSetupMatchTypeDefault;
    /// `[0x4bc728]`, index into `kMatchSetupStartingCash` (clamped to 0..11);
    /// item 1 prints the value, not the index.
    int starting_cash = kMatchSetupCashDefault;
    /// `[0x68cd88]`, the `kMatchSetupFlag*` bits; unknown bits are ignored.
    int flags = kMatchSetupFlagsDefault;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/mouse_setup_render`, docs/MATCH_SETUP_RENDER.md §2).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `match_setup_render: fade LUT needs ...`.
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
/// `rows` is the fixed `0x67f10c` value 8; `sliders` the `SLIDE`/`KNOB` pairs
/// (screen 27 has none, so always 0); `checks` the five `CHECK`/`NOCHECK`
/// sprites; `ink_pixels` the canvas pixels covered by at least one glyph or
/// sprite pixel (unique; the optional backdrop panel is not counted, the same
/// rule as `ui/menu_render`/`ui/restart_render`); `missing_glyphs` the
/// characters with no glyph in the atlas (skipped, never fatal); `arrows` the
/// number of `MENUARRW` sprites painted — screen 27 has no arrow call at
/// `0x4885c0`, so it is always 0 and the test pins the 0.
struct MatchSetupRenderStats {
    int rows = 0;
    int sliders = 0;
    int checks = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// `y` of item `item` (`kMatchSetupItemY`); an out-of-range item returns the
/// `START GAME!` row.
int match_setup_item_y(int item);

/// `x` of item `item`: 23 for items 0/1/7, 35 for items 2..6.
int match_setup_item_x(int item);

/// Clamp a match-type index to the proven 0..2.
int match_setup_match_type(int requested);

/// Clamp a starting-cash index to the proven 0..11.
int match_setup_cash_index(int requested);

/// The value printed by item 1 for a (clamped) cash index.
long long match_setup_starting_cash(int index);

/// True when the checkbox of item `item` (2..6) is checked in `flags`; other
/// items return false.
bool match_setup_checked(int flags, int item);

/// Anchor `y` of the checkbox of item `item`: `match_setup_item_y(item) - 8`
/// (`CHECK` top; `NOCHECK` goes 8 rows lower, `0x4833e0`).
int match_setup_checkbox_y(int item);

/// The label of item `item` for `options` (the handler's `%s`/`%ld`
/// formatting): the match type, `Starting Cash: <value>`, `Teamplay`,
/// `No Enemies`, `Can't Buy Ammo`, `Handicap Better Players`,
/// `Allow Mouse Cheating`, `START GAME!`.  An out-of-range `item` returns
/// `START GAME!`'s text.
std::string match_setup_label(int item, const MatchSetupRenderOptions& options);

/// Paint screen 27 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kMatchSetupTitleY` (font 5); item `item` sits at
/// (`match_setup_item_x(item)`, `match_setup_item_y(item)`) with
/// `match_setup_label(item, options)` — font 0 for items 0..6, font 5 for
/// `START GAME!` — and the item equal to `options.cursor` uses font 4 instead.
/// Each checkbox row paints `CHECK` at (`kMatchSetupCheckboxX`,
/// `match_setup_checkbox_y(item)`) when its bit is set and `NOCHECK` 8 rows
/// lower otherwise.  Every glyph and sprite is clipped to the 196x131 text
/// band.  With `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_match_setup(const std::filesystem::path& game,
                        const MatchSetupRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        MatchSetupRenderStats* stats = nullptr,
                        std::string* error = nullptr);

} // namespace gm
