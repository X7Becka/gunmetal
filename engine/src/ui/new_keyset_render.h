#pragma once

// One frame of screen 31, the new-key-set name dialog (`M8.8` slice 23).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/save_name_render`
// 15, `ui/play_options_render` 32, `ui/keycfg_render` 30/34/35): the caller
// owns the text of the name field and this module paints it into the 320x200
// mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads the
// keyboard, the `KEYCFG` profiles, `GUNMETAL.CFG` or sounds, and nothing is
// written back; the input rule `0x405b20`, the key-set table `0x6c2058` and the
// transition to the editor (`0x4892d0`) stay out of scope
// (docs/NEW_KEYSET_RENDER.md §1).
//
// The handler the map does not list (docs/UI_SCREENS.md §2)
// --------------------------------------------------------
// Screen 31 has no entry in the dispatcher table `0x4883d8`, so the map shows
// `handler —`; the whole frame is painted by one function `0x48ebb0`
// (`sub esp,0x20` at `0x48ebb0`, `ret` at `0x48ed0f`) that takes the **key-set
// index** as its only argument:
//
//   * `0x48ebb3` stores `rows = 1` into `0x67f10c`, `0x48ebbd` stores
//     `step = 0` into `0x662d58` — those two immediates are exactly the
//     `rows 1` / `step 0` of the map;
//   * `0x48ebca` calls `0x486050`, the `PLATE` copy into the compose buffer
//     `0x6865f8` (196x131), so every line is clipped to that band;
//   * `0x48ebcf`..`0x48ebde` paints the title `NAME OF NEW KEY SET`
//     (`0x4bdd8c`) with font 5, `centered = 1`, row 14, `item = -1`;
//   * `0x48ebe3`..`0x48ecaa` paint the text field: the name is
//     `0x6c2058 + set*0x108` (the `name[16]` of the key-set record) and is
//     drawn with **font 3** at `x = 98 - width/2`, `y = 28`, width 196;
//   * `0x48ec8b` draws the caret: a hollow frame `8x17` (the *default* width,
//     see below) with the top edge on row **27** (`0x687aa4 - 0x6865f8 =
//     0x14ac = 27*196`), painted by `0x485d40` **before** the name, so glyph
//     pixels overwrite it;
//   * `0x48ecad`..`0x48ed01` paint four font-0 lines at `x 26`: `TYPE A NAME
//     FOR` (y 48) and `THE NEW KEY SET` (y 58), then `PRESS ENTER TO CONTINUE`
//     (y 87) and `PRESS ESC TO ABORT` (y 102).
//
// Two call sites paint it: `0x48d1a5` (entering state 31 from the key-set list)
// and `0x48f432` (the state-31 branch of the input pass `0x48efc0`, while the
// result of the text rule is 0 = "redraw").
//
// The text field and its caret (proven, §2 of the doc)
// ---------------------------------------------------
// * The name lives in the 16-byte field `0x6c2058 + set*0x108`; the text rule
//   `0x405b20` is called with the maximum 0x10 (`push 0x10` at `0x48f323`) and
//   refuses an insertion while `strlen >= 15` (`lea eax,[ebx-1]; cmp ecx,eax;
//   jge` at `0x405c1d`), so a name carries at most **15** characters plus the
//   NUL.  The rule accepts `A-Z`, `0-9`, space and `-` (`0x405bef`..`0x405c0b`)
//   and upper-cases `a-z` before the test (`0x405b8f`).
// * On entry the field is cleared (`mov byte [ebp],0` at `0x48d19b`), i.e. the
//   fresh dialog draws an empty line; an accepted **empty** field is replaced
//   by `NEW SET` (`0x4bdd3c`) at `0x48f3df`.  `options.name` is the text of the
//   field *without* any caret character: the original never stores one — the
//   caret is a frame drawn at the cursor position.
// * The caret width is the advance of the character **under** the cursor
//   (`0x48ec0c`..`0x48ec47`): 8 by default (`mov ebp,0x8` at `0x48ebfc`), 16
//   for `M`/`W` (`0x48ec3b`) and 6 for `I` (`0x48ec34`) — the variable advances
//   of the 16x16 `FONTS2` face.  With the cursor at the end of the name there
//   is no character (NUL), so the frame is the default `8x17`.
//
// Proven geometry (docs/UI_SCREENS.md §2/§3.7, docs/NEW_KEYSET_RENDER.md §2)
// ------------------------------------------------------------------------
//   * `rows 1`, `step 0` (`0x67f10c`/`0x662d58`, both written by the handler);
//   * title `NAME OF NEW KEY SET` (`0x4bdd8c`), font 5, centred at y 14
//     (`push 0xe` at `0x48ebd1`; the pushed `x = 0x26` is ignored by the
//     `centered` branch `0x485de2`..`0x485dfa`, which computes
//     `x = (196 - width)/2`);
//   * name line: font 3 (`push 0x3`), y 28 (`push 0x1c`), stride 196,
//     `x = 0x62 - width/2 = 98 - width/2` (`mov esi,0x62` at `0x48ec60`,
//     `sub esi,eax` after `sar eax,1`);
//   * caret frame: top edge row 27, height 17 (`push 0x11` at `0x48ec82`),
//     width 8/16/6 as above, `x = name_x + width(prefix)` — with the cursor at
//     the end that is `name_x + width(name)`;
//   * prompts: font 0, `x 26` (`push 0x1a`), rows 0x30/0x3a/0x57/0x66.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE` (196x131, copied into `0x6865f8` by
// `0x486050`) and the `FONTS4` faces 5 (title) and 0 (prompts) plus the
// `FONTS2` face 3 (the name, a 16x16 cell — docs/TEXT_LAYOUT.md §2.1).  All of
// them are validated even when `background` is empty, because the original
// composes the frame on the plate copy; the canvas is opaque black unless
// `background` names an entry (`"PLATE"` reproduces the panel).  Colours come
// from the shared front-end palette: title font 5 -> (155,15,15) dark red,
// prompts font 0 -> (19,15,15) dark grey, font 3 -> its own greys
// (docs/MENU_RENDER.md §3).
//
// The caret colour is the one part of the frame the original cannot be
// reproduced from statically: `0x48ec77` loads `[0x5a4a74]` — the main-loop
// tick counter (docs/INIT.md §3, +1 per tick at `0x44bdda`) — and `0x485d40`
// uses its low byte as the palette index of the frame, so in the shipped game
// the box changes colour every tick.  This module paints it with the fixed
// index `kNewKeysetCaretIndex` (the pure white of the shared palette); the
// choice is a **model**, documented in docs/NEW_KEYSET_RENDER.md §3.4.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/save_name_render`/`ui/keycfg_render`:
// every painted palette index (background, caret frame, glyphs) goes through
// `render/hud_lut.h::map(level, index)` **before** its colour is looked up.
// The table comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2); with neither the call refuses with a
// text, never a silently unfaded frame.  The untouched black canvas has no
// index and stays black.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"new_keyset_render: "`: an empty game
// directory, a missing `FRONTEND/BMP.IDX`, a missing face 5/3/0, a
// missing/broken/not-196x131 `PLATE`, a background entry that is
// missing/broken/not 8 bpp, and an exact fade without a table and a dump (or
// with an unreadable dump).  Out-of-range *values* (a name longer than 15
// characters, undrawable bytes) are truncated/skipped, not refused: the
// original cannot produce them and the screen stays paintable.  On failure
// `rgba` is cleared, `width`/`height` are 0 (never a stale or half-drawn
// picture) and `*stats` is zeroed.  `stats`/`error` may be null.  The call only
// reads the installation, is deterministic and does not depend on the current
// working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the key poll `0x405ae0`, the `A-Z`/`0-9`/space filter, Backspace,
// Delete, Home/End, arrows and Enter/Esc of `0x405b20`, and the `escape`
// return of the state branch), the cursor position inside the name (the API
// keeps the caret at the end), the sound effects, the write of the name into
// `0x6c2058`, the `KEYCFG` copy `0x484cc0`/`0x4859c0` that follows Enter
// (`0x48f3aa`..`0x48f3c9`), the switch to the editor 34 (`0x48f409`) and the
// CLI (`main.cpp`): this module is a frame, not the dialog.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kNewKeysetCanvasWidth = 320;
inline constexpr int kNewKeysetCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kNewKeysetBackground = 0x000000FFu;

