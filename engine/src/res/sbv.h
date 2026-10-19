#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SbvFrame {
    std::uint32_t timestamp_ms = 0;
    std::uint32_t end_offset = 0; // cumulative, relative to data_start
};

/// ``.sbv`` video clip container (``SBV0``).  Frames are intra-coded blocks;
/// the pixel codec is implemented in ``gm/sbv.py`` and not ported here yet.
struct Sbv {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_count = 0;
    std::size_t data_start = 0;
    std::vector<SbvFrame> table;
    std::size_t file_size = 0;
};

std::optional<Sbv> parse_sbv(std::span<const std::uint8_t> data, std::string& error);

/// True when the cumulative frame offsets tile the file exactly.
bool sbv_offsets_valid(const Sbv& clip);

} // namespace gm
