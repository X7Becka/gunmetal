#pragma once

// One frame of screens 24/25, the local player name entry (`M8.8` slice 22).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/save_name_render`
// 15, `ui/key_jamming_render` 16, `ui/joystick_centering_render` 26): the
// caller owns the state (the 17-byte name buffers `0x682f60`/`0x682f71` and
// the edit position `0x683f90`) and this module paints one frame into the
// 320x200 mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads
// the keyboard, the random-name table, the game state or `GUNMETAL.CFG`, and
// nothing is written back.
//
// The handler `0x489020(player)` (docs/UI_SCREENS.md §2, rows 24/25)
// --------------------------------------------------------------------
// The whole painter is `0x489020`..`0x4891b0` (`sub esp,0x20` at `0x489020`,
// `ret` at `0x4891b0`); the only argument selects the player and with it the
// title (`test esi,esi` at `0x489049`, `je 0x489056`):
//
//   * `0x489023` stores `rows 1` into `0x67f10c`, `0x48902d` stores step 0 into
//     `0x662d58` — the map's `rows 1`/`step 0`.  Every text call passes
//     `item = -1`, so `item*step` is never used and the 0 step is invisible;
//   * `0x48903a` calls `0x486050`, which copies the preloaded 196x131 `PLATE`
//     from `0x6d2b60` into the compose buffer `0x6865f8` (`rep movs` of
//     `0x1913` dwords = 25 676 bytes).  Every glyph, the hint block and the
//     caret are drawn into that copy, so **196x131 is a hard clip**;
//   * the title is `0x485dd0(font=5, text, centered=1, x=0x26, row=0xe,
//     item=-1)` (`0x489043`..`0x48905d`): `0x4bd730 "LOCAL PLAYER ONE NAME"`
//     for player 0 and `0x4bd748 "LOCAL PLAYER TWO NAME"` for anything else;
//   * the name line is drawn directly with `0x47faf0(font=3, buffer, dest=
//     0x6865f8, x=0x62-width/2, y=0x1c, stride=0xc4, 8 bpp)` (`0x489105`..
//     `0x489118`): the 16x16 `FONTS2` face, centred on x = 98 (= 196/2) of the
//     plate, y = 28, stride 196;
//   * the caret is a **hollow 17 px box** drawn by `0x485d40` (`0x4890f4`..
//     `0x4890fe`) *before* the text: destination `0x687aa4 + x + w_before`
//     (= plate row 27), height `0x11` = 17, width from the character at the
//     edit position (`0x489095`..`0x4890ae`: `M`/`W` -> 16, `I` -> 6, anything
//     else, including the NUL at the end, -> 8), fill byte =
//     `[0x5a4a74] & 0xFF` — the low byte of the tick counter;
//   * six hint lines follow the name (`0x489120`..`0x4891a2`), all
//     `0x485dd0(font=0, item=-1, centered=0, x=0x10)`: `0x4bd71c`,
//     `0x4bd710`, `0x4bd6f8`, `0x4bd6ec`, `0x4bd6d4`, `0x4bd6c0` at
//     y = 48, 56, 67, 75, 89, 102.
//
// There is no `MENUARRW`, `FILESLOT`, `MENULIT` or sound call in the handler.
//
// The name buffer and its limit
// -----------------------------
// The two names live at `0x682f60` (player 0) and `0x682f71` (player 1): the
// painter addresses them as `0x682f60 + player*0x11` (`0x48906e`,
// `shl ecx,4` + `add`), i.e. **17 bytes each** — the allocation.  The *input*
// limit is smaller: the shared line editor `0x405b20` is called as
// `0x405b20(buffer, 0xb, &0x683f90)` (`0x48f4c3`..`0x48f4da`) and refuses a
// new character once `strlen >= max-1` (`lea eax,[ebx-1]; cmp ecx,eax; jge` at
// `0x405c1d`..`0x405c22`), so a name is at most **10 characters** plus NUL.
// The random default names of `0x4bbc98` (29 entries, `0x480120`) and the
// easter-egg replacements are all <= 10 characters as well.  `options.name` is
// therefore cut to 10 characters and the dropped bytes are counted in
// `stats.truncated`.
//
// The caret position `[0x683f90]` is shared by both screens (the editor clamps
// it to the new buffer, `0x405b2b`/`0x405b45`); the text is the **whole**
// buffer (`0x489115` pushes `edi`), the box only marks the edit position.
// `options.caret_pos` models it: `-1` = after the last character, which is
// what a freshly typed name looks like (the box is then always 8 px wide,
// because the character at the position is the NUL).
//
// Where the default name comes from (not drawn)
// ---------------------------------------------
// The screen opens with two **empty** buffers (`0x48e3af`/`0x48e3b6` store 0
// into `0x682f60`/`0x682f71`), so a fresh frame has no name.  Only on Enter
// (`eax == 1` of `0x405b20`, `0x48f4f3`) an empty buffer is filled with a
// random name: `0x480120` draws from the 29-entry table `0x4bbc98` (`"JIM BOB"`,
// `"AUTOBOT"`, `"SLIME MOLD"`, …) and copies it without a length check
// (`0x48f513`..`0x48f54c`) — all entries are <= 10 characters.  Afterwards
// `"WREN"` becomes `"THE KNIFE"`, `"SPHINXX"` becomes `"SPHINX"` and
// `"SPHINX"` becomes `"WANNA BE"` (`0x48f551`..`0x48f643`, strings `0x4bde10`,
// `0x4bde04`, `0x4bddfc`, `0x4bddf4`, `0x4bdde8`).  None of that is painting:
// this module only draws what the caller passes.
//
// Canvas, background and colours
// ------------------------------
// `options.background` names a `FRONTEND/BMP.IDX` entry painted at (0,0) before
// the text; an empty string means the opaque black canvas.  The proven panel of
// the screen is `PLATE` (196x131), but it is never painted implicitly, exactly
// like the sibling renderers.  Glyph colours come from the atlas palettes
// (docs/MENU_RENDER.md §3): title font 5 -> (155,15,15) dark red, hints font 0
// -> (19,15,15) dark grey, name font 3 (`FONTS2`) -> its shaded face.  The
// caret fill index is looked up in the **PLATE** palette, because the box
// replaces plate pixels; the engine reads the whole 8-bpp buffer through
// `FRONTEND/GAMEPAL`, and `PLATE` differs from it only at index 0 (the cyan
// placeholder), so this is the closest single source.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/save_name_render`: every painted palette
// index (glyph and caret fill) goes through `render/hud_lut.h::map` **before**
// its colour is looked up.  The table comes pre-loaded (`fade_lut_table`) or
// from a 4096-byte dump (`fade_lut_dump`, docs/HUD_LUT.md §2); with neither the
// call refuses with a text, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"player_name_render: "`: an empty game
// directory, a missing `FRONTEND/BMP.IDX`, a missing face 5/0 (`FONTS4`) or 3
// (`FONTS2`), a missing/broken/not-196x131 `PLATE`, a background entry that is
// missing/broken/not 8 bpp, and an exact fade without a table and a dump (or
// with an unreadable dump).  Out-of-range *values* (`player` other than 0/1, a
// name longer than 10 characters, a caret position outside the name) are
// modelled/clamped, not refused: the original cannot produce them and the
// screen stays paintable.  On failure `rgba` is cleared, `width`/`height` are 0
// (never a stale or half-drawn picture) and `*stats` is zeroed.  `stats`/
// `error` may be null.  The call only reads the installation, is deterministic
// and does not depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the editor `0x405b20`: `A-Z`/`0-9`/space/`-`, Backspace, Delete,
// arrows, Enter -> random name/state 28/29 at `0x48f646`/`0x48f68d`, Esc ->
// state 1 at `0x48f6df`), the random-name table, the flag screens 28/29
// (`0x4891c0`), UI sounds and the CLI.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kPlayerNameCanvasWidth = 320;
inline constexpr int kPlayerNameCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kPlayerNameBackground = 0x000000FFu;

