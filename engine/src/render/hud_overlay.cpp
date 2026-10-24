#include "render/hud_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "render/hud.h"

namespace gm {
namespace {

/// ``blit_indexed`` needs 256 RGBA quads; checked here too, so a stack fails
/// before the first layer writes anything.
constexpr std::size_t kPaletteBytes = 256 * 4;

/// Options shared by every entry point.  Returns an empty string when valid.
/// The opacity range is enforced here rather than in ``blit_indexed``, which
/// clamps finite values (docs/HUD_CPP.md §2) - see docs/HUD_OVERLAY.md §1.
std::string option_error(const HudOverlayOptions& options) {
    if (options.scale < 1 || options.scale > kHudMaxScale) {
        return "scale " + std::to_string(options.scale) + " outside 1.." +
               std::to_string(kHudMaxScale);
    }
    if (!std::isfinite(options.opacity)) {
        return "opacity is not finite";
    }
    if (options.opacity < 0.0f || options.opacity > 1.0f) {
        return "opacity " + std::to_string(options.opacity) + " outside 0..1";
    }
    return {};
}

/// Classify every scaled cell of one decoded frame.  Mirrors the loop of
/// ``blit_indexed`` exactly: the transparency key wins over clipping, a
/// ``scale x scale`` block is split into the part inside the target (drawn)
/// and the rest (clipped).  *image* must already be a valid target.
void classify(const RenderResult& image, const std::vector<std::uint8_t>& pixels,
              const HudOverlayOptions& options, HudOverlayStats& stats) {
    const std::int64_t scale = options.scale;
    const std::int64_t width = image.width;
    const std::int64_t height = image.height;
    const std::int64_t origin_x = options.x;
    const std::int64_t origin_y = options.y;
    const std::int64_t block_area = scale * scale;

    std::int64_t drawn = 0;
    std::int64_t skipped = 0;
    std::int64_t clipped = 0;

    for (std::int64_t sy = 0; sy < kSofTileHeight; ++sy) {
        const std::int64_t block_y = origin_y + sy * scale;
        for (std::int64_t sx = 0; sx < kSofTileWidth; ++sx) {
            const std::uint8_t index =
                pixels[static_cast<std::size_t>(sy) * kSofTileWidth +
                       static_cast<std::size_t>(sx)];
            if (options.transparent_zero && index == 0) {
                skipped += block_area;
                continue;
            }
            const std::int64_t block_x = origin_x + sx * scale;
            // Same intersection arithmetic as blit_indexed (int64, no wrap).
            const std::int64_t nx =
                (block_x >= width || block_x + scale <= 0)
                    ? 0
                    : (std::min(block_x + scale, width) - std::max(block_x, std::int64_t{0}));
            const std::int64_t ny =
                (block_y >= height || block_y + scale <= 0)
                    ? 0
                    : (std::min(block_y + scale, height) - std::max(block_y, std::int64_t{0}));
            drawn += nx * ny;
            clipped += block_area - nx * ny;
        }
    }

    stats.layer_width = kSofTileWidth * options.scale;
    stats.layer_height = kSofTileHeight * options.scale;
    stats.drawn_pixels = static_cast<int>(drawn);
    stats.skipped_transparent = static_cast<int>(skipped);
    stats.clipped_pixels = static_cast<int>(clipped);
}

} // namespace

bool overlay_glyph_layer(RenderResult& image, const Sof& film, std::size_t frame,
                         const Palette& palette, const HudOverlayOptions& options,
                         HudOverlayStats* stats, std::string* error) {
    std::string scratch;
    std::string& err = error != nullptr ? *error : scratch;
    err.clear();
    if (stats != nullptr) {
        *stats = HudOverlayStats{};
    }

    const std::string bad = option_error(options);
    if (!bad.empty()) {
        err = "hud_overlay: " + bad;
        return false;
    }

    // Decode the frame up front when the stats need it, so an out-of-range
    // index is refused before blit_indexed writes a byte.
    std::vector<std::uint8_t> pixels;
    if (stats != nullptr) {
        auto decoded = film.frame(frame);
        if (!decoded) {
            err = "hud_overlay: frame " + std::to_string(frame) + " out of range (" +
                  std::to_string(film.tiles()) + " tiles)";
            return false;
        }
        pixels = std::move(*decoded);
    }

    HudLayer layer;
    layer.x = options.x;
    layer.y = options.y;
    layer.transparent = options.transparent_zero;
    layer.transparent_index = 0; // VGA background index of the HUD films
    layer.opacity = static_cast<double>(options.opacity);
    layer.scale = options.scale;
    if (!blit_indexed(image, film, frame, palette, layer, err)) {
        err = "hud_overlay: " + err;
        return false;
    }

    if (stats != nullptr) {
        classify(image, pixels, options, *stats);
    }
    return true;
}

bool overlay_film(RenderResult& image, const HudFilm& film, const Palette& palette,
                  const HudOverlayOptions& options, HudOverlayStats* stats,
                  std::string* error) {
    std::string scratch;
    std::string& err = error != nullptr ? *error : scratch;
    err.clear();
    if (stats != nullptr) {
        *stats = HudOverlayStats{};
    }

    // frame_at owns the clock: count/fps/start_frame/loop and tick overflow.
    const auto current = frame_at(film, options.tick, err);
    if (!current) {
        err = "hud_overlay: " + err;
        return false;
    }
    return overlay_glyph_layer(image, film.sof, current->frame, palette, options, stats,
                               &err);
}

bool compose_hud_overlay(RenderResult& image, std::span<const HudOverlayLayer> layers,
                         const Palette& palette, HudOverlayStats* stats,
                         std::string* error) {
    std::string scratch;
    std::string& err = error != nullptr ? *error : scratch;
    err.clear();
    if (stats != nullptr) {
        *stats = HudOverlayStats{};
    }

    if (image.width <= 0 || image.height <= 0) {
        err = "hud_overlay: target has no pixels (" + std::to_string(image.width) + "x" +
              std::to_string(image.height) + ")";
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(image.width) *
                                 static_cast<std::size_t>(image.height) * 3u;
    if (image.rgb.size() != expected) {
        err = "hud_overlay: target RGB is " + std::to_string(image.rgb.size()) +
              " bytes, expected " + std::to_string(expected);
        return false;
    }
    if (palette.size() < kPaletteBytes) {
        err = "hud_overlay: palette has " + std::to_string(palette.size() / 4) +
              " entries, need 256";
        return false;
    }

    // Pass 1: everything that can be refused, before the first write.
    std::vector<std::size_t> frames;
    frames.reserve(layers.size());
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const HudOverlayLayer& entry = layers[i];
        if (entry.film == nullptr) {
            err = "hud_overlay: layer " + std::to_string(i) + " has no film";
            return false;
        }
        const std::string bad = option_error(entry.options);
        if (!bad.empty()) {
            err = "hud_overlay: layer " + std::to_string(i) + ": " + bad;
            return false;
        }
        const auto current = frame_at(*entry.film, entry.options.tick, err);
        if (!current) {
            err = "hud_overlay: layer " + std::to_string(i) + ": " + err;
            return false;
        }
        frames.push_back(current->frame);
    }

    // Pass 2: pixels, in order, later layers over earlier ones.
    std::int64_t drawn = 0;
    std::int64_t skipped = 0;
    std::int64_t clipped = 0;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        HudOverlayStats one;
        if (!overlay_glyph_layer(image, layers[i].film->sof, frames[i], palette,
                                 layers[i].options, &one, &err)) {
            err = "hud_overlay: layer " + std::to_string(i) + ": " + err;
            return false;
        }
        drawn += one.drawn_pixels;
        skipped += one.skipped_transparent;
        clipped += one.clipped_pixels;
        if (stats != nullptr) {
            stats->layer_width = std::max(stats->layer_width, one.layer_width);
            stats->layer_height = std::max(stats->layer_height, one.layer_height);
        }
    }
    if (stats != nullptr) {
        stats->drawn_pixels = static_cast<int>(drawn);
        stats->skipped_transparent = static_cast<int>(skipped);
        stats->clipped_pixels = static_cast<int>(clipped);
    }
    return true;
}

} // namespace gm
