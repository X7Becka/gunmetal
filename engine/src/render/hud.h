#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "render/render.h"
#include "res/sof.h"

namespace gm {

/// Largest integer zoom a HUD layer may request.  The engine's blitter is 1:1,
/// so scale only exists for the windowed front-ends (nearest neighbour: every
/// source pixel becomes an ``scale x scale`` block, no filtering).
constexpr int kHudMaxScale = 4;

/// One decoded ``.sof`` frame composed onto an RGB frame.
///
/// * ``x``/``y``     - top-left corner in target pixels; may be negative or far
///                     outside the target (the blit clips, it never wraps).
/// * ``transparent`` - skip source pixels equal to ``transparent_index``.
/// * ``opacity``     - 0..1, applied per channel with round-half-up; finite
///                     values outside the range are clamped, NaN/inf rejected.
/// * ``scale``       - integer nearest-neighbour zoom, 1..kHudMaxScale.
///
/// The target is ``RenderResult::rgb``, tightly packed RGB (``width * 3``
/// stride), exactly as ``render_scene`` returns it.
struct HudLayer {
    int x = 0;
    int y = 0;
    bool transparent = false;
    std::uint8_t transparent_index = 0;
    double opacity = 1.0;
    int scale = 1;
};

/// Compose ``film`` frame ``frame`` onto *target* using *rgba_palette*
/// (256 RGBA quads, the layout of ``BmpImage::palette``; the A byte is ignored
/// - the films are opaque VGA palette indices).  Returns false and fills
/// *error* on a malformed target, palette, frame index, scale or opacity;
/// *error* is cleared on success.  A layer entirely outside the target is a
/// successful no-op (``true``).
bool blit_indexed(RenderResult& target, const Sof& film, std::size_t frame,
                  const std::vector<std::uint8_t>& rgba_palette,
                  const HudLayer& layer, std::string& error);

} // namespace gm
