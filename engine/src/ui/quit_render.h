#pragma once

// One frame of screen 33, `QUIT GAME?` / `QUIT OPTIONS` (`M8.8` slice 4).
//
// Division of labour
// ------------------
// The screen is a flat list with **two variants**, selected by the in-game flag
// `[0x83cb7c]`: outside the game it asks `QUIT GAME?` with `Yes`/`No`, inside
// the game it is `QUIT OPTIONS` with `Quit Game`/`Quit To Title`/
// `Return To Game`.  There is no state object: the caller owns the flag and the
// cursor (`0x68ccf4`), this module paints the 320x200 mode-13h canvas
// (`0x6d8fc0`, docs/UI.md §6.3) from the options alone.  The building blocks are
// the same as `ui/game_type_render` (19) and `ui/difficulty_render` (23): the
// `FONTS4` atlases of `FRONTEND/BMP.IDX` (title face 5, rows 0, highlight 4),
// the optional 196x131 `PLATE` of `0x486050` and the exact fade tables of
// `0x682f90`.  `render/text_layout` owns the atlases and the advances; nothing
// is decoded or measured twice.
//
// Proven geometry (`0x488780`, docs/UI_SCREENS.md §2/§3.8/§12.4)
// --------------------------------------------------------------
// The handler is 0xD0 bytes from `0x488780` to the second `ret` at `0x48884e`
// (`nop` at `0x48884f`); every number below is one of its sites:
//
//   * `0x488780` calls `0x486050`, which copies the 196x131 `PLATE` into the
//     compose buffer `0x6865f8`; every text call writes into that copy, so
//     196x131 is a hard clip (§3);
//   * `0x488785` loads `[0x83cb7c]`, `0x48878a` stores the step
//     `[0x662d58] = 0x10` and `0x488796` (`jne 0x4887e8`) chooses the variant;
//   * outside the game (`[0x83cb7c] == 0`): `[0x67f10c] = 2` at `0x4887a7`,
//     the title is `0x485dd0(font=5, text=0x4bd3e0 "QUIT GAME?", centered=1,
//     x=38, row=14, item=-1)` at `0x488798`..`0x4887b1`, then item 0
//     `Yes` (`0x4bd3dc`) at `0x4887b9`..`0x4887c8` and item 1 `No`
//     (`0x4bc7a4`) at `0x4887d0`..`0x4887df`, both font 0, `x 38`, `row 27`;
//   * inside the game: `[0x67f10c] = 3` at `0x4887f7`, the title is
//     `0x485dd0(font=5, text=0x4bd3cc "QUIT OPTIONS", centered=1, x=38,
//     row=14, item=-1)` at `0x4887e8`..`0x488801`, then item 0 `Quit Game`
//     (`0x4bd3c0`) at `0x488809`..`0x488818`, item 1 `Quit To Title`
//     (`0x4bd3b0`) at `0x488820`..`0x48882f` and item 2 `Return To Game`
//     (`0x4bd3a0`) at `0x488837`..`0x488846`, all font 0, `x 38`, `row 27`;
//   * the row model of `0x485dd0` (§3, model at `0x485dd0`): `centered` makes
//     `x = (196 - measure(text)) / 2` (`0x485de2`..`0x485dfa`, integer
//     division) and `item == -1` zeroes `item*step`, so the title is centred
//     at y = 14 (`0x485e04`..`0x485e09`); `item >= 0` gives
//     `y = row + item*step = 27 + 16*item`, i.e. 27 / 43 / 59;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the text is not empty the face becomes 4.
//     There is no bar, box or `MENULIT` marker on this screen;
//   * there is no `MENUARRW` call, no slider and no checkbox in the handler:
//     `0x486050` plus the 2 or 3 `0x485dd0` calls are the whole frame;
//   * `rows` written by the handler is exactly the item count of the variant
//     (2 or 3), so the caller's navigation over `[0x68ccf4]` sees 0..1 /
//     0..2.  The module does not clamp the cursor: an id outside the variant's
//     item range highlights nothing, which is what `0x485dd0` does for a
//     cursor that matches no call (same contract as `ui/game_type_render`).
//
// What the items do (model, docs/UI_SCREENS.md §12.4)
// ---------------------------------------------------
// The activation dispatcher compares the state with `0x21` (`0x48d92a`) and
// then branches on `[0x83cb7c]` and `[0x68ccf4]`:
//
//   * outside, `Yes` (0) → `0x48d978`: `0x4913a0(0x4bdd34)`, `0x44b830`
//     (in-game teardown, which also clears `[0x83cb7c]`), `0x404740` — the
//     process-exit path (`push 0; call [0x8413ec]`).  `No` (1) →
//     `mov [0x4bc440], ecx` with `ecx = 1` (set at `0x48cf15` and untouched by
//     the dispatch chain), i.e. back to the title;
//   * inside, `Quit Game` (0) → the same exit path; `Quit To Title` (1) →
//     `0x48da31` writes state 1; `Return To Game` (2) → `0x48d969`:
//     `0x498d40`, then `0x485c50` (state 0, reload/resume).
//
// None of that is in this module: it paints one frame and writes no globals.
// The second writer of state 33 in the shipped code is `0x4951af` (the
// in-game path; docs/UI_SCREENS.md §4 has no row for 33).
//
// `ESC` on screen 33
// ------------------
// There is **no** `Esc` branch for this screen: the keyboard dispatcher
// `0x48b4d0` compares `0x1b` only for states 9 (`0x48b536`), 5 (`0x48b571`),
// 2 (`0x48b5d9`), 0x0f (`0x48b6ff`), 0x23 (`0x48b880`) and 0x22 (`0x48b973`),
// and no comparison sends state 33 anywhere.  The way back is the `No` /
// `Return To Game` item (and the exit items for quitting).  A frame renderer
// therefore has no `ESC TO CANCEL` line to draw.
//
// Canvas, background and colours
// ------------------------------
// `QuitRenderOptions::background` names a `FRONTEND/BMP.IDX` entry painted at
// (0,0) before the text; an empty string means the opaque black canvas.  The
// proven panel of screen 33 is `PLATE` (`menu_panel_asset(Screen::Quit)`), but
// it is never painted implicitly, like screens 13/14/17/19/23/30.  Glyphs are
// clipped to the 196x131 band `0x485dd0` writes into.  Colours come from the
// atlas palette: title font 5 -> (155,15,15) dark red, rows font 0 ->
// (19,15,15) dark grey, highlight font 4 -> (239,239,239) white (the `FONTS4`
// faces, docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90` (same layer as `ui/start_game_render`)
// ---------------------------------------------------------------
// `fade_lut` is the exact index -> index path: the table is applied to the
// palette index of each painted pixel **before** the palette lookup
// (`render/hud_lut.h::map`), so it sees the index the engine blitted, not the
// colour it became.  The table either comes pre-loaded (`fade_lut_table`) or
// from a 4096-byte dump (`fade_lut_dump`, docs/HUD_LUT.md §2).  Without both the
// call is a text refusal prefixed `quit_render:`, never a silently unfaded
// frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"quit_render: "`: an empty game directory,
// a missing/broken `FRONTEND/BMP.IDX` or one of the faces 5/0/4, a background
// entry that is missing/broken/not 8 bpp, and an exact fade without a table and
// a dump (or with an unreadable/wrong-sized dump).  On failure `rgba` is
// cleared, `width`/`height` are 0 (never a stale or half-drawn picture) and
// `*stats` is zeroed.  `stats`/`error` may be null.  The call only reads the
// installation, is deterministic and does not depend on the current working
// directory.
//
// What is not drawn
// -----------------
// Keyboard/mouse input and navigation, the `updown`/`select` sounds
// (`docs/UI_SCREEN_SOUNDS` / `docs/UI_SCREENS.md` §8), the state transition and
// the quit itself (process exit, `0x404740`).  This module paints one frame;
// `frontend_menu` and the CLI own the rest.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kQuitCanvasWidth = 320;
inline constexpr int kQuitCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kQuitBackground = 0x000000FFu;

