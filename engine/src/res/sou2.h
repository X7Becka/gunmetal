#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// PCM clip decoded from a SOU2 blob (``MUSIC/SOU.DAT`` entries; see
/// docs/FORMATS.md §5.2 and tools/re/sou_decode.py).
struct PcmClip {
    int sample_rate = 0;
    int channels = 0;
    std::vector<std::int16_t> samples; // interleaved, little-endian at write time
};

/// Decode one SOU2 blob.  This is the nibble-DPCM variant (engine decoder
/// ``0x852d80``); SOU1 is a different, still-unproven model.
std::optional<PcmClip> decode_sou2(std::span<const std::uint8_t> blob, std::string& error);

} // namespace gm
