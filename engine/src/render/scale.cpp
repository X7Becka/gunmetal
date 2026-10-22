#include "render/scale.h"

#include <algorithm>
#include <cmath>

namespace gm {
namespace {

std::uint32_t pack(const std::uint8_t* p) {
    return 0xff000000u | (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}

std::uint8_t lerp(std::uint8_t a, std::uint8_t b, double t) {
    const double v = static_cast<double>(a) + (static_cast<double>(b) - a) * t;
    return static_cast<std::uint8_t>(std::lround(v));
}

} // namespace

ScaleRect fit_letterbox(int width, int height, int dst_w, int dst_h) {
    ScaleRect out;
    if (width <= 0 || height <= 0 || dst_w <= 0 || dst_h <= 0) {
        return out;
    }
    // Compare the ratios cross-multiplied to stay in integers where possible.
    if (static_cast<std::int64_t>(dst_w) * height <=
        static_cast<std::int64_t>(dst_h) * width) {
        out.width = dst_w;
        out.height = static_cast<int>(static_cast<std::int64_t>(dst_w) * height / width);
    } else {
        out.height = dst_h;
        out.width = static_cast<int>(static_cast<std::int64_t>(dst_h) * width / height);
    }
    out.width = std::clamp(out.width, 1, dst_w);
    out.height = std::clamp(out.height, 1, dst_h);
    out.x = (dst_w - out.width) / 2;
    out.y = (dst_h - out.height) / 2;
    return out;
}

void blit_scaled(const std::uint8_t* rgb, int width, int height, std::uint32_t* dst,
                 int dst_w, int dst_h, ScaleFilter filter) {
    if (rgb == nullptr || dst == nullptr || width <= 0 || height <= 0 || dst_w <= 0 ||
        dst_h <= 0) {
        return;
    }
    std::fill(dst, dst + static_cast<std::size_t>(dst_w) * dst_h, 0xff000000u);
    const ScaleRect box = fit_letterbox(width, height, dst_w, dst_h);
    for (int y = 0; y < box.height; ++y) {
        std::uint32_t* row =
            dst + static_cast<std::size_t>(box.y + y) * dst_w + box.x;
        for (int x = 0; x < box.width; ++x) {
            if (filter == ScaleFilter::Nearest) {
                const int sx = std::min(width - 1, x * width / box.width);
                const int sy = std::min(height - 1, y * height / box.height);
                row[x] = pack(rgb + (static_cast<std::size_t>(sy) * width + sx) * 3);
                continue;
            }
            // Bilinear: sample at the pixel centre, clamp at the edges.
            const double fx = (x + 0.5) * width / box.width - 0.5;
            const double fy = (y + 0.5) * height / box.height - 0.5;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, width - 1);
            const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, height - 1);
            const int x1 = std::clamp(x0 + 1, 0, width - 1);
            const int y1 = std::clamp(y0 + 1, 0, height - 1);
            const double tx = std::clamp(fx - x0, 0.0, 1.0);
            const double ty = std::clamp(fy - y0, 0.0, 1.0);
            const std::uint8_t* p00 = rgb + (static_cast<std::size_t>(y0) * width + x0) * 3;
            const std::uint8_t* p10 = rgb + (static_cast<std::size_t>(y0) * width + x1) * 3;
            const std::uint8_t* p01 = rgb + (static_cast<std::size_t>(y1) * width + x0) * 3;
            const std::uint8_t* p11 = rgb + (static_cast<std::size_t>(y1) * width + x1) * 3;
            std::uint8_t channels[3];
            for (int c = 0; c < 3; ++c) {
                const std::uint8_t top = lerp(p00[c], p10[c], tx);
                const std::uint8_t bottom = lerp(p01[c], p11[c], tx);
                channels[c] = lerp(top, bottom, ty);
            }
            row[x] = 0xff000000u | (static_cast<std::uint32_t>(channels[0]) << 16) |
                     (static_cast<std::uint32_t>(channels[1]) << 8) | channels[2];
        }
    }
}

} // namespace gm
