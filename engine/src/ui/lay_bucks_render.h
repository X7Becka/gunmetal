#pragma once

// One frame of screen 20, `LAY DOWN THE BUCKS` (`M8.8` slice 14).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/key_jamming_render`
// 16, `ui/match_setup_render` 27, `ui/mouse_setup_render` 22,
// `ui/video_mode_render` 37, `ui/restart_render` 6): the caller owns the state
// and this module paints one frame into the 320x200 mode-13h canvas
// (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads the keyboard,
// `GUNMETAL.CFG`, sounds or saves, and nothing is written back.
//
// Proven geometry (`0x48a830`, docs/UI_SCREENS.md §2)
// --------------------------------------------------
// The handler is `0x48a830`..`0x48a8a5` (44 bytes, `ret` at `0x48a8a5`).  It
// writes `rows = 1` (`0x48a830` -> `0x67f10c`) and `step = 0` (`0x48a83a` ->
// `0x662d58`), copies the preloaded `PLATE` into the 196x131 compose buffer
// (`0x48a844` -> `0x486050`: 0x1913 dwords from `0x6d2b60` to `0x6865f8`), and
// then makes exactly four `0x485dd0` calls — no loop, no condition, no other
// draw call:
//
//   text                        VA          font  centered  x   row   item   y
//   `LAY DOWN THE BUCKS`        `0x4bdb80`  5     yes       38   14   -1     14
//   `THIS FEATURE IS NOT`       `0x4bdb6c`  0     no        42   27   -1     27
//   `AVAILABLE IN THE`          `0x4bdb58`  0     no        42   36   -1     36
//   `SHAREWARE VERSION`         `0x4bdb44`  0     no        42   45   -1     45
//
// (`push` order in every call: item, row, x, centered, text, font — e.g.
// `0x48a849`..`0x48a858` for the title.)  `0x485dd0` replaces `x` by
// `(196 - width)/2` when `centered != 0` (`0x485de2`..`0x485dfa`) and multiplies
// the `item` argument by `[0x662d58]` (`0x485e4b`), so `item = -1` zeroes the
// `item*step` term (`0x485e04`..`0x485e0b`) and `row` *is* the absolute `y`.
// The `x = 38` of the title call is the unused fallback of the centred branch;
// `step = 0` would make the `item` term vanish anyway.
//
// There is no cursor, no highlight and no item
// -------------------------------------------
// All four calls pass `item = -1` (`push 0xffffffff`), which short-circuits the
// highlight rule of `0x485dd0` (`0x485e04`: `cmp ecx,0xffffffff; je` -> skip the
// `[0x68ccf4]` compare at `0x485e0d`), so the screen has **no selectable item**
// and never loads font 4.  `rows = 1` is the handler's own write to `0x67f10c`
// and not a list length: `stats.rows` reports it, the frame has four text runs.
//
// No money: `0x4bc6f8` is not read
// --------------------------------
// The title promises a wager, but the handler reads **no value**: there is no
// `0x4bc6f8` (the twelve-entry starting-cash table of screen 27), no
// `0x4bc728`, no `0x68cd84`/`0x68cd88`, no `sprintf`, no `%s`/`%ld`
// substitution and no digit glyph.  The three body lines are fixed strings that
// say the feature is absent from the shareware build.  `options.cash` is kept
// only for API symmetry with `ui/match_setup_render`; it is accepted and
// ignored (the test pins byte-identical frames for cash 0/4/11/99).
//
// Input, transitions, reachability
// --------------------------------
// The only handler of state 20 in the input function is
// `0x48e06c`: `cmp ebp,0x14; jne ...; mov ebp,ecx; mov [0x4bc440],ebp;
// jmp 0x48e4ac` — the state is replaced by the `ecx` fallback without touching
// any other variable.  No instruction of v1.36 writes the immediate 20 to
// `0x4bc440` (all writes are listed in docs/LAY_BUCKS_RENDER.md §2.4), so the
// screen is a static notice that the shipped build never enters; the frame is
// still fully defined by the four calls above.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE` (docs/UI_SCREENS.md §9) plus the `FONTS4`
// faces 5 (title) and 0 (body).  `PLATE` is loaded and validated because
// `0x486050` copies it as the frame buffer; the frame itself is painted on
// opaque black unless `background` names an entry (`"PLATE"` reproduces the
// panel), because where the engine blits the 196x131 compose buffer on the
// 320x200 canvas is not documented (docs/MENU_RENDER.md §2).
//
// Colours come from the atlas palette: title font 5 -> (155,15,15) dark red,
// body font 0 -> (19,15,15) dark grey (docs/MENU_RENDER.md §3).  `PLATE` uses
// its own BMP palette.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/key_jamming_render`/`ui/match_setup_render`:
// every painted pixel's palette index goes through `render/hud_lut.h::map(level,
// index)` **before** its colour is looked up.  The table comes pre-loaded
// (`fade_lut_table`) or from a 4096-byte dump (`fade_lut_dump`,
// docs/HUD_LUT.md §2).  With neither, the call is a text refusal prefixed
// `lay_bucks_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"lay_bucks_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a missing `FONTS4` face 5 or
// 0, a missing/broken/not-196x131 `PLATE`, a background entry that is
// missing/broken/not 8 bpp, and an exact fade without a table and a dump (or
// with an unreadable dump).  `options.cash` is out of range in no way: it is
// ignored.  On failure `rgba` is cleared, `width`/`height` are 0 (never a stale
// or half-drawn picture) and `*stats` is zeroed.  `stats`/`error` may be null.
// The call only reads the installation, is deterministic and does not depend on
// the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the `0x48e06c` branch — there is no per-item handler, no arrow keys and
// no confirm), UI sounds, the wager/transaction itself, the CLI, and anything
// that would make the screen reachable (v1.36 does not).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kLayBucksCanvasWidth = 320;
inline constexpr int kLayBucksCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kLayBucksBackground = 0x000000FFu;

