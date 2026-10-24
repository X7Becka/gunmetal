#pragma once

// One frame of screen 21, `NUMBER OF LOCAL PLAYERS` (`M8.8` slice 3).
//
// Division of labour
// ------------------
// This module is stateless: it paints the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3) from the options alone, exactly like `ui/game_type_render`
// (screen 19) and `ui/difficulty_render` (23).  The cursor (`0x68ccf4`) and the
// navigation belong to the caller.  The building blocks are the `FONTS4`
// atlases of `FRONTEND/BMP.IDX` (title face 5, rows face 0, highlight face 4),
// the optional 196x131 `PLATE` copied by `0x486050` and the exact fade tables
// of `0x682f90`.  Nothing is decoded or measured twice: `render/text_layout`
// owns the atlases and the advances.
//
// Proven geometry (`0x48a780`, docs/UI_SCREENS.md §2/§3.8)
// -------------------------------------------------------
// The handler is short (from `0x48a780` to the `ret` at `0x48a823`, then `nop`
// up to `0x48a830`); it is one `rows`/`step` pair, one `PLATE` copy and six
// `0x485dd0` calls:
//
//   * `0x48a780` stores `rows 3` into `0x67f10c` and `0x48a78a` sets the step
//     `0x662d58 = 27`; the three rows are `One Player` (item 0), `Two Players`
//     (item 1) and `Cancel` (item 2);
//   * `0x48a794` calls `0x486050`, which copies the 196x131 `PLATE` into the
//     compose buffer `0x6865f8`.  Every text call writes into that copy, so
//     196x131 is a hard clip (§3);
//   * the title is `0x485dd0(font=5, text=0x4bdb2c "NUMBER OF LOCAL PLAYERS",
//     centered=1, x=38, row=14, item=-1)` (`0x48a799`..`0x48a7a8`).  `centered`
//     makes `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`, integer division)
//     and `item == -1` zeroes `item*step`, so the title is centred at y = 14
//     (`0x485e04`..`0x485e09`, `0x485e4b`..`0x485e5c`);
//   * **two text lines per item**, both at `x = 38` (`push 0x26`) and font 0,
//     each pair sharing one `item` id so the highlight hits both lines:
//       item 0: `0x4bdb20 "One Player"`, `row = 27` → y = 27  (`0x48a7b0`)
//               `0x4bdb14 "Full Screen"`, `row = 36` → y = 36 (`0x48a7c7`)
//       item 1: `0x4bdb08 "Two Players"`, `row = 27` → y = 54 (`0x48a7de`)
//               `0x4bdaf8 "Split Screen"`, `row = 36` → y = 63 (`0x48a7f5`)
//       item 2: `0x4bda0c "Cancel"`,      `row = 27` → y = 81 (`0x48a80c`)
//     A non-negative `row` means `y = row + item*[0x662d58]` (`0x485e4b`..
//     `0x485e5c`), so the 27 px step spaces the *items* (27 -> 54 -> 81) and the
//     two lines of one item sit 9 px apart (27 -> 36).  That is exactly
//     §3.8's "two lines per item (27 and 36), `Cancel` — 27", i.e. base rows 27
//     and 36;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the caller passed font 0 (`test esi,esi` at
//     `0x485e15`) the face becomes 4.  Both lines of the selected item turn
//     white, and `Cancel` is a normal item (2), so it can be highlighted too.
//     There is no bar, box or `MENULIT` marker and no scroll arrow: the screen
//     has no `MENUARRW` call and the list ends at y 81 in a 131 px band.
//   * the cursor default is proven at the only entry that paints this screen:
//     `0x48b11c` writes state `0x15` and `0x48b126` copies `edx` into
//     `[0x68ccf4]` before `jmp 0x48a780`; `edx` is zeroed at `0x48b0c5`
//     (`xor edx,edx`), so the original enters on item 0, `One Player`
//     (`kLocalPlayersDefaultCursor`).  The branch is taken when the
//     multiplayer type `[0x664598]` is not 0 and not 3 (`0x48b115`/`0x48b117`,
//     docs/MAINLOOP.md §4.1).
//
// Canvas, background and colours
// ------------------------------
// `LocalPlayersRenderOptions::background` names a `FRONTEND/BMP.IDX` entry
// painted at (0,0) before the text; an empty string means the opaque black
// canvas.  The proven panel of screen 21 is `PLATE` (196x131,
// `menu_panel_asset(Screen::LocalPlayers)`), but it is never painted
// implicitly, like screens 13/14/17/19/23/30.  Glyphs are clipped to the
// 196x131 band `0x485dd0` writes into.  Colours come from the atlas palette:
// title font 5 -> (155,15,15) dark red, rows font 0 -> (19,15,15) dark grey,
// highlight font 4 -> (239,239,239) white (the `FONTS4` faces,
// docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// `fade_lut` is the same exact index -> index path as `ui/start_game_render`
// and `ui/difficulty_render`: the table is applied to the palette index of each
// painted pixel **before** the palette lookup (`render/hud_lut.h::map`).  The
// table either comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  Without both the call is a text
// refusal prefixed `local_players_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"local_players_render: "`: an empty game
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
// Keyboard/mouse input, the click that accepts a count, the `updown`/`select`
// sounds (`docs/UI_SOUNDS.md`), the player-name screens (24/25) and the level
// list (17) behind each choice.  This module paints one frame; `frontend_menu`
// and the future CLI own the navigation.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kLocalPlayersCanvasWidth = 320;
inline constexpr int kLocalPlayersCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kLocalPlayersBackground = 0x000000FFu;

/// `NUMBER OF LOCAL PLAYERS` (`0x4bdb2c`): the title pushed at `0x48a7a1`.
inline constexpr std::string_view kLocalPlayersTitle = "NUMBER OF LOCAL PLAYERS";

