#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace gm::audio {
namespace {

constexpr std::uint32_t kMinRate = 1000;
constexpr std::uint32_t kMaxRate = 384000;
constexpr std::uint32_t kMixerMinRate = 1000;
constexpr std::uint32_t kMixerMaxRate = 192000;
constexpr std::uint32_t kSou1SlotRate = 32000;
constexpr std::int32_t kQ15 = 32768;

float finite_or(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

Vec3 finite_vec(Vec3 v) {
    v.x = finite_or(v.x, 0.0f);
    v.y = finite_or(v.y, 0.0f);
    v.z = finite_or(v.z, 0.0f);
    return v;
}

bool valid_rate(std::uint32_t rate) { return rate >= kMinRate && rate <= kMaxRate; }

std::int16_t clamp16(std::int64_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}

std::int32_t sample_at(const SoundRef& sound, std::uint32_t frame, int channel) {
    const std::size_t index =
        static_cast<std::size_t>(frame) * sound.channels + static_cast<std::size_t>(channel);
    switch (sound.format) {
    case SampleFormat::U8:
        return (static_cast<std::int32_t>(static_cast<const std::uint8_t*>(sound.data)[index]) -
                128) *
               256;
    case SampleFormat::S16:
        return static_cast<const std::int16_t*>(sound.data)[index];
    case SampleFormat::S32: {
        const std::int32_t shift = std::clamp<std::int32_t>(sound.shift, 0, 31);
        const auto raw = static_cast<std::int64_t>(
            static_cast<const std::int32_t*>(sound.data)[index]);
        // C++20 defines >> on a negative signed value as an arithmetic shift,
        // matching the manifest's `clamp(value >> 5)`.
        const std::int64_t scaled = raw >> shift;
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, -32768, 32767));
    }
    }
    return 0;
}

/// Linear interpolation of one channel at the 32.32 cursor ``pos``.
///
/// The second tap is the next frame, the first frame again across a loop
/// boundary, or the same sample at the end of a non-looping sound (a hold, not
/// a fade to zero — this keeps the last frame's value exact).
std::int32_t interpolate(const SoundRef& sound, std::uint64_t pos, int channel, bool loop) {
    const std::uint32_t frame = static_cast<std::uint32_t>(pos >> 32);
    const std::int32_t frac = static_cast<std::int32_t>((pos >> 16) & 0xFFFFu);
    const std::int32_t first = sample_at(sound, frame, channel);
    std::int32_t second = first;
    if (frame + 1 < sound.frames) {
        second = sample_at(sound, frame + 1, channel);
    } else if (loop) {
        second = sample_at(sound, 0, channel);
    }
    return first + static_cast<std::int32_t>(
                       (static_cast<std::int64_t>(second - first) * frac) >> 16);
}

std::int32_t q15(float gain) {
    const long value = std::lround(std::clamp(gain, 0.0f, Mixer::kMaxGain) *
                                   static_cast<float>(kQ15));
    return static_cast<std::int32_t>(std::clamp<long>(value, 0, 4L * kQ15));
}

} // namespace

std::optional<Sound> make_sound_u8(std::span<const std::uint8_t> pcm,
                                   std::uint32_t sample_rate, int channels,
                                   std::string& error) {
    if (channels < 1 || channels > 2) {
        error = "SOU0 adapter: channels must be 1 or 2";
        return std::nullopt;
    }
    if (!valid_rate(sample_rate)) {
        error = "SOU0 adapter: bad sample rate " + std::to_string(sample_rate);
        return std::nullopt;
    }
    if (pcm.size() % static_cast<std::size_t>(channels) != 0) {
        error = "SOU0 adapter: payload is not a whole number of frames";
        return std::nullopt;
    }
    Sound sound;
    sound.sample_rate = sample_rate;
    sound.channels = static_cast<std::uint8_t>(channels);
    sound.samples.resize(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        sound.samples[i] = static_cast<std::int16_t>(
            (static_cast<std::int32_t>(pcm[i]) - 128) * 256);
    }
    return sound;
}

