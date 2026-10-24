#pragma once

// One frame of screen 15, the save-name entry (`M8.8` slice 13).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/key_jamming_render`
// 16, `ui/joystick_centering_render` 26, `ui/restart_render` 6,
// `ui/match_setup_render` 27): the caller owns the state (the input buffer
// `0x661280` and the list cursor `0x68ccf4`) and this module paints it into the
// 320x200 mode-13h canvas (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads the
// keyboard, the save files, `GUNMETAL.CFG` or sounds, and nothing is written
// back; the writer `0x4422d0` and the slot table `0x6c2fe0` are out of scope
// (docs/SAVE_SLOTS.md, docs/SAVE_WRITE_CPP.md).
//
// The handler is the state-15 branch of the input function `0x48b4d0`
// --------------------------------------------------------------------
// The screen map (`docs/UI_SCREENS.md` §2) lists no handler for 15 because the
// dispatcher has no per-state draw entry for it: the state-15 branch of the
// input function `0x48b4d0` (`sub esp,8` at `0x48b4d0`, `cmp ebp,0xf` at
// `0x48b691`, `jne 0x48b877`, `ret` at `0x48b876`) paints the whole frame at the
// end of every pass:
//
//   0x48b816  call 0x48abd0   the SAVE GAME frame: rows 6 / step 12, PLATE
//                             copy `0x486050`, MENUARRW, title `SAVE GAME`
//                             (font 5, centred, y 14) and `ESC TO CANCEL`
//                             (font 0, x 38, y 101)
//   0x48b840  band 0x48ab90   `FILESLOT` 124x10 at `0x687aca + c*0x930`, i.e.
//                             (38, 27 + 12*c) of the plate copy `0x6865f8`
//   0x48b867  text 0x485dd0   the buffer `0x661280` at x 42, row `-0x1c-12*c`
//                             (= y 28 + 12*c), font 0 -> **forced to 4** by
//                             `0x485e19` because `item = c = [0x68ccf4]`
//
// `c` is `[0x68ccf4]`, the window row inherited from screen 14 (the slot the
// player picked, 0..5; docs/UI_SCREENS.md §5.2).  This module paints the three
// pieces the input screen owns (title of the frame it is drawn on, the band and
// the input line) plus `ESC TO CANCEL`, i.e. one row (`rows` 1 / step 0 of the
// map) rather than the six slot rows of screen 14: those are
// `ui/save_menu_render`'s frame, and re-deriving them would need the slot table
// that this module deliberately does not read.
//
// The input buffer and the caret (`M8.8` slice 13, docs/SAVE_NAME_RENDER.md §2)
// ----------------------------------------------------------------------------
// * `0x661280` is a NUL-terminated byte buffer of at most **18** bytes
//   (`cmp ecx,0x12; jge 0x48b816` at `0x48b6c2` rejects a 19th byte), i.e. at
//   most **17** name characters **plus** the caret.
// * The caret is **not** a separate sprite and does **not** blink: it is the
//   literal trailing `'-'` (0x2d) that the input rules keep at the end of the
//   buffer.  Entering screen 15 fills the buffer with `-` (empty slot,
//   `0x4bdd28`) or `"%s-"` of the slot name (`0x4bdd24`, `0x48e0fb`); every
//   accepted `A-Z`/`0-9`/space is written over the caret and the caret is
//   re-appended (`0x48b6cb`/`0x48b6d1`); Backspace/arrows move it one left
//   (`0x48b803`); Enter (`0xd`/`0xa4`) strips it (`0x48b75f`) or, when the
//   buffer is only the caret, replaces it with `A SAVED GAME` (`0x4bdd44`,
//   `0x48b768`).  There is no timer or blink flag anywhere in
//   `0x48b691`..`0x48b876`, so `options.caret` draws exactly that trailing `-`
//   in the same face as the rest of the buffer (font 4, forced by `0x485e19`).
//   The position field of the first sketch is therefore not modelled: the only
//   caret the original can draw is the last character.
// * `options.name` is the text **without** the caret (default `A SAVED GAME`,
//   the post-Enter fallback `0x4bdd44`); `options.caret = true` appends the
//   proven trailing `-`.  `save_name_buffer()` returns exactly the bytes the
//   original would hold, i.e. the drawn run.
//
// Proven geometry (docs/UI_SCREENS.md §2/§3/§5.2)
// -----------------------------------------------
//   * `rows 1`, `step 0` — the state-15 branch writes neither `0x67f10c` nor
//     `0x662d58`; `0x48abd0` sets 6/12 for the underlying screen-14 frame, but
//     every call of that frame (and of the input line) passes a **negative**
//     `row`, so the step term `item*step` is never used on this screen.
//   * title `SAVE GAME` (`0x4bdc2c`), font 5, centred (`0x48ac2c`), y 14
//     (`push 0xe` at `0x48ac28`), `item = -1` (`0x48ac26`);
//   * input line x 42 (`push 0x2a` at `0x48b85c`), y = 28 + 12*c
//     (`mov ecx,0xffffffe4`, `sub ecx,eax` with `eax = 12*c`), font 4
//     (forced), `item = c`;
//   * `FILESLOT` bar 124x10 at (38, 27 + 12*c) (`0x687aca - 0x6865f8 =
//     0x14d2 = 27*196 + 38`, step `0x930 = 12*196`), **below** the text call in
//     the emission order: the name lies on the bar;
//   * `ESC TO CANCEL` (`0x4bd9d8`), font 0, x 38, y 101 (`0x48ac9e`..`0x48acb4`,
//     row `0x65`);
//   * the text band is the 196x131 plate copy `0x6865f8` (`0x486050`), so glyphs
//     are clipped to it.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE` (196x131, copied into `0x6865f8` by
// `0x486050` at `0x48abe7`), `FILESLOT` (124x10) and the `FONTS4` faces 5
// (title), 0 (`ESC TO CANCEL`) and 4 (the input line).  All three are validated
// even when `background` is empty, because the original composes the frame on
// the plate copy; the canvas is opaque black unless `background` names an entry
// (`"PLATE"` reproduces the panel).  Colours come from the atlas palette: title
// font 5 -> (155,15,15) dark red, body font 0 -> (19,15,15) dark grey, font 4 ->
// (239,239,239) white (docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/save_menu_render`/`ui/key_jamming_render`:
// every painted palette index goes through `render/hud_lut.h::map(level, index)`
// **before** its colour is looked up.  The table comes pre-loaded
// (`fade_lut_table`) or from a 4096-byte dump (`fade_lut_dump`,
// docs/HUD_LUT.md §2); with neither the call refuses with a text, never a
// silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"save_name_render: "`: an empty game
// directory, a missing `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken/not-196x131 `PLATE`, a missing/broken/not-124x10 `FILESLOT`
// while `fileslot` is on, a background entry that is missing/broken/not 8 bpp,
// and an exact fade without a table and a dump (or with an unreadable dump).
// Out-of-range *values* (`row` outside 0..5, a name longer than 17 characters)
// are clamped/truncated, not refused: the original cannot produce them and the
// screen stays paintable.  On failure `rgba` is cleared, `width`/`height` are 0
// (never a stale or half-drawn picture) and `*stats` is zeroed.  `stats`/`error`
// may be null.  The call only reads the installation, is deterministic and does
// not depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the key poll `0x498ac0`, the `A-Z`/`0-9`/space filter, Backspace,
// Enter -> writer `0x4422d0`, Esc -> state 14 at `0x48b710`), the six slot rows
// and the scroll arrows of the underlying screen-14 frame, UI sounds and the
// CLI.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kSaveNameCanvasWidth = 320;
inline constexpr int kSaveNameCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kSaveNameBackground = 0x000000FFu;

