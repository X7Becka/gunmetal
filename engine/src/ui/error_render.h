#pragma once

// One frame of screen 5, `ERROR` (`M8.8` slice 16).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/key_jamming_render`
// 16, `ui/lay_bucks_render` 20, `ui/match_setup_render` 27,
// `ui/mouse_setup_render` 22, `ui/video_mode_render` 37, `ui/restart_render`
// 6): the caller owns the state — the formatted error text — and this module
// paints one frame into the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3).  Nothing here reads the keyboard, `GUNMETAL.CFG`, sounds or
// saves, and nothing is written back.
//
// Proven geometry (`0x48acd0`, docs/UI_SCREENS.md §2)
// ---------------------------------------------------
// The handler is `0x48acd0`..`0x48ada5` (0xd5 bytes, `ret` at `0x48ada5`, then
// 10 `nop`s).  The dispatcher `0x487e71` computes `eax = [0x4bc440] - 5` and
// jumps through the 33-entry table `0x4883d8`; entry 0 is the stub
// `0x487f01: call 0x48acd0; jmp 0x487f64` (`ui_screens.py` re-checks both).
//
//   rows / step: `0x48acd4 xor eax,eax`; `0x48acd7 mov [0x67f10c],eax` and
//                `0x48acdc mov [0x662d58],eax` — both are **0**, unlike most
//                screens (the ERROR frame has no list).
//   panel:       `0x48ace1 call 0x486050` copies the preloaded 196x131 `PLATE`
//                into the compose buffer `0x6865f8`, exactly like screens 16
//                and 20.
//
// Then it makes exactly one fixed call, an optional word loop and one more
// fixed call — no other draw call, no checkbox, slider, arrow or `sprintf`:
//
//   text                        VA          font  centered  x    row  y
//   `ERROR`                     `0x4bdc50`  5     yes       38   14   14
//   every word of `0x4aa2f8`    (buffer)    0     no        10*  y*   y*
//   `ESC TO RETURN TO MENU`     `0x4bdc38`  0     yes       100  91   91
//
// (`push` order in every call: item, row, x, centered, text, font — e.g.
// `0x48ace6`..`0x48acf5` for the title and `0x48ad89`..`0x48ad98` for the
// prompt.)  `0x485dd0` replaces `x` by `(196 - width)/2` when `centered != 0`
// (`0x485de2`..`0x485dfa`) and multiplies the `item` argument by `0x662d58`
// (`0x485e4b`), so `item = -1` (`push 0xffffffff`) zeroes the `item*step` term
// (`0x485e04`..`0x485e0b`) and `row` *is* the absolute `y`.  The `x = 38` and
// `x = 100` immediates are the unused fallback of the centred branch.
//
// The dynamic message: buffer `0x4aa2f8`, word-wrapped by the handler
// ------------------------------------------------------------------
// `0x48acfa` reads the first byte of the shared error buffer `0x4aa2f8` and
// `0x48ad02`/`0x48ad16` skip the whole loop when it is 0.  Otherwise the loop
// `0x48ad1a`..`0x48ad85` walks the string word by word:
//
//   * a word runs up to the next `0x20` space (`0x48ad1e`..`0x48ad30`); the
//     space is temporarily replaced by a NUL (`0x48ad37`), `0x47fa50` (font 0)
//     measures the word (`0x48ad3a`), and the byte is restored afterwards
//     (`0x48ad7d`);
//   * the pen `x` is `edi`, initially `0xa` = 10 (`0x48ad0c`), and `y` is the
//     local `[esp+0x10]`, initially `0x22` = 34 (`0x48ad04`; the loop's own
//     `push ebp/ebx` at `0x48ad18` is why the same dword is `[esp+0x8]` before
//     the loop and `[esp+0x10]` inside it);
//   * if `x + word_width > 0xb4` = 180 (`0x48ad44`..`0x48ad4d`, strictly
//     greater: 180 still fits) the pen resets to 10 and `y += 0xa` = 10
//     (`0x48ad4f`..`0x48ad5a`) *before* the word is drawn;
//   * the word is drawn with font 0, `centered = 0`, `item = -1`
//     (`0x48ad66`..`0x48ad6f`), then `x += word_width + 6` (`0x48ad77`).
//
// The 6-px gap replaces the space glyph, so a doubled space is a second loop
// iteration with an empty word: it draws nothing but still costs 6 px.  A word
// wider than 170 px therefore triggers one wrap even at the start of a line,
// and the 196-px band (not the 180-px wrap width) is the clip.  `y` has no
// upper bound in the handler: long text simply runs past the 131-row buffer.
//
// There is **no `%s`/`%d` in the handler**.  The substitution happens *before*
// the screen: the error routines format into `0x4aa2f8` with `sprintf`
// (`0x40d8e0`) — e.g. `0x4424e5` (`"Unable to load world file (%s)."`,
// `0x4ab5e4`), `0x466b78` (`"Network client program version (%ld.%02ld) ..."`,
// `0x4b9fc8`) and `0x466ce7` (`... level version ...`, `0x4b9f64`) — and then
// write state 5 to `0x4bc440` (`0x4424fa`, `0x466b98`, `0x466cf9`, …).  The
// handler draws that buffer verbatim, which is why the API takes the finished
// `message` string and no format arguments.
//
// The shipped `gundx.exe` image already contains `"No Error."` at `0x4aa2f8`;
// that is the `.data` initial value and the default of `options.message`.  The
// next referenced dword is `0x4aa348`, i.e. the buffer has 0x50 bytes of room,
// but `sprintf` is unbounded and no length is enforced.
//
// Input and reachability
// ----------------------
// State 5 has exactly one input branch, `0x48b56c` in the front-end input
// function: `cmp ebp,0x5; jne ...; cmp bl,0x1b; jne 0x48e659;
// mov [0x4bc440],1; ret` — **Esc returns to the title (state 1)**, any other
// key is a plain `ret` (no arrow keys, no Enter, no sound).  Unlike screen 20,
// state 5 is reachable: every error routine that formats the buffer writes it.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE` (docs/UI_SCREENS.md §9) plus the `FONTS4`
// faces 5 (title) and 0 (message and prompt).  Font 4 is unreachable: every
// call passes `item = -1`, so the highlight rule of `0x485e0d` never fires.
// `PLATE` is loaded and validated because `0x486050` copies it as the frame
// buffer; the frame itself is painted on opaque black unless `background` names
// an entry (`"PLATE"` reproduces the panel), because where the engine blits the
// 196x131 compose buffer on the 320x200 canvas is not documented
// (docs/MENU_RENDER.md §2).
//
// Colours come from the atlas palette: title font 5 -> (155,15,15) dark red,
// body font 0 -> (19,15,15) dark grey (docs/MENU_RENDER.md §3).  `PLATE` uses
// its own BMP palette.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/key_jamming_render`/`ui/lay_bucks_render`:
// every painted pixel's palette index goes through `render/hud_lut.h::map(level,
// index)` **before** its colour is looked up.  The table comes pre-loaded
// (`fade_lut_table`) or from a 4096-byte dump (`fade_lut_dump`,
// docs/HUD_LUT.md §2).  With neither, the call is a text refusal prefixed
// `error_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"error_render: "`: an empty game directory,
// a missing/broken `FRONTEND/BMP.IDX`, a missing `FONTS4` face 5 or 0, a
// missing/broken/not-196x131 `PLATE`, a background entry that is
// missing/broken/not 8 bpp, and an exact fade without a table and a dump (or
// with an unreadable dump).  `options.message` cannot be refused: any byte
// string is drawn, characters without a glyph are skipped and counted in
// `missing_glyphs`, and an empty message reproduces the empty-buffer branch of
// `0x48ad16` (title and prompt only).  On failure `rgba` is cleared,
// `width`/`height` are 0 (never a stale or half-drawn picture) and `*stats` is
// zeroed.  `stats`/`error` may be null.  The call only reads the installation,
// is deterministic and does not depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the `0x48b56c` branch: Esc -> state 1, nothing else), UI sounds, the
// error generation itself (the buffer is filled by unrelated routines), the
// CLI, and a UTF-8/format interpretation of the message: the bytes are the
// engine's latin-1 glyph codes.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kErrorCanvasWidth = 320;
inline constexpr int kErrorCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kErrorBackground = 0x000000FFu;