/// `QUIT GAME?` (`0x4bd3e0`): the title pushed at `0x4887a0`, outside the game.
inline constexpr std::string_view kQuitTitleAsk = "QUIT GAME?";
/// `QUIT OPTIONS` (`0x4bd3cc`): the title pushed at `0x4887f0`, in game.
inline constexpr std::string_view kQuitTitleOptions = "QUIT OPTIONS";

/// The outside-game items (`[0x83cb7c] == 0`), item 0..1.
inline constexpr std::string_view kQuitAskItems[2] = {
    "Yes", // item 0, `0x4bd3dc`, pushed at `0x4887c1`
    "No",  // item 1, `0x4bc7a4`, pushed at `0x4887d8`
};
/// The in-game items, item 0..2.
inline constexpr std::string_view kQuitInGameItems[3] = {
    "Quit Game",      // item 0, `0x4bd3c0`, pushed at `0x488811`
    "Quit To Title",  // item 1, `0x4bd3b0`, pushed at `0x488828`
    "Return To Game", // item 2, `0x4bd3a0`, pushed at `0x48883f`
};

/// `0x67f10c = 2` outside the game (`0x4887a7`).
inline constexpr int kQuitAskRows = 2;
/// `0x67f10c = 3` in game (`0x4887f7`).
inline constexpr int kQuitInGameRows = 3;

