#pragma once

// One frame of screen 37, `NEW VIDEO MODE OK?` (`M8.8` slice 5).
//
// Division of labour
// ------------------
// This module is stateless: it paints the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3) from the options alone, exactly like
// `ui/local_players_render` (screen 21), `ui/game_type_render` (19) and
// `ui/difficulty_render` (23).  The cursor (`0x68ccf4`) and the navigation
// belong to the caller.  The building blocks are the `FONTS4` atlases of
// `FRONTEND/BMP.IDX` (title face 5, rows face 0, highlight face 4), the
// optional 196x131 `PLATE` copied by `0x486050` and the exact fade tables of
// `0x682f90`.  Nothing is decoded or measured twice: `render/text_layout` owns
// the atlases and the advances.
//
// Proven geometry (`0x488990`, docs/UI_SCREENS.md §2/§3.8)
// -------------------------------------------------------
// The handler is the shortest one of the menu family (from `0x488990` to the
// `ret` at `0x4889ee`; the jump table `0x4883d8` reaches it through the stub
// `0x487eec` -> `call 0x488990`).  It is one `rows`/`step` pair, one `PLATE`
// copy and three `0x485dd0` calls:
//
//   * `0x488990` calls `0x486050`, which copies the 196x131 `PLATE` into the
//     compose buffer `0x6865f8`.  Every text call writes into that copy, so
//     196x131 is a hard clip (§3);
//   * the title is `0x485dd0(font=5, text=0x4bd474 "NEW VIDEO MODE OK?",
//     centered=1, x=38, row=14, item=-1)` (`0x488995`..`0x4889b8`).  `centered`
//     makes `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`, integer division)
//     and `item == -1` zeroes `item*step`, so the title is centred at y = 14
//     (`0x485e04`..`0x485e09`);
//   * `0x4889a4` stores the step `0x662d58 = 16` and `0x4889ae` the row count
//     `0x67f10c = 2`, i.e. two items and no third line;
//   * `Yes` is `0x485dd0(font=0, text=0x4bd3dc "Yes", centered=0, x=38,
//     row=27, item=0)` (`0x4889c0`..`0x4889d4`) -> y = 27 + 0*16 = 27;
//   * `No` is `0x485dd0(font=0, text=0x4bc7a4 "No", centered=0, x=38, row=27,
//     item=1)` (`0x4889d7`..`0x4889eb`) -> y = 27 + 1*16 = **43**.  A
//     non-negative `row` means `y = row + item*[0x662d58]` (`0x485e4b`..
//     `0x485e5c`), which is exactly §3.8's "`Yes`/`No`, rows 27, step 16": the
//     base row is 27 for both items and the 16 px step spaces them (27, 43);
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the caller passed font 0 (`test esi,esi`
//     at `0x485e15`) the face becomes 4.  Both labels are normal items (0/1),
//     so either can be white.  There is no bar, box or `MENULIT` marker and no
//     scroll arrow: the screen has no `MENUARRW` call and the list ends at y 43
//     in a 131 px band;
//   * the cursor default is proven at the only entry that paints this screen:
//     the state-18 (SELECT VIDEO MODE) branch `0x48e182` picks a mode, stores
//     the *previous* mode index in `0x678180` (`0x4864a0` matches the 5-dword
//     descriptor `0x4c05c0` against the table `0x4bfd1c`), applies the chosen
//     one with `0x48ed10(cursor)` (`0x48e1a7`), then writes state `0x25` and
//     **cursor 0** (`0x48e1c6`).  So the original enters on item 0, `Yes`
//     (`kVideoModeDefaultCursor`).
//
// What the two answers do (`0x48e1db`, `0x48e4df`, `0x48f2be`)
// ------------------------------------------------------------
//   * confirm with item 1 (`No`): `0x48e1e0` compares the cursor with `ecx`,
//     which is `1` in this whole input handler (`mov ebx,1` at `0x48b9c4` and
//     `mov ebp,ecx` elsewhere), and calls `0x48ed10([0x678180])`, i.e. it
//     restores the previous mode;
//   * `ESC`: `0x48e4df` does the same restore for state `0x25` and leaves to
//     state `0x0c` (GRAPHICS OPTIONS) with the cursor `[0x660788]`;
//   * the `No` branch leaves to the same state `0x0c` and cursor;
//   * a timeout also answers `No`: `0x48f2be` restores `[0x678180]` once the
//     tick counter `[0x5a4a74]` passes `[0x678140]`, which the entry set to
//     `now + 1000` (`0x48e1cd`).
// `Yes` (item 0) keeps the applied mode.  This renderer paints the question;
// it does not apply or revert any mode (no display code here).
//
// Canvas, background and colours
// ------------------------------
// `VideoModeRenderOptions::background` names a `FRONTEND/BMP.IDX` entry
// painted at (0,0) before the text; an empty string means the opaque black
// canvas.  The proven panel of screen 37 is `PLATE` (196x131,
// `menu_panel_asset(Screen::NewVideoMode)`), but it is never painted
// implicitly, like screens 13/14/17/19/21/23/30.  Glyphs are clipped to the
// 196x131 band `0x485dd0` writes into.  Colours come from the atlas palette:
// title font 5 -> (155,15,15) dark red, rows font 0 -> (19,15,15) dark grey,
// highlight font 4 -> (239,239,239) white (the `FONTS4` faces,
// docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// `fade_lut` is the same exact index -> index path as `ui/local_players_render`
// and `ui/start_game_render`: the table is applied to the palette index of each
// painted pixel **before** the palette lookup (`render/hud_lut.h::map`).  The
// table either comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  Without both the call is a text
// refusal prefixed `video_mode_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"video_mode_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX` or `FONTS4` atlas, a
// background entry that is missing/broken/not 8 bpp, and an exact fade without
// a table and a dump (or with an unreadable/wrong-sized dump).  On failure
// `rgba` is cleared, `width`/`height` are 0 (never a stale or half-drawn
// picture) and `*stats` is zeroed.  `stats`/`error` may be null.  The call only
// reads the installation, is deterministic and does not depend on the current
// working directory.
//
// What is not drawn
// -----------------
// Keyboard/mouse input, the mode switch itself (`0x48ed10`), the revert on
// `No`/`ESC`/timeout, the timer `[0x678140]`, the `updown`/`select` sounds
// (`docs/UI_SOUNDS.md`) and the SELECT VIDEO MODE list (18) behind the
// question.  This module paints one frame; `frontend_menu` and the future CLI
// own the navigation.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kVideoModeCanvasWidth = 320;
inline constexpr int kVideoModeCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kVideoModeBackground = 0x000000FFu;

/// `NEW VIDEO MODE OK?` (`0x4bd474`): the title pushed at `0x4889a2`.
inline constexpr std::string_view kVideoModeTitle = "NEW VIDEO MODE OK?";

/// Item 0 (`0x4bd3dc`), pushed at `0x4889c8`; keeps the applied mode.
inline constexpr std::string_view kVideoModeYes = "Yes";
/// Item 1 (`0x4bc7a4`), pushed at `0x4889df`; the shared `No` label.
inline constexpr std::string_view kVideoModeNo = "No";

/// The panel of screen 37 (`0x486050`; `menu_panel_asset(Screen::NewVideoMode)`).
inline constexpr std::string_view kVideoModePanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kVideoModePanelWidth = 196;
inline constexpr int kVideoModePanelHeight = 131;

/// `0x67f10c = 2` (`0x4889ae`): `Yes` and `No`, two items and two lines.
inline constexpr int kVideoModeRows = 2;
/// `0x662d58 = 16` (`0x4889a4`): the item step of `0x485dd0`.
inline constexpr int kVideoModeItemStep = 16;
/// Item ids 0 (`Yes`) and 1 (`No`).
inline constexpr int kVideoModeItemCount = 2;
inline constexpr int kVideoModeNoItem = 1;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kVideoModeTitleFont = 5;
inline constexpr int kVideoModeRowFont = 0;
inline constexpr int kVideoModeSelectedFont = 4;

/// Title row (`push 0xe` at `0x488997`), centred in the band; `x = 38`
/// (`push 0x26`) is the `centered` call's fallback argument.
inline constexpr int kVideoModeTitleY = 14;
inline constexpr int kVideoModeTitleX = 38;
/// `x = 38` of both items (`push 0x26`), base row 27 (`push 0x1b`), step 16.
inline constexpr int kVideoModeItemX = 38;
inline constexpr int kVideoModeItemFirstY = 27;
/// `y` of item 1: 27 + 16 = **43**.
inline constexpr int kVideoModeNoY = kVideoModeItemFirstY + kVideoModeItemStep;

/// `[0x68ccf4] = 0` on entry: `0x48e1c6` writes the zero after the state.
inline constexpr int kVideoModeDefaultCursor = 0;

/// One selectable entry of the screen: the `item` id and its label VA.
struct VideoModeEntry {
    std::string_view label;
    int item = 0;
};

/// The two items in the order `0x4889c0`/`0x4889d7` paints them.
inline constexpr VideoModeEntry kVideoModeEntries[kVideoModeItemCount] = {
    {kVideoModeYes, 0},
    {kVideoModeNo, kVideoModeNoItem},
};

/// Optional parts of one frame.
struct VideoModeRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 37, but it is
    /// never painted implicitly.
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** `0..1` (0 = `Yes`, 1 = `No`).
    /// The selected item uses font 4.  The comparison is the exact one of
    /// `0x485e0d`: values outside `0..1` highlight nothing (the screen's own
    /// navigation keeps it in range), and a caller can pass `-1` for a frame
    /// without a highlight.
    int cursor = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/local_players_render`, docs/LOCAL_PLAYERS_RENDER.md
    /// §2).  Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `video_mode_render: fade LUT needs ...`.
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
/// `rows` is the number of **items** (`0x67f10c = 2`), which is also the number
/// of text lines of the list.  `ink_pixels` is the canvas pixels covered by at
/// least one glyph pixel (unique, independent of the background),
/// `missing_glyphs` the characters with no glyph in the atlas (skipped, never
/// fatal) and `arrows` the number of scroll sprites painted — always 0, screen
/// 37 has none; the field keeps the stats shape of the sibling renderers and
/// the test pins the 0.
struct VideoModeRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// Paint screen 37 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kVideoModeTitleY` (font 5); `Yes` sits at
/// (`kVideoModeItemX`, 27) and `No` at (`kVideoModeItemX`, 43) with font 0,
/// because both calls pass `row 27` and the step is 16.  The item equal to
/// `options.cursor` uses font 4 instead of font 0.  Every glyph is clipped to
/// the 196x131 text band.  With `options.fade_lut` every painted palette index
/// goes through `render/hud_lut.h::map` at the level before its colour is
/// looked up; the untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true
/// with `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_video_mode(const std::filesystem::path& game,
                       const VideoModeRenderOptions& options,
                       std::vector<std::uint32_t>& rgba, int& width, int& height,
                       VideoModeRenderStats* stats = nullptr,
                       std::string* error = nullptr);

} // namespace gm
