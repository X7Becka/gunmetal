#pragma once

// One frame of screen 30 (`KEY SET VIEW`, `M8.7` slice 7) or of the editor
// screens 34/35 (`0x4892d0`, slice 10): the bindings of one `KEYCFG/*.KEY`
// profile as a 320x200 RGBA canvas.
//
// Division of labour
// ------------------
// `input/controls` owns the `.key` format (`name[16]` + 45 `{u32 a; u32 b}`
// pairs, `kKeyFileSize` = 0x178) and the game's 123-entry key-name table
// (`0x4a5b88`, `Controls::key_name`).  This module only *paints*: it reads a
// profile, the `FONTS4` atlases of `FRONTEND/BMP.IDX` and, on request, the
// 196x131 `PLATE` plus the `MENUARRW` scroll arrows, exactly like the sibling
// renderers (`ui/menu_render`, `ui/start_game_render`, `ui/save_menu_render`).
// Nothing is decoded or measured twice (`render/text_layout` owns the atlases).
//
// Proven geometry (`0x489850`, ending at the `ret` of `0x489a0d`)
// -----------------------------------------------------------------
// Every number below is a site of the handler (`docs/UI_SCREENS.md` §2/§3.7,
// `docs/KEYCFG_RENDER.md` §1):
//
//   * `0x489855` stores `rows = 0x1f = 31` into `0x67f10c` and `0x48985f` sets
//     the step `0x662d58 = 8`; `0x489869` copies the `PLATE` (196x131) into the
//     compose buffer `0x6865f8` — every text call writes into that copy, so
//     196x131 is a hard clip;
//   * the title is `0x485dd0(font=5, text=name, centered=1, x=38, row=11,
//     item=-1)` at `0x489872`..`0x48988b`.  The text pointer is
//     `0x6c2058 + set*0x108` (`shl eax,5; add eax,ebx; lea ecx,[eax*8+...]`),
//     i.e. `name[16]` of the profile record; `centered=1` makes
//     `x = (196 - width)/2` (`0x485de2`..`0x485dfa`) and `item=-1` zeroes the
//     `item*step` term, so the title sits at y = 11;
//   * the hint is `0x485dd0(font=0, text=0x4bd7c8 "Press Esc to close",
//     centered=1, x=38, row=20, item=-1)` at `0x489893`..`0x4898a3`, i.e.
//     y = 20.  (There is no `x` use: the call is centred in the 196 px band.)
//   * the list window is `top = max(0, cursor - 5)` (`0x4898a8`..`0x4898c2`,
//     cursor `0x68ccf4`): `cmp eax,5; jl` keeps `top = 0` below the sixth item,
//     otherwise `top = cursor - 5`.  It is then clamped to `top <= 21`
//     (`top + 10 >= 31`, `0x4898c4`..`0x4898d4`) and explicitly to `top >= 0`
//     (`cmp esi,ebp`/`jge` with `ebp = 0` from `0x489893`,
//     `0x4898d8`..`0x4898e4`).  The up arrow is `MENUARRW` blitted backwards by
//     `0x485d00` to `0x68681f` when `top > 0` (`0x4898e6`..`0x4898f0`); the down
//     arrow is the same sprite blitted forward by `0x485cc0` to `0x68c33b` when
//     `top + 10 < 31` (`0x4898f8`..`0x48990a`).  Both anchors are the ones of
//     screens 13/14/17: `0x68681f = 2*196 + 159` and `0x68c33b = 121*196 + 159`,
//     i.e. column 159, rows 2 and 121 (16x8 sprite);
//   * the row loop `0x489928`..`0x489a02` paints at most ten rows: `edi` starts
//     at `0xffffffe3 = -29` and is decremented by 8 while `edi > -109`, so the
//     rows are `y = 29 + 8*i`, `i = 0..9` (`row = -29 - 8*i`).  Per row:
//     - the action label comes from the 31-pointer table `0x4bc558`
//       (`lea ecx,[esi*4+0x4bc558]`, `0x489912`..`0x489936`), font 0, `x = 5`
//       (`push 0x5`), `item = -1`;
//     - the primary key code is the 31-entry `u32` array at
//       `0x6c1f60 + set*0x108 + action*4` (`0x489962`); its display name is
//       `0x4a5b88 + code*6` (`lea eax,[eax+eax*2]; lea ecx,[eax*2+0x4a5b88]`),
//       font 0 at `x = 0x6c = 108`, and font 4 (the white face) when
//       `top + i == cursor` (`0x48993d`..`0x489975`): the handler compares
//       `lea ecx,[esi+ebp]` (top + loop row) with `[0x68ccf4]` itself, so the
//       highlight is a *screen* decision, not the `item == cursor` branch of
//       `0x485dd0` (which this screen never triggers: every call passes
//       `item = -1`);
//     - the second code is the parallel `u32` array at
//       `0x6c1fdc + set*0x108 + action*4` (`0x48997a`/`0x4899c8`); its name is
//       drawn at `x = 0x9a = 154`, font 0, in both branches (`0x489983`
//       ..`0x4899e8`).  That array is the `b` half of each file pair: the loader
//       `0x4853c1`..`0x4853d7` unpacks `pair.a` to `+0x00` and `pair.b` to
//       `+0x7c`.  In every shipped profile `b = 0` (93/93 used pairs,
//       `docs/KEY_WRITE.md` §4), and `0x4a5b88 + 0` is `"NONE"`, so the column
//       prints `NONE` until a profile actually carries a second code.
//
// Rows vs the painted window (proven, not a simplification)
// ---------------------------------------------------------
// `0x67f10c = 31` is the *length of the list* (all 31 actions), not the number
// of bands on the canvas: the loop bound above paints ten bands per frame and
// the two scroll arrows move that window.  `KeycfgRenderStats::rows` reports
// the proven 31; `kKeycfgVisibleRows` is the window size.
//
// Cursor and window (`M8.7` slice 9)
// ----------------------------------
// `KeycfgRenderOptions::cursor` is `[0x68ccf4]`, the list cursor of the input
// side: `0x48bb22`/`0x48bcb7` step it by one and clamp it to
// `[0x67f10c]-1 = 30` (`0x48bc9e`..`0x48bcb8`, the same `rows-1` bound as
// `0x48bb34`).  Values outside `0..30` are clamped, not a refusal: the original
// render never checks the cursor, it only derives `top` from it, so an
// out-of-range cursor is a caller bug with a well-defined frame.
//
// The window rule of `0x4898a8`..`0x4898d4` is reproduced literally: the first
// *visible* row is `top = max(0, cursor - 5)`, clamped to `0..21`, the ten rows
// are `top + i` (`i = 0..9`) and the primary key of the row with
// `top + i == cursor` uses font 4.  Consequences, all of them sites above:
// the cursor sits on the middle band (`top + 5 == cursor`, band index 5) while
// scrolling, on band `cursor` for the first six items and on band
// `cursor - 21` for the last four; `cursor 0` and `cursor 30` show a single
// arrow (down / up), every middle window both.  With 31 rows and 10 bands the
// arrow count is never 0.
//
// Canvas, background and colours
// ------------------------------
// `KeycfgRenderOptions::background` names a `FRONTEND/BMP.IDX` entry painted at
// (0,0) before the text; an empty string means the opaque black canvas.  The
// proven panel of screen 30 is `PLATE` (`menu_panel_asset(Screen::KeySetView)`),
// but it is never painted implicitly.  Glyphs are clipped to the same 196x131
// band.  Colours come from the atlas palette: title font 5 -> (155,15,15) dark
// red, action/second-key rows font 0 -> (19,15,15) dark grey, the selected
// primary key font 4 -> (239,239,239) white (`docs/MENU_RENDER.md` §3).
//
// Editor screens 34/35 (`0x4892d0`, slice 10)
// -------------------------------------------
// The editor is a second front-end handler (`0x4892d0`, the sibling of screen
// 30's `0x489850`): the same list length 31/step 8, the same `PLATE` copy, the
// same title (`0x4892f4`..`0x48930b`: name, font 5, centred, row 11), the same
// window/arrow rule (`0x48932a`..`0x489391`), the same ten bands at
// `y = 29 + 8*i` with the label at `x 5` (`0x4893ae`) and the two code columns
// at `x 108`/`x 154`.  Four things differ:
//
//   * the hint is `0x4bd7a8 "Enter changes, Delete clears"` instead of
//     `0x4bd7c8` (`0x48931f`), same font 0, centred, row 20 (`0x489319`);
//   * the cursor row highlights **one cell**, not always the primary one:
//     `[0x68cd80]` (the column flag the input side `0x48e661` uses to pick the
//     array it writes) zero -> primary column (`x 108`), non-zero -> the second
//     code (`x 154`), and the other cell of that row stays font 0
//     (`0x4893d2`..`0x4894c9`);
//   * the face of the highlighted cell comes from `[0x4bc440]`: screen 35
//     (`0x23`, the steady editor) -> font 5, anything else — the one-frame alias
//     screen 34 (`0x48e212`, set by `0x48f409`) -> font 4.  The face is pushed
//     at `0x489431`/`0x489443` for the second cell and `0x48947b`/`0x4894a4`
//     for the primary one, both under the test at `0x489417`/`0x48944a`;
//   * after the ten bands the handler calls the duplicate checker `0x4895e0`
//     with the set index: 1 -> `sprintf("%s has been used twice", 0x4bd790)`,
//     2 -> `sprintf("%s is used by other player", 0x4bd774)`, then paints that
//     line with font 5, centred, at row `-29 - 8*10 = -109` (`0x489563`), i.e.
//     `y = 109` (a negative row means `y = -row` in `0x485dd0`,
//     `0x485e2b`..`0x485e38`), the eleventh band of 196x131.  Return 0 paints
//     nothing (`0x48954a`/`0x489592`).
//
// What is *not* drawn (docs/KEYCFG_RENDER.md §5)
// ----------------------------------------------
// The input side of the cursor (`updown`/`select` sounds `0x48e607`/`0x48e634`,
// the wrap keys), the edit/delete input of screens 34/35 (including the
// `0x4895e0` scan itself — the caller passes the ready text), the mouse cursor
// and the CLI (`--fade-lut` for screens 13/30 is the next slice).
//
// Exact fade of `0x682f90` (slice 8, docs/KEYCFG_RENDER.md §6)
// -----------------------------------------------------------
// `KeycfgRenderOptions::fade_lut` is the **exact** path of the sibling
// renderers (docs/PAGER_RENDER.md §2.6, docs/START_GAME_RENDER.md §6): the
// table of `0x682f90` is index -> index, so it is applied to the palette index
// of each painted pixel **before** the palette lookup
// (`render/hud_lut.h::map`).  The engine blits the composed 8-bpp buffer
// through `LUT[k]`; this module maps at the pixel write (`PaletteFade` in the
// .cpp), so background, arrows and glyphs fade together and a pixel without an
// index (the black canvas) stays black.  Levels 0..15 are taken as they are, so
// 12..15 brighten instead of aliasing 11.  The table is either pre-loaded
// (`fade_lut_table`, e.g. from `build_hud_lut`) or read from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2); without both the call is a text
// refusal prefixed `keycfg_render:`.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"keycfg_render: "`: an empty game directory,
// a profile that cannot be read or is not exactly 376 bytes, a `FRONTEND/
// BMP.IDX` or `FONTS4` atlas the module cannot load, a background entry that is
// missing/broken/not 8 bpp, and an exact fade without a table and a dump (or
// with an unreadable/wrong-sized dump).  On failure `rgba` is cleared,
// `width`/`height` are 0 (never a stale or half-drawn picture) and `*stats` is
// zeroed.  `stats`/`error` may be null.  The call only reads the installation
// and the profile, is deterministic and does not depend on the current working
// directory.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kKeycfgCanvasWidth = 320;
inline constexpr int kKeycfgCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kKeycfgBackground = 0x000000FFu;

