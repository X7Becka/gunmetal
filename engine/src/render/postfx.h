// Full-screen post effects (`M4.2.8`): the camera haze fade.
//
// The engine draws exactly one full-screen quad: `0x4188b2`, white
// (`grConstantColorValue(0xffffff)`, `0x418cb8`), `grDepthMask(FALSE)`, with
//
//     alpha = ftol(255 * ctx[0xe8])          (0x418c7c..0x418c90)
//
// and `ctx[0xe8]` (`0x83d288`) is the camera haze set by `SET_CAMERA_HAZE`
// (`0x446764` copies it each frame, docs/SCREEN_FX.md).  `ftol` truncates, so
// `haze = 0.999` still gives alpha 254.
//
// Blend is the standard over operator; the C++ and Python (`gm/render.py
// fade_rgb`) implementations must agree byte for byte, hence the integer
// arithmetic.
#pragma once

#include <cstdint>
#include <span>

namespace gm {

/// 8-bit alpha the engine's quad uses for *haze*: `ftol(255 * haze)`, clamped.
std::uint8_t haze_alpha(double haze);

/// White haze over an RGB24 frame in place.  `alpha == 0` leaves it untouched.
void fade_white(std::span<std::uint8_t> rgb, double haze);

} // namespace gm