/// The panel and the text band: `0x48903a` -> `0x486050` copies the 196x131
/// `PLATE` into `0x6865f8`, so this is also the glyph clip.
inline constexpr std::string_view kPlayerNamePanelName = "PLATE";
inline constexpr int kPlayerNamePanelWidth = 196;
inline constexpr int kPlayerNamePanelHeight = 131;

/// `0x4bd730`, pushed at `0x489056` when the argument is 0.
inline constexpr std::string_view kPlayerNameTitleOne = "LOCAL PLAYER ONE NAME";
/// `0x4bd748`, pushed at `0x48904f` for a non-zero argument.
inline constexpr std::string_view kPlayerNameTitleTwo = "LOCAL PLAYER TWO NAME";

/// The six hint lines of `0x489120`..`0x4891a2`, in emission order.
inline constexpr int kPlayerNameHintCount = 6;
inline constexpr std::string_view kPlayerNameHintTypeTheName = "TYPE THE NAME YOU";
inline constexpr std::string_view kPlayerNameHintWishToUse = "WISH TO USE";
inline constexpr std::string_view kPlayerNameHintLeaveBlank = "LEAVE BLANK TO USE A";
inline constexpr std::string_view kPlayerNameHintRandomName = "RANDOM NAME";
inline constexpr std::string_view kPlayerNameHintEnter = "PRESS ENTER TO CONTINUE";
inline constexpr std::string_view kPlayerNameHintEsc = "PRESS ESC TO ABORT";
inline constexpr std::string_view kPlayerNameHints[kPlayerNameHintCount] = {
    kPlayerNameHintTypeTheName,
    kPlayerNameHintWishToUse,
    kPlayerNameHintLeaveBlank,
    kPlayerNameHintRandomName,
    kPlayerNameHintEnter,
    kPlayerNameHintEsc,
};