/// `0x67f10c = 0x1f` (`0x489855`): the 31 actions of the profile.
inline constexpr int kKeycfgRows = 31;
/// `0x662d58 = 8` (`0x48985f`): the row step.
inline constexpr int kKeycfgRowStep = 8;
/// Bands per frame: the loop `0x489928`..`0x489a02` (`row -29 .. -101`).
inline constexpr int kKeycfgVisibleRows = 10;

/// `cmp eax,0x5` at `0x4898b2`: the cursor keeps the middle band while the list
/// scrolls (`top = max(0, cursor - kKeycfgScrollBand)`).
inline constexpr int kKeycfgScrollBand = 5;
/// Last list slot: `[0x67f10c] - 1 = 30` (`0x48bb3a`, `0x48bc9e`).
inline constexpr int kKeycfgLastRow = kKeycfgRows - 1;
/// Largest first visible row: `mov ... ,0x15` at `0x4898cc` (31 - 10).
inline constexpr int kKeycfgMaxFirstRow = kKeycfgRows - kKeycfgVisibleRows;

/// `edi = -29` at `0x489919`: the first band's row, i.e. y = 29.
inline constexpr int kKeycfgFirstRowY = 29;
/// Action label column (`push 0x5` at `0x48992f`).
inline constexpr int kKeycfgActionX = 5;
/// Primary key column (`push 0x6c` at `0x489956`/`0x4899a4`).
inline constexpr int kKeycfgKeyX = 108;
/// Second code (`pair.b`) column (`push 0x9a` at `0x489989`/`0x4899d7`).
inline constexpr int kKeycfgSecondKeyX = 154;

