#include "render/sprite.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gm {
namespace {

constexpr std::size_t kPaletteBytes = 256u * 4u;

/// ``gm/render.py::_clamp``: truncate towards zero, then clamp to a byte.
int clamp_channel(double value) {
    if (value < 0.0) {
        return 0;
    }
    if (value > 255.0) {
        return 255;
    }
    return static_cast<int>(value);
}

/// ``mix``/``add`` from ``gm/render.py`` with the sprite opacity as the blend
/// weight: ``dst * (1 - w) + src * w`` or ``dst + src * w``, then ``_clamp``.
int blend_channel(int dst, int src, int alpha, bool additive) {
    if (alpha == 255) {
        if (additive) {
            return std::min(255, dst + src);
        }
        return src;
    }
    const double weight = static_cast<double>(alpha) / 255.0;
    if (additive) {
        return clamp_channel(static_cast<double>(dst) + static_cast<double>(src) * weight);
    }
    return clamp_channel(static_cast<double>(dst) * (1.0 - weight) +
                         static_cast<double>(src) * weight);
}

const std::uint8_t* palette_color(std::span<const std::uint8_t> palette,
                                  std::uint8_t index) {
    return palette.data() + static_cast<std::size_t>(index) * 4u;
}

bool validate(SpriteTarget target, std::span<const std::uint8_t> palette,
              const Sprite& sprite, const SpriteBlit& params, std::string& error) {
    if (target.width <= 0 || target.height <= 0) {
        error = "sprite: target has no pixels (" + std::to_string(target.width) + "x" +
                std::to_string(target.height) + ")";
        return false;
    }
    const std::int64_t needed_stride = static_cast<std::int64_t>(target.width) * 3;
    if (target.stride < 0 || static_cast<std::int64_t>(target.stride) < needed_stride) {
        error = "sprite: stride " + std::to_string(target.stride) + " is smaller than " +
                std::to_string(needed_stride);
        return false;
    }
    const std::int64_t row_bytes = static_cast<std::int64_t>(target.height - 1) * target.stride +
                                   static_cast<std::int64_t>(target.width) * 3;
    if (row_bytes < 0 || target.pixels.size() < static_cast<std::size_t>(row_bytes)) {
        error = "sprite: target has " + std::to_string(target.pixels.size()) +
                " bytes, need " + std::to_string(row_bytes);
        return false;
    }
    if (palette.size() < kPaletteBytes) {
        error = "sprite: palette has " + std::to_string(palette.size() / 4) +
                " entries, need 256";
        return false;
    }
    if (sprite.width <= 0 || sprite.height <= 0) {
        error = "sprite: image has no pixels (" + std::to_string(sprite.width) + "x" +
                std::to_string(sprite.height) + ")";
        return false;
    }
    const std::size_t wanted = static_cast<std::size_t>(sprite.width) *
                               static_cast<std::size_t>(sprite.height);
    if (sprite.pixels.size() != wanted) {
        error = "sprite: image has " + std::to_string(sprite.pixels.size()) +
                " texels, expected " + std::to_string(wanted);
        return false;
    }
    if (params.scale < 1 || params.scale > kSpriteMaxScale) {
        error = "sprite: scale " + std::to_string(params.scale) + " outside 1.." +
                std::to_string(kSpriteMaxScale);
        return false;
    }
    if (params.alpha < 0 || params.alpha > 255) {
        error = "sprite: alpha " + std::to_string(params.alpha) + " outside 0..255";
        return false;
    }
    if (params.transparent_index < -1 || params.transparent_index > 255) {
        error = "sprite: transparent index " + std::to_string(params.transparent_index) +
                " outside -1..255";
        return false;
    }
    return true;
}

/// Clipped destination rectangle of one sprite, in target pixels.
struct Clip {
    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int64_t right = 0;
    std::int64_t bottom = 0;
};

Clip clip_rect(const Sprite& sprite, const SpriteBlit& params, SpriteTarget target) {
    const std::int64_t width = static_cast<std::int64_t>(sprite.width) * params.scale;
    const std::int64_t height = static_cast<std::int64_t>(sprite.height) * params.scale;
    const std::int64_t left = params.x;
    const std::int64_t top = params.y;
    Clip clip;
    clip.left = std::max<std::int64_t>(left, 0);
    clip.top = std::max<std::int64_t>(top, 0);
    clip.right = std::min<std::int64_t>(left + width, target.width);
    clip.bottom = std::min<std::int64_t>(top + height, target.height);
    return clip;
}

void store_pixel(SpriteTarget target, std::int64_t x, std::int64_t y,
                 const std::uint8_t* color, int alpha, bool additive) {
    std::uint8_t* dst = target.pixels.data() +
                        static_cast<std::ptrdiff_t>(y * target.stride + x * 3);
    dst[0] = static_cast<std::uint8_t>(blend_channel(dst[0], color[0], alpha, additive));
    dst[1] = static_cast<std::uint8_t>(blend_channel(dst[1], color[1], alpha, additive));
    dst[2] = static_cast<std::uint8_t>(blend_channel(dst[2], color[2], alpha, additive));
}

} // namespace

