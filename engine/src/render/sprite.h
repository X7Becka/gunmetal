#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// One 8-bit palettised sprite/texture, level 0 of a ``.sbm`` (or any index
/// buffer with the same layout).  ``pixels`` is row-major, top-down and must
/// hold exactly ``width * height`` palette indices.  ``flags`` is the ``.sbm``
/// header word, kept verbatim; the blitter does not interpret it (the mip
/// flags live in it, but sprites are always sampled from level 0).
struct Sprite {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
    std::uint32_t flags = 0;
};

/// Destination surface: 24-bit RGB, top-down, with an explicit row stride in
/// bytes.  ``stride`` may exceed ``width * 3``; it must not be smaller.
struct SpriteTarget {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::span<std::uint8_t> pixels;
};

/// Largest zoom accepted by :func:`blit`/:func:`blit_scaled_bilinear`.  The
/// clipping arithmetic uses 64-bit intermediates, so this is a policy limit
/// (and a guard against typo-sized scales), not an overflow workaround.
constexpr int kSpriteMaxScale = 64;

/// One blit request.
///
/// * ``x``/``y``             - top-left corner of the unclipped sprite in target
///                             pixels; any sign, the blit clips and never wraps.
/// * ``scale``               - integer zoom, 1..``kSpriteMaxScale``.  Zero or
///                             negative is an error, as is a larger value.
/// * ``flip_x``/``flip_y``   - mirror inside the sprite rectangle.
/// * ``alpha``               - 0..255; 0 is a valid no-op, 255 opaque.
/// * ``additive``            - add the source instead of replacing/mixing it.
/// * ``transparent_index``   - skipped palette index, -1 disables the key.
struct SpriteBlit {
    int x = 0;
    int y = 0;
    int scale = 1;
    bool flip_x = false;
    bool flip_y = false;
    int alpha = 255;
    bool additive = false;
    int transparent_index = 0;
};

/// Nearest-neighbour blit: every source texel becomes a ``scale x scale``
/// block.  Returns false and fills *error* on a malformed target, palette,
/// sprite or parameter; *error* is cleared on success.  A sprite entirely
/// outside the target is a successful no-op.
bool blit(SpriteTarget target, std::span<const std::uint8_t> palette,
          const Sprite& sprite, const SpriteBlit& params, std::string& error);

/// Bilinear blit with the same geometry and blending rules as :func:`blit`.
///
/// Destination pixel ``(px, py)`` maps to the source texel coordinate
/// ``u = (dx + 0.5) / scale - 0.5`` (``dx`` is the offset inside the sprite,
/// mirrored for flips), then samples the four surrounding texels with wrap
/// around the power-of-two edges - the same weights, wrap and coverage rule as
/// the sampler in ``gm/render.py`` (``docs/MIPMAPS.md`` §3).  With
/// ``transparent_index >= 0`` a pixel is dropped when the summed opaque weight
/// is ``<= 0.5``.
bool blit_scaled_bilinear(SpriteTarget target, std::span<const std::uint8_t> palette,
                          const Sprite& sprite, const SpriteBlit& params,
                          std::string& error);

} // namespace gm
