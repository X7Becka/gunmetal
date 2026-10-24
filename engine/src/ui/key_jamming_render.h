#pragma once

// One frame of screen 16, `KEY JAMMING` (`M8.8` slice 11).
//
// Division of labour
// ------------------
// Stateless, like the other per-screen frame renderers (`ui/match_setup_render`
// 27, `ui/mouse_setup_render` 22, `ui/keycfg_render` 30/34/35,
// `ui/video_mode_render` 37, `ui/restart_render` 6): the caller owns the state
// (the inherited list cursor `0x68ccf4` and the keyboard state array
// `0x4bf2a8`) and this module paints it into the 320x200 mode-13h canvas
// (`0x6d8fc0`, docs/UI.md §6.3).  Nothing here reads the keyboard,
// `GUNMETAL.CFG`, sounds or saves, and nothing is written back.
//
// Proven geometry (`0x488e90`, docs/UI_SCREENS.md §2/§3.8)
// -------------------------------------------------------
// The handler is `push ebx/esi/edi` .. `ret` at `0x488e90`..`0x488f6c`
// (`docs/KEY_JAMMING_RENDER.md` §2).  It writes `rows = 1` (`0x488e93` ->
// `0x67f10c`) and `step = 0` (`0x488e9d` -> `0x662d58`) before copying `PLATE`
// (`0x488ea7` -> `0x486050`, the 196x131 compose buffer `0x6865f8`, so 196x131
// is the hard text clip), then makes four `0x485dd0` calls and one loop:
//
//   text                          VA         font  centered  x   row  y
//   `KEY JAMMING`                 `0x4bd640`  5     yes      38   14   14
//   `Hold down keys to check`     `0x4bd628`  0     yes      38   26   26
//   `if they jam.`                `0x4bd618`  0     yes      38   35   35
//   `Press Esc to close.`         `0x4bd604`  0     yes      38   44   44
//
// All four pass `item = -1` (`0x488eac`/`0x488ec3`/`0x488eda`/`0x488ef1`) and
// `centered = 1` (`0x488eb2`/`0x488ec9`/`0x488ee0`/`0x488ef7`), so `0x485dd0`
// replaces `x` by `(196 - width)/2` (`0x485de2`..`0x485dfa`) and treats `row`
// as an absolute `y` (`item = -1` zeroes the `item*step` term at
// `0x485e04`..`0x485e0b`).  The `x = 38` immediates (`push 0x26`) are the
// unused fallback of the centred branch; they are kept for the listing.
//
// The key list (`0x488f08`..`0x488f67`)
// -------------------------------------
// The loop scans the 123-entry keyboard state array `0x4bf2a8` (the game's own
// key codes 0..122, the index space of the 123 x 6-byte name table `0x4a5b88`,
// `input/controls.h::Controls::key_name`).  For every non-zero byte it paints
// `0x4a5b88 + code*6` (`edi += 6`) with font 0, `centered = 0`, `item = 0`,
// accumulating the drawn count in `esi`; once `esi >= 0x1c = 28` the remaining
// non-zero codes are skipped (`cmp esi,0x1c; jge 0x488f5d` at `0x488f1b`).
// Row and column come from that drawn count `n`:
//
//   row  = 0x36 + 8*(n/4)   (`0x488f20`..`0x488f34`: `esi/4`, `lea ...,[eax*8+0x36]`)
//   x    = 0x1e + 34*(n%4)  (`0x488f35`..`0x488f4e`: `esi%4`, `ecx*17`, `*2 + 0x1e`)
//
// i.e. a 4-column grid: `x` 30/64/98/132, `y` 54/62/70/78/86/94/102; `step = 0`
// makes the row independent of `item`, and `item = 0` matters only for the
// highlight rule below.  Codes are scanned in ascending order, so the grid
// order is the code order, not the press order.
//
// Highlight quirk of `0x485dd0` (`0x485e0d`..`0x485e19`)
// ------------------------------------------------------
// `0x485dd0` swaps the caller's face for font 4 when `item == [0x68ccf4]`
// **and** the caller passed font 0.  The key-name call passes `item = 0` and
// font 0, so a frame whose inherited cursor is 0 paints *every* key name white;
// any other cursor (the real entry value is 4, see below) leaves them dark
// grey.  The four hint/title calls pass `item = -1`, which short-circuits the
// test, so they never highlight.  `options.cursor` models this exactly.
//
// Proven input state and value ranges
// -----------------------------------
// * `0x4bf2a8` is the keyboard "key is down" array: the DirectInput poll
//   `0x405450` maps a device key through `0x405790` and writes
//   `BYTE [0x4bf2a8 + code] = 1` on press (`0x4054eb`, `0x4055de`) and `0` on
//   release (`0x40550a`, `0x4055c3`); the init `0x404b00` clears the first
//   `0x80` bytes (`0x404b14`).  Screen 16 is the only reader of the array in the
//   front end; docs/INIT.md §8.  `options.pressed_keys` is that
//   array as a list of codes (duplicates and codes outside 0..122 are ignored,
//   the order is ascending like the loop).
// * `[0x68ccf4]` is **not** written by the transition into 16
//   (`0x48d1ce`..`0x48d1d9` only sets `[0x660788]` and `[0x4bc440] = 0x10`), so
//   the screen inherits the list cursor of screen 10, where `Keyjamming` is
//   item 4 (`docs/UI_SCREENS.md` §3.1); `kKeyJammingEntryCursor` is therefore 4
//   and the default options value.
// * There is **no `%s` substitution** on this screen: the dynamic content is
//   the list of held key names, taken verbatim from the 6-byte table.  There is
//   no "jamming" computation anywhere in the handler either — it is a passive
//   readout of which keys the keyboard reports, i.e. the player watches how
//   many simultaneous keys survive.  Hence no `key_name` option.
// * `rows = 1` is the handler's own `0x67f10c` write; the screen has no items,
//   no cursor of its own, no `CHECK`/`NOCHECK`, no `SLIDE`/`KNOB` and no
//   `MENUARRW` call, so `stats.arrows` is always 0.
//
// Assets, background, colours
// ---------------------------
// `FRONTEND/BMP.IDX` supplies `PLATE` (docs/UI_SCREENS.md §9) plus the `FONTS4`
// faces 5 (title) and 0 (hints and key names); face 4 is loaded only because
// the highlight quirk above can select it.  `PLATE` is loaded and validated
// because `0x488ea7` copies it as the frame buffer; the frame itself is painted
// on opaque black unless `background` names an entry (`"PLATE"` reproduces the
// panel), because where the engine blits the 196x131 compose buffer on the
// 320x200 canvas is not documented (docs/MENU_RENDER.md §2).
//
// Colours come from the atlas palette: title font 5 -> (155,15,15) dark red,
// hints and key names font 0 -> (19,15,15) dark grey, font 4 ->
// (239,239,239) white (docs/MENU_RENDER.md §3).  `PLATE` uses its own BMP
// palette.
//
// Exact fade of `0x682f90`
// ------------------------
// The same index -> index path as `ui/match_setup_render`: every painted
// pixel's palette index goes through `render/hud_lut.h::map(level, index)`
// **before** its colour is looked up.  The table comes pre-loaded
// (`fade_lut_table`) or from a 4096-byte dump (`fade_lut_dump`,
// docs/HUD_LUT.md §2).  With neither, the call is a text refusal prefixed
// `key_jamming_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"key_jamming_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX`, a missing face 5/0/4, a
// missing/broken/not-196x131 `PLATE`, a background entry that is
// missing/broken/not 8 bpp, and an exact fade without a table and a dump (or
// with an unreadable dump).  Out-of-range *values* (a negative cursor, a key
// code of -1 or 999) are ignored, not refused: the original only ever indexes
// the 123-entry table (`0x4a5b88`) and the state array in range, and the screen
// stays paintable.  On failure `rgba` is cleared, `width`/`height` are 0 (never
// a stale or half-drawn picture) and `*stats` is zeroed.  `stats`/`error` may
// be null.  The call only reads the installation, is deterministic and does
// not depend on the current working directory.
//
// What is not drawn / not done
// ----------------------------
// Input (the keyboard poll `0x405450`, `Esc` -> state 10 at `0x48e599`), UI
// sounds, the "jamming" test itself (there is none in the binary: the screen
// only lists the keys the poll reports) and the CLI.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kKeyJammingCanvasWidth = 320;
inline constexpr int kKeyJammingCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kKeyJammingBackground = 0x000000FFu;

