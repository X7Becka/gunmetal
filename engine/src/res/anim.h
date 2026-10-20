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

/// One instant of playback, mirroring gm/soo.py's ``Pose``.
struct SooPose {
    int index = 0;
    int next_index = 0;
    double alpha = 0.0;
    int elapsed = 0;
    int phase = 0;
    int total = 0;
    std::string name;
    std::string next_name;
};

/// Engine timeline (player 0x433e00): mode 0 holds the last frame, mode 1
/// wraps to frame 0; negative time clamps to 0.
std::optional<SooPose> soo_pose_at(const Soo& soo, int ms, std::string& error);

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