/// `push <y>` of the six hint calls (`0x489122`, `0x489139`, `0x489150`,
/// `0x489167`, `0x48917e`, `0x489195`).
inline constexpr int kPlayerNameHintY[kPlayerNameHintCount] = {48, 56, 67, 75, 89, 102};

/// docs/UI_SCREENS.md §3: title 5, hints 0 (both `FONTS4`) and the 16x16 name
/// face 3 (`FONTS2`, `0x489116`).
inline constexpr int kPlayerNameTitleFont = 5;
inline constexpr int kPlayerNameHintFont = 0;
inline constexpr int kPlayerNameNameFont = 3;

/// `0x67f10c = 1` / `0x662d58 = 0` (`0x489023`/`0x48902d`), docs/UI_SCREENS.md
/// §2.  All calls pass `item = -1`, so the step term is zero anyway.
inline constexpr int kPlayerNameRows = 1;
inline constexpr int kPlayerNameItemStep = 0;

/// Title anchors: `push 0xe` (`row`, y 14) and `push 0x26` (`x`, ignored by
/// `centered = 1`, which centres in the 196 px band: `0x485de2`..`0x485dfa`).
inline constexpr int kPlayerNameTitleY = 14;
inline constexpr int kPlayerNameTitleX = 38;

/// The hint block: `x = 0x10` (`push 0x10`) for all six lines, font 0,
/// `centered = 0`, `item = -1`.
inline constexpr int kPlayerNameHintX = 16;

/// The name line: `0x4890d3` loads `0x62` and subtracts `width/2`, so the run is
/// centred on x = 98 of the 196 px plate; `0x48910d` pushes `0x1c` = y 28.
inline constexpr int kPlayerNameNameCenterX = 98;
inline constexpr int kPlayerNameNameY = 28;

/// The caret box of `0x4890f4`..`0x4890fe`: plate row 27
/// (`0x687aa4 - 0x6865f8 = 27*196`), height 17 (`push 0x11`, i.e. the 16 px
/// `FONTS2` cell plus the top and bottom rules), drawn before the text.
inline constexpr int kPlayerNameCaretY = 27;
inline constexpr int kPlayerNameCaretHeight = 17;
/// Width of the box: the character at the edit position decides
/// (`0x489095`..`0x4890ae`), the default `ebp = 8` is set at `0x489075` and is
/// what a caret at the end of the name (NUL) uses.
inline constexpr int kPlayerNameCaretWidth = 8;
inline constexpr int kPlayerNameCaretWideWidth = 16;
inline constexpr int kPlayerNameCaretNarrowWidth = 6;

/// The fill index of the box: the original passes `[0x5a4a74]` (`0x4890e4`)
/// and `0x485d40` writes its **low byte** into the plate copy.  `0x5a4a74` is
/// the tick counter (`inc` at `0x44bdda`), so the box cycles through palette
/// indices every frame; time is not modelled here and the frame uses this
/// fixed index (15 = the (167,167,167) grey ramp entry of `GAMEPAL`).
inline constexpr int kPlayerNameCaretColour = 15;

/// The name buffers `0x682f60`/`0x682f71`: 17 bytes per player (`0x11` stride
/// in `0x48906e`).  The input editor is capped at 11 bytes including NUL
/// (`push 0xb` at `0x48f4cd` + `cmp ecx,[ebx-1]` at `0x405c1d`), so at most
/// **10** characters are reachable.
inline constexpr int kPlayerNameBufferBytes = 17;
inline constexpr int kPlayerNameMaxChars = 10;