/// Title: `push 0xb` at `0x489876`, `item = -1` -> y = 11.
inline constexpr int kKeycfgTitleY = 11;
/// `push 0x26` at `0x48987d`; ignored by `centered` (`0x485de2`).
inline constexpr int kKeycfgTitleX = 38;
/// Hint: `push 0x14` at `0x489897`, `item = -1` -> y = 20.
inline constexpr int kKeycfgHintY = 20;
/// `push 0x26` at `0x489899`; ignored by `centered`.
inline constexpr int kKeycfgHintX = 38;

/// `Press Esc to close` (`0x4bd7c8`), pushed at `0x48989d`.
inline constexpr std::string_view kKeycfgHint = "Press Esc to close";
/// The same line of the editor screens 34/35: `Enter changes, Delete clears`
/// (`0x4bd7a8`), pushed at `0x48931f` with font 0, centred, row 20 (`0x489319`).
inline constexpr std::string_view kKeycfgHintEdit = "Enter changes, Delete clears";

/// Conflict line of the editor (`0x489539`..`0x48957f`): the handler computes
/// `row = -(29 + 8*10) = -109` (`0x489563`/`0x489573`) and `0x485dd0` maps a
/// negative row to `y = -row`, so the line sits in the eleventh band, just
/// below the ten list bands of `y 29..101`.
inline constexpr int kKeycfgConflictY =
    kKeycfgFirstRowY + kKeycfgRowStep * kKeycfgVisibleRows;