/// The panel and the text band: `0x48abe7` -> `0x486050` copies the 196x131
/// `PLATE` into `0x6865f8`, so this is also the glyph clip.
inline constexpr std::string_view kSaveNamePanelName = "PLATE";
inline constexpr int kSaveNamePanelWidth = 196;
inline constexpr int kSaveNamePanelHeight = 131;

/// `0x4bdc2c`, pushed at `0x48ac2e` by `0x48abd0` (font 5, centred, y 14).
inline constexpr std::string_view kSaveNameTitle = "SAVE GAME";

/// `0x4bd9d8`, pushed at `0x48acaf` (font 0, x 38, y 101).
inline constexpr std::string_view kSaveNameCancel = "ESC TO CANCEL";

/// docs/UI_SCREENS.md §3: title 5, body 0, the highlight face 4 that
/// `0x485dd0` swaps in for the input line (`0x485e15`/`0x485e19`).
inline constexpr int kSaveNameTitleFont = 5;
inline constexpr int kSaveNameRowFont = 0;
inline constexpr int kSaveNameInputFont = 4;

/// `0x67f10c`/`0x662d58` of the state-15 branch: it writes neither, and the
/// map records `rows 1`/`step 0` (`docs/UI_SCREENS.md` §2).
inline constexpr int kSaveNameRows = 1;
inline constexpr int kSaveNameItemStep = 0;

/// `0x4bdc2c` title anchors: `push 0xe` (`row`) and `push 0x26` (`x`, ignored
/// by `centered = 1`).
inline constexpr int kSaveNameTitleY = 14;
inline constexpr int kSaveNameTitleX = 38;

/// `ESC TO CANCEL` anchors of `0x48abd0` (`0x48acab`/`0x48acaa`).
inline constexpr int kSaveNameCancelX = 38;
inline constexpr int kSaveNameCancelY = 101;

/// The input line: x 42 (`push 0x2a`), y 28 + 12*c (`row = -0x1c - 12*c`).
inline constexpr int kSaveNameTextX = 42;
inline constexpr int kSaveNameFirstRowY = 28;
inline constexpr int kSaveNameRowStep = 12;

