#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace gm {

/// Encode RGBA8 pixels as a PNG.
///
/// Deliberately dependency-free: the zlib stream uses uncompressed ("stored")
/// deflate blocks.  Files are bigger than a real zlib output, but byte-stable
/// and good enough for tooling and golden dumps (same approach as gm/png.py).
std::vector<std::uint8_t> encode_png_rgba(int width, int height,
                                          std::span<const std::uint8_t> rgba);

} // namespace gm
