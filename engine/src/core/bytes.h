#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace gm {

/// Little-endian readers used by every Gunmetal container.
inline std::uint8_t rd8(const std::uint8_t* p) {
    return p[0];
}

inline std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::uint32_t rd24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
}

inline std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t rd64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(rd32(p)) |
           (static_cast<std::uint64_t>(rd32(p + 4)) << 32);
}

/// "53 42 4d 00" — up to `limit` bytes, space separated uppercase hex.
std::string format_hex(std::span<const std::uint8_t> bytes, std::size_t limit = 32);

/// Printable ASCII rendering of the bytes (non-printable -> '.').
std::string format_ascii(std::span<const std::uint8_t> bytes, std::size_t limit = 32);

} // namespace gm