/// `push 0x5` at `0x48957d`: the conflict line uses font 5 (dark red).
inline constexpr int kKeycfgConflictFont = 5;

/// The panel of screen 30 (`menu_panel_asset(Screen::KeySetView)`): 196x131
/// `PLATE`, the buffer `0x485dd0` writes into (`0x489869`).
inline constexpr std::string_view kKeycfgPanelName = "PLATE";
inline constexpr int kKeycfgPanelWidth = 196;
inline constexpr int kKeycfgPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kKeycfgTitleFont = 5;
inline constexpr int kKeycfgRowFont = 0;
inline constexpr int kKeycfgSelectedFont = 4;

/// Editor highlight faces (`0x489417` picks one from `[0x4bc440]`): screen 35
/// (`0x23`, the editor itself) uses font 5, the one-frame alias screen 34
/// (`0x48e212`) uses font 4 — i.e. the steady editor's current cell is dark red
/// and its very first frame is white.
inline constexpr int kKeycfgEditCursorFont = 5;
inline constexpr int kKeycfgEditAliasCursorFont = kKeycfgSelectedFont;

/// Scroll arrows: `MENUARRW` (16x8, 8 bpp) at column 159, rows 2 and 121 of the
/// plate copy (`0x68681f`/`0x68c33b`, `0x4898e6`..`0x48990a`).
inline constexpr std::string_view kKeycfgArrowName = "MENUARRW";
inline constexpr int kKeycfgArrowWidth = 16;
inline constexpr int kKeycfgArrowHeight = 8;
inline constexpr int kKeycfgArrowX = 159;
inline constexpr int kKeycfgArrowUpY = 2;
inline constexpr int kKeycfgArrowDownY = 121;