/// The panel of screen 33 (`0x486050`; `menu_panel_asset(Screen::Quit)`).
inline constexpr std::string_view kQuitPanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kQuitPanelWidth = 196;
inline constexpr int kQuitPanelHeight = 131;

/// `0x662d58 = 0x10` (`0x48878a`): the row step of both variants.
inline constexpr int kQuitItemStep = 16;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kQuitTitleFont = 5;
inline constexpr int kQuitRowFont = 0;
inline constexpr int kQuitSelectedFont = 4;

/// Title row (`push 0xe` at every title call): centred at y = 14.
inline constexpr int kQuitTitleY = 14;
/// `x = 38` (`push 0x26`) is passed to every call; the title's `centered=1`
/// replaces it with `(196 - measure(text)) / 2` (`0x485de2`..`0x485dfa`).
inline constexpr int kQuitTitleX = 38;
/// `x = 38` of every item (`push 0x26`), base row 27 (`push 0x1b`), step 16.
inline constexpr int kQuitItemX = 38;
inline constexpr int kQuitFirstRow = 27;

/// The item count of the variant (`kQuitAskRows` / `kQuitInGameRows`).
constexpr int quit_item_count(bool in_game) {
    return in_game ? kQuitInGameRows : kQuitAskRows;
}

/// The `0x67f10c` value the handler writes for this variant (same as the item
/// count on screen 33, but kept as a named function like the sibling modules).
constexpr int quit_rows(bool in_game) { return quit_item_count(in_game); }

/// `y = 27 + 16*item` for item >= 0 (`0x485e4b`..`0x485e5c`).
constexpr int quit_item_y(int item) { return kQuitFirstRow + kQuitItemStep * item; }

/// The item labels of the variant, in `item` order.
constexpr std::string_view quit_item_label(bool in_game, int item) {
    return in_game ? (item >= 0 && item < kQuitInGameRows ? kQuitInGameItems[item]
                                                          : std::string_view{})
                   : (item >= 0 && item < kQuitAskRows ? kQuitAskItems[item]
                                                       : std::string_view{});
}

/// The title of the variant.
constexpr std::string_view quit_title(bool in_game) {
    return in_game ? kQuitTitleOptions : kQuitTitleAsk;
}

/// Optional parts of one frame.
struct QuitRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 33, but it is
    /// never painted implicitly.
    std::string background;
    /// `[0x83cb7c] != 0` (`0x488785`/`0x488796`): in game the screen is
    /// `QUIT OPTIONS` with three items, outside it asks `QUIT GAME?` and has
    /// two.  The caller reads the flag; this module only paints.
    bool in_game = false;
    /// `[0x68ccf4]`: the highlighted **item id** (0..1 outside, 0..2 in game),
    /// not a visible row.  Values outside the variant's range highlight
    /// nothing — navigation belongs to the caller.
    int cursor = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/start_game_render`, docs/START_GAME_RENDER.md §6).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `quit_render: fade LUT needs ...`.
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
    const gm::HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the item count the handler writes into `0x67f10c` (2 outside the
/// game, 3 in game), `ink_pixels` the canvas pixels covered by at least one
/// glyph pixel (unique, independent of the background), `missing_glyphs` the
/// characters with no glyph in the atlas (skipped, never fatal) and `arrows`
/// the number of `MENUARRW` sprites painted — always 0, screen 33 has no
/// scroll window; the field keeps the stats shape of the sibling renderers.
struct QuitRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// Paint screen 33 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// With `options.in_game == false` the title is `QUIT GAME?` (font 5, centred
/// at y 14) and `Yes`/`No` are painted at (`kQuitItemX`, 27 + 16*i); with
/// `options.in_game == true` the title is `QUIT OPTIONS` and `Quit Game` /
/// `Quit To Title` / `Return To Game` are painted the same way.  The item whose
/// id equals `options.cursor` uses font 4 instead of font 0; any other cursor
/// highlights nothing.  Every glyph is clipped to the 196x131 text band.  With
/// `options.fade_lut` every painted palette index (background, glyphs) goes
/// through `render/hud_lut.h::map` at the level before its colour is looked up;
/// the untouched black canvas has no index and stays black.  Returns false +
/// `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_quit(const std::filesystem::path& game,
                 const QuitRenderOptions& options,
                 std::vector<std::uint32_t>& rgba, int& width, int& height,
                 QuitRenderStats* stats = nullptr,
                 std::string* error = nullptr);

} // namespace gm