/// Item 0, first line (`0x4bdb20`), pushed at `0x48a7b8`.
inline constexpr std::string_view kLocalPlayersOnePlayer = "One Player";
/// Item 0, second line (`0x4bdb14`), pushed at `0x48a7cf`.
inline constexpr std::string_view kLocalPlayersFullScreen = "Full Screen";
/// Item 1, first line (`0x4bdb08`), pushed at `0x48a7e6`.
inline constexpr std::string_view kLocalPlayersTwoPlayers = "Two Players";
/// Item 1, second line (`0x4bdaf8`), pushed at `0x48a7fd`.
inline constexpr std::string_view kLocalPlayersSplitScreen = "Split Screen";
/// Item 2 (`0x4bda0c`), the only line of the third item, pushed at `0x48a814`.
inline constexpr std::string_view kLocalPlayersCancel = "Cancel";

/// The panel of screen 21 (`0x486050`; `menu_panel_asset(Screen::LocalPlayers)`).
inline constexpr std::string_view kLocalPlayersPanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kLocalPlayersPanelWidth = 196;
inline constexpr int kLocalPlayersPanelHeight = 131;

/// `0x67f10c = 3` (`0x48a780`): `One Player`, `Two Players`, `Cancel`.
inline constexpr int kLocalPlayersRows = 3;
/// `0x662d58 = 27` (`0x48a78a`): the item step of `0x485dd0`.
inline constexpr int kLocalPlayersItemStep = 27;
/// Item ids 0..1 are the player counts, 2 is `Cancel` (`push 0x2` at
/// `0x48a80c`).
inline constexpr int kLocalPlayersItemCount = 3;
inline constexpr int kLocalPlayersCancelItem = 2;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kLocalPlayersTitleFont = 5;
inline constexpr int kLocalPlayersRowFont = 0;
inline constexpr int kLocalPlayersSelectedFont = 4;

/// Title row (`push 0xe` at `0x48a79b`), centred in the band; `x = 38`
/// (`push 0x26`) is the `centered` call's fallback argument.
inline constexpr int kLocalPlayersTitleY = 14;
inline constexpr int kLocalPlayersTitleX = 38;
/// `x = 38` of every line (`push 0x26`), base row 27 (`push 0x1b`).
inline constexpr int kLocalPlayersItemX = 38;
inline constexpr int kLocalPlayersItemFirstY = 27;
/// The second line of an item is pushed with `row = 36`, i.e. 9 px below the
/// first one; `y = 36 + 27*item` follows from the same `row + item*step`.
inline constexpr int kLocalPlayersSecondLineOffset = 9;

/// `[0x68ccf4] = 0` on entry: `xor edx,edx` at `0x48b0c5`, then `0x48b126`
/// copies `edx` into the cursor before jumping to the painter.  Item 0.
inline constexpr int kLocalPlayersDefaultCursor = 0;

/// One selectable entry of the screen: the `item` id, its two lines (the second
/// one is empty for `Cancel`) and the label VAs from the handler.
struct LocalPlayersEntry {
    std::string_view first;
    std::string_view second;
    int item = 0;
};

/// The three items in the order `0x48a7b0`..`0x48a81b` paints them.
inline constexpr LocalPlayersEntry kLocalPlayersEntries[kLocalPlayersItemCount] = {
    {kLocalPlayersOnePlayer, kLocalPlayersFullScreen, 0},
    {kLocalPlayersTwoPlayers, kLocalPlayersSplitScreen, 1},
    {kLocalPlayersCancel, std::string_view{}, kLocalPlayersCancelItem},
};

/// Optional parts of one frame.
struct LocalPlayersRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 21, but it is
    /// never painted implicitly.
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** `0..2` (0 = `One Player`,
    /// 2 = `Cancel`), not a visible line.  Both lines of the selected item use
    /// font 4.  The comparison is the exact one of `0x485e0d`: values outside
    /// `0..2` highlight nothing (the screen's own navigation keeps it in
    /// range), and a caller can pass `-1` for a frame without a highlight.
    int cursor = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/start_game_render`, docs/START_GAME_RENDER.md §6).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `local_players_render: fade LUT needs ...`.
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
/// `rows` is the number of **items** (`0x67f10c = 3`), not of text lines: seven
/// lines are painted (two for item 0, two for item 1, one for `Cancel`).
/// `ink_pixels` is the canvas pixels covered by at least one glyph pixel
/// (unique, independent of the background), `missing_glyphs` the characters
/// with no glyph in the atlas (skipped, never fatal) and `arrows` the number of
/// scroll sprites painted — always 0, screen 21 has none; the field keeps the
/// stats shape of the sibling renderers and the test pins the 0.
struct LocalPlayersRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// Paint screen 21 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kLocalPlayersTitleY` (font 5); the two lines of
/// `item` sit at (`kLocalPlayersItemX`, `27 + 27*item`) and
/// (`kLocalPlayersItemX`, `36 + 27*item`) with font 0, and `Cancel` (item 2)
/// has only the first line (y 81).  The item equal to `options.cursor` uses
/// font 4 instead of font 0 for *both* of its lines.  Every glyph is clipped to
/// the 196x131 text band.  With `options.fade_lut` every painted palette index
/// goes through `render/hud_lut.h::map` at the level before its colour is
/// looked up; the untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true
/// with `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_local_players(const std::filesystem::path& game,
                          const LocalPlayersRenderOptions& options,
                          std::vector<std::uint32_t>& rgba, int& width, int& height,
                          LocalPlayersRenderStats* stats = nullptr,
                          std::string* error = nullptr);

} // namespace gm
