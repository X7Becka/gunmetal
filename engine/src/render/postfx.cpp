#include "render/postfx.h"

#include <cmath>

namespace gm {

std::uint8_t haze_alpha(double haze) {
    if (!(haze > 0.0)) { // also catches NaN
        return 0;
    }
    const double scaled = std::trunc(255.0 * haze);
    if (scaled >= 255.0) {
        return 255;
    }
    return static_cast<std::uint8_t>(scaled);
}

void fade_white(std::span<std::uint8_t> rgb, double haze) {
    const std::uint8_t a = haze_alpha(haze);
    if (a == 0) {
        return;
    }
    if (a == 255) {
        for (std::uint8_t& value : rgb) {
            value = 255;
        }
        return;
    }
    const int keep = 255 - a;
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        for (std::size_t c = 0; c < 3; ++c) {
            rgb[i + c] = static_cast<std::uint8_t>(
                (static_cast<int>(rgb[i + c]) * keep + 255 * a) / 255);
        }
    }
}

} // namespace gm