/// `0x4bd640`, pushed at `0x488eb4` (font 5, centred, y 14).
inline constexpr std::string_view kKeyJammingTitle = "KEY JAMMING";
/// `0x4bd628`, pushed at `0x488ecb` (font 0, centred, y 26).
inline constexpr std::string_view kKeyJammingHintHold = "Hold down keys to check";
/// `0x4bd618`, pushed at `0x488ee2` (font 0, centred, y 35).
inline constexpr std::string_view kKeyJammingHintJam = "if they jam.";
/// `0x4bd604`, pushed at `0x488ef9` (font 0, centred, y 44).
inline constexpr std::string_view kKeyJammingHintEsc = "Press Esc to close.";

/// `PLATE` 196x131 (docs/UI_SCREENS.md §9); `0x488ea7` copies it into
/// `0x6865f8` (`0x486050`), so this is also the text clip.
inline constexpr std::string_view kKeyJammingPanelName = "PLATE";
inline constexpr int kKeyJammingPanelWidth = 196;
inline constexpr int kKeyJammingPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, hints and key names 0, the highlight face
/// 4 of `0x485dd0` (only reachable through the `item = 0` quirk).
inline constexpr int kKeyJammingTitleFont = 5;
inline constexpr int kKeyJammingRowFont = 0;
inline constexpr int kKeyJammingSelectedFont = 4;