bool blit(SpriteTarget target, std::span<const std::uint8_t> palette,
          const Sprite& sprite, const SpriteBlit& params, std::string& error) {
    error.clear();
    if (!validate(target, palette, sprite, params, error)) {
        return false;
    }
    if (params.alpha == 0) {
        return true; // valid request, fully transparent
    }

    const Clip clip = clip_rect(sprite, params, target);
    if (clip.right <= clip.left || clip.bottom <= clip.top) {
        return true; // entirely outside the target
    }

    const std::int64_t scale = params.scale;
    const std::int64_t origin_x = params.x;
    const std::int64_t origin_y = params.y;
    const std::int64_t sprite_width = sprite.width;
    const std::int64_t sprite_height = sprite.height;

    for (std::int64_t py = clip.top; py < clip.bottom; ++py) {
        std::int64_t sy = (py - origin_y) / scale;
        if (params.flip_y) {
            sy = sprite_height - 1 - sy;
        }
        const std::uint8_t* row = sprite.pixels.data() + static_cast<std::size_t>(sy) *
                                                       static_cast<std::size_t>(sprite_width);
        for (std::int64_t px = clip.left; px < clip.right; ++px) {
            std::int64_t sx = (px - origin_x) / scale;
            if (params.flip_x) {
                sx = sprite_width - 1 - sx;
            }
            const std::uint8_t index = row[sx];
            if (params.transparent_index >= 0 && index == params.transparent_index) {
                continue;
            }
            store_pixel(target, px, py, palette_color(palette, index), params.alpha,
                        params.additive);
        }
    }
    return true;
}

bool blit_scaled_bilinear(SpriteTarget target, std::span<const std::uint8_t> palette,
                          const Sprite& sprite, const SpriteBlit& params,
                          std::string& error) {
    error.clear();
    if (!validate(target, palette, sprite, params, error)) {
        return false;
    }
    if (params.alpha == 0) {
        return true; // valid request, fully transparent
    }

    const Clip clip = clip_rect(sprite, params, target);
    if (clip.right <= clip.left || clip.bottom <= clip.top) {
        return true; // entirely outside the target
    }

    const int width = sprite.width;
    const int height = sprite.height;
    const double scale = static_cast<double>(params.scale);
    const std::int64_t origin_x = params.x;
    const std::int64_t origin_y = params.y;

    for (std::int64_t py = clip.top; py < clip.bottom; ++py) {
        double fv = (static_cast<double>(py - origin_y) + 0.5) / scale - 0.5;
        if (params.flip_y) {
            fv = static_cast<double>(height - 1) - fv;
        }
        const double fy0 = std::floor(fv);
        const double ay = fv - fy0;
        std::int64_t ty0 = static_cast<std::int64_t>(fy0) % height;
        if (ty0 < 0) {
            ty0 += height;
        }
        const std::int64_t ty1 = (ty0 + 1) % height;
        const std::uint8_t* row0 = sprite.pixels.data() + static_cast<std::size_t>(ty0) *
                                                       static_cast<std::size_t>(width);
        const std::uint8_t* row1 = sprite.pixels.data() + static_cast<std::size_t>(ty1) *
                                                       static_cast<std::size_t>(width);

        for (std::int64_t px = clip.left; px < clip.right; ++px) {
            double fu = (static_cast<double>(px - origin_x) + 0.5) / scale - 0.5;
            if (params.flip_x) {
                fu = static_cast<double>(width - 1) - fu;
            }
            const double fx0 = std::floor(fu);
            const double ax = fu - fx0;
            std::int64_t tx0 = static_cast<std::int64_t>(fx0) % width;
            if (tx0 < 0) {
                tx0 += width;
            }
            const std::int64_t tx1 = (tx0 + 1) % width;

            const std::uint8_t i00 = row0[tx0];
            const std::uint8_t i10 = row0[tx1];
            const std::uint8_t i01 = row1[tx0];
            const std::uint8_t i11 = row1[tx1];

            const double w00 = (1.0 - ax) * (1.0 - ay);
            const double w10 = ax * (1.0 - ay);
            const double w01 = (1.0 - ax) * ay;
            const double w11 = ax * ay;

            if (params.transparent_index >= 0) {
                const double covered =
                    w00 * (i00 != params.transparent_index) +
                    w10 * (i10 != params.transparent_index) +
                    w01 * (i01 != params.transparent_index) +
                    w11 * (i11 != params.transparent_index);
                if (covered <= 0.5) {
                    continue;
                }
            }

            const std::uint8_t* c00 = palette_color(palette, i00);
            const std::uint8_t* c10 = palette_color(palette, i10);
            const std::uint8_t* c01 = palette_color(palette, i01);
            const std::uint8_t* c11 = palette_color(palette, i11);
            std::uint8_t color[3];
            for (int k = 0; k < 3; ++k) {
                const double value = static_cast<double>(c00[k]) * w00 +
                                     static_cast<double>(c10[k]) * w10 +
                                     static_cast<double>(c01[k]) * w01 +
                                     static_cast<double>(c11[k]) * w11;
                color[k] = static_cast<std::uint8_t>(clamp_channel(value));
            }
            store_pixel(target, px, py, color, params.alpha, params.additive);
        }
    }
    return true;
}

} // namespace gm
