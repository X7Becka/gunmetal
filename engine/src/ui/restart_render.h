#pragma once

// One frame of screen 6, `RESTART GAME` (`M8.8` slice 6).
//
// Division of labour
// ------------------
// This module is stateless: it paints the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3) from the options alone, exactly like `ui/quit_render`
// (screen 33), `ui/game_type_render` (19), `ui/difficulty_render` (23) and
// `ui/local_players_render` (21).  The cursor (`0x68ccf4`), the gates that
// decide *which* rows exist and the actions behind them belong to the caller.
// The building blocks are the `FONTS4` atlases of `FRONTEND/BMP.IDX` (title
// face 5, rows face 0, highlight face 4), the optional 196x131 `PLATE` copied
// by `0x486050` and the exact fade tables of `0x682f90`.  Nothing is decoded or
// measured twice: `render/text_layout` owns the atlases and the advances.
//
// Proven geometry (`0x488850`, docs/UI_SCREENS.md §2/§3.8,
// docs/GAME_OVER.md §4.1)
// ---------------------------------------------------------------------------
// The builder is short: `0x488850` (push esi / `xor esi,esi`) to the `ret` at
// `0x488980`, then `nop` up to `0x488990` (screen 37).  It is one `PLATE` copy,
// one title call and up to four item calls; `esi` is the running item id:
//
//   * `0x488851` zeroes the item counter `esi`; `0x488853` calls `0x486050`,
//     which copies the 196x131 `PLATE` into the compose buffer `0x6865f8`;
//     every text call writes into that copy, so 196x131 is a hard clip;
//   * `0x488867` stores the step `[0x662d58] = 0x10` (16);
//   * the title is `0x485dd0(font=5, text=0x4bd464 "RESTART GAME", centered=1,
//     x=38, row=14, item=-1)` (`0x488858`..`0x488871`).  `centered` makes
//     `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`), `item == -1` makes the
//     row absolute, so the title is centred at y = 14 (the x=38 argument is
//     the `centered` branch's unused fallback);
//   * every item is `0x485dd0(font=0, centered=0, x=10, row=27, item=esi)`
//     (`push 0xa` / `push 0x1b` / `push 0x0`), i.e. `y = 27 + 16*item`:
//     27, 43, 59, 75.  The `item` id is dynamic: the emitted rows are numbered
//     `0..n-1` and the id is stored next to the label VA in
//     `0x4bc278/0x4bc27c/0x4bc280/0x4bc284` (`0x48888f`, `0x4888bb`,
//     `0x488931`, `0x48895c`); `-1` means "this row is not drawn";
//   * the count is written **last**: `0x488979 stores esi into 0x67f10c`, i.e.
//     `rows` of §2 is the number of emitted items, not a constant.  The code
//     therefore cannot produce a fixed table: the rows are gated at runtime
//     (docs/GAME_OVER.md §4.1):
//       item 0 `Reload last saved game` (`0x4bd44c`) — `[0x4bc274] >= 0`
//              (index of the last save slot; `-1` = none)  `0x488876`..`0x488880`
//       item 1 `Restart level with same R.P.V.` (`0x4bd42c`) — `[0x836ad0] != 0`
//              `0x4888b0`..`0x4888b7`
//       item 2 `Restart level from HQ` (`0x4bd3fc`) — the level name
//              `0x5a48b8` is not `LEVEL20`/`SECRT1`/`SECRT2` (`0x4888e4`..
//              `0x488924`, three `strcmp`-style calls to `0x416850`) **and**
//              `[0x818650] != 0` (`0x488926`..`0x48892d`)
//       item 3 `Quit to title` (`0x4bd3ec`) — unconditional (`0x48895a`..`0x488971`)
//   * there is no other call: no arrow (`MENUARRW`), no slider, no checkbox and
//     **no separate `ESC` line** on this screen.  The way out is the last row
//     `Quit to title` (`0x48da25` → `[0x4bc440] = 1` at `0x48da31`), not a hint;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the caller passed font 0 the face becomes 4.
//     There is no bar, box or `MENULIT` marker.  The cursor is a front-end dword
//     (`0x68ccf4`, docs/UI.md §6.2), an index into the emitted rows; neither
//     entry into state 6 (`0x48b54c` from the in-game menu, `0x443af9` from the
//     mission-end path) writes it, so a stale value simply matches no row.
//
// The count model (model, not code)
// ---------------------------------
// The three gates are independent, so the original can show any subset of
// `Reload`/`R.P.V.`/`HQ` followed by `Quit to title` (1..4 rows).  §2/§3.8 fix
// the proven range at **2..4**; this API follows the doc with one `items`
// number and takes the **first `items-1` gated rows plus the unconditional
// `Quit to title`**:
//
//   items 2 -> Reload last saved game, Quit to title
//   items 3 -> Reload last saved game, Restart level with same R.P.V., Quit to title
//   items 4 -> Reload last saved game, Restart level with same R.P.V.,
//              Restart level from HQ, Quit to title
//
// That is exactly the handler's emission order with the trailing gates closed,
// and each of the three frames is reachable in the original (items 2/3 need
// only the `Reload` gate open, items 4 all three).  Subsets that skip a leading
// gated row (e.g. `R.P.V.` + `HQ` + `Quit`) need per-row gates and are out of
// scope of this frame renderer (docs/RESTART_RENDER.md §5).
//
// Canvas, background and colours
// ------------------------------
// `RestartRenderOptions::background` names a `FRONTEND/BMP.IDX` entry painted
// at (0,0) before the text; an empty string means the opaque black canvas,
// which is what the original shows: entering state 6 clears the 320x200 screen
// to 0 first (`0x487500(0, 0, 320, 200, 0)` at `0x48b53f`..`0x48b55c`) and only
// then composes the `PLATE`.  The proven panel of screen 6 is `PLATE`
// (`menu_panel_asset(Screen::Restart)`), but it is never painted implicitly,
// like screens 13/14/17/19/21/23/30/33.  Glyphs are clipped to the 196x131 band
// `0x485dd0` writes into.  Colours come from the atlas palette: title font 5 ->
// (155,15,15) dark red, rows font 0 -> (19,15,15) dark grey, highlight font 4 ->
// (239,239,239) white (the `FONTS4` faces, docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// `fade_lut` is the same exact index -> index path as `ui/start_game_render`
// and `ui/quit_render`: the table is applied to the palette index of each
// painted pixel **before** the palette lookup (`render/hud_lut.h::map`).  The
// table either comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  Without both the call is a text
// refusal prefixed `restart_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"restart_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX` or one of the faces 5/0/4, a
// background entry that is missing/broken/not 8 bpp, and an exact fade without
// a table and a dump (or with an unreadable/wrong-sized dump).  `items` outside
// 2..4 is **clamped**, not refused: the original's row count is a runtime value
// (`esi` -> `0x67f10c`) and the cursor contract already tolerates values the
// screen cannot produce, so a count of 1 or 5 is a caller's modelling error,
// not a broken asset; the frame stays paintable and the clamp is documented.
// On failure `rgba` is cleared, `width`/`height` are 0 (never a stale or
// half-drawn picture) and `*stats` is zeroed.  `stats`/`error` may be null.  The
// call only reads the installation, is deterministic and does not depend on the
// current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input, the highlight motion, the gate evaluation (`0x4bc274`/`0x836ad0`/
// `0x818650`/`0x5a48b8`), the actions (`0x442340(slot)`, `0x4428f0(0/1)`,
// `[0x4bc440] = 1`), UI sounds and the actual restart/reload at all.  This
// module paints one frame; `frontend_menu` and the future CLI own navigation.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kRestartCanvasWidth = 320;
inline constexpr int kRestartCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph (the
/// colour the original clears the screen to on entry, `0x487500`).
inline constexpr std::uint32_t kRestartBackground = 0x000000FFu;

/// `RESTART GAME` (`0x4bd464`): the title pushed at `0x488860`.
inline constexpr std::string_view kRestartTitle = "RESTART GAME";

/// Gated row 0 (`0x4bd44c`), `[0x4bc274] >= 0`, pushed at `0x48888a`.
inline constexpr std::string_view kRestartReloadLastSaved = "Reload last saved game";
/// Gated row 1 (`0x4bd42c`), `[0x836ad0] != 0`, pushed at `0x4888c8`.
inline constexpr std::string_view kRestartSameRpv = "Restart level with same R.P.V.";
/// Gated row 2 (`0x4bd3fc`), level not `LEVEL20`/`SECRT1`/`SECRT2` **and**
/// `[0x818650] != 0`, pushed at `0x48893e`.
inline constexpr std::string_view kRestartFromHq = "Restart level from HQ";
/// The unconditional last row (`0x4bd3ec`), pushed at `0x488969`.
inline constexpr std::string_view kRestartQuitToTitle = "Quit to title";

/// The three gated labels in emission order (`0x4bd44c`, `0x4bd42c`, `0x4bd3fc`).
inline constexpr std::string_view kRestartGatedItems[3] = {
    kRestartReloadLastSaved,
    kRestartSameRpv,
    kRestartFromHq,
};

/// `0x67f10c` is written last (`0x488979`) with the emitted item count: 2..4
/// rows, the last one always `Quit to title`.
inline constexpr int kRestartMinItems = 2;
inline constexpr int kRestartMaxItems = 4;
inline constexpr int kRestartMaxRows = kRestartMaxItems;

/// The panel of screen 6 (`0x486050`; `menu_panel_asset(Screen::Restart)`).
inline constexpr std::string_view kRestartPanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kRestartPanelWidth = 196;
inline constexpr int kRestartPanelHeight = 131;

/// `0x662d58 = 16` (`0x488867`): the item step of `0x485dd0`.
inline constexpr int kRestartItemStep = 16;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kRestartTitleFont = 5;
inline constexpr int kRestartRowFont = 0;
inline constexpr int kRestartSelectedFont = 4;

/// Title row (`push 0xe` at `0x48885a`), centred in the band; `x = 38`
/// (`push 0x26`) is the `centered` call's fallback argument.
inline constexpr int kRestartTitleY = 14;
inline constexpr int kRestartTitleX = 38;
/// `x = 10` of every row (`push 0xa`), base row 27 (`push 0x1b`), step 16:
/// y = 27, 43, 59, 75.
inline constexpr int kRestartItemX = 10;
inline constexpr int kRestartItemFirstY = 27;

/// Neutral API default: the original does not initialise `[0x68ccf4]` at either
/// entry into state 6, so there is no proven "first frame" cursor; 0 is the
/// first emitted row.
inline constexpr int kRestartDefaultCursor = 0;

/// `items` clamped to the proven 2..4 (`0x67f10c` written from `esi`).
int restart_item_count(int requested);

/// `y` of row `item` (`kRestartItemFirstY + kRestartItemStep * item`).
int restart_item_y(int item);

/// Label of row `item` for a frame of `items` rows (clamped): the first
/// `items-1` gated labels, then `Quit to title`.  An out-of-range `item`
/// returns the last label (`Quit to title`).
std::string_view restart_item_label(int item, int items);

/// Optional parts of one frame.
struct RestartRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 6, but it is
    /// never painted implicitly.
    std::string background;
    /// Number of emitted rows (`0x67f10c`), clamped to 2..4; the last row is
    /// always `Quit to title` (see the count model in the header comment).
    int items = kRestartMinItems;
    /// The cursor `[0x68ccf4]`: an **item id** `0..items-1`, not a text line.
    /// The comparison is the exact one of `0x485e0d`, so values outside the
    /// range highlight nothing (the original's cursor can be stale) and a
    /// caller can pass `-1` for a frame without a highlight.
    int cursor = kRestartDefaultCursor;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/start_game_render`, docs/START_GAME_RENDER.md §6).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `restart_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten).
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read
    /// with `load_hud_lut`.  Used when `fade_lut_table` is null; a missing or
    /// wrong-sized file is a text refusal, never a silently unfaded frame.
    std::string fade_lut_dump;
    /// A table already in memory (e.g. `build_hud_lut` output); wins over
    /// `fade_lut_dump`.  Only read during the call, the caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the number of emitted items (`0x67f10c`), 2..4; `ink_pixels` the
/// canvas pixels covered by at least one glyph pixel (unique, independent of
/// the background), `missing_glyphs` the characters with no glyph in the atlas
/// (skipped, never fatal) and `arrows` the number of `MENUARRW` sprites painted.
/// Screen 6 has no scroll window and no arrow call at `0x488850`, so `arrows`
/// is always 0; the field keeps the stats shape of the sibling renderers and
/// the test pins the 0.
struct RestartRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// Paint screen 6 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kRestartTitleY` (font 5); row `item` sits at
/// (`kRestartItemX`, `kRestartItemFirstY + kRestartItemStep * item`) with
/// `restart_item_label(item, items)` (font 0); the row equal to
/// `options.cursor` uses font 4 instead.  Every glyph is clipped to the 196x131
/// text band.  With `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_restart(const std::filesystem::path& game,
                    const RestartRenderOptions& options,
                    std::vector<std::uint32_t>& rgba, int& width, int& height,
                    RestartRenderStats* stats = nullptr,
                    std::string* error = nullptr);

} // namespace gm
