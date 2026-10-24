#pragma once

// One frame of screen 18, `SELECT VIDEO MODE` (`M8.8` slice 7).
//
// Division of labour
// ------------------
// This module is stateless: it paints the 320x200 mode-13h canvas (`0x6d8fc0`,
// docs/UI.md §6.3) from the options alone, exactly like `ui/level_select_render`
// (17), `ui/video_mode_render` (37) and `ui/game_type_render` (19).  The cursor
// (`0x68ccf4`) and the navigation belong to the caller; the building blocks are
// the `FONTS4` atlases of `FRONTEND/BMP.IDX` (title face 5, rows face 0,
// highlight face 4), the 196x131 `PLATE` copied by `0x486050`, the `MENUARRW`
// 16x8 scroll sprite and the exact fade tables of `0x682f90`.  Nothing is
// decoded or measured twice: `render/text_layout` owns the atlases and the
// advances.
//
// One thing is *not* provable from the installation: the list itself.
//
// Proven geometry (`0x48a8b0`, docs/UI_SCREENS.md §2/§3.8)
// -------------------------------------------------------
// The handler runs from `0x48a8b0` to the `ret` at `0x48a999` (then `nop` up to
// `0x48a9a0`, where screen 23 starts); the dispatcher table `0x4883d8` reaches
// it through the stub `0x487f1d` (slot `0x48840c`) -> `call 0x48a8b0;
// jmp 0x487f64`.  It is the same shape as screen 17, with a 9 px step:
//
//   * `0x48a8b0` reads the mode count `[0x4c05bc]` into `[0x67f10c]` (the
//     `rows` global of `0x485dd0`) and `0x48a8bd` stores the step
//     `[0x662d58] = 9`;
//   * `0x48a8ca` calls `0x486050`, which copies the 196x131 `PLATE` into the
//     compose buffer `0x6865f8`.  Every text and arrow call below writes into
//     that copy, so 196x131 is a hard clip (§3);
//   * the title is `0x485dd0(font=5, text=0x4bdb94 "SELECT VIDEO MODE",
//     centered=1, x=38, row=11, item=-1)` (`0x48a8cf`..`0x48a8de`).  `centered`
//     makes `x = (196 - width) / 2` (`0x485de2`..`0x485dfa`, integer division)
//     and `item == -1` zeroes `item*step`, so the title sits at y = 11;
//   * the second line is `0x485dd0(font=0, text=0x4bd7c8 "Press Esc to close",
//     centered=1, x=25, row=20, item=-1)` (`0x48a8e6`..`0x48a8f5`), i.e. centred
//     at y = 20.  It is the only non-row text of the screen;
//   * the window start is `0x48a8fa`..`0x48a921`: `first = cursor >= 5 ?
//     cursor - 5 : 0`, then `if (first + 10 >= count) first = count - 10`, then
//     `if (first < 0) first = 0`.  That is `video_mode_list_first_row()` below:
//     the cursor stays on the sixth visible band while scrolling down and the
//     window stops at the bottom.  The original does **not** clamp the cursor
//     itself, only the resulting window;
//   * the scroll arrows are `MENUARRW`, the 16x8 index sprite loaded into
//     `0x67fcb0` (`0x48fe59` -> `0x47efe0`, `push 8; push 0x10; push 0x67fcb0`):
//     `0x48a923`..`0x48a939` blits it **up** with `0x485d00` when `first > 0`
//     and `0x48a93c`..`0x48a952` blits it **down** with `0x485cc0` when
//     `first + 10 < count`.  Both calls pass the destination `0x68681f` /
//     `0x68c33b` and the stride `0xc4` (196); inside the `PLATE` copy those are
//     `2*196 + 159` and `121*196 + 159`, i.e. column 159, rows 2 and 121.
//     `0x485cc0` reads the sprite forward and `0x485d00` reads the same 128
//     bytes backwards (`mov al,[edx-1]; dec edx` at `0x485d1a`), so the up
//     arrow is the down arrow rotated by 180 degrees — one asset, two
//     directions;
//   * the row loop `0x48a955`..`0x48a991` draws ten bands, `i = 0..9`:
//     `item = first + i`, the text is `0x486500(&buf, item)` and the call is
//     `0x485dd0(font=0, text=buf, centered=0, x=38, row=-(29 + 9*i),
//     item=first+i)`.  A **negative** row is the absolute form of the item
//     renderer (`0x485e27`..`0x485e48`: the destination is `x - row*196`, so
//     `y = -row`), hence the bands start at y = 29 with a 9 px step and end at
//     y = 110 (cell 8 px, so the last band covers 110..117 of the 131 px
//     window).  `item` is used only for the highlight in that branch;
//   * the highlight is the font switch of `0x485dd0` (`0x485e0d`..`0x485e19`):
//     when `item == [0x68ccf4]` and the caller passed font 0 the face becomes
//     4.  There is no bar, box or `MENULIT` marker; the comparison is exact, so
//     a cursor outside the list highlights nothing;
//   * the entry from GRAPHICS OPTIONS is `0x48d74d`..`0x48d784`: item 5 of
//     screen 12 copies the current 5-dword mode descriptor `0x4c05c0`, writes
//     state `0x12`, then `[0x68ccf4] = 0x4864a0(descriptor)` — the index of the
//     current mode in the table, or 0 when it is not there — and calls
//     `0x48a8b0` for an immediate redraw.  `ESC` leaves to state 12
//     (`0x48e55a`), `Enter` applies the mode and opens screen 37 (`0x48e182`,
//     docs/VIDEO_MODE_RENDER.md §1.2).
//
// The texts of the rows (`0x486500`)
// ----------------------------------
// `0x486500(buf, index)` indexes the mode table at `0x4bfd18 + index*20`
// (`width`, `height`, `bpp`, `refresh`, `flags`) and formats it with
// `sprintf`-style templates; the flag/bpp chain at `0x486510`..`0x4866f7`
// selects one of twelve strings.  `video_mode_list_label()` implements exactly
// that chain (see the .cpp); it is proven code, only the *table* is a model.
//
// Model: which modes are in the list
// ----------------------------------
// The table is **built at runtime** and cannot be reproduced from the shipped
// files: `0x405fc0` calls `DirectDraw::EnumDisplayModes(3, ...)` with the
// callback `0x4065a0`, which appends every mode of 320..800 x 200..600
// (halved height, flags `0x100`, docs/INIT.md §6.1), and `0x406083` appends the
// Glide set `512x384 / 640x480 / 800x600` (flags `0x804`, bpp 32) when
// `[0x4a6170] & 0x800` is set; `0x406189`..`0x4061f6` then bubble-sorts the
// table **descending by width, then height, then bpp**.  The count goes to
// `[0x4c05bc]` and is capped at 100 (`0x4065f9`).
//
// `kVideoModeListModes` is therefore a *model*: a typical Voodoo-era
// `EnumDisplayModes(3)` set (the standard VGA modes plus the usual SVGA ones,
// 8 and 16 bpp) followed by the documented Glide trio, sorted by the proven
// rule.  Every entry is labelled by the proven formatter, and the order, the
// window, the arrows and the highlight are proven; only *which* modes a given
// machine reports is not.  docs/VIDEO_MODE_LIST_RENDER.md §2 lists what is left.
//
// Canvas, background and colours
// ------------------------------
// `VideoModeListRenderOptions::background` names a `FRONTEND/BMP.IDX` entry
// painted at (0,0) before the text; an empty string means the opaque black
// canvas.  The proven panel of screen 18 is `PLATE` (196x131,
// `menu_panel_asset(Screen::VideoMode)`), but it is never painted implicitly.
// Glyphs and arrows are clipped to the 196x131 band `0x485dd0` writes into.
// Colours come from the atlases: title font 5 -> (155,15,15) dark red, rows
// font 0 -> (19,15,15) dark grey, highlight font 4 -> (239,239,239) white, and
// the arrows keep the palette of `MENUARRW` (docs/MENU_RENDER.md §3).
//
// Exact fade of `0x682f90`
// ------------------------
// `fade_lut` is the same exact index -> index path as `ui/level_select_render`:
// the table is applied to the palette index of each painted pixel (background,
// arrow, glyph) **before** the palette lookup (`render/hud_lut.h::map`).  The
// table either comes pre-loaded (`fade_lut_table`) or from a 4096-byte dump
// (`fade_lut_dump`, docs/HUD_LUT.md §2).  Without both the call is a text
// refusal prefixed `video_mode_list_render:`, never a silently unfaded frame.
//
// Errors and buffer contract
// --------------------------
// Every refusal is a text prefixed `"video_mode_list_render: "`: an empty game
// directory, a missing/broken `FRONTEND/BMP.IDX` or `FONTS4` atlas, a
// background entry that is missing/broken/not 8 bpp, a missing/broken
// `MENUARRW` (only when an arrow is painted) and an exact fade without a table
// and a dump (or with an unreadable/wrong-sized dump).  On failure `rgba` is
// cleared, `width`/`height` are 0 (never a stale or half-drawn picture) and
// `*stats` is zeroed.  `stats`/`error` may be null.  The call only reads the
// installation, is deterministic and does not depend on the current working
// directory.
//
// What is not drawn
// -----------------
// Keyboard/mouse input, the `updown`/`select` sounds (`docs/UI_SOUNDS.md`), the
// mode switch itself (`0x48ed10`), the confirmation of screen 37 and the
// transitions to states 12/37.  The list content is the model above, not the
// driver's answer.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_lut.h"