/// `mov DWORD PTR ds:0x67f10c,0x1` at `0x48ebb3`: one row.
inline constexpr int kNewKeysetRows = 1;
/// `mov DWORD PTR ds:0x662d58,0x0` at `0x48ebbd`: no row step at all.
inline constexpr int kNewKeysetRowStep = 0;

/// `0x4bdd8c`, pushed at `0x48ebd7`.
inline constexpr std::string_view kNewKeysetTitle = "NAME OF NEW KEY SET";
/// `push 0x5` at `0x48ebdc`.
inline constexpr int kNewKeysetTitleFont = 5;
/// `push 0x26` at `0x48ebd3`; ignored by `centered` (`0x485de2`).
inline constexpr int kNewKeysetTitleX = 38;
/// `push 0xe` at `0x48ebd1`, `item = -1` (`0x48ebcf`) -> y = 14.
inline constexpr int kNewKeysetTitleY = 14;

/// `push 0x3` at `0x48eca3`: the name is drawn with the 16x16 `FONTS2` face.
inline constexpr int kNewKeysetNameFont = 3;
/// `push 0x1c` at `0x48ec9a`: top row of the glyph cell.
inline constexpr int kNewKeysetNameY = 28;
/// `mov esi,0x62` at `0x48ec60`: `x = 98 - width(name)/2` (`0x48ec65` `sar`).
inline constexpr int kNewKeysetNameCentre = 98;
/// `push 0xc4` at `0x48ec95`: the 196-px band of `0x485dd0`.
inline constexpr int kNewKeysetNameWidth = 196;

