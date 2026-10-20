#pragma once

#include "audio/sou1.h" // gm::audio::Sou1Sound (SOU1 slot stream)
#include "res/sou2.h"   // gm::PcmClip (SOU2 16-bit stereo)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm::audio {

/// A non-owning view of decoded sample data.
///
/// The mixer keeps only this view, so the buffer must outlive every voice that
/// plays it (``Sound::ref()`` over an owning ``Sound`` is the safe form).  All
/// three shipped codecs are representable without an intermediate copy:
///
///   SOU0  U8   raw unsigned 8-bit PCM, ``value - 128`` scaled by 256
///   SOU1  S32  32-bit mixer accumulators, ``clamp(value >> shift)``
///   SOU2  S16  already-normalised interleaved 16-bit PCM
///
/// ``frames`` is the number of frames per channel; ``data`` is interleaved
/// (``frames * channels`` samples).  ``sample_rate`` is the *source* rate
/// (8000/16000/22050/32000/…), not the mixer rate.
enum class SampleFormat : std::uint8_t { U8, S16, S32 };

struct SoundRef {
    const void* data = nullptr;
    std::uint32_t frames = 0;
    std::uint32_t sample_rate = 0;
    std::uint8_t channels = 1;
    SampleFormat format = SampleFormat::S16;
    /// S32 only: arithmetic right shift that maps the accumulator to int16.
    /// The SOU1 full scale on the shipped corpus is about 2^20, so 5 matches
    /// ``docs/manifests/sou1-fnv.json`` and ``res::sou1_to_wav``.
    std::int32_t shift = 5;
};

/// Owning canonical 16-bit sound, the result of the SOU0/SOU1/SOU2 adapters.
struct Sound {
    std::vector<std::int16_t> samples; // interleaved
    std::uint32_t sample_rate = 0;
    std::uint8_t channels = 1;

    std::uint32_t frames() const {
        return channels == 0 ? 0 : static_cast<std::uint32_t>(samples.size() / channels);
    }
    SoundRef ref() const { return SoundRef{samples.data(), frames(), sample_rate, channels}; }
};

/// SOU0 (raw unsigned 8-bit PCM) -> canonical S16.  ``pcm`` is interleaved.
std::optional<Sound> make_sound_u8(std::span<const std::uint8_t> pcm,
                                   std::uint32_t sample_rate, int channels,
                                   std::string& error);

/// SOU1 slot stream -> canonical S16 at the *source* rate.
///
/// ``Sou1Sound::samples`` are interleaved 32 kHz mixer slots that repeat every
/// source frame ``32000 / rate`` times; the adapter drops those duplicates
/// (1/2/4 copies for 32/16/8 kHz) and applies ``clamp(value >> shift)``.  The
/// result is ``Sou1Sound::sample_rate`` frames/s, which is what the linear
/// resampler expects.
std::optional<Sound> make_sound(const Sou1Sound& sound, std::string& error,
                                int shift = 5);

/// SOU2 (already-normalised 16-bit stereo/mono PCM) -> canonical S16.
std::optional<Sound> make_sound(const PcmClip& clip, std::string& error);

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// Listener pose.  ``yaw`` is in radians around +Y; forward is
/// ``(sin yaw, 0, cos yaw)`` and right is ``(cos yaw, 0, -sin yaw)``.
struct Listener {
    Vec3 position;
    float yaw = 0.0f;
};

/// Per-voice parameters.  ``position`` selects the 3D path: leaving it empty
/// plays a plain 2D sound with only ``pan``.  ``velocity`` is stored for the
/// caller (voice introspection) but no Doppler is applied — the original
/// shift at ``0x40a4b0``/``0x40a690`` is not traced (see docs/AUDIO_MIXER.md).
struct PlayParams {
    bool loop = false;
    float gain = 1.0f; // 0..kMaxGain, NaN/inf is treated as silence
    float pan = 0.0f;  // -1 left, 0 centre, +1 right
    std::optional<Vec3> position;
    std::optional<Vec3> velocity;
};

/// Deterministic software mixer: fixed 16-bit stereo output, 32-voice pool.
///
/// Everything the mixer needs is allocated by ``init``/``play``/the adapters;
/// ``mix`` performs no allocation and no exception-throwing operation.  Voices
/// are always summed in slot order 0..31, and the sample arithmetic is integer
/// (32.32 resampler, Q15 gains), so two runs over the same input are bit-exact.
class Mixer {
public:
    static constexpr int kMaxVoices = 32;
    static constexpr float kMaxGain = 4.0f;
    static constexpr float kRefDistance = 1.0f;
    static constexpr float kRolloff = 1.0f;
    static constexpr float kMaxDistance = 1000.0f;

    struct Stats {
        std::uint64_t frames = 0;          // frames written by mix()
        std::uint64_t frames_silent = 0;   // frames whose output was all zero
        std::uint64_t voices_started = 0;
        std::uint64_t voices_finished = 0; // reached the end (non-loop)
        std::uint64_t voices_stopped = 0;  // explicit stop()/stop_all()
        std::uint64_t voices_dropped = 0;  // play() found the pool full
        std::uint64_t clipped = 0;         // output samples clamped to int16
        std::uint32_t peak = 0;            // max |output sample| seen
        std::uint32_t active = 0;          // voices active right now
        std::uint32_t max_active = 0;      // high-water mark
    };

    /// Set the output rate (1000..192000 Hz).  Resets the pool, listener and
    /// statistics.  Returns false and leaves the mixer untouched on a bad rate.
    bool init(std::uint32_t sample_rate = 22050);

    bool ready() const { return sample_rate_ != 0; }
    std::uint32_t sample_rate() const { return sample_rate_; }

    /// Start a voice.  Returns a positive voice id, or -1 for a bad sound or a
    /// full pool.  The id stays valid until stop()/the sound finishes; ids are
    /// never reused, so a stale id cannot stop a newer voice.
    int play(const SoundRef& sound, const PlayParams& params = {});
    int play(const Sound& sound, const PlayParams& params = {}) {
        return play(sound.ref(), params);
    }

    bool stop(int voice);
    void stop_all();
    void set_listener(const Listener& listener);

    /// Write ``frames`` interleaved stereo frames.  ``out`` may be null (the
    /// call is then a no-op); ``frames == 0`` is a no-op.
    void mix(std::int16_t* out, std::size_t frames);

    int voices() const; // active voice count
    const Stats& stats() const { return stats_; }

private:
    struct Voice {
        SoundRef source;
        bool active = false;
        bool loop = false;
        bool positional = false;
        std::uint64_t pos = 0; // 32.32 source-frame cursor
        std::uint64_t step = 0; // 32.32 source frames per output frame
        std::int32_t gain_l = 0; // Q15
        std::int32_t gain_r = 0;
        float base_gain = 1.0f;
        float base_pan = 0.0f;
        Vec3 position;
        Vec3 velocity;
        std::uint32_t id = 0;
    };

    void update_gains();
    void finish(Voice& voice, bool natural);

    std::uint32_t sample_rate_ = 0;
    Listener listener_{};
    Voice voices_[kMaxVoices]{};
    Stats stats_{};
    std::uint32_t next_id_ = 1;
};

} // namespace gm::audio