std::optional<Sound> make_sound(const Sou1Sound& sound, std::string& error, int shift) {
    if (sound.channels < 1 || sound.channels > 2) {
        error = "SOU1 adapter: channels must be 1 or 2";
        return std::nullopt;
    }
    if (!valid_rate(sound.sample_rate)) {
        error = "SOU1 adapter: bad source rate " + std::to_string(sound.sample_rate);
        return std::nullopt;
    }
    if (shift < 0 || shift > 31) {
        error = "SOU1 adapter: shift out of range";
        return std::nullopt;
    }
    if (kSou1SlotRate % sound.sample_rate != 0) {
        error = "SOU1 adapter: source rate " + std::to_string(sound.sample_rate) +
                " does not divide the 32 kHz slot rate";
        return std::nullopt;
    }
    const std::uint32_t copies = kSou1SlotRate / sound.sample_rate;
    const std::size_t channels = static_cast<std::size_t>(sound.channels);
    if (sound.samples.size() % (channels * copies) != 0) {
        error = "SOU1 adapter: slot count is not a whole number of source frames";
        return std::nullopt;
    }

    const std::size_t frames = sound.samples.size() / (channels * copies);
    Sound out;
    out.sample_rate = sound.sample_rate;
    out.channels = sound.channels;
    out.samples.resize(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            // Slots for one source frame are stored copy-major and interleaved;
            // the first copy is the source value, the rest are the 32 kHz
            // duplication of the 8/16 kHz variants.
            const std::size_t slot = frame * copies * channels + channel;
            // C++20 >> on a negative signed value is an arithmetic shift, the
            // same `clamp(value >> shift)` convention as the manifest.
            const std::int64_t scaled = sound.samples[slot] >> shift;
            out.samples[frame * channels + channel] = clamp16(scaled);
        }
    }
    return out;
}

std::optional<Sound> make_sound(const PcmClip& clip, std::string& error) {
    if (clip.channels < 1 || clip.channels > 2) {
        error = "SOU2 adapter: channels must be 1 or 2";
        return std::nullopt;
    }
    if (!valid_rate(static_cast<std::uint32_t>(clip.sample_rate))) {
        error = "SOU2 adapter: bad sample rate " + std::to_string(clip.sample_rate);
        return std::nullopt;
    }
    if (clip.samples.size() % static_cast<std::size_t>(clip.channels) != 0) {
        error = "SOU2 adapter: sample count is not a whole number of frames";
        return std::nullopt;
    }
    Sound sound;
    sound.sample_rate = static_cast<std::uint32_t>(clip.sample_rate);
    sound.channels = static_cast<std::uint8_t>(clip.channels);
    sound.samples = clip.samples;
    return sound;
}

bool Mixer::init(std::uint32_t sample_rate) {
    if (sample_rate < kMixerMinRate || sample_rate > kMixerMaxRate) {
        return false;
    }
    sample_rate_ = sample_rate;
    listener_ = {};
    for (Voice& voice : voices_) {
        voice = {};
    }
    stats_ = {};
    next_id_ = 1;
    return true;
}

int Mixer::play(const SoundRef& sound, const PlayParams& params) {
    if (sample_rate_ == 0 || sound.data == nullptr || sound.frames == 0 ||
        !valid_rate(sound.sample_rate) || sound.channels < 1 || sound.channels > 2) {
        return -1;
    }
    if (sound.format != SampleFormat::U8 && sound.format != SampleFormat::S16 &&
        sound.format != SampleFormat::S32) {
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!voices_[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ++stats_.voices_dropped;
        return -1;
    }

    Voice& voice = voices_[slot];
    voice = {};
    voice.source = sound;
    voice.source.shift = std::clamp<std::int32_t>(sound.shift, 0, 31);
    voice.loop = params.loop;
    voice.positional = params.position.has_value();
    voice.base_gain = std::clamp(finite_or(params.gain, 0.0f), 0.0f, kMaxGain);
    voice.base_pan = std::clamp(finite_or(params.pan, 0.0f), -1.0f, 1.0f);
    voice.position = finite_vec(params.position.value_or(Vec3{}));
    voice.velocity = finite_vec(params.velocity.value_or(Vec3{}));
    voice.step = (static_cast<std::uint64_t>(sound.sample_rate) << 32) / sample_rate_;
    if (voice.step == 0) {
        voice.step = 1;
    }
    voice.active = true;
    voice.id = next_id_++;
    if (next_id_ == 0) {
        next_id_ = 1;
    }
    ++stats_.voices_started;
    stats_.active = static_cast<std::uint32_t>(voices());
    stats_.max_active = std::max(stats_.max_active, stats_.active);
    return static_cast<int>(voice.id);
}

bool Mixer::stop(int id) {
    if (id <= 0) {
        return false;
    }
    for (Voice& voice : voices_) {
        if (voice.active && voice.id == static_cast<std::uint32_t>(id)) {
            finish(voice, false);
            ++stats_.voices_stopped;
            stats_.active = static_cast<std::uint32_t>(voices());
            return true;
        }
    }
    return false;
}

void Mixer::stop_all() {
    for (Voice& voice : voices_) {
        if (voice.active) {
            finish(voice, false);
            ++stats_.voices_stopped;
        }
    }
    stats_.active = static_cast<std::uint32_t>(voices());
}

void Mixer::set_listener(const Listener& listener) {
    listener_.position = finite_vec(listener.position);
    listener_.yaw = finite_or(listener.yaw, 0.0f);
}

void Mixer::finish(Voice& voice, bool natural) {
    if (!voice.active) {
        return;
    }
    voice.active = false;
    voice.pos = 0;
    if (natural) {
        ++stats_.voices_finished;
    }
}

void Mixer::update_gains() {
    const float sin_yaw = std::sin(listener_.yaw);
    const float cos_yaw = std::cos(listener_.yaw);
    const float right_x = cos_yaw;
    const float right_z = -sin_yaw;

    for (Voice& voice : voices_) {
        if (!voice.active) {
            continue;
        }
        float gain = voice.base_gain;
        float pan = voice.base_pan;
        if (voice.positional) {
            const float dx = voice.position.x - listener_.position.x;
            const float dy = voice.position.y - listener_.position.y;
            const float dz = voice.position.z - listener_.position.z;
            const float distance =
                std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > 1e-4f) {
                const float clamped = std::min(distance, kMaxDistance);
                const float falloff =
                    kRefDistance /
                    (kRefDistance + kRolloff * std::max(0.0f, clamped - kRefDistance));
                gain *= falloff;
                // Pan only uses the horizontal plane: a source above or below
                // the listener stays centred.
                pan += (dx * right_x + dz * right_z) / distance;
            }
        }
        pan = std::clamp(pan, -1.0f, 1.0f);
        voice.gain_l = q15(gain * (1.0f - std::max(0.0f, pan)));
        voice.gain_r = q15(gain * (1.0f + std::min(0.0f, pan)));
    }
}

