#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gm {

/// Streaming audio device (SDL3) with a headless no-op backend.
///
/// The engine queues signed 16-bit interleaved PCM; the device owns the stream
/// and the OS mixer.  Headless mode only counts the queued bytes, which keeps
/// the playback/decoder paths testable without an audio device.
class AudioDevice {
public:
    static std::optional<AudioDevice> open(int sample_rate, int channels, bool headless,
                                           std::string& error);

    ~AudioDevice();
    AudioDevice(AudioDevice&& other) noexcept;
    AudioDevice& operator=(AudioDevice&& other) noexcept;
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    /// Queue *frames* frames of interleaved S16 samples.
    void queue(const std::int16_t* samples, std::size_t frames);
    void pause(bool paused);
    std::uint64_t queued_bytes() const;
    bool headless() const;

private:
    struct Impl;
    explicit AudioDevice(Impl* impl);
    Impl* impl_ = nullptr;
};

/// Platform audio *output* with two backends: a real SDL3 device and a
/// deterministic offline/null sink.
///
/// Unlike `AudioDevice`, which only queues bytes into an SDL stream, this class
/// owns the producer/consumer boundary: signed 16-bit interleaved frames go in
/// through `push`, and the backend pulls them through a single-producer /
/// single-consumer ring.  The SDL3 audio thread calls `pull` from the stream
/// callback (no locks, no allocation, never waits on the device); the null
/// backend never touches SDL, so a test or a headless run drives `pull`
/// directly and gets bit-exact output.
///
/// A `PullSource` can be attached to synthesise frames whenever the ring runs
/// dry (for example `Mixer::mix`).  With a source, an empty ring is not an
/// underrun; without one, a short pull is padded with silence and counted.
///
/// The null backend is already running when `open` returns (there is no device
/// to start); the SDL backend opens paused and waits for `start`.
class AudioOutput {
public:
    enum class Backend : std::uint8_t {
        Auto, ///< SDL3 when it opens, otherwise the null sink.
        SDL,  ///< Require SDL3; `open` fails cleanly when no device/driver exists.
        Null, ///< Never touch SDL; deterministic, driven by the caller.
    };

    struct Config {
        int sample_rate = 22050; ///< 1000..192000 Hz.
        int channels = 2;        ///< 1 (mono) or 2 (stereo).
        int frames = 1024;       ///< Frames per device request / mix chunk (1..1<<20).
        Backend backend = Backend::Auto;
        /// Ring capacity in frames; 0 selects `8 * frames` (rounded up to a
        /// power of two).  Values above 1<<22 are rejected.
        int queue_frames = 0;
    };

    /// Optional producer called by `pull` for the frames the ring cannot
    /// supply.  It must write exactly `frames` interleaved samples.  Installed
    /// with `set_source` before `start`; with the SDL backend it runs on the
    /// audio thread, so it must not block (the software mixer does not).
    using PullSource = void (*)(void* user, std::int16_t* out, std::size_t frames);

    struct Stats {
        std::uint64_t frames = 0;    ///< Frames written by `pull` (including silence).
        std::uint64_t underruns = 0; ///< Pulls the ring + source could not satisfy.
        std::uint64_t overruns = 0;  ///< Frames dropped by `push` (ring full).
        std::string device;          ///< "null", "SDL: pipewire (…)", or "null (SDL: …)".
    };

    static std::optional<AudioOutput> open(const Config& config, std::string& error);

    ~AudioOutput();
    AudioOutput(AudioOutput&& other) noexcept;
    AudioOutput& operator=(AudioOutput&& other) noexcept;
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    /// Begin pulling.  The null backend is already running after `open`, so
    /// this is an idempotent no-op there; SDL resumes its device and returns
    /// false only if the device refuses.
    bool start();
    /// Stop pulling and pause the device.  Queued frames stay in the ring.
    void stop();

    /// While paused, `pull` emits silence and leaves the ring untouched.
    void pause(bool paused);

    /// 0..255, clamped.  255 is unity gain, 0 is silence.  Applied in `pull`.
    void set_volume(int volume);
    int volume() const;

    bool running() const;
    bool paused() const;

    void set_source(PullSource source, void* user = nullptr);

    /// Queue interleaved frames.  Returns the number accepted; the rest is
    /// dropped and counted in `Stats::overruns`.  May be called while stopped.
    std::size_t push(const std::int16_t* samples, std::size_t frames);

    /// Emit `frames` interleaved frames.  Never blocks, never allocates and
    /// never throws.  `out == nullptr` or `frames == 0` is a no-op.  A short
    /// ring is padded with silence (or filled by the source) and counted as a
    /// single underrun.  Outside `start`/while paused the call is silence.
    void pull(std::int16_t* out, std::size_t frames);

    std::size_t capacity() const; ///< Ring capacity in frames.
    std::size_t queued() const;   ///< Frames waiting in the ring.

    Backend backend() const;          ///< The backend actually in use.
    const char* backend_name() const; ///< "sdl" or "null".
    const Config& config() const;
    Stats stats() const;

private:
    struct Impl;
    explicit AudioOutput(Impl* impl);
    Impl* impl_ = nullptr;
};

} // namespace gm
