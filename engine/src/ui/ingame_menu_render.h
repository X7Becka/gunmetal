#pragma once

// One frame of screen 9, the in-game screen (`M8.8` slice 12).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/mouse_setup_render`
// 22, `ui/game_type_render` 19, `ui/restart_render` 6, `ui/match_setup_render`
// 27, ...): the caller owns the only piece of state this screen has — which of
// the two full-screen pictures is in the canvas — and this module paints it
// into the 320x200 mode-13h canvas (`0x6d8fc0`/`0x6c3160`, docs/UI.md §6.3).
// Nothing here reads the keyboard, `GUNMETAL.CFG`, sounds or saves.
//
// This screen **has no menu**
// ---------------------------
// docs/UI_SCREENS.md §2 lists id 9 with `rows`/`step`/`title` all "—".  That is
// not an omission:
//
//  * the dispatcher `0x487e4f` maps `id - 5` through `0x4883d8` and sends ids
//    outside 5..37 (and the ids 7/8/9/28/29/31 in the table) to the shared tail
//    `0x487f64`; that tail is the "blit the composed `PLATE` + carousel" finish
//    step (`0x487f81`: `0x6865f8`, `0x484cc0`, the entry art `0x660790`/
//    `0x662268`/`0x662f60`), i.e. the menu machinery itself;
//  * but the state-9 branch never gets there: it returns at `0x487739`, before
//    the dispatcher call at `0x487e4f`, so no `PLATE`, no carousel and no item
//    is drawn for id 9;
//  * all 182 `call 0x485dd0` (the item blitter with the cursor highlight) sites
//    of the exe lie between `0x485f0d` and `0x48ed01`, i.e. inside front-end
//    handlers; the level code (`0x44xxxx..0x47xxxx`) never calls it.
//
// So there are no labels, no `y`/`x`, no rows/step, no title, no `PLATE`, no
// cursor highlight and no `MENUARRW` on this screen; `stats.rows`,
// `stats.ink_pixels`, `stats.missing_glyphs` and `stats.arrows` are all 0 by
// construction, and `options.cursor` changes nothing (the test pins that with
// pixel diffs).  Player-visible in-game menus are **other** front-end screens:
// `Esc` in the level goes to state 6 (`RESTART GAME`, `ui/restart_render`) at
// `0x48b531`..`0x48b54c`, and QUIT OPTIONS is state 33 (`ui/quit_render`).
//
// Proven frame (`0x4876af`)
// -------------------------
// The frame of state 9 is built in the front-end frame function, branch
// `cmp DWORD PTR ds:0x4bc440, 0x9` (`0x4876af`):
//
//  * the canvas pointer `[0x68cca0]` (default `0x6c3160`, `0x4876bc`..`0x4876ca`,
//    zeroed by `Esc` at `0x48b556` so the default comes back);
//  * one load per entry, guarded by `[0x68cca4]` (`0x4876cf`, reset at
//    `0x48d9aa`): `0x47efe0(folder=0x4abbec "frontend", name=0x4bc1b8,
//    dst=canvas, palette=0, 320, 200)` — `0x4876d9`..`0x4876f0`;
//  * a load failure prints `0x4bd2dc "error loading DEMOTION.BMP\n"` and falls
//    back to state 1 (`0x4876fc`..`0x487709`), so every refusal below is a real
//    dead end in the original too;
//  * then the canvas is presented (`0x4848b0`, `0x48772a`..`0x487731`).  Nothing
//    else is drawn into the canvas: the branch has no text, sprite or panel
//    call.
//
// `0x47efe0` is the full-screen BMP loader (palette at `+0x4a5b08` is written
// only when its 4th argument is non-NULL; here it is 0, `0x4876e3`): it opens
// `frontend\NAME.BMP` through `0x40d1a0` (extension `0x4bbc90 "BMP"`), checks
// the 0x36-byte header and the 0x400-byte palette, compares the loaded size
// with its two size arguments (`0x47f0e5`..`0x47f0f6`, mismatch -> return 5)
// and then copies `width * height` bytes with **one raw read**
// (`0x47f0f8`..`0x47f100`).  Consequences, all reproduced here:
//
//  * position (0,0), size 320x200 — the size arguments are `push 0xc8` /
//    `push 0x140` (`0x4876d9`/`0x4876de`) and a size mismatch is a refusal;
//  * **no transparency key**: index 0 is copied like any other index.  The
//    shipped `DEMOTION`/`DISBAND` do not even use index 0 (0 pixels each), so
//    the frame is fully opaque either way;
//  * **no palette swap** (4th argument 0), so the picture is coloured by the
//    palette the loader carries in the asset; the shipped palettes are the
//    global `GAMEPAL` entry for indices 1..255 and differ only in the unused
//    index 0 (255/256), which the test cross-checks against `FRONTEND/GAMEPAL`;
//  * **engine row order**: the raw copy keeps the file's row order, while the
//    canvas is addressed top-down — `0x487500` builds `canvas + y*320 + x`
//    (`0x48751a`..`0x487524`, rows step `0x140`) — so the visible picture is
//    `flipV(parse_bmp)`, exactly like the pager background
//    (`ui/pager_render.cpp`, docs/PAGER_RENDER.md §2.4: `flipV(LOADPAGE)` reads
//    `LOADING`).  For these two assets the flip is self-evident: `YOU'RE FIRED.`
//    and `GAME OVER` read upright only in engine order.
//
// Which picture, and when (`0x497210`)
// ------------------------------------
// `[0x4bc1b8]` holds the name (10 bytes: 8 chars + NUL).  `.data` starts as
// `"DEMOTION"` (`0x4bc1b8`); the only writer is the "start level" case of the
// VM action dispatcher `0x4971e0` (case 0 at `0x4971f4` sets `[0x4bc440] = 9`):
// when the action's 9th argument is 0 it copies `0x4be8a8 "DEMOTION"`
// (`0x497210`..`0x497237`), otherwise `0x4be8a0 "DISBAND"`
// (`0x497238`..`0x497253`).  The *meaning* of that argument is not proven
// (hypothesis: a single-player vs multiplayer/team flag), so the API takes the
// name directly and only offers the mapping as a helper.
//
// Both entries are ordinary records of `FRONTEND/BMP.IDX`
// (docs/UI_SCREENS.md §9): 320x200, 8 bpp, 65078 bytes = 1078 header + 64000
// pixels, 256-entry palette.  The other assets listed for screen 9 in
// docs/UI_SCREENS.md §2 (`HUMPAN`, `PANEL`, `PANELMID`) are the in-game HUD
// panel, selected by VM action case 2 (`0x497264`, `[0x6e89c0]`) and composed
// by the HUD layer `0x491a70` (docs/COCKPIT.md, docs/HUD_AUDIT.md §2.5), not by
// this screen; they are **not** painted here.
//
// Exact fade of `0x682f90`
// ------------------------
// Same index -> index layer as `ui/mouse_setup_render`/`ui/match_setup_render`:
// every painted pixel's palette index goes through
// `render/hud_lut.h::map(level, index)` **before** its colour is looked up.  The
// table comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  With neither, the call is a text
// refusal prefixed `ingame_menu_render:`, never a silently unfaded frame.  On
// the real palette level 11 is an identity for the indices these two pictures
// use (they never use 0 or 65, the two non-identity entries) and 12..15 differ
// (docs/INGAME_MENU_RENDER.md §3).
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"ingame_menu_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a background that is not in
// the archive, one that is not 8 bpp / has no palette, one that is not 320x200,
// and an exact fade without a table and a dump (or with an unreadable dump).
// There is no font refusal because this screen has no text.  Out-of-range
// *values* (`cursor` of 99) are ignored, not refused: the screen stays
// paintable and the frame is the same.  On failure `rgba` is cleared,
// `width`/`height` are 0 (never a stale or half-drawn picture) and `*stats` is
// zeroed.  `stats`/`error` may be null.  The call only reads the installation,
// is deterministic and does not depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (`Esc` -> state 6 at `0x48b531`, the mission-failure/restart triggers),
// UI sounds, the transient `GAME SAVED` banner the frame function paints into
// the same canvas while its timer runs (`0x4875ad`..`0x487622`, string
// `0x4bd304`, counter `0x4bc3dc`), the "PAUSED" banner the game frame draws on
// the screen buffer (`0x449d82`, docs/HUD_AUDIT.md §2.5) and the HUD panels
// listed above.  There is no CLI switch for this screen (main.cpp is outside
// this slice).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3); the state-9
/// branch writes the picture into `0x6c3160` (`0x4876bc`..`0x4876ca`), which is
/// the same 320x200 frame.
inline constexpr int kIngameMenuCanvasWidth = 320;
inline constexpr int kIngameMenuCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background.  The original
/// always loads a background in state 9; an empty `background` option is the
/// bare canvas of the sibling renderers (used to measure the picture).
inline constexpr std::uint32_t kIngameMenuBackgroundColour = 0x000000FFu;

