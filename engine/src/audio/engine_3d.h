// The engine's 3D audio maths, isolated from the mixer (`M7.3`).
//
// Proven from `gundx.exe` 1.36 (docs/AUDIO_3D.md): the distance is *not*
// Euclidean, the gain is a linear attenuation shifted by the listener sector's
// "muffle" bits, and the pan is a dot product with the listener's right axis.
// The mixer (`audio/mixer.h`) still uses its own linear rolloff; these
// functions are the exact formulas so it can be switched over without
// re-deriving them.
//
// Everything is `double`, matching the Python reference
// `tools/re/audio_3d.py` bit for bit on the pinned vectors.
#pragma once

#include <array>
#include <optional>

namespace gm::audio {

/// `0x4344df`: `max(|.|) + 0.25 * (mid + min)` — the engine's cheap distance.
double length_approx(double dx, double dy, double dz);

/// `0x46bd84`: distances below one unit are treated as one (`[0x4a2628]`).
double clamped_distance(double dist);

/// Gain of one queue entry (`0x46bda4..0x46bdcd`):
/// `min(trunc(volume - atten*2^n*dist) >> 2n, volume)`, where `n` is the count
/// of the listener sector's flag bits `0x2`/`0x4` (`0x46bce5`/`0x46bcec`).
/// `<= 0` means "not audible"; the request is only created for `gain >= 2`.
int gain_of(int volume, double atten, double dist, int muffle = 0);

/// `0x46bddd..0x46be45`: `clamp(trunc((dot(d, right)/dist + 1) * 128), 0, 255)`;
/// a source closer than one unit sits at the centre (128).
int pan_of(double dx, double dy, double dz, const std::array<double, 3>& right,
           double dist);

/// `0x40be4a`: `v = (gain * 32) >> 6`, clamped to 32; `0` = no request.
int request_volume(int gain);

struct ChannelGains {
    int left = 1;
    int right = 1;
};

/// `0x40a4b0`: 5-bit L/R from the request volume and pan, each clamped to
/// `1..32` (the shipped pan 159 with volume 29 gives 23/32, not 23/38).
std::optional<ChannelGains> channel_gains(int volume_index, int pan);

/// Column `e1` (right) of the sound-listener basis `0x41e025`
/// (`R = Rz(-yaw) * Rx(-pitch) * Ry(roll)`, docs/CAMERA.md §3).  The engine
/// fills it from the player's chassis angles, not from the view pitch.
std::array<double, 3> basis_right(double roll, double pitch, double yaw);

} // namespace gm::audio
