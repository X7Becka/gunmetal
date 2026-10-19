#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// One decoded ``.sbv`` frame as RGB (top-down).
struct VideoFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

/// Decode one intra-coded frame block of an ``SBV0`` clip.  The container is
/// parsed by ``res/sbv.h``; *block* is one entry of the frame table
/// (``data_start + previous_end .. data_start + end``).  This is a 1:1 port of
/// the pyramid codec in ``gm/sbv.py``; see docs/FORMATS.md §11.  Frames are
/// purely intra-coded, so the result never depends on other frames.
std::optional<VideoFrame> decode_sbv_frame(std::span<const std::uint8_t> block, int width,
                                           int height, std::string& error);

} // namespace gm
