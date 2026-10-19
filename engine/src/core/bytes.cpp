#include "core/bytes.h"

#include <cstdio>

namespace gm {

std::string format_hex(std::span<const std::uint8_t> bytes, std::size_t limit) {
    std::string out;
    const std::size_t count = bytes.size() < limit ? bytes.size() : limit;
    out.reserve(count * 3);
    char buffer[4];
    for (std::size_t i = 0; i < count; ++i) {
        std::snprintf(buffer, sizeof(buffer), "%02X", bytes[i]);
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(buffer, 2);
    }
    return out;
}

std::string format_ascii(std::span<const std::uint8_t> bytes, std::size_t limit) {
    std::string out;
    const std::size_t count = bytes.size() < limit ? bytes.size() : limit;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t c = bytes[i];
        out.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.');
    }
    return out;
}

} // namespace gm