/// The `FILESLOT` bar: same 124x10 entry and anchors as screen 14
/// (`0x687aca + c*0x930`, docs/SAVE_MENU_RENDER.md §1).
inline constexpr std::string_view kSaveNameFileslotName = "FILESLOT";
inline constexpr int kSaveNameFileslotWidth = 124;
inline constexpr int kSaveNameFileslotHeight = 10;
inline constexpr int kSaveNameFileslotX = 38;
inline constexpr int kSaveNameFileslotFirstY = 27;

/// Rows of the screen-14 window the cursor lives in: `[0x68ccf4]` is 0..5
/// (`0x48abd0` draws six rows at `-0x1c - 12*i`, `0x48ac99`).
inline constexpr int kSaveNameWindowRows = 6;

/// The input buffer `0x661280`: at most 18 bytes including the caret
/// (`cmp ecx,0x12` at `0x48b6c2`), i.e. at most 17 characters + the caret.
inline constexpr int kSaveNameMaxChars = 17;
inline constexpr int kSaveNameMaxBytes = 18;
/// The caret of `0x48b6d1`/`0x48b803`: a literal dash, not a sprite.
inline constexpr char kSaveNameCaret = '-';
/// The post-Enter fallback name `0x4bdd44` (`0x48b768`).
inline constexpr std::string_view kSaveNameDefault = "A SAVED GAME";

/// Optional parts of one frame.
struct SaveNameRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of the screen and is never
    /// painted implicitly.
    std::string background;

    /// The text of the input buffer without the caret (the post-Enter content).
    /// A NUL ends the buffer (`repnz scas` at `0x48b69a`); longer names are
    /// truncated to `kSaveNameMaxChars` characters, the proven buffer limit, and
    /// `stats.truncated` counts the dropped bytes.
    std::string name = std::string(kSaveNameDefault);

    /// Draw the trailing caret `'-'` of `0x48b6d1` (on by default: screen 15 is
    /// an input screen and its buffer always ends with the caret while open).
    /// There is no blink and no position: see the header comment.
    bool caret = true;

    /// The window row `[0x68ccf4]` carrying the input line (0..5, inherited from
    /// screen 14).  Out-of-range values are clamped to `[0, 5]`.
    int row = 0;

    /// Paint the `FILESLOT` 124x10 bar at (38, 27 + 12*row) under the input
    /// line, as the branch does unconditionally (`0x48b840`).  The asset is
    /// mandatory while this is on; `false` is a deliberate text-only frame for
    /// a partial install, not a way to hide a broken asset.
    bool fileslot = true;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level.
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `save_name_render: fade LUT needs ...`.
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
/// `rows` is the fixed 1 of the map (`0x67f10c` is not written by the branch;
/// the single input row is the only band this module paints); `chars` the
/// number of buffer bytes actually drawn (the caret included, at most
/// `kSaveNameMaxBytes`); `truncated` the name bytes dropped by the 17-character
/// limit; `ink_pixels` the canvas pixels covered by at least one glyph pixel
/// (unique, independent of the background); `missing_glyphs` the characters with
/// no glyph in the atlas (skipped, never fatal); `arrows` the number of
/// `MENUARRW` sprites painted — the input line has none, so it is always 0 (the
/// underlying screen-14 frame can paint 0..2 for its scroll, which is not this
/// module's frame); `fileslot_bands` the number of `FILESLOT` bars copied
/// (0 or 1).
struct SaveNameRenderStats {
    int rows = 0;
    int chars = 0;
    int truncated = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
    int fileslot_bands = 0;
};

/// The bytes the original would keep in `0x661280` for *options*: `name`
/// truncated to `kSaveNameMaxChars` and, when `options.caret` is on, the
/// trailing `'-'` of `0x48b6d1` (never beyond `kSaveNameMaxBytes` bytes).  This
/// is exactly the run the renderer draws.
std::string save_name_buffer(const SaveNameRenderOptions& options);

/// `y` of the input line for window row `row` (clamped to 0..5):
/// `kSaveNameFirstRowY + kSaveNameRowStep * row`.
int save_name_text_y(int row);

/// `y` of the `FILESLOT` bar of window row `row` (clamped to 0..5):
/// `kSaveNameFileslotFirstY + kSaveNameRowStep * row`.
int save_name_band_y(int row);

/// Paint screen 15 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The frame is painted in the handler's emission order: the title `SAVE GAME`
/// centred at y 14 in font 5, `ESC TO CANCEL` at (38, 101) in font 0, then the
/// input line of `save_name_buffer(options)` at (42, 28 + 12*row) in font 4,
/// with the `FILESLOT` bar at (38, 27 + 12*row) under it while
/// `options.fileslot` is on.  Every glyph is clipped to the 196x131 text band.
/// With `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.  Returns false +
/// `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_save_name(const std::filesystem::path& game,
                      const SaveNameRenderOptions& options,
                      std::vector<std::uint32_t>& rgba, int& width, int& height,
                      SaveNameRenderStats* stats = nullptr,
                      std::string* error = nullptr);

} // namespace gm
