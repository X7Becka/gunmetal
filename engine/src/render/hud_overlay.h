#pragma once

// Reusable HUD overlay: one `.sof` film frame (or a stack of them) composed
// onto an already rendered RGB frame, with the geometry/statistics a front-end
// needs.  This is the piece `main.cpp::overlay_hud` currently does by hand
// (`load_hud_film` -> `installed_palette` -> `hud_player::draw`), lifted into a
// module so the viewer, the CLI and future front-ends share one code path.
//
// Layering
// --------
//   * ``render/hud.*``         - ``blit_indexed``: palette indices -> RGB, the
//                                only code that writes pixels (called, not
//                                duplicated here);
//   * ``render/hud_player.*``  - ``HudFilm`` + ``frame_at``: the 91 Hz clock;
//   * ``render/hud_overlay.*`` - placement options, the 0..1 opacity contract,
//                                clipping classification and the stats.
//
// What this module does *not* do: read ``.sml`` screens (that is
// ``render/hud_compose.*``), pick a film family or a palette, or hook into the
// main loop.  Inputs are already-loaded assets; nothing is read from disk.
//
// Statistics
// ----------
// ``drawn_pixels + skipped_transparent + clipped_pixels == layer_width *
// layer_height`` always holds, because every scaled *cell* of the layer is
// classified exactly once:
//   * transparent key hit (checked first, as ``blit_indexed`` does) -> skipped;
//   * otherwise the part of the ``scale x scale`` block inside the target ->
//     drawn, the rest -> clipped.
// ``drawn_pixels`` is therefore *coverage*: the cells ``blit_indexed`` writes.
// The one exception is ``opacity == 0``, where the blitter returns before
// touching the buffer but coverage is still reported so the invariant holds.
//
// Proven vs. assumed
// ------------------
// The geometry (128x64 tiles, nearest-neighbour zoom, clip-not-wrap) and the
// blend are ``render/hud.*``'s; the parity of ``overlay_film`` with
// ``hud_player::draw``/``main.cpp::overlay_hud`` is checked for byte equality
// on the shipped AMMO4 film (docs/HUD_OVERLAY.md §3).  The stricter
// ``opacity`` range below is a deliberate API choice, not engine behaviour
// (the engine blitter clamps).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "render/hud_player.h"
#include "render/render.h"
#include "res/sof.h"

namespace gm {

/// 256 RGBA quads, the layout of ``BmpImage::palette``/``installed_palette``
/// (the alpha byte is ignored: ``.sof`` frames are opaque VGA indices).
using Palette = std::vector<std::uint8_t>;

/// Where and how one film frame is placed on the target.
///
/// * ``x``/``y``          - top-left corner in target pixels; any ``int`` is
///                          accepted, the layer clips (it never wraps).  A layer
///                          entirely outside the target is a successful no-op.
/// * ``scale``            - integer nearest-neighbour zoom, 1..``kHudMaxScale``.
/// * ``opacity``          - 0..1 inclusive; outside the range is an *error*
///                          here (``blit_indexed`` would clamp, but a front-end
///                          asking for 1.5 is a bug worth reporting).
/// * ``transparent_zero`` - skip VGA index 0 (the films' background), the key
///                          ``main.cpp::overlay_hud`` passes to ``draw``.
/// * ``tick``             - 91 Hz clock value used to pick the frame through
///                          ``frame_at`` (``HUDFilm::start_frame``/``loop``
///                          apply).
struct HudOverlayOptions {
    int x = 0;
    int y = 0;
    int scale = 1;
    float opacity = 1.0f;
    bool transparent_zero = true;
    std::uint64_t tick = 0;
};

/// What one layer (or a stack, for ``compose_hud_overlay``) did.  The three
/// pixel counts the layer area sum is documented above; ``layer_width``/
/// ``layer_height`` are the *scaled* layer extent (``128 * scale`` by
/// ``64 * scale``), not the clipped one.
struct HudOverlayStats {
    int layer_width = 0;
    int layer_height = 0;
    int drawn_pixels = 0;
    int skipped_transparent = 0;
    int clipped_pixels = 0;
};

/// One already-decoded frame composed onto *image*.
///
/// Validates the options, the frame index (when *stats* is requested the frame
/// is decoded first, so a bad index is refused before any write), then
/// delegates the pixels to ``blit_indexed`` and finally classifies the cells
/// into *stats*.  On failure the image is untouched, *stats* is zeroed and
/// *error* holds a message prefixed ``hud_overlay: `` (nested ``hud: ...``
/// messages are kept).  *error* is cleared on success; *stats* may be null.
bool overlay_glyph_layer(RenderResult& image, const Sof& film, std::size_t frame,
                         const Palette& palette, const HudOverlayOptions& options,
                         HudOverlayStats* stats, std::string* error);

/// ``overlay_glyph_layer`` for the frame ``frame_at(film, options.tick)``
/// selects.  This is the drop-in replacement for ``main.cpp::overlay_hud``:
/// the same film, palette and transparent-index-0 blit, plus clipping of
/// negative origins (``hud_player::draw`` rejects them) and the stats.
///
/// Fails for an empty film, a tick whose ``tick * fps`` overflows 64 bits, the
/// option/target/palette refusals of the layer call.
bool overlay_film(RenderResult& image, const HudFilm& film, const Palette& palette,
                  const HudOverlayOptions& options, HudOverlayStats* stats,
                  std::string* error);

/// One entry of a ``compose_hud_overlay`` stack: a film plus its placement.
/// The film is borrowed, so the stack is cheap to build per frame.
struct HudOverlayLayer {
    const HudFilm* film = nullptr;
    HudOverlayOptions options{};
};

/// Compose several films in order (later layers paint over earlier ones).
///
/// All layers are validated - target, palette, options and the tick -> frame
/// step - before the first blit, so a rejection leaves *image* byte-identical
/// (the same all-or-nothing rule as ``compose_hud``).  *stats* accumulates:
/// the counts are summed and ``layer_width``/``layer_height`` are the largest
/// scaled layer.  An empty stack is a successful no-op with zero stats.
///
/// This is *not* the ``.sml`` screen pipeline (``render/hud_compose.h``): the
/// caller decides which films exist and where they go.
bool compose_hud_overlay(RenderResult& image, std::span<const HudOverlayLayer> layers,
                         const Palette& palette, HudOverlayStats* stats,
                         std::string* error);

} // namespace gm