/// `0x4bdb80`, pushed at `0x48a851` (font 5, centred, row 14).
inline constexpr std::string_view kLayBucksTitle = "LAY DOWN THE BUCKS";
/// `0x4bdb6c`, pushed at `0x48a868` (font 0, x 42, row 27).
inline constexpr std::string_view kLayBucksNotice1 = "THIS FEATURE IS NOT";
/// `0x4bdb58`, pushed at `0x48a87f` (font 0, x 42, row 36).
inline constexpr std::string_view kLayBucksNotice2 = "AVAILABLE IN THE";
/// `0x4bdb44`, pushed at `0x48a896` (font 0, x 42, row 45).
inline constexpr std::string_view kLayBucksNotice3 = "SHAREWARE VERSION";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x486050` copies it into
/// `0x6865f8`, so this is also the text clip.
inline constexpr std::string_view kLayBucksPanelName = "PLATE";
inline constexpr int kLayBucksPanelWidth = 196;
inline constexpr int kLayBucksPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, body 0.  Font 4 (the highlight face) is
/// never loaded: no call passes a real `item`, so `0x485e0d` is unreachable.
inline constexpr int kLayBucksTitleFont = 5;
inline constexpr int kLayBucksRowFont = 0;

/// `0x67f10c = 1` (`0x48a830`): the handler's own "row" count, not a list
/// length (see the header comment).
inline constexpr int kLayBucksRows = 1;
/// `0x662d58 = 0` (`0x48a83a`): no list step.
inline constexpr int kLayBucksItemStep = 0;

/// Title: `push 0xe` at `0x48a84b` (row 14; `item = -1` makes it `y`),
/// `push 0x26` at `0x48a84d` (x 38, ignored by `centered`).
inline constexpr int kLayBucksTitleY = 14;
inline constexpr int kLayBucksTitleX = 38;

/// The three body lines: `push 0x1b` (`0x48a862`), `push 0x24` (`0x48a879`),
/// `push 0x2d` (`0x48a890`), each with `x = 42` (`push 0x2a`), font 0,
/// `centered = 0`, `item = -1`.
inline constexpr int kLayBucksNoticeX = 42;
inline constexpr int kLayBucksNotice1Y = 27;
inline constexpr int kLayBucksNotice2Y = 36;
inline constexpr int kLayBucksNotice3Y = 45;

/// The four draws of `0x48a830`, in emission order: 0 = title, 1..3 = notices.
inline constexpr int kLayBucksLineCount = 4;

/// `[0x4bc728]` of screen 27 (`.data` initial value 4 = 150000).  Screen 20
/// never reads it or `0x4bc6f8`: the option exists for API symmetry only.
inline constexpr int kLayBucksCashDefault = 4;

/// Optional parts of one frame.
struct LayBucksRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 20 and is painted
    /// from the copy that is validated anyway; it is never painted implicitly
    /// (see the header comment).
    std::string background;
    /// Accepted and **ignored**: `0x48a830` prints no amount (no `0x4bc6f8`
    /// read, no digits).  Default 4 is the `.data` value of screen 27's index,
    /// so a caller can pass the same value to both renderers.
    int cash = kLayBucksCashDefault;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/key_jamming_render`, docs/LAY_BUCKS_RENDER.md §4).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `lay_bucks_render: fade LUT needs ...`.
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
/// `rows` is the fixed `0x67f10c` value 1 (not a list length); `ink_pixels` the
/// canvas pixels covered by at least one glyph pixel (unique; the optional
/// backdrop panel is not counted, the same rule as `ui/menu_render`/
/// `ui/key_jamming_render`); `missing_glyphs` the characters with no glyph in
/// the atlas (skipped, never fatal); `arrows` the number of `MENUARRW` sprites
/// painted — screen 20 has no arrow call at `0x48a830`, so it is always 0 and
/// the test pins the 0.
struct LayBucksRenderStats {
    int rows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// `y` of line `line` in emission order (0 title at `kLayBucksTitleY`, 1..3 the
/// notices at `kLayBucksNotice1Y`..`kLayBucksNotice3Y`); an out-of-range line
/// returns the title row.
int lay_bucks_line_y(int line);

/// The text of line `line`; an out-of-range line returns the title.
std::string_view lay_bucks_line_text(int line);

/// The font of line `line`: 5 for the title, 0 for the three notices.
int lay_bucks_line_font(int line);

/// Paint screen 20 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kLayBucksTitleY` (font 5); `kLayBucksNotice1..3`
/// sit left-aligned at `kLayBucksNoticeX` and `kLayBucksNotice1Y`/`..2Y`/`..3Y`
/// (font 0).  Every glyph is clipped to the 196x131 text band.  With
/// `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_lay_bucks(const std::filesystem::path& game,
                      const LayBucksRenderOptions& options,
                      std::vector<std::uint32_t>& rgba, int& width, int& height,
                      LayBucksRenderStats* stats = nullptr,
                      std::string* error = nullptr);

} // namespace gm
