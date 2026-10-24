#include "render/upscale.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace gm {
namespace {

bool set_error(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
    return false;
}

} // namespace

bool upscale_nearest(const std::uint32_t* src, int w, int h, int scale,
                     std::vector<std::uint32_t>& out, std::string* error,
                     UpscaleStats* stats) {
    out.clear();
    if (error != nullptr) {
        error->clear();
    }
    if (src == nullptr) {
        return set_error(error, "upscale: null source");
    }
    if (w <= 0 || h <= 0) {
        return set_error(error, "upscale: source size " + std::to_string(w) + "x" +
                                    std::to_string(h) + " is not positive");
    }
    if (scale < 1 || scale > kUpscaleMaxScale) {
        return set_error(error, "upscale: scale " + std::to_string(scale) +
                                    " outside 1.." + std::to_string(kUpscaleMaxScale));
    }
    // `w`/`h`/`scale` are positive ints, so both products are computed in
    // 64-bit `size_t` and only the destination size can overflow.
    const std::size_t pixels_in =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const std::size_t factor = static_cast<std::size_t>(scale) *
                               static_cast<std::size_t>(scale);
    if (pixels_in > std::numeric_limits<std::size_t>::max() / factor) {
        return set_error(error, "upscale: " + std::to_string(w) + "x" +
                                    std::to_string(h) + " at scale " +
                                    std::to_string(scale) + " overflows the buffer");
    }
    const std::size_t dst_w = static_cast<std::size_t>(w) * static_cast<std::size_t>(scale);
    const std::size_t pixels_out = pixels_in * factor;
    out.resize(pixels_out);

    // One source row becomes one destination row (`scale` copies of every
    // pixel), then that row is copied `scale - 1` times: the same bytes as
    // writing every destination pixel, with the inner loop over source pixels.
    for (int y = 0; y < h; ++y) {
        const std::uint32_t* source_row = src + static_cast<std::size_t>(y) *
                                                   static_cast<std::size_t>(w);
        std::uint32_t* row = out.data() + static_cast<std::size_t>(y) * dst_w *
                                              static_cast<std::size_t>(scale);
        for (int x = 0; x < w; ++x) {
            std::fill_n(row + static_cast<std::size_t>(x) *
                                static_cast<std::size_t>(scale),
                        static_cast<std::size_t>(scale), source_row[x]);
        }
        for (int copy = 1; copy < scale; ++copy) {
            std::copy(row, row + dst_w, row + static_cast<std::size_t>(copy) * dst_w);
        }
    }

    if (stats != nullptr) {
        stats->scale = scale;
        stats->pixels_in = pixels_in;
        stats->pixels_out = pixels_out;
    }
    return true;
}

bool fade_lut(int level, int max_level, std::uint8_t (&lut)[kFadeLutSize],
              std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (max_level < 1) {
        return set_error(error, "upscale: fade max_level " + std::to_string(max_level) +
                                    " must be >= 1");
    }
    // The engine's tables cover 0..15 with 11 as the identity; a level outside
    // `0..max_level` keeps that meaning instead of being an error.
    const int clamped = std::clamp(level, 0, max_level);
    const std::int64_t half = static_cast<std::int64_t>(max_level) / 2;
    const std::int64_t scale = static_cast<std::int64_t>(max_level);
    for (int value = 0; value < kFadeLutSize; ++value) {
        const std::int64_t scaled =
            static_cast<std::int64_t>(value) * static_cast<std::int64_t>(clamped);
        lut[value] = static_cast<std::uint8_t>((scaled + half) / scale);
    }
    return true;
}

bool fade_rgba(std::vector<std::uint32_t>& rgba, int level, int max_level,
               std::string* error) {
    std::uint8_t lut[kFadeLutSize] = {};
    if (!fade_lut(level, max_level, lut, error)) {
        return false;
    }
    for (std::uint32_t& colour : rgba) {
        const std::uint32_t r = lut[(colour >> 24u) & 0xFFu];
        const std::uint32_t g = lut[(colour >> 16u) & 0xFFu];
        const std::uint32_t b = lut[(colour >> 8u) & 0xFFu];
        colour = (r << 24u) | (g << 16u) | (b << 8u) | (colour & 0xFFu);
    }
    return true;
}

} // namespace gm
