// Music playback model (ROADMAP M7.5): one SOU2 track, linear amplitude
// fades and a crossfade between two tracks, on top of the already decoded
// `gm::PcmClip` (`res/sou2.h`) and the pure-DSP part of `audio/mixer.h`.
//
// Why a separate module: `main.cpp --music` currently queues a whole `.sog`
// playlist into the platform device at a constant level, so the transitions
// and the music gain live in the CLI, not in a testable place.  This class is
// deliberately standalone and platform-free:
//
//   * no SDL, no PulseAudio, no threads, no locks, no I/O — only `std::vector`
//     and arithmetic, so the tests are deterministic;
//   * no allocation in `advance()`/`mix_into()` (the clip is copied once by
//     `load`/`crossfade_to`);
//   * 32-bit float output, interleaved, additive — the caller owns the final
//     clip to int16 and the device.
//
// Gain law (the model, not a recovered original curve): all fades are linear
// in *amplitude*, sampled once per output frame.
//
//     fade_in(N):   gain(k) = k / N          k = 1..N
//     fade_out(N):  gain(k) = 1 - k / N
//     crossfade(N): old(k) = 1 - k / N, new(k) = k / N
//
// A linear crossfade keeps the two amplitude weights summing to one.  For
// source material in [-1, 1] the mixed contribution then stays inside [-1, 1]
// (it is a convex combination), and `old^2 + new^2 <= 1`; an equal-power
// (sin/cos) law would reach a peak of sqrt(2) on correlated tracks and clip.
// The audible price is the usual -3 dB dip at the middle of the fade.
//
// `N = max(1, round(seconds * sample_rate))` whole output frames, so the ramp
// is sample-accurate in the module's own clock but not tied to the original
// 91 Hz game tick (`docs/AUDIO_MIXER.md`, `docs/MUSIC.md`).

#pragma once

