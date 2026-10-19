#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SbcRecord {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // 8-bit, packed pitch
};

/// Per-level lightmap block (``SBC0``/``SBC1``).  Layout: docs/FORMATS.md §?,
/// implementation reference gm/sbc.py.
struct Sbc {
    std::string magic;
    std::vector<SbcRecord> records;
    std::size_t file_end = 0;
};

std::optional<Sbc> parse_sbc(std::span<const std::uint8_t> data, std::string& error);

} // namespace gm