namespace gm {

/// The 320x200 mode-13h front-end canvas (`0x6d8fc0`, docs/UI.md §6.3).
inline constexpr int kVideoModeListCanvasWidth = 320;
inline constexpr int kVideoModeListCanvasHeight = 200;

/// Opaque `#000`: the canvas before the optional background and any glyph.
inline constexpr std::uint32_t kVideoModeListBackground = 0x000000FFu;

/// `SELECT VIDEO MODE` (`0x4bdb94`), the title pushed at `0x48a8d7`.
inline constexpr std::string_view kVideoModeListTitle = "SELECT VIDEO MODE";
/// `Press Esc to close` (`0x4bd7c8`), pushed at `0x48a8ee`.
inline constexpr std::string_view kVideoModeListHint = "Press Esc to close";

/// The panel of screen 18 (`0x486050`; `menu_panel_asset(Screen::VideoMode)`).
inline constexpr std::string_view kVideoModeListPanelName = "PLATE";

/// `PLATE` is 196x131 (docs/UI_SCREENS.md §9); the text band is that same
/// rectangle, because `0x485dd0` writes into a copy of it.
inline constexpr int kVideoModeListPanelWidth = 196;
inline constexpr int kVideoModeListPanelHeight = 131;

/// docs/UI_SCREENS.md §3: title 5, rows 0, highlight 4 (three `FONTS4` faces).
inline constexpr int kVideoModeListTitleFont = 5;
inline constexpr int kVideoModeListRowFont = 0;
inline constexpr int kVideoModeListSelectedFont = 4;

/// Title row (`push 0xb` at `0x48a8d1`), centred in the band; `x = 38`
/// (`push 0x26`) is the `centered` call's unused fallback argument.
inline constexpr int kVideoModeListTitleY = 11;
inline constexpr int kVideoModeListTitleX = 38;
/// `Press Esc to close`, also centred (`push 0x14` at `0x48a8e8`).
inline constexpr int kVideoModeListHintY = 20;
inline constexpr int kVideoModeListHintX = 25;

/// `x = 38` of every row (`push 0x26` at `0x48a97e`), first band at y 29
/// (`mov edx, 0xffffffe3` at `0x48a971`), step 9 (`[0x662d58]`, `0x48a8bd`).
inline constexpr int kVideoModeListRowX = 38;
inline constexpr int kVideoModeListFirstRowY = 29;
inline constexpr int kVideoModeListRowStep = 9;

/// `0x48a98e`: the row loop runs exactly ten times.
inline constexpr int kVideoModeListVisibleRows = 10;

/// Scroll arrows: the `MENUARRW` entry of `FRONTEND/BMP.IDX` (`0x4be2fc`,
/// loaded into `0x67fcb0` by `0x48fe59`), an 8 bpp 16x8 index sprite.
inline constexpr std::string_view kVideoModeListArrowName = "MENUARRW";
inline constexpr int kVideoModeListArrowWidth = 16;
inline constexpr int kVideoModeListArrowHeight = 8;
/// Anchor inside the 196x131 plate copy `0x6865f8`: `0x68681f` is
/// `2*196 + 159` and `0x68c33b` is `121*196 + 159` (both blits pass the 0xc4
/// stride), i.e. column 159, rows 2 (up arrow) and 121 (down arrow).
inline constexpr int kVideoModeListArrowX = 159;
inline constexpr int kVideoModeListArrowUpY = 2;
inline constexpr int kVideoModeListArrowDownY = 121;

/// One entry of the mode table `0x4bfd18` as `0x486500` reads it: `width`,
/// `height`, `bpp` (`+0x08`) and the flag word (`+0x10`).  `refresh`
/// (`+0x0c`) is never used by the formatter and is not modelled.
struct VideoModeListEntry {
    int width = 0;
    int height = 0;
    int bpp = 0;
    std::uint32_t flags = 0;
};

/// Number of entries of the **model** list (see the header comment).  This is
/// what `[0x67f10c]` holds in the original: the driver-dependent count
/// `[0x4c05bc]`, capped at 100.
inline constexpr int kVideoModeListModeCount = 15;

/// The model list: the typical `EnumDisplayModes(3)` set of the era (standard
/// VGA + common SVGA modes, 8 and 16 bpp, halved height and flags `0x100`, as
/// `0x4065f0` stores them) followed by the documented Glide trio
/// (docs/INIT.md §6.1: `512x384 / 640x480 / 800x600`, flags `0x804`, bpp 32),
/// sorted by the proven descending `(width, height, bpp)` key of
/// `0x406189`..`0x4061f6`.  Only the membership is a model; every label comes
/// from `video_mode_list_label()`, which is `0x486500` itself.
inline constexpr VideoModeListEntry kVideoModeListModes[kVideoModeListModeCount] = {
    {800, 600, 32, 0x804}, // 800x600 Glide 3Dfx
    {800, 300, 16, 0x100}, // 800x300 64k Skip
    {800, 300, 8, 0x100},  // 800x300 256 Skip
    {640, 480, 32, 0x804}, // 640x480 Glide 3Dfx
    {640, 240, 16, 0x100}, // 640x240 64k Skip
    {640, 240, 8, 0x100},  // 640x240 256 Skip
    {640, 200, 16, 0x100}, // 640x200 64k Skip
    {640, 200, 8, 0x100},  // 640x200 256 Skip
    {512, 384, 32, 0x804}, // 512x384 Glide 3Dfx
    {360, 240, 16, 0x100}, // 360x240 64k Skip
    {360, 240, 8, 0x100},  // 360x240 256 Skip
    {360, 200, 16, 0x100}, // 360x200 64k Skip
    {360, 200, 8, 0x100},  // 360x200 256 Skip
    {320, 200, 16, 0x100}, // 320x200 64k Skip
    {320, 200, 8, 0x100},  // 320x200 256 Skip
};

/// Optional parts of one frame.
struct VideoModeListRenderOptions {
    /// `FRONTEND/BMP.IDX` entry painted at (0,0) before the text; empty =
    /// opaque black.  `"PLATE"` is the proven panel of screen 18, but it is
    /// never painted implicitly.
    std::string background;
    /// The cursor `[0x68ccf4]`: an **index into the mode list** `0..14` (the
    /// original stores the index of the current mode, `0x48d77a`).  The item
    /// equal to it uses font 4; the comparison is the exact one of `0x485e0d`,
    /// so a value outside the list highlights nothing (no clamp).
    int cursor = 0;

