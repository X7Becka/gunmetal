#pragma once

// One frame of the MULTIPLAYER GAME TYPE screen (19) as a 320x200 RGBA canvas
// (`M8.8` slice 1).
//
// Division of labour
// ------------------
// This module is stateless: the *caller* owns the cursor (`0x68ccf4`) and the
// availability mask (`0x4c1290`), exactly like `ui/start_game_render` owns the
// frame only.  It paints screen 19 with the same building blocks as
// `ui/start_game_render` / `ui/save_menu_render` / `ui/keycfg_render`: the
// `FONTS4` atlases of `FRONTEND/BMP.IDX` (title font 5, rows 0, highlight 4),
// the optional 196x131 `PLATE` of `0x486050` and the 320x200 mode-13h canvas
// (`0x6d8fc0`, docs/UI.md §6.3).  Nothing is decoded or measured twice
// (`render/text_layout` owns the atlases and the advances).
//
// Proven geometry (`0x48a580`, docs/UI_SCREENS.md §2/§3.8)
// --------------------------------------------------------
// The handler is short (from `0x48a580` to the `ret` at `0x48a77d`); every
// number below is one of its sites:
//
//   * `0x48a581` stores `rows 12` into `0x67f10c` and `0x48a58b` sets the step
//     `0x662d58 = 8`; the twelve rows are the eleven game types (items 0..10)
//     plus `Cancel` (item 11);
//   * `0x48a595` calls `0x486050`, which copies the 196x131 `PLATE` into the
//     compose buffer `0x6865f8`; every text call writes into that copy, so
//     196x131 is a hard clip (§3);
//   * the title is `0x485dd0(font=5, text=0x4bdae0 "MULTIPLAYER GAME TYPE",
//     centered=1, x=18, row=10, item=-1)` at `0x48a59a`..`0x48a5ae`;
//     `centered` makes `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`, integer
//     division) and `item == -1` zeroes `item*step`, so the title is centred at
//     y = 10 (`0x485e04`..`0x485e09`, `0x485e4b`..`0x485e5c`);
//   * `One Computer` (`0x4bdad0`) is **unconditional**:
//     `0x485dd0(font=0, centered=0, x=18, row=-18, item=0)` at
//     `0x48a5b1`..`0x48a5c0`; a negative row is the absolute form (`y = -row`,
//     §3), hence y = 18;
//   * the next row starts at `esi = [0x662d58] + 18 = 26` (`0x48a5c5`..
//     `0x48a5cd`).  Each of the eight `test` blocks appends its own
//     `add esi, step` **inside** the taken branch, so `esi` only grows for
//     modes the mask allows: with `k` painted modes the next row is
//     `y = 18 + 8*k` and disabled modes shift the rest up (no blank rows);
//   * the mask at `0x4c1290` is read as a **byte** (`mov al` at `0x48a5d0`,
//     `test BYTE PTR` at `0x48a5fa`/`0x48a646`/...), so only the low eight bits
//     exist.  The test order is 0x02, 0x01, 0x08, 0x04, 0x80, 0x40, 0x20,
//     0x10; three blocks paint two labels each (`Network Server` + `No Render
//     Server`, and `TCP/IP Server` + `No Render Server`), the others one;
//   * `Cancel` (`0x4bda0c`) is always painted last at
//     `0x485dd0(font=0, centered=0, x=18, row=-(18 + 8*k), item=11)`
//     (`0x48a764`..`0x48a774`): not `item -1`, so it takes part in the
//     highlight like any mode;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the text is not empty the face becomes 4.
//     There is no bar, box or `MENULIT` marker on this screen, and no scroll
//     arrow: the full list ends at `Cancel` y = 106 with an 8 px cell.
//   * the navigation helper `0x48ef00` walks `[0x68ccf4]` by ±1 and skips the
//     entries whose bit is clear (it re-tests until the candidate is enabled,
//     `0x48ef1b`..`0x48ef72`); `Cancel` wraps to `One Computer` and moving up
//     from `One Computer` wraps to `Cancel` (`0x48ef2c`, `0x48ef69`).  The
//     cursor is therefore an *item id* (`0..11`), not a visible row number.
//
// Bit mapping of the mask `0x4c1290` (proven at both ends)
// --------------------------------------------------------
// The four protocol types of the network start-up `0x407560`
// (`0x407c23`..`0x407cc9` jump tables `0x407cec`/`0x407cfc`) set one server and
// one client bit per transport; screen 19 tests those same bits:
//
//   bit 0x01 -> `Network Server`, `Network No Render Server`   (server type 1)
//   bit 0x02 -> `Network Client`                               (client type 1)
//   bit 0x04 -> `TCP/IP Server`, `TCP/IP No Render Server`     (server type 2)
//   bit 0x08 -> `TCP/IP Client`                                (client type 2)
//   bit 0x10 -> `Modem Server`                                 (server type 3)
//   bit 0x20 -> `Modem Client`                                 (client type 3)
//   bit 0x40 -> `Serial Cable Server`                          (server type 4)
//   bit 0x80 -> `Serial Cable Client`                          (client type 4)
//
// `One Computer` has no bit (`0x48a5b1`); `Cancel` has none either.  A mask of
// 0 therefore still paints two rows (`One Computer`, `Cancel`) — an empty set
// is not reachable from the shipped handler, and this module does not invent a
// refusal for it (see docs/GAME_TYPE_RENDER.md §5).
//
// Canvas, background and colours
// ------------------------------
// `GameTypeRenderOptions::background` names a `FRONTEND/BMP.IDX` entry painted
// at (0,0) before the text; an empty string means the opaque black canvas.  The
// proven panel of screen 19 is `PLATE` (196x131), but it is never painted
// implicitly, like screens 13/14/17/30.  Glyphs are clipped to the 196x131
// band, the buffer `0x485dd0` writes into.  Colours come from the atlas
// palette: title font 5 -> (155,15,15) dark red, rows font 0 -> (19,15,15) dark
// grey, highlight font 4 -> (239,239,239) white (the `FONTS4` faces,
// docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90` (same layer as `ui/start_game_render`)
// ---------------------------------------------------------------
// `GameTypeRenderOptions::fade_lut` is the exact index -> index path: the table
// is applied to the palette index of each painted pixel **before** the palette
// lookup (`render/hud_lut.h::map`), so it sees the index the engine blitted,
// not the colour it became.  The table either comes pre-loaded
// (`fade_lut_table`) or from a 4096-byte dump (`fade_lut_dump`,
// docs/HUD_LUT.md §2).  Without both the call is a text refusal prefixed
// `game_type_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"game_type_render: "`: an empty game
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
// Keyboard/mouse input, the `updown`/`select` sounds (`docs/UI_SOUNDS.md`), the
// network start-up dialogs behind each entry and the click that turns the
// cursor into a mode.  This module paints one frame; `frontend_menu` and the
// future CLI own the navigation.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kGameTypeCanvasWidth = 320;
inline constexpr int kGameTypeCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kGameTypeBackground = 0x000000FFu;