/// Top edge of the caret frame: `0x687aa4 = 0x6865f8 + 27*196`.
inline constexpr int kNewKeysetCaretY = 27;
/// `push 0x11` at `0x48ec82`: the frame covers y 27..43, one pixel more than
/// the 16-row glyph cell at y 28..43.
inline constexpr int kNewKeysetCaretHeight = 17;
/// `mov ebp,0x8` at `0x48ebfc`: the width when no character is under the cursor
/// (an empty name or the cursor at the end).
inline constexpr int kNewKeysetCaretWidth = 8;
/// `mov ebp,0x10` at `0x48ec3b`: `M`/`W` under the cursor.
inline constexpr int kNewKeysetCaretWide = 16;
/// `mov ebp,0x6` at `0x48ec34`: `I` under the cursor.
inline constexpr int kNewKeysetCaretNarrow = 6;
/// **Model**: the palette index of the caret frame.  The original uses the low
/// byte of the tick counter `[0x5a4a74]` (`0x48ec77`), so its colour churns;
/// index 96 is the pure white `(255,255,255)` of the shared front-end palette
/// (docs/NEW_KEYSET_RENDER.md §3.4).
inline constexpr std::uint8_t kNewKeysetCaretIndex = 96;

/// `push 0x10` at `0x48f323` minus the `lea eax,[ebx-1]` guard of `0x405c1d`:
/// a name carries at most 15 characters plus the NUL of the 16-byte field.
inline constexpr int kNewKeysetMaxChars = 15;

/// The accepted-empty fallback `0x4bdd3c`, copied by `0x48f3df` (`strncpy`
/// into the 16-byte field).  The dialog itself enters with the field cleared
/// (`0x48d19b`), i.e. `name = ""` is the fresh frame.
inline constexpr std::string_view kNewKeysetDefault = "NEW SET";

/// `0x4bdd7c`, pushed at `0x48ecb5` with row 0x30 (y 48).
inline constexpr std::string_view kNewKeysetPromptOne = "TYPE A NAME FOR";
/// `0x4bdd6c`, pushed at `0x48eccc` with row 0x3a (y 58).
inline constexpr std::string_view kNewKeysetPromptTwo = "THE NEW KEY SET";
/// `0x4bd6d4`, pushed at `0x48ece3` with row 0x57 (y 87).
inline constexpr std::string_view kNewKeysetEnterHint = "PRESS ENTER TO CONTINUE";
/// `0x4bd6c0`, pushed at `0x48ecfa` with row 0x66 (y 102).
inline constexpr std::string_view kNewKeysetEscHint = "PRESS ESC TO ABORT";

/// `push 0x0` at `0x48ecba`/`0x48ecd1`/`0x48ece8`/`0x48ecff`: font 0.
inline constexpr int kNewKeysetHintFont = 0;
/// `push 0x1a` at `0x48ecb3`/`0x48ecca`/`0x48ece1`/`0x48ecf8`: x 26, not centred.
inline constexpr int kNewKeysetHintX = 26;
/// `push 0x30` at `0x48ecb1` -> y 48.
inline constexpr int kNewKeysetPromptOneY = 48;
/// `push 0x3a` at `0x48ecc8` -> y 58.
inline constexpr int kNewKeysetPromptTwoY = 58;
/// `push 0x57` at `0x48ecdf` -> y 87.
inline constexpr int kNewKeysetEnterHintY = 87;
/// `push 0x66` at `0x48ecf6` -> y 102.
inline constexpr int kNewKeysetEscHintY = 102;