/// Which key-set screen a frame is for (`docs/UI_SCREENS.md` §3.7).
///
/// `view` is screen 30 (`0x489850`, hint `Press Esc to close`, the primary key
/// of the cursor row always font 4), `edit` the editor 34/35 (`0x4892d0`, hint
/// `Enter changes, Delete clears`, one highlighted cell + optional conflict
/// line).  The default keeps every byte of the slice 7/9 frame.
enum class KeycfgMode { view, edit };

/// Editor only: the cell of the cursor row that is current — `[0x68cd80]`, the
/// flag `0x48e661` reads to choose the array it writes (`+0x00` vs `+0x7c`).
/// `primary` = the `x 108` column, `second` = the `pair.b` column at `x 154`.
enum class KeycfgCell { primary, second };

/// Optional parts of one frame.
struct KeycfgRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 30, but it is
    /// never painted implicitly.
    std::string background;

    /// List cursor `[0x68ccf4]`, 0..30 (`kKeycfgLastRow`), clamped: it selects
    /// the scroll window (`first_row` in the stats) and the one primary key
    /// painted with font 4.  Default 0 is the untouched first window.
    int cursor = 0;

    /// Screen 30 (`view`) or the editor 34/35 (`edit`); `view` by default.
    KeycfgMode mode = KeycfgMode::view;

    /// Editor only: which cell of the cursor row is current (`[0x68cd80]`).
    /// `primary` (default) highlights the key column `x 108`, `second` the
    /// `pair.b` column `x 154`; the other cell of that row is font 0.
    KeycfgCell edit_cell = KeycfgCell::primary;

    /// Editor only: draw the one-frame alias screen 34 (`0x48e212`) instead of
    /// the steady screen 35.  `0x489417` derives the face of the current cell
    /// from `[0x4bc440]`: `35` -> font 5 (red), `34` -> font 4 (white).  This
    /// only changes that one face, never the geometry.
    bool edit_alias_frame = false;

    /// Editor only: the conflict text of `0x489539`..`0x48957f`, painted as one
    /// line at `kKeycfgConflictY` (109) with font 5, centred in the 196 px band.
    /// The original builds it with `%s has been used twice` (`0x4bd790`, a
    /// duplicate inside the profile) or `%s is used by other player`
    /// (`0x4bd774`, a code of the other player's set) around the key name; the
    /// scan itself is the input side and stays out of this module.  Empty = no
    /// line (the original paints nothing when `0x4895e0` returns 0); ignored in
    /// `view`.
    std::string conflict;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (slice 8, docs/KEYCFG_RENDER.md §6).  Needs a table, either pre-loaded
    /// (`fade_lut_table`) or read from `fade_lut_dump`; without both the call
    /// fails with `keycfg_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten), unlike the RGBA model's clamp to 11.
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
/// `rows` is the length of the action list, the proven `0x67f10c = 31` (the
/// painted window is `kKeycfgVisibleRows` bands of that list), `ink_pixels` the
/// canvas pixels covered by at least one glyph pixel (unique, independent of the
/// background; the arrow sprite is not glyph ink), `missing_glyphs` the
/// characters with no glyph in the atlas (skipped, never fatal) and
/// `named_actions` the actions of this profile whose primary code is non-zero
/// (the `a` half of the pair, 29/19/19 for the three shipped files).
///
/// Slice 9 adds the window: `cursor` is the clamped cursor actually painted
/// (`0..30`), `first_row` the first visible slot (`0..21`) and `arrows` how many
/// `MENUARRW` sprites the frame carries (`0..2`; with 31 rows and 10 bands it is
/// 1 at both ends of the list and 2 in the middle, never 0).
///
/// Slice 10 adds `mode` (the screen the frame was painted for) and
/// `conflict_rows` — how many conflict lines the frame carries (`0` or `1`: the
/// editor paints one line at `y 109` when `options.conflict` is non-empty, and
/// screen 30 never paints one).
struct KeycfgRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int named_actions = 0;
    int cursor = 0;
    int first_row = 0;
    int arrows = 0;
    KeycfgMode mode = KeycfgMode::view;
    int conflict_rows = 0;
};

