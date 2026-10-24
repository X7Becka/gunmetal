#pragma once

// Cockpit frame composition (`M4.4.5` slice 3): PANEL + icons + numbers + tape.
//
// One call builds the whole 320x200 cockpit frame of the shipped `level0`
// cockpit: `PANEL` from `FRONTEND/BMP.IDX`, the two 24x24 icons, the text and
// digit layer of `0x4926a0`, the gauge fills and — optionally — the heading
// tape.  Nothing else is drawn, so the caller can compose the result over a
// world frame (`render_scene`) later; this module neither reads a world frame
// nor knows about `main.cpp`.
//
// Layers and order (the order of `0x491a70`/`0x4926a0`, docs/HUD_AUDIT.md §2.9)
// --------------------------------------------------------------------------
//   1. `PANEL` 320x78 at (0,122), index 0 is the transparent key, rows in the
//      engine order (`flipV(parse_bmp)`, docs/HUD_AUDIT.md §2.7);
//   2. icons 24x24: weapon `WICONn`/`WCONn` from the record (drawn top-down) at
//      (23,162), equipment `equip<n>` (drawn bottom-up) at (201,162);
//   3. gauge fills: throttle from (202,155), 4 rows, <= 53 px, `0xf3`; damage
//      hexagon bars at (148,151)/(148,177), 3 rows, <= 17 px, colour by length
//      (`0xf3` < 6, `0x2b` < 12, `0xff` from 12);
//   4. text, drawn **after** the fills with raw palette indices (no fade):
//      labels in font 6 (`FONTS3`) and digits in fonts 9/10/11 (`NUMFONTS`);
//   5. optional compass ribbon (model, see below).
//
// Coordinates: the code constants of `0x47faf0` are *buffer* rows and the HUD
// layer of the reference frames (`case 3` of `DAT_004bc1c8`, `0x491f81`)
// offsets the buffer by 8 rows (docs/HUD_AUDIT.md §2.9), so every text/icon
// position below is the documented constant minus 8, which is what `win1..win3`
// show.  The panel row (122) is the `default` layout the task asks for; the
// frames use 130 (docs/COCKPIT.md §4.6) - the panel is therefore checked
// against an independent blit, not against a frame.
//
// Proven vs model
// ---------------
// Proven (xref + frames, docs/HUD_AUDIT.md §2.7-2.9, docs/HUD_ICONS.md §3-4):
// panel asset/order/key, all label and digit positions, font numbers and
// metrics, icon assets and the row direction of both icon blits, the throttle
// geometry, the damage-bar geometry/clamp/colour rule.  The text palette is the
// engine `GAMEPAL` table (`FRONTEND/GAMEPAL.BMP`, `0x83d320`): the documented
// index check is 965/965 text pixels and 576/576+576/576 icon pixels.
//
// Model (documented as such, docs/COCKPIT.md §4):
//   * the damage value on the digits is derived from `armor`/`armor_max`
//     (`100 - 100*armor/armor_max`, proven formula) but the *hexagon bars* in
//     the code read `charge_accumulated (+0x2FA8)` (hypothesis, empty in the
//     frames), so driving them from the damage percentage is a choice;
//   * `CockpitState::speed` is a 0..1 throttle fraction; the code reads
//     `engine_power (+0x2F48)` and clamps to 53 px, the normalisation is ours;
//   * the compass is the empirical tape of docs/HUD_ICONS.md §5 (window
//     `x 79..129`, `y 2..15`, `x = 102.62 + 0.2940*(bearing - yaw)`), the
//     drawing code was never found; marks every 22.5 deg, long marks/letters on
//     the cardinals and the ink index (191, `(0,255,0)`) are a model.
//
// Missing assets are not fatal: an absent icon or font is counted in
// `CockpitStats::missing_assets` and the rest of the frame is still built.
// A missing `FRONTEND/BMP.IDX`, `PANEL`, a malformed palette or a PANEL that is
// not 320x78 *is* fatal (text in `*error`) - the frame cannot be called a
// cockpit frame then.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gm {

/// The shipped cockpit frame (mode 13h buffer, docs/HUD_AUDIT.md §1).
inline constexpr int kCockpitWidth = 320;
inline constexpr int kCockpitHeight = 200;

/// `PANEL` placement: 320x78 at (0,122) - the `default` layout of
/// docs/HUD_AUDIT.md §2.1/§2.9 (`200 - 78`, four players).  The reference
/// frames `win2`/`win3` use `case 3` (panel row 130, 70 rows visible); this
/// slice follows the task's 320x78 at (0,122) and compares against an
/// independent blit, not against the frames (docs/COCKPIT.md §4.6).
inline constexpr int kCockpitPanelX = 0;
inline constexpr int kCockpitPanelY = 122;
inline constexpr int kCockpitPanelWidth = 320;
inline constexpr int kCockpitPanelHeight = 78;

/// The player state the HUD reads.  Values are **for drawing only**: this
/// module owns no simulation and writes nothing into `state`.
///
/// The first six fields are the contract of the task (`M4.4.5` slice 3);
/// `equipment_id` and `yaw_deg` are additions the cockpit needs and no other
/// field provided: the equipment icon (`equip<n>`) and the heading tape.
struct CockpitState {
    int weapon_id = -1;   ///< `DEFINE_WEAPONS` slot, 0..18 = level0 order
    int ammo = 0;         ///< rounds of the current weapon
    int armor = 0;        ///< armour in hundredths (`player+0x3248`)
    int armor_max = 10000; ///< full armour in hundredths (`player+0x3244`)
    int kills = 0;        ///< kill counter (`player+0x3270`)
    double speed = 0.0;   ///< throttle fraction 0..1 (see header comment)
    int equipment_id = 0; ///< equipment slot 0..12 (`player+0x3320`)
    double yaw_deg = 0.0; ///< heading for the tape (`player+0x0028`)
};

/// Which optional layers to draw.  `compass` is off by default because its
/// drawing is a model, not proven code (docs/COCKPIT.md §4).
struct CockpitOptions {
    bool icons = true;
    bool numbers = true;
    bool compass = false;
};

/// What a render produced.
///
/// `elements_drawn` counts the logical elements that wrote at least one pixel
/// (panel, each icon, each text element, each non-empty fill, the tape);
/// `missing_assets` counts requested assets that could not be resolved (an
/// icon or a font) - those are skipped, never fatal; `ink_pixels` counts the
/// pixels written by the text layer (labels and digits); `panel_pixels` counts
/// the non-key pixels of the `PANEL` blit.
struct CockpitStats {
    int elements_drawn = 0;
    int missing_assets = 0;
    int ink_pixels = 0;
    int panel_pixels = 0;
};

/// Compose the cockpit frame of *game* into *rgba* (320x200, `0xRRGGBBAA`,
/// alpha 255; the world layer is black and meant to be replaced by the caller).
///
/// Returns false and fills *error* (and clears *rgba*, zeroing *width*/*height*)
/// on: `armor_max <= 0`; no `FRONTEND/BMP.IDX` (or an unreadable index); no
/// palette (a missing/broken `GAMEPAL` and no fallback carrier); no `PANEL` or a
/// `PANEL` whose size is not 320x78.  On success *error* is cleared.
///
/// *stats* may be null; it is zeroed on entry and filled on success.
bool render_cockpit(const std::filesystem::path& game, const CockpitState& state,
                    CockpitOptions options, std::vector<std::uint32_t>& rgba,
                    int& width, int& height, std::string* error,
                    CockpitStats* stats = nullptr);

} // namespace gm