    /// Apply the **exact** fade tables of `0x682f90` at the palette-index level
    /// (same layer as `ui/level_select_render`, docs/LEVEL_SELECT_RENDER.md
    /// §4b).  Needs a table, either pre-loaded (`fade_lut_table`) or read from
    /// `fade_lut_dump`; without both the call fails with
    /// `video_mode_list_render: fade LUT needs ...`.
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
/// `rows` is the number of entries of the list (`[0x67f10c]`, the model's
/// `kVideoModeListModeCount`), `first_row` the first visible entry (`0..rows-10`,
/// `0x48a8fa`..`0x48a921`), `arrows` how many `MENUARRW` sprites the frame
/// carries (`0..2`: up when `first_row > 0`, down when
/// `first_row + 10 < rows`), `ink_pixels` the canvas pixels covered by at least
/// one glyph pixel (unique, independent of the background; arrow pixels are not
/// glyph ink) and `missing_glyphs` the characters with no glyph in the atlas
/// (skipped, never fatal).
struct VideoModeListRenderStats {
    int rows = 0;
    int first_row = 0;
    int arrows = 0;
    int ink_pixels = 0;
    int missing_glyphs = 0;
};

/// Label of one mode table entry: the proven `0x486500` chain.
///
/// The flags are tested in this order (`0x486510`..`0x4865e6`): `0x200` ->
/// `"<w>x<h> Direct 3D"`, `0x400` -> `" Open GL"`, `0x800` -> `" Glide 3Dfx"`,
/// `0x002` -> `" TV Mode"`, `0x001` -> `" 256 Mode X"` / `" 256 Skip X"`
/// (`0x100`).  Without those, `0x100` selects the "Skip" family by bpp
/// (`8` -> `" 256 Skip"`, `15` -> `" 32k Skip"`, `16` -> `" 64k Skip"`, else
/// `" Skip"`) and the plain family by bpp (`8` -> `" 256 Colours"`, `15` ->
/// `" 32k Colours"`, `16` -> `" 64k Colours"`, else the bare `"<w>x<h>"`).
std::string video_mode_list_label(const VideoModeListEntry& entry);

/// Window start of the ten-band list (`0x48a8fa`..`0x48a921`), proven:
/// `first = cursor >= 5 ? cursor - 5 : 0`; when `first + 10 >= count` the
/// window is clamped to `count - 10`; the result is never negative.
///
/// *count* is the number of list entries, *cursor* the value of `[0x68ccf4]`
/// (the original does not clamp it).  A list of at most ten entries always
/// starts at 0.
int video_mode_list_first_row(int count, int cursor);

/// Paint screen 18 of the model list into `rgba` as a 320x200 `0xRRGGBBAA`
/// canvas.
///
/// The title is centred at `kVideoModeListTitleY` (font 5), `Press Esc to
/// close` at `kVideoModeListHintY` (font 0), both centred in the 196 px band.
/// Band `i` of the window shows entry `first_row + i` at
/// (`kVideoModeListRowX`, `kVideoModeListFirstRowY + kVideoModeListRowStep * i`)
/// with font 0, or font 4 when its index equals `options.cursor`.  Up/down
/// `MENUARRW` arrows are painted at (159, 2) and (159, 121) when the window has
/// rows above/below it.  Every glyph is clipped to the 196x131 text band.  With
/// `options.fade_lut` every painted palette index goes through
/// `render/hud_lut.h::map` at the level before its colour is looked up; the
/// untouched black canvas has no index and stays black.
/// Returns false + `*error` on each refusal of the header comment, and true
/// with `width == 320`, `height == 200` and `rgba.size() == 64000` otherwise.
bool render_video_mode_list(const std::filesystem::path& game,
                            const VideoModeListRenderOptions& options,
                            std::vector<std::uint32_t>& rgba, int& width, int& height,
                            VideoModeListRenderStats* stats = nullptr,
                            std::string* error = nullptr);

} // namespace gm
