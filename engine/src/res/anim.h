#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SooFrame {
    std::string name;
    int duration = 0;
};

/// Model animation (``Sobanim/*.soo``) — plain text, see docs/FORMATS.md §8.1.
struct Soo {
    int mode = 0;
    int declared = 0;
    std::vector<SooFrame> frames;
};

std::optional<Soo> parse_soo(std::span<const std::uint8_t> data, std::string& error);

struct Soa {
    int mode = 0;
    int period = 0;
    int declared = 0;
    int loop = 0;
    std::vector<std::string> names;
};

/// Sprite/texture animation descriptor (``anims/*.SOA``), text.
std::optional<Soa> parse_soa(std::span<const std::uint8_t> data, std::string& error);

} // namespace gm
