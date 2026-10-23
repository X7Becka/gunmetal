#include "audio/engine_3d.h"

#include <algorithm>
#include <cmath>

namespace gm::audio {
namespace {

int trunc_to_int(double value) {
    // `ftol`/`fistp` with the default round-to-nearest would differ; the engine
    // runs these stores with RC=truncate (docs/AUDIO_3D.md §2.2), and so does
    // Python's `int()`.
    return static_cast<int>(value);
}

} // namespace

double length_approx(double dx, double dy, double dz) {
    std::array<double, 3> a{std::fabs(dx), std::fabs(dy), std::fabs(dz)};
    std::sort(a.begin(), a.end(), std::greater<double>());
    return a[0] + (a[1] + a[2]) * 0.25; // [0x4a2248] = 0.25
}

double clamped_distance(double dist) {
    return dist >= 1.0 ? dist : 1.0;
}

int gain_of(int volume, double atten, double dist, int muffle) {
    const double scale = static_cast<double>(1 << muffle);
    const double t = atten * scale * clamped_distance(dist);
    int gain = trunc_to_int(static_cast<double>(volume) - t);
    if (muffle) {
        gain >>= 2 * muffle; // `sar` with cl = 2n
    }
    return std::min(gain, volume);
}

int pan_of(double dx, double dy, double dz, const std::array<double, 3>& right,
           double dist) {
    if (dist <= 1.0) {
        return 0x80; // 0x46be1a
    }
    const double dot = dx * right[0] + dy * right[1] + dz * right[2];
    const double pan = (dot / clamped_distance(dist) + 1.0) * 128.0;
    return std::clamp(trunc_to_int(pan), 0, 255);
}

int request_volume(int gain) {
    const int volume = (gain * 32) >> 6;
    return volume <= 0 ? 0 : std::min(volume, 32);
}

std::optional<ChannelGains> channel_gains(int volume_index, int pan) {
    if (volume_index <= 0) {
        return std::nullopt;
    }
    const int left = ((255 - pan) * volume_index + 0x3D) / 0x7A;
    const int right = (pan * volume_index + 0x3D) / 0x7A;
    return ChannelGains{std::clamp(left, 1, 32), std::clamp(right, 1, 32)};
}

std::array<double, 3> basis_right(double roll, double pitch, double yaw) {
    const double sA = std::sin(roll), cA = std::cos(roll);
    const double sB = std::sin(pitch), cB = std::cos(pitch);
    const double sC = std::sin(yaw), cC = std::cos(yaw);
    return {cC * cA - sC * sB * sA, -sC * cA - cC * sB * sA, -cB * sA};
}

} // namespace gm::audio