#include "res/sou2.h" // gm::PcmClip

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gm::audio {

/// One music track with a master gain, a fade envelope and an optional
/// crossfade to the next track.
///
/// The class owns a copy of the interleaved 16-bit samples, so the `PcmClip`
/// may be discarded after `load`.  `sample_rate` in `load` is the *output*
/// rate (the clock of `advance`/`mix_into`); the clip's own `sample_rate` is
/// the source rate and is resampled linearly when the two differ.
///
/// SOU2 note (`docs/SOU2.md` §2): the original music mixer always runs at
/// 32000 Hz and ignores the container's `sample_rate` field, which is 16000
/// for `L01BONUS`/`LOADSCRN`.  Use `load_at(clip, 32000, rate, ...)` for those
/// tracks; plain `load` is the literal "clip header is the source rate" form.
///
/// Not thread safe: one track belongs to one audio thread, like `Mixer`.
class MusicTrack {
public:
    /// Output-clock bounds.  The lower bound is 1 because `advance` is also
    /// used as a bare tick clock in tests; the platform device only accepts
    /// the `Mixer` range (1000..192000).
    static constexpr int kMinRate = 1;
    static constexpr int kMaxRate = 192000;

    /// Source bounds, the same as `mixer.cpp`'s `kMinRate`/`kMaxRate`.
    static constexpr int kMinSourceRate = 1000;
    static constexpr int kMaxSourceRate = 384000;

    MusicTrack() = default;

    // ---- loading -------------------------------------------------------
    /// Take a copy of `clip` and use `clip.sample_rate` as the source rate.
    /// `sample_rate` is the output rate.  Returns false and fills
    /// `*error` (when not null) on a bad rate, channel count, empty clip or
    /// a payload that is not a whole number of frames; the previous track is
    /// then left untouched.  `error` may be null.
    bool load(const PcmClip& clip, int sample_rate, std::string* error = nullptr);

    /// Same, but the source rate is given explicitly — the shape needed by
    /// the SOU2 music path, where playback is always 32000 Hz.
    bool load_at(const PcmClip& clip, int source_rate, int sample_rate,
                 std::string* error = nullptr);

    // ---- transport -----------------------------------------------------
    /// Start from frame 0 at envelope 1 (master gain unchanged).  A previous
    /// fade is discarded: call `fade_in()` after `play()` for a ramp.
    /// Returns false (and sets `last_error()`) if nothing is loaded.
    bool play();

    /// Silence immediately.  The clip stays loaded; `finished()` becomes
    /// false because this is a deliberate stop, not the end of the track.
    void stop();
    bool playing() const { return playing_; }

    /// True once playback has been active and there is nothing left to hear:
    /// the clip reached its end or the fade-out gate closed.  `play()` resets
    /// it, so a finished track can be replayed from frame 0.
    bool finished() const { return finished_; }

    // ---- gain ----------------------------------------------------------
    /// Master gain, 0..1 (clamped).  NaN/inf is rejected: the value is left
    /// unchanged and `last_error()` explains why.
    void set_gain(double gain);

    /// Current *effective* gain: master x fade envelope of the current track
    /// (0 outside playback).  This is the number the tests pin.
    double gain() const;

    /// The value passed to `set_gain`, independent of the envelope.
    double master_gain() const { return master_gain_; }

    /// Linear amplitude ramp 0 -> 1 over `seconds` (0 = immediate).  A
    /// negative or non-finite duration is rejected.  Restarts the envelope
    /// from zero if it was part-way through another fade.  Called before
    /// `play()`, the duration is remembered and applied by `play()`, so
    /// `load -> fade_in -> play` works.
    void fade_in(double seconds);

    /// Linear amplitude ramp from the current envelope to 0 over `seconds`
    /// (0 = immediate silence).  Negative/NaN is rejected.
    void fade_out(double seconds);

    // ---- clock / rendering --------------------------------------------
    /// Move the position and the envelopes by `frames` output frames without
    /// producing samples.  Negative counts are rejected.  No-op when not
    /// playing.  `mix_into()` advances the same clock, so callers use one or
    /// the other for a given span, never both.
    void advance(int frames);

    /// Add the current track (and the outgoing one during a crossfade) into
    /// `out` and advance by the same number of frames.  `channels` is 1
    /// (mono: stereo sources are averaged) or 2 (interleaved).  Returns the
    /// number of frames written, or 0 when not playing / no data / bad
    /// `channels`; `out` is then untouched.  Samples are added, never
    /// overwritten, and are not clamped: the module guarantees only that a
    /// mono/stereo source in [-1, 1] with gain <= 1 contributes at most
    /// [-1, 1] per track sum.
    int mix_into(std::span<float> out, int channels);

    // ---- crossfade -----------------------------------------------------
    /// Start `next` from frame 0 while the current track ramps to zero over
    /// `seconds` (linear amplitude, weights sum to one).  `next`'s source
    /// rate is its own `PcmClip::sample_rate`; see `crossfade_to_at` for the
    /// SOU2 32000 Hz rule.  Requires playback to be active; a second call
    /// during a fade drops the previous outgoing track and fades from the
    /// current one.  Failures are reported through `last_error()`.
    void crossfade_to(const PcmClip& next, double seconds);
    bool crossfade_to_at(const PcmClip& next, int source_rate, double seconds);

    struct CrossfadeInfo {
        bool active = false;
        double progress = 0.0;   // incoming envelope 0..1
        double old_gain = 0.0;   // outgoing envelope, master included
        double new_gain = 0.0;   // incoming envelope, master included
    };
    /// Introspection for the CLI/tests; the module does not depend on it.
    CrossfadeInfo crossfade() const;

    // ---- introspection -------------------------------------------------
    int sample_rate() const { return out_rate_; }
    /// Frames in the current clip (per channel), 0 before `load`.
    std::uint64_t frames() const;
    /// Current clip frames / source rate, or 0 before `load`.
    double duration_seconds() const;
    /// Position of the current track in source frames (fractional while
    /// resampling).
    double position_frames() const;
    double position_seconds() const;
    /// Why the last operation failed; cleared by a successful one.  This is
    /// the channel used by the `void` mutators (`set_gain`, `fade_*`).
    const std::string& last_error() const { return last_error_; }

private:
    struct Slot {
        std::vector<std::int16_t> samples; // interleaved copy
        int channels = 0;
        int source_rate = 0;
        double pos = 0.0;  // source frames, fractional
        double step = 1.0; // source frames per output frame
        double env = 1.0;  // envelope when not ramping
        bool ramping = false;
        double env_from = 0.0;
        double env_to = 1.0;
        std::int64_t ramp_total = 0;
        std::int64_t ramp_done = 0;
        bool active = false;

        std::uint64_t frames() const {
            return channels <= 0 ? 0
                                 : static_cast<std::uint64_t>(samples.size() /
                                                              static_cast<std::size_t>(channels));
        }
        double current_env() const {
            if (!ramping) {
                return env;
            }
            if (ramp_total <= 0) {
                return env_to;
            }
            const double t = static_cast<double>(ramp_done) /
                             static_cast<double>(ramp_total);
            return env_from + (env_to - env_from) * t;
        }
    };

    bool fill_slot(Slot& slot, const PcmClip& clip, int source_rate, std::string& error) const;
    void begin_ramp(Slot& slot, double from, double to, double seconds);
    void step_one();
    void step_slot(Slot& slot);
    float sample_at(const Slot& slot, std::uint64_t frame, int channel) const;
    bool fail(const std::string& message, std::string* error);

    Slot current_;
    Slot outgoing_;
    int out_rate_ = 0;
    double master_gain_ = 1.0;
    bool playing_ = false;
    bool finished_ = false;
    bool crossfading_ = false;
    double pending_fade_in_ = -1.0; // seconds, applied by play()
    std::string last_error_;
};

} // namespace gm::audio