/// `MULTIPLAYER GAME TYPE` (`0x4bdae0`): the title pushed at `0x48a5a2`.
inline constexpr std::string_view kGameTypeTitle = "MULTIPLAYER GAME TYPE";
/// `Cancel` (`0x4bda0c`), always the last row, item 11.
inline constexpr std::string_view kGameTypeCancel = "Cancel";
/// `One Computer` (`0x4bdad0`), item 0, always painted.
inline constexpr std::string_view kGameTypeOneComputer = "One Computer";
/// The panel of screen 19 (`0x486050`; `menu_panel_asset(Screen::Multiplayer)`).
inline constexpr std::string_view kGameTypePanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kGameTypePanelWidth = 196;
inline constexpr int kGameTypePanelHeight = 131;

/// `0x67f10c = 12` (`0x48a581`): eleven game types (items 0..10) + `Cancel`.
inline constexpr int kGameTypeRows = 12;
/// `0x662d58 = 8` (`0x48a58b`): the row step of the list.
inline constexpr int kGameTypeItemStep = 8;
/// Item ids 0..10 are the modes, 11 is `Cancel` (`push 0xb` at `0x48a766`).
inline constexpr int kGameTypeModeItems = 11;
inline constexpr int kGameTypeCancelItem = 11;
/// The full availability mask: all eight transport bits of `0x4c1290`.
inline constexpr std::uint32_t kGameTypeFullMask = 0xFFu;
/// Only the low byte of `0x4c1290` exists (`test BYTE PTR` at `0x48a5fa`).
inline constexpr std::uint32_t kGameTypeMaskBits = 0xFFu;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kGameTypeTitleFont = 5;
inline constexpr int kGameTypeRowFont = 0;
inline constexpr int kGameTypeSelectedFont = 4;

/// Title row (`push 0xa` at `0x48a59c`), centred in the band; `x = 18` is the
/// `centered` call's fallback argument (`push 0x12` at `0x48a59e`).
inline constexpr int kGameTypeTitleY = 10;
inline constexpr int kGameTypeTitleX = 18;
/// `x = 18` of every row (`push 0x12`), first mode at y = 18.
inline constexpr int kGameTypeItemX = 18;
inline constexpr int kGameTypeFirstRow = 18;