/// The two `FRONTEND/BMP.IDX` names `0x497210`/`0x497238` copy into
/// `[0x4bc1b8]`: `0x4be8a8` and `0x4be8a0`, 8 characters each.
inline constexpr std::string_view kIngameMenuDemotion = "DEMOTION";
inline constexpr std::string_view kIngameMenuDisband = "DISBAND";

/// `.data` initial value of `[0x4bc1b8]`: `"DEMOTION"` (`0x4bc1b8`), so a
/// default-constructed options object is the shipped default, not black.
inline constexpr std::string_view kIngameMenuDefaultBackground = kIngameMenuDemotion;

/// `DEMOTION`/`DISBAND` are 320x200, 8 bpp, 65078 bytes = 1078 + 64000
/// (docs/UI_SCREENS.md §9; verified against the BMP headers), and `0x4876d9` /
/// `0x4876de` pass exactly these two numbers to `0x47efe0`.
inline constexpr int kIngameMenuBackgroundWidth = 320;
inline constexpr int kIngameMenuBackgroundHeight = 200;
inline constexpr std::size_t kIngameMenuBackgroundBytes = 65078u;

/// The menu item count of this screen: 0.  `0x4883d8 + 4*(9-5)` is the stub
/// `0x487f64` and the state-9 branch has no `0x485dd0` call, so there is no
/// item list, no `rows`/`step` value and no highlight (docs/UI_SCREENS.md §2
/// prints "—" in all three columns).
inline constexpr int kIngameMenuRows = 0;

