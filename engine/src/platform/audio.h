#pragma once

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

} // namespace gm