/// The eight mask bits, named as the start-up sets them (docs/DM_RULES.md is
/// about deathmatch rules, not this byte; the proof is `0x407560`).
inline constexpr std::uint32_t kGameTypeBitNetworkServer = 0x01u;
inline constexpr std::uint32_t kGameTypeBitNetworkClient = 0x02u;
inline constexpr std::uint32_t kGameTypeBitTcpServer = 0x04u;
inline constexpr std::uint32_t kGameTypeBitTcpClient = 0x08u;
inline constexpr std::uint32_t kGameTypeBitModemServer = 0x10u;
inline constexpr std::uint32_t kGameTypeBitModemClient = 0x20u;
inline constexpr std::uint32_t kGameTypeBitSerialServer = 0x40u;
inline constexpr std::uint32_t kGameTypeBitSerialClient = 0x80u;

/// One selectable entry of the screen: the handler's `item` id, its label and
/// the mask bit that enables it (`0` = unconditional).
struct GameTypeEntry {
    std::string_view label;
    std::uint32_t bit = 0;
    int item = 0;
};

/// The eleven modes in the order `0x48a5b1`..`0x48a75f` paints them (labels and
/// VAs from `exelib.cstr`, item ids from the `push` immediates).  `bit == 0`
/// marks `One Computer`, which has no `test` in the handler.
const std::vector<GameTypeEntry>& game_type_entries();

/// True when `mask` allows `entry` (`entry.bit == 0` is always allowed).
bool game_type_allowed(const GameTypeEntry& entry, std::uint32_t mask);

/// The rows painted for `mask`, in screen order: the allowed modes followed by
/// `Cancel` (item 11, bit 0).  Always contains `Cancel`.
std::vector<GameTypeEntry> game_type_rows(std::uint32_t mask);

/// Optional parts of one frame.
struct GameTypeRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 19, but it is
    /// never painted implicitly.
    std::string background;
    /// The cursor `[0x68ccf4]`: an **item id** `0..11` (0 = `One Computer`,
    /// 11 = `Cancel`), not a visible row.  The matching row uses font 4 while
    /// it is enabled; a cursor pointing at a mode the mask disabled highlights
    /// nothing (that is the state `0x48ef00` skips over).  Values outside
    /// `0..11` are accepted and highlight nothing — navigation belongs to the
    /// caller.
    int cursor = 0;
    /// The availability byte `0x4c1290`; only the low eight bits are read
    /// (`kGameTypeMaskBits`).  `One Computer` and `Cancel` have no bit, so a
    /// zero mask still paints those two rows.  Allowed modes are painted in
    /// the proven order and disabled ones shift the rest up.
    std::uint32_t mask = kGameTypeFullMask;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/start_game_render`, docs/START_GAME_RENDER.md §6).
    /// Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `game_type_render: fade LUT needs ...`.
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
/// `rows` is the number of text rows painted including `Cancel` (`modes + 1`),
/// `modes` the enabled game types (0..11; 11 with the full mask), `first_row`
/// the y of the first mode (always 18), `cancel_row` the y of `Cancel`
/// (`18 + 8*modes`, 106 with the full mask), `highlight_item` the item that got
/// font 4 (`-1` when the cursor points at a disabled mode or outside 0..11),
/// `ink_pixels` the canvas pixels covered by at least one glyph pixel (unique,
/// independent of the background), `missing_glyphs` the characters with no
/// glyph in the atlas (skipped, never fatal) and `arrows` the number of scroll
/// sprites painted — always 0, screen 19 has none; the field mirrors the
/// sibling stats for uniform CLI output.
struct GameTypeRenderStats {
    int rows = 0;
    int modes = 0;
    int first_row = 0;
    int cancel_row = 0;
    int highlight_item = -1;
    int arrows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
};

/// Paint screen 19 into `rgba` as a 320x200 `0xRRGGBBAA` canvas.
///
/// The title is centred at `kGameTypeTitleY` (font 5); the allowed modes of
/// `options.mask` are painted at (`kGameTypeItemX`, `18 + 8*k`) with font 0,
/// where `k` counts only painted modes, and `Cancel` follows at
/// (`kGameTypeItemX`, `18 + 8*modes`).  The row whose item equals
/// `options.cursor` uses font 4 instead of font 0.  Every glyph is clipped to
/// the 196x131 text band.  With `options.fade_lut` every painted palette index
/// goes through `render/hud_lut.h::map` at the level before its colour is
/// looked up; the untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true
/// with `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_game_type(const std::filesystem::path& game,
                      const GameTypeRenderOptions& options,
                      std::vector<std::uint32_t>& rgba, int& width, int& height,
                      GameTypeRenderStats* stats = nullptr,
                      std::string* error = nullptr);

} // namespace gm