void Mixer::mix(std::int16_t* out, std::size_t frames) {
    if (out == nullptr || frames == 0 || sample_rate_ == 0) {
        return;
    }
    update_gains();

    for (std::size_t n = 0; n < frames; ++n) {
        std::int64_t acc_l = 0;
        std::int64_t acc_r = 0;
        bool any = false;

        // Fixed slot order: determinism does not depend on start/stop history.
        for (Voice& voice : voices_) {
            if (!voice.active) {
                continue;
            }
            // Safety net for a cursor that landed past the end without being
            // finished by the previous block.
            if (static_cast<std::uint32_t>(voice.pos >> 32) >= voice.source.frames) {
                finish(voice, true);
                continue;
            }

            any = true;
            std::int32_t left = 0;
            std::int32_t right = 0;
            if (voice.source.channels == 1) {
                left = right = interpolate(voice.source, voice.pos, 0, voice.loop);
            } else {
                left = interpolate(voice.source, voice.pos, 0, voice.loop);
                right = interpolate(voice.source, voice.pos, 1, voice.loop);
            }
            acc_l += static_cast<std::int64_t>(left) * voice.gain_l;
            acc_r += static_cast<std::int64_t>(right) * voice.gain_r;

            voice.pos += voice.step;
            if (voice.loop) {
                const std::uint64_t length =
                    static_cast<std::uint64_t>(voice.source.frames) << 32;
                if (voice.pos >= length) {
                    voice.pos %= length;
                }
            } else if (static_cast<std::uint32_t>(voice.pos >> 32) >= voice.source.frames) {
                finish(voice, true);
            }
        }

        // 0x8610d0: the original rounds before the final shift
        // (`(acc + 0x10) >> 5`) instead of truncating.  In this fixed-point
        // layout the accumulator is Q15, so half an LSB is 1 << 14.
        constexpr std::int64_t kRound = 1 << 14;
        std::int64_t left = (acc_l + kRound) >> 15;
        std::int64_t right = (acc_r + kRound) >> 15;
        if (left > 32767) {
            left = 32767;
            ++stats_.clipped;
        } else if (left < -32768) {
            left = -32768;
            ++stats_.clipped;
        }
        if (right > 32767) {
            right = 32767;
            ++stats_.clipped;
        } else if (right < -32768) {
            right = -32768;
            ++stats_.clipped;
        }
        out[2 * n] = static_cast<std::int16_t>(left);
        out[2 * n + 1] = static_cast<std::int16_t>(right);

        if (!any || (left == 0 && right == 0)) {
            ++stats_.frames_silent;
        }
        const std::uint32_t peak = static_cast<std::uint32_t>(
            std::max<std::int64_t>(left < 0 ? -left : left, right < 0 ? -right : right));
        stats_.peak = std::max(stats_.peak, peak);
    }

    stats_.frames += frames;
    std::uint32_t active = 0;
    for (const Voice& voice : voices_) {
        active += voice.active ? 1u : 0u;
    }
    stats_.active = active;
    stats_.max_active = std::max(stats_.max_active, active);
}

int Mixer::voices() const {
    int count = 0;
    for (const Voice& voice : voices_) {
        count += voice.active ? 1 : 0;
    }
    return count;
}

} // namespace gm::audio