/// Display label of action `slot` 0..30 from the game's pointer table
/// `0x4bc558` (`Move Forward` … `Linear Right`); empty outside the range.
/// That table is the one the handler pushes at `0x489931`; `input/controls`
/// only keeps the snake_case twin used by the binding model.
std::string_view keycfg_action_label(int slot);

/// Paint screen 30 (`options.mode == KeycfgMode::view`) or the editor 34/35
/// (`edit`) for `key_file` into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is the profile's `name[16]`, font 5, centred at
/// `kKeycfgTitleY`; the hint is `Press Esc to close` (`kKeycfgHint`) in `view`
/// and `Enter changes, Delete clears` (`kKeycfgHintEdit`) in `edit`, font 0,
/// centred at `kKeycfgHintY`; the ten-row window (`top = max(0, cursor - 5)`,
/// clamped to `0..21`) carries the action label of slot `top + i` at
/// (`kKeycfgActionX`, `kKeycfgFirstRowY + kKeycfgRowStep*i`), the primary key
/// name at `kKeycfgKeyX` and the `pair.b` name at `kKeycfgSecondKeyX` (`NONE`
/// in the shipped profiles).  In `view` the primary key of the cursor row uses
/// font 4 (`kKeycfgSelectedFont`); in `edit` exactly one cell of that row is
/// highlighted — `options.edit_cell` picks it and `options.edit_alias_frame`
/// picks font 5 (screen 35) or font 4 (the one-frame alias, screen 34) — and
/// `options.conflict` is painted as one font-5 line at `kKeycfgConflictY` (109)
/// when it is not empty.  Every glyph is clipped to the 196x131 text band
/// (a conflict line wider than 196 px starts at `x 0` and is cut, where the
/// original's negative centred `x` would wrap into the previous band; see
/// docs/KEYCFG_RENDER.md §7).  With `options.fade_lut`
/// every painted palette index (background, arrow, glyphs) goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.  `options.cursor`
/// outside `0..30` is clamped (`stats->cursor` reports the value used).
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_keycfg(const std::filesystem::path& game,
                   const std::filesystem::path& key_file,
                   const KeycfgRenderOptions& options,
                   std::vector<std::uint32_t>& rgba, int& width, int& height,
                   KeycfgRenderStats* stats = nullptr,
                   std::string* error = nullptr);

} // namespace gm