/// Optional parts of one frame.
struct PlayerNameRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of the screen and is never
    /// painted implicitly.
    std::string background;

    /// The handler argument of `0x489020`: 0 = screen 24 (`"LOCAL PLAYER ONE
    /// NAME"`, buffer `0x682f60`), anything else = screen 25 (`"LOCAL PLAYER
    /// TWO NAME"`, buffer `0x682f71`).  Only the title depends on it.
    int player = 0;

    /// The text of the name buffer (a C string: a NUL ends it, `0x405b20`
    /// measures with `repnz scas`).  Longer than `kPlayerNameMaxChars`
    /// characters is truncated, the dropped bytes are counted in
    /// `stats.truncated`.  Empty by default — the original enters the screen
    /// with a cleared buffer.
    std::string name;

    /// Draw the hollow caret box of `0x485d40` (on by default: the screen is
    /// an input screen and the box is painted unconditionally).
    bool caret = true;

    /// The edit position `[0x683f90]`: 0..`name.size()`, `-1` = after the last
    /// character.  Out-of-range values are clamped to `0..size`.  It selects
    /// the box position and, through the character under it, the box width.
    int caret_pos = -1;

    /// Palette index of the caret box fill (the original's `[0x5a4a74] & 0xFF`,
    /// see `kPlayerNameCaretColour`); taken modulo 256.
    int caret_colour = kPlayerNameCaretColour;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level.
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `player_name_render: fade LUT needs ...`.
    bool fade_lut = false;
    /// Fade level of the exact path, 0..15; `map` clamps, but 12..15 are real
    /// tables (they brighten).
    int fade_lut_level = kHudLutIdentityLevel;
    /// Path of the 4096-byte dump (`hud_lut.bin`, docs/HUD_LUT.md §2), read with
    /// `load_hud_lut`.  Used when `fade_lut_table` is null.
    std::string fade_lut_dump;
    /// A table already in memory (e.g. `build_hud_lut` output); wins over
    /// `fade_lut_dump`.  Only read during the call, the caller keeps ownership.
    const HudLut* fade_lut_table = nullptr;
};

/// Numbers of one frame.
///
/// `rows` is the fixed 1 of the map; `chars` the number of name bytes drawn
/// (the whole buffer, at most `kPlayerNameMaxChars`); `truncated` the name
/// bytes dropped by the 10-character limit; `ink_pixels` the canvas pixels
/// covered by at least one **glyph** pixel (title, hints and name; unique,
/// independent of the background); `missing_glyphs` the characters with no
/// glyph in their atlas (skipped, never fatal); `arrows` the number of
/// `MENUARRW` sprites painted — the handler has no arrow call, so always 0;
/// `caret_pixels` the plate pixels painted by the caret box (0 while `caret` is
/// off, at most 2*width + 2*(height-2) = 46 for the 8x17 box).  The box is
/// painted before the text, so a glyph can overwrite a box pixel and appear in
/// both counts.
struct PlayerNameRenderStats {
    int rows = 0;
    int chars = 0;
    int truncated = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
    int caret_pixels = 0;
};

/// The title of `0x489020(player)`: `kPlayerNameTitleOne` for 0,
/// `kPlayerNameTitleTwo` for any other value (the handler only tests for zero,
/// `0x489049`).
std::string_view player_name_title(int player);

/// The bytes the original would keep in the name buffer for *options*: `name`
/// up to the first NUL, truncated to `kPlayerNameMaxChars` characters.  This is
/// exactly the run the renderer draws and measures.
std::string player_name_buffer(const PlayerNameRenderOptions& options);

/// The edit position inside `player_name_buffer(options)`: `caret_pos` clamped
/// to `0..size`, or `size` for `-1`.
int player_name_caret_pos(const PlayerNameRenderOptions& options);

/// Width of the caret box for the character at the edit position
/// (`0x489095`..`0x4890ae`): 16 for `M`/`W`, 6 for `I`, 8 for everything else
/// (including the NUL at the end of the name).
int player_name_caret_width(unsigned char ch);

/// `x` of the name run: `kPlayerNameNameCenterX - text_width / 2`, the
/// centre of `0x485dd0`/`0x47fa50` applied to the 196 px band (`0x4890d3`).
int player_name_text_x(int text_width);

/// Paint the frame of screen 24/25 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The frame is painted in the handler's emission order: the title of
/// `player_name_title(options.player)` centred at y 14 in font 5, the caret box
/// at (`player_name_text_x` + width of the prefix, 27) in the PLATE palette
/// while `options.caret` is on, the name of `player_name_buffer(options)` at
/// (`player_name_text_x`, 28) in font 3 and the six hint lines at
/// (`kPlayerNameHintX`, 48/56/67/75/89/102) in font 0.  Every glyph and the box
/// are clipped to the 196x131 text band.  With `options.fade_lut` every painted
/// palette index goes through `render/hud_lut.h::map` at the level before its
/// colour is looked up; the untouched black canvas has no index and stays
/// black.  Returns false + `*error` on each refusal of the header comment, and
/// true with `width == 320`, `height == 200` and `rgba.size() == 64000`
/// otherwise.
bool render_player_name(const std::filesystem::path& game,
                        const PlayerNameRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        PlayerNameRenderStats* stats = nullptr,
                        std::string* error = nullptr);

} // namespace gm