/// The panel of screen 31 (`menu_panel_asset(Screen::NewKeySetName)`): the
/// 196x131 `PLATE` copy `0x486050` (`0x48ebca`).
inline constexpr std::string_view kNewKeysetPanelName = "PLATE";
inline constexpr int kNewKeysetPanelWidth = 196;
inline constexpr int kNewKeysetPanelHeight = 131;

/// Optional parts of one frame.
struct NewKeysetRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 31, but it is
    /// never painted implicitly.
    std::string background;

    /// Text of the name field `0x6c2058 + set*0x108`, **without** a caret
    /// character (the original stores none: the caret is a frame).  Long names
    /// are cut to `kNewKeysetMaxChars` (15) and counted in `stats.truncated`.
    /// The default is the proven accepted-empty fallback `NEW SET`; an empty
    /// string paints the fresh dialog (field cleared at `0x48d19b`).
    std::string name = std::string(kNewKeysetDefault);

    /// Draw the caret frame at the end of the name (`kNewKeysetCaretY`, 8x17).
    /// `false` paints the name alone (the frame the original draws while the
    /// text rule reports "nothing to redraw" is the same minus the box; the
    /// caret is never stored in the field).
    bool caret = true;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (docs/NEW_KEYSET_RENDER.md §6).  Needs a table, either pre-loaded
    /// (`fade_lut_table`) or read from `fade_lut_dump`; without both the call
    /// fails with `new_keyset_render: fade LUT needs ...`.
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
/// `rows` is the proven `0x67f10c = 1` (the row step of the screen is 0, so a
/// row number carries no `item*step` term), `chars` the characters of the name
/// actually drawn (after the NUL and the 15-character limit), `truncated` how
/// many characters the limit dropped, `ink_pixels` the canvas pixels covered by
/// at least one **glyph** pixel (unique, independent of the background; the
/// caret frame is not glyph ink), `missing_glyphs` the characters with no glyph
/// in the atlas (skipped, never fatal) and `arrows` how many `MENUARRW` sprites
/// the frame carries — always 0: `0x48ebb0` has no such call.
struct NewKeysetRenderStats {
    int rows = 0;
    int chars = 0;
    int truncated = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// The text drawn for `name`: cut at the first NUL (the field is a C string,
/// `0x405b20` measures it with `repnz scas`) and at `kNewKeysetMaxChars`.
std::string new_keyset_name(const NewKeysetRenderOptions& options);

/// `x` of the name line for a run `name_width` wide: `98 - name_width/2`
/// (`mov esi,0x62` at `0x48ec60`, `sar eax,1` at `0x48ec65`).  A negative
/// result is returned as is — the original writes at a negative column, this
/// module clips the run to the 196-px band.
int new_keyset_text_x(int name_width);

/// Width of the caret frame for the character under the cursor (`0x48ebfc`,
/// `0x48ec34`, `0x48ec3b`): 16 for `M`/`W`, 6 for `I`, 8 for everything else
/// and for the NUL of a cursor at the end of the name.
int new_keyset_caret_width(std::uint8_t character);

/// Paint one frame of screen 31 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// `PLATE` is copied first (as `0x486050` does), then the title
/// `NAME OF NEW KEY SET` (font 5, centred in the 196-px band at y 14), then the
/// caret frame at `kNewKeysetCaretY` (8x17, `options.caret`), then the name
/// (font 3, `x = 98 - width/2`, y 28) and the four font-0 prompts at x 26
/// (y 48/58/87/102).  Every glyph and the frame are clipped to the 196x131 band
/// (a name wider than 196 px, whose centred `x` is negative, is cut at the left
/// edge — see docs/NEW_KEYSET_RENDER.md §3.5).  With `options.fade_lut` every
/// painted palette index (background, caret, glyphs) goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas stays black.  Returns false + `*error` on each refusal
/// of the header comment, and true with `width == 320`, `height == 200` and
/// `rgba.size() == 64000` otherwise.
bool render_new_keyset(const std::filesystem::path& game,
                       const NewKeysetRenderOptions& options,
                       std::vector<std::uint32_t>& rgba, int& width, int& height,
                       NewKeysetRenderStats* stats = nullptr,
                       std::string* error = nullptr);

} // namespace gm
