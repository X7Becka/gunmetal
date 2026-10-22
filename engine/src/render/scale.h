// Interface scaling (`M4.3.3`): fit a 320x200 frame into any window.
//
// The original renders 320x200 (docs/PALETTE.md, docs/LOOK16.md) and the
// window only ever scales it up.  Two properties matter for the presentation:
//
//   * the aspect ratio must survive — a 320x200 frame in a 1920x1080 window
//     gets 96-pixel bars on the left and right, not a stretched picture;
//   * the filter is selectable: nearest keeps the pixel art crisp (and is what
//     the golden frames use), bilinear is the smooth option.
//
// The blit writes XRGB8888 into a caller buffer, so it is testable without a
// window (and the window uses the same code in both modes).
#pragma once

#include <cstdint>

namespace gm {

/// Where a source image lands inside the destination (aspect preserved).
struct ScaleRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class ScaleFilter {
    Nearest,
    Bilinear,
};

/// Largest *width*×*height* box with the same aspect ratio that fits into
/// *dst_w*×*dst_h*, centred.  Integer windows usually leave a few pixels.
ScaleRect fit_letterbox(int width, int height, int dst_w, int dst_h);

/// Scale an RGB24 image into an XRGB8888 buffer.
///
/// *dst* is *dst_w*×*dst_h*; everything outside the fitted box is black.
/// Bilinear clamps at the source edges and rounds to nearest.
void blit_scaled(const std::uint8_t* rgb, int width, int height, std::uint32_t* dst,
                 int dst_w, int dst_h, ScaleFilter filter = ScaleFilter::Nearest);

} // namespace gm
