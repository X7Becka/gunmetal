#include "render/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gm {
namespace {

// 256 VGA colours, RGBA quads (one byte per channel).  HUD films index this
// table directly; there is no per-film palette in the ``.sof``.
constexpr std::size_t kPaletteEntries = 256;
constexpr std::size_t kPaletteBytes = kPaletteEntries * 4;

/// Round-half-up blend of one channel.  ``alpha`` is clamped to [0, 1] by the
/// caller, so the intermediate stays in 0..255 and floor(x + 0.5) is exact.
int blend_channel(int dst, int src, double alpha) {
    const double value = static_cast<double>(dst) +
                         (static_cast<double>(src) - static_cast<double>(dst)) * alpha;
    return static_cast<int>(std::floor(value + 0.5));
}

} // namespace

bool blit_indexed(RenderResult& target, const Sof& film, std::size_t frame,
                  const std::vector<std::uint8_t>& rgba_palette,
                  const HudLayer& layer, std::string& error) {
    error.clear();

    if (target.width <= 0 || target.height <= 0) {
        error = "hud: target has no pixels (" + std::to_string(target.width) + "x" +
                std::to_string(target.height) + ")";
        return false;
    }
    const std::size_t expected =
        static_cast<std::size_t>(target.width) * static_cast<std::size_t>(target.height) * 3u;
    if (target.rgb.size() != expected) {
        error = "hud: target RGB is " + std::to_string(target.rgb.size()) + " bytes, expected " +
                std::to_string(expected);
        return false;
    }
    if (rgba_palette.size() < kPaletteBytes) {
        error = "hud: palette has " + std::to_string(rgba_palette.size() / 4) +
                " entries, need 256";
        return false;
    }
    if (layer.scale < 1 || layer.scale > kHudMaxScale) {
        error = "hud: scale " + std::to_string(layer.scale) + " outside 1.." +
                std::to_string(kHudMaxScale);
        return false;
    }
    if (!std::isfinite(layer.opacity)) {
        error = "hud: opacity is not finite";
        return false;
    }

    const auto pixels = film.frame(frame);
    if (!pixels) {
        error = "hud: frame " + std::to_string(frame) + " out of range (" +
                std::to_string(film.tiles()) + " tiles)";
        return false;
    }

    const double alpha = std::clamp(layer.opacity, 0.0, 1.0);
    if (alpha <= 0.0) {
        return true; // valid layer, fully transparent
    }

    const std::int64_t width = target.width;
    const std::int64_t height = target.height;
    const std::int64_t stride = width * 3;
    const std::int64_t scale = layer.scale;
    const std::int64_t origin_x = layer.x;
    const std::int64_t origin_y = layer.y;
    const bool opaque = alpha >= 1.0;

    for (std::int64_t sy = 0; sy < kSofTileHeight; ++sy) {
        const std::int64_t block_y = origin_y + sy * scale;
        if (block_y >= height || block_y + scale <= 0) {
            continue;
        }
        const std::int64_t py0 = std::max<std::int64_t>(block_y, 0);
        const std::int64_t py1 = std::min<std::int64_t>(block_y + scale, height);
        for (std::int64_t sx = 0; sx < kSofTileWidth; ++sx) {
            const std::uint8_t index =
                (*pixels)[static_cast<std::size_t>(sy) * kSofTileWidth +
                          static_cast<std::size_t>(sx)];
            if (layer.transparent && index == layer.transparent_index) {
                continue;
            }
            const std::int64_t block_x = origin_x + sx * scale;
            if (block_x >= width || block_x + scale <= 0) {
                continue;
            }
            const std::int64_t px0 = std::max<std::int64_t>(block_x, 0);
            const std::int64_t px1 = std::min<std::int64_t>(block_x + scale, width);
            const std::uint8_t* color = rgba_palette.data() + static_cast<std::size_t>(index) * 4;
            for (std::int64_t py = py0; py < py1; ++py) {
                std::uint8_t* row = target.rgb.data() + py * stride;
                for (std::int64_t px = px0; px < px1; ++px) {
                    std::uint8_t* dst = row + px * 3;
                    if (opaque) {
                        dst[0] = color[0];
                        dst[1] = color[1];
                        dst[2] = color[2];
                    } else {
                        dst[0] = static_cast<std::uint8_t>(blend_channel(dst[0], color[0], alpha));
                        dst[1] = static_cast<std::uint8_t>(blend_channel(dst[1], color[1], alpha));
                        dst[2] = static_cast<std::uint8_t>(blend_channel(dst[2], color[2], alpha));
                    }
                }
            }
        }
    }
    return true;
}

} // namespace gm