/// No item can be highlighted: the module has nothing to compare the cursor
/// `[0x68ccf4]` against (`0x485dd0`'s font-4 swap never runs on this screen).
inline constexpr int kIngameMenuNoHighlight = -1;

/// Optional parts of one frame.
struct IngameMenuRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0): `"DEMOTION"` (default, the
    /// `.data` value of `[0x4bc1b8]`) or `"DISBAND"` (`0x497238`).  An empty
    /// string paints nothing, i.e. the bare opaque black canvas; any other name
    /// is looked up case-insensitively and a size other than 320x200 is refused.
    std::string background = std::string(kIngameMenuDefaultBackground);
    /// The cursor `[0x68ccf4]` of the sibling screens.  Screen 9 has no items,
    /// so this value is accepted and **ignored** (the test proves the frames
    /// are identical for -1/0/3); it is kept so callers can drive every screen
    /// with one state object.
    int cursor = 0;
    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/mouse_setup_render`, docs/MATCH_SETUP_RENDER.md §2).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `ingame_menu_render: fade LUT needs ...`.
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
/// `rows` is the fixed `0x67f10c` value of this screen: 0 (no handler, no item
/// list); `ink_pixels` the canvas pixels covered by a glyph or sprite: 0, there
/// is no text and no sprite; `missing_glyphs`: 0, no text is laid out; `arrows`:
/// 0, there is no `MENUARRW` call.  The fields keep the shape of the sibling
/// stats so one caller can print every screen the same way; the shape of this
/// screen is pinned by the test, not left as "not measured".
struct IngameMenuRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// The background name `0x497210` selects from the "start level" action's 9th
/// argument: 0 -> `"DEMOTION"` (`0x4be8a8`), anything else -> `"DISBAND"`
/// (`0x4be8a0`).  The sign/range of the argument is not clamped by the original
/// (`test eax,eax` at `0x49720c`), so any non-zero value selects `DISBAND`.
std::string ingame_menu_background_for_start_arg(int argument);

/// The item `[0x68ccf4]` would highlight: always `kIngameMenuNoHighlight`,
/// because screen 9 has no item list (`0x485dd0` is never called in the
/// state-9 path).  `cursor` is ignored on purpose; the helper exists so a
/// caller (and the test) can ask instead of assuming.
int ingame_menu_highlighted_item(const IngameMenuRenderOptions& options);

/// Paint screen 9 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// With `options.background` non-empty the 320x200 picture of that
/// `FRONTEND/BMP.IDX` entry is painted over the whole canvas in engine row
/// order (`flipV` of the decoded BMP, see the header comment); an empty name
/// leaves the opaque black canvas.  No text, sprite, panel or highlight is
/// painted — this screen has none — so `*stats` is all zeros by construction.
/// With `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_ingame_menu(const std::filesystem::path& game,
                        const IngameMenuRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        IngameMenuRenderStats* stats = nullptr,
                        std::string* error = nullptr);

} // namespace gm