/// `0x67f10c = 1` (`0x488e93`); the screen has a single "row" and no items.
inline constexpr int kKeyJammingRows = 1;
/// `0x662d58 = 0` (`0x488e9d`): no list step.
inline constexpr int kKeyJammingItemStep = 0;

/// Title: `push 0xe` at `0x488eae` (row 14, and `item = -1` makes it `y`),
/// `push 0x26` at `0x488eb0` (x 38, ignored by `centered`).
inline constexpr int kKeyJammingTitleY = 14;
inline constexpr int kKeyJammingTitleX = 38;
/// The three hint rows: `push 0x1a` (`0x488ec5`), `push 0x23` (`0x488edc`),
/// `push 0x2c` (`0x488ef3`); `x = 38` on each call, ignored by `centered`.
inline constexpr int kKeyJammingHintHoldY = 26;
inline constexpr int kKeyJammingHintJamY = 35;
inline constexpr int kKeyJammingHintEscY = 44;
inline constexpr int kKeyJammingHintX = 38;

/// The key grid of `0x488f20`..`0x488f4e`: four columns of `x = 30 + 34*(n%4)`
/// and rows of `y = 54 + 8*(n/4)`, at most `0x1c = 28` names (`0x488f1b`).
inline constexpr int kKeyJammingKeyColumns = 4;
inline constexpr int kKeyJammingKeyRows = 7;
inline constexpr int kKeyJammingMaxKeys = 28;
inline constexpr int kKeyJammingKeyColumnPitch = 34;
inline constexpr int kKeyJammingKeyRowPitch = 8;
inline constexpr int kKeyJammingKeyX = 30;
inline constexpr int kKeyJammingKeyY = 54;

/// The game's own key codes: the 123 entries of the 6-byte name table
/// `0x4a5b88` (ending at `0x4a5e6a`), i.e. the range the loop scans in
/// `0x4bf2a8` and `Controls::key_name` accepts.
inline constexpr int kKeyJammingKeyNameCount = 123;