/// `0x4bdc50`, pushed at `0x48acee` (font 5, centred, row 14).
inline constexpr std::string_view kErrorTitle = "ERROR";
/// `0x4bdc38`, pushed at `0x48ad91` (font 0, centred, row 91).
inline constexpr std::string_view kErrorPrompt = "ESC TO RETURN TO MENU";
/// `.data` initial content of the message buffer `0x4aa2f8`: the default of
/// `ErrorRenderOptions::message`.
inline constexpr std::string_view kErrorDefaultMessage = "No Error.";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x486050` copies it into
/// `0x6865f8`, so this is also the text clip.
inline constexpr std::string_view kErrorPanelName = "PLATE";
inline constexpr int kErrorPanelWidth = 196;
inline constexpr int kErrorPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, message and prompt 0.  Font 4 (the highlight
/// face) is never loaded: every call passes `item = -1`.
inline constexpr int kErrorTitleFont = 5;
inline constexpr int kErrorTextFont = 0;

/// `0x67f10c = 0` (`0x48acd7`) and `0x662d58 = 0` (`0x48acdc`): the ERROR frame
/// is not a list, so `rows` is 0 and the `item*step` term is always 0.
inline constexpr int kErrorRows = 0;
inline constexpr int kErrorItemStep = 0;

/// Title: `push 0xe` at `0x48ace8` (row 14, and `item = -1` makes it `y`),
/// `push 0x26` at `0x48acea` (x 38, ignored by `centered`).
inline constexpr int kErrorTitleY = 14;
inline constexpr int kErrorTitleX = 38;

/// The word loop of `0x48ad1a`..`0x48ad85`: pen starts at `0xa` = 10
/// (`0x48ad0c`), first row is `0x22` = 34 (`0x48ad04`), a wrap adds `0xa` = 10
/// (`0x48ad58`), the wrap threshold is `x + width > 0xb4` = 180
/// (`0x48ad48`), and every word is followed by `0x6` = 6 px (`0x48ad77`).
inline constexpr int kErrorMessageX = 10;
inline constexpr int kErrorMessageY = 34;
inline constexpr int kErrorMessageLinePitch = 10;
inline constexpr int kErrorMessageWrapWidth = 180;
inline constexpr int kErrorWordGap = 6;

/// Prompt: `push 0x5b` at `0x48ad8b` (row 91), `push 0x64` = 100 at `0x48ad8d`
/// (x, ignored by `centered`).
inline constexpr int kErrorPromptY = 91;
inline constexpr int kErrorPromptX = 100;

/// Optional parts of one frame.
struct ErrorRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 5 and is painted
    /// from the copy that is validated anyway; it is never painted implicitly
    /// (see the header comment).
    std::string background;

    /// The finished text of the error buffer `0x4aa2f8`, drawn word by word
    /// (`centered = 0`, font 0) exactly as the loop at `0x48ad1a` does.  The
    /// substitution of `%s`/`%d` happens in the error routines **before** the
    /// screen (§header), so this is the message and not a format string.  An
    /// empty string reproduces the `0x48ad16` branch: title and prompt only.
    /// NUL bytes cannot occur in a `std::string`, so the loop always runs to the
    /// end of this value.  Default `"No Error."` = the shipped `.data` image.
    std::string message = std::string(kErrorDefaultMessage);

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/key_jamming_render`, docs/ERROR_RENDER.md §3).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `error_render: fade LUT needs ...`.
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
/// `rows` is the fixed `0x67f10c` value 0 (not a list length); `lines` the
/// number of text runs the handler emits — 1 for `ERROR`, one per loop
/// iteration (i.e. per word, including the empty words a doubled space
/// produces) and 1 for `ESC TO RETURN TO MENU`; `ink_pixels` the canvas pixels
/// covered by at least one glyph pixel (unique; the optional backdrop panel is
/// not counted, the same rule as `ui/menu_render`/`ui/key_jamming_render`);
/// `missing_glyphs` the characters with no glyph in the atlas (skipped, never
/// fatal); `arrows` the number of `MENUARRW` sprites painted — screen 5 has no
/// arrow call at `0x48acd0`, so it is always 0 and the test pins the 0.
struct ErrorRenderStats {
    int rows = 0;
    int lines = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// The exact word sequence of the loop `0x48ad1a`..`0x48ad85`: split at single
/// `' '` bytes; a leading space, a doubled space or a trailing space yields an
/// empty word that draws nothing but still advances the pen by
/// `kErrorWordGap` (the listing restores the terminator and skips one space at
/// a time).  `"No Error."` -> `{"No", "Error."}`.
std::vector<std::string> error_words(std::string_view message);

/// Paint screen 5 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// `ERROR` is centred at `kErrorTitleY` in font 5; the words of
/// `options.message` are laid out from (`kErrorMessageX`, `kErrorMessageY`) in
/// font 0, wrapping to the next row when `x + word_width` exceeds
/// `kErrorMessageWrapWidth` (strictly greater), with `kErrorWordGap` px between
/// words; `ESC TO RETURN TO MENU` is centred at `kErrorPromptY` in font 0.
/// Every glyph is clipped to the 196x131 text band.  With `options.fade_lut`
/// every painted palette index goes through `render/hud_lut.h::map` at the
/// level before its colour is looked up; the untouched black canvas has no
/// index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_error(const std::filesystem::path& game, const ErrorRenderOptions& options,
                  std::vector<std::uint32_t>& rgba, int& width, int& height,
                  ErrorRenderStats* stats = nullptr, std::string* error = nullptr);

} // namespace gm
