#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SouSample {
    std::string magic; // SOU0 / SOU1 / SOU2
    std::uint32_t rate = 0;
    std::uint32_t flags = 0;   // SOU0/1: 0 mono, 1 stereo; SOU2: meta1
    std::uint32_t length = 0;  // SOU0/1: payload bytes; SOU2: meta2
    std::uint32_t units = 0;   // SOU2 only
    std::vector<std::uint8_t> payload;
};

/// Parse the `.sou` container (docs/FORMATS.md §5.1).  The payload codec is
/// not decoded here — see tools/re/sou_decode.py.
std::optional<SouSample> parse_sou(std::span<const std::uint8_t> data, std::string& error);

} // namespace gm