/// `[0x68ccf4]` is inherited from screen 10 (the transition `0x48d1ce` does not
/// write it) where `Keyjamming` is item 4, so 4 is both the entry value and the
/// options default.  `0` is *not* a neutral default here: `0x485dd0` turns every
/// key name white when the cursor equals the `item = 0` argument.
inline constexpr int kKeyJammingEntryCursor = 4;
inline constexpr int kKeyJammingDefaultCursor = kKeyJammingEntryCursor;

/// Optional parts of one frame.
struct KeyJammingRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 16 and is painted
    /// from the copy that is validated anyway; it is never painted implicitly
    /// (see the header comment).
    std::string background;

    /// The inherited list cursor `[0x68ccf4]`.  The key-name calls pass
    /// `item = 0` and font 0, so `0x485e0d`..`0x485e19` repaints **all** key
    /// names with font 4 (white) when this is 0; any other value (the entry
    /// value 4 included, and any out-of-range value) leaves them font 0.
    /// The title and the hints pass `item = -1` and never highlight.
    int cursor = kKeyJammingDefaultCursor;

    /// The keyboard state array `0x4bf2a8` as the codes that are down: the
    /// handler scans `0..122` ascending and paints the name of every non-zero
    /// byte (`Controls::key_name`), at most 28 of them.  Duplicates and codes
    /// outside `0..122` are ignored; the order of this vector does not matter
    /// (the original is a byte array, not a press log).
    std::vector<int> pressed_keys;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/match_setup_render`, docs/KEY_JAMMING_RENDER.md §4).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `key_jamming_render: fade LUT needs ...`.
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
/// `rows` is the fixed `0x67f10c` value 1; `keys` how many key names the frame
/// carries (`0..28`, the `esi` counter of `0x488f1b`); `ink_pixels` the canvas
/// pixels covered by at least one glyph pixel (unique; the optional backdrop
/// panel is not counted, the same rule as `ui/menu_render`/`ui/match_setup_render`);
/// `missing_glyphs` the characters with no glyph in the atlas (skipped, never
/// fatal); `arrows` the number of `MENUARRW` sprites painted — screen 16 has no
/// arrow call at `0x488e90`, so it is always 0 and the test pins the 0.
struct KeyJammingRenderStats {
    int rows = 0;
    int keys = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
    int arrows = 0;
};

/// `x` of grid slot `slot` 0..27: `kKeyJammingKeyX + 34*(slot%4)`; a slot
/// outside the range is clamped to the last one (the original never gets
/// there, `0x488f1b`).
int key_jamming_key_x(int slot);

/// `y` of grid slot `slot` 0..27: `kKeyJammingKeyY + 8*(slot/4)`, clamped like
/// `key_jamming_key_x`.
int key_jamming_key_y(int slot);

/// The codes the handler would actually paint for `pressed_keys`: duplicates
/// and codes outside `0..122` dropped, ascending, at most
/// `kKeyJammingMaxKeys` (the `0x4bf2a8` scan of `0x488f11`..`0x488f67`).
std::vector<int> key_jamming_keys(const std::vector<int>& pressed_keys);

/// Font of the key names for a frame with cursor `cursor`: 4 when it is 0
/// (`item = 0` of the call equals `[0x68ccf4]`, `0x485e15`/`0x485e19`), else 0.
int key_jamming_key_font(int cursor);

/// Paint screen 16 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title `KEY JAMMING` is centred at `kKeyJammingTitleY` in font 5, the
/// three hints are centred at `kKeyJammingHintHoldY`/`kKeyJammingHintJamY`/
/// `kKeyJammingHintEscY` in font 0, and the key names of
/// `key_jamming_keys(options.pressed_keys)` are painted in font
/// `key_jamming_key_font(options.cursor)` at
/// (`key_jamming_key_x(n)`, `key_jamming_key_y(n)`) for slot `n`, in the
/// handler's order (ascending code).  Every glyph is clipped to the 196x131
/// text band.  With `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true with
/// `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_key_jamming(const std::filesystem::path& game,
                        const KeyJammingRenderOptions& options,
                        std::vector<std::uint32_t>& rgba, int& width, int& height,
                        KeyJammingRenderStats* stats = nullptr,
                        std::string* error = nullptr);

} // namespace gm
