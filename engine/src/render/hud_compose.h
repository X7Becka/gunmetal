#pragma once

// HUD composition: one call from an ``.sml``/``.sme`` screen to pixels.
//
// The layers below this module are already typed and independent:
//
//   * ``res/sml.*``          - markup -> ``SmlDocument`` (tags, ``<I>`` elements);
//   * ``render/hud_bind.*``  - screen name -> family/id, ``resolve_film`` and the
//                              ``<I x y a>`` -> 128x64 rectangles of ``layout``;
//   * ``render/hud_player.*``- ``.sof``/``.fdx`` -> the frame shown at tick *t*
//                              on the 91 Hz clock;
//   * ``render/hud.*``       - ``blit_indexed`` (palette indices -> RGB).
//
// ``compose_hud`` is the glue the engine's screen code would call: read the
// screen, bind it, resolve the film, advance to the tick, blit every placement,
// and report what happened.  It owns no state and writes nothing but *target*;
// a failure leaves *target* untouched, because every input is validated (target,
// palette, document, binding, film, tick) before the first blit.
//
// Screen resolution
// -----------------
// ``compose_hud`` accepts a path or raw bytes.  The path form tries the exact
// file first, then the sibling extensions ``.sml``/``.sme`` (both cases) with
// ASCII-case-insensitive resolution relative to *game*, so
// ``DESIGN/AMMO4.sml`` finds the shipped ``design/ammo4.sml``/``ammo4.sme`` on a
// case-sensitive filesystem.  The bytes form takes the logical screen name
// separately, because the name (not the markup) selects the film family.
//
// Blit policy
// -----------
// The composition is the parity blit of docs/HUD_PLAYER.md §4: scale 1,
// opacity 1, and *no* transparency key, so an AMMO4 screen at the overlay
// position reproduces the player's FNV goldens byte for byte.  The films' index
// 0 is a real colour here; transparent composition is a front-end option that
// is deliberately not part of this API yet (docs/HUD_COMPOSE.md §7).
//
// Proven vs hypothesis
// --------------------
// The mechanism (screen -> family -> same id in ``.sof`` and ``.fdx`` line,
// ``<I>`` geometry, overlay at (14,125)) is xref-backed where docs/HUD_BIND.md
// says so.  That a *design page* is the page that shows its family film stays a
// hypothesis, which is why ``HudComposeSummary`` carries both bases instead of
// alleging certainty.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/hud_bind.h"
#include "render/render.h"

namespace gm {

/// Smallest accepted target.  A film tile is 128x64, so anything smaller would
/// show nothing at all; that is an explicit error, not an empty success.
inline constexpr int kHudComposeMinWidth = kSofTileWidth;
inline constexpr int kHudComposeMinHeight = kSofTileHeight;

/// ``blit_indexed`` needs 256 RGBA quads; a shorter palette is rejected before
/// any pixel is written.
inline constexpr std::size_t kHudComposePaletteBytes = 256 * 4;

/// Everything a caller needs to log or assert, without touching the geometry
/// types.  ``placements`` counts laid-out rectangles, ``visible`` those with a
/// non-empty intersection with the target, ``clipped`` those partly or fully
/// outside it, and ``drawn`` those actually blitted (== ``visible`` today).
struct HudComposeSummary {
    std::string screen; ///< name the caller passed (path or logical name)
    std::string stem;   ///< upper-case basename used for classification
    HudFamily family = HudFamily::Default;
    std::string family_name;      ///< "ammo", "weapon", ...
    bool has_id = false;          ///< the screen name carried a numeric id
    int id = 0;                   ///< family id (== .fdx line) when has_id
    HudBasis screen_basis = HudBasis::Hypothesis; ///< screen -> family
    HudBasis film_basis = HudBasis::Hypothesis;   ///< family/id choice
    std::string film;             ///< resolved upload name, e.g. "AMMO4"
    std::string note;             ///< why this film, from resolve_film
    int line = 0;                 ///< .fdx line of the film
    std::uint32_t count = 0;      ///< played frames (exclusive bound)
    std::uint32_t fps = 0;        ///< .fdx clock
    std::uint64_t tick = 0;       ///< clock value used
    std::size_t frame = 0;        ///< tile index drawn
    bool ended = false;           ///< non-loop film ran past count-1
    std::size_t placements = 0;
    std::size_t visible = 0;
    std::size_t clipped = 0;
    std::size_t drawn = 0;
};

/// Compose the screen at *screen_path* onto *target*.
///
/// Steps: resolve/read the screen, ``parse_sml`` -> ``parse_binding`` ->
/// ``resolve_film`` -> ``frame_at(tick)`` -> ``layout`` -> one
/// ``blit_indexed`` per visible placement.  *state* only matters when the
/// screen name carries no id (docs/HUD_BIND.md §2.2).
///
/// Fails (returns ``nullopt``, fills *error*, leaves *target* unchanged) on:
///   * a target outside 128x64 .. 8192x8192 or with a wrong-sized ``rgb``;
///   * a palette shorter than 1024 bytes;
///   * a missing/unreadable screen (nothing at the path nor at a sibling
///     ``.sml``/``.sme``);
///   * malformed markup (``parse_sml``/``parse_binding``, including an ``<I>``
///     without coordinates or an alignment);
///   * an unloadable film: missing ``.sof``/``.fdx``, a line outside the index,
///     ``count``/``fps`` out of range - ``EQUIP<n>`` is the shipped case, there
///     are no ``EQUIP*.SOF`` files;
///   * a tick whose ``tick * fps`` overflows 64 bits (a negative tick cast to
///     ``uint64_t`` lands here);
///   * a blit rejection (not expected after the checks above).
///
/// Nested errors keep their origin (``hud_compose: hud_bind: ...``), so a log
/// reader can see which layer refused.  *error* is cleared on success.
std::optional<HudComposeSummary> compose_hud(
    RenderResult& target, const std::filesystem::path& game,
    const std::filesystem::path& screen_path, const HudState& state,
    const std::vector<std::uint8_t>& palette, std::uint64_t tick,
    std::string& error);

/// Same pipeline for an already-read screen (``std::span`` of the raw
/// ``.sml``/``.sme`` bytes).  *screen_name* drives classification exactly like
/// the path form and is copied into ``HudComposeSummary::screen``.
std::optional<HudComposeSummary> compose_hud(
    RenderResult& target, const std::filesystem::path& game,
    std::span<const std::uint8_t> screen_bytes, std::string_view screen_name,
    const HudState& state, const std::vector<std::uint8_t>& palette,
    std::uint64_t tick, std::string& error);

} // namespace gm
