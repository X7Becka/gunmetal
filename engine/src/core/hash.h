#pragma once

#include <cstdint>
#include <span>

namespace gm {

/// FNV-1a 64-bit — stable content checksum for golden tests.
inline std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

} // namespace gm
