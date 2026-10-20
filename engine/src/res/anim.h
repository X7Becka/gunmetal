#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// Engine clock rate: the main loop sleeps ~11 ms and the level clock counts
/// 91 ticks per second (0x407504 / 0x44db8d); .soa and .soo share it.
constexpr int kEngineTickHz = 91;

inline int ms_to_ticks(int ms) {
    return ms <= 0 ? 0 : static_cast<int>(static_cast<long long>(ms) * kEngineTickHz / 1000);
}
inline double ticks_to_ms(int ticks) {
    return static_cast<double>(ticks) * 1000.0 / kEngineTickHz;
}

struct SooFrame {
    std::string name;
    int duration = 0; // engine ticks, not milliseconds
};

/// Model animation (``Sobanim/*.soo``) — plain text, see docs/FORMATS.md §8.1.
struct Soo {
    int mode = 0;
    int declared = 0;
    std::vector<SooFrame> frames;
};

/// One instant of playback, mirroring gm/soo.py's ``Pose``.  ``elapsed``,
/// ``phase`` and ``total`` are 91 Hz ticks; ``alpha`` is the morph fraction.
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
/// wraps to frame 0; negative time clamps to 0.  ``ms`` is converted to ticks.
std::optional<SooPose> soo_pose_at(const Soo& soo, int ms, std::string& error);

/// Raw-tick lookup used by the engine; ``tick``/``start_tick`` are 91 Hz.
std::optional<SooPose> soo_pose_at_tick(const Soo& soo, int tick,
                                        int start_tick, std::string& error);

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

/// One instant of a .soa animation (player 0x41de7d).
struct SoaPose {
    int index = 0;
    std::string name;
    int tick = 0;
    int period_ticks = 0; // 0 for the special clock/film modes
    bool loop = false;    // mode == 1
    int mode = 0;
};

/// Frame active ``ms`` milliseconds after the animation started.  ``period``
/// is centiseconds and becomes ``period*91/100`` ticks; modes 100/101/102
/// always select frame 0.  Negative time and a non-positive period are
/// rejected (the engine has no clamp and divides by the period).
std::optional<SoaPose> soa_frame_at(const Soa& soa, int ms, std::string& error);

} // namespace gm
