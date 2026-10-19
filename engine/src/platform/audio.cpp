#include "platform/audio.h"

#include "core/log.h"

#include <cstddef>
#include <utility>

#ifndef GM_HAVE_SDL3
#define GM_HAVE_SDL3 0
#endif

#if GM_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace gm {

struct AudioDevice::Impl {
    bool headless = false;
    int sample_rate = 0;
    int channels = 0;
    std::uint64_t queued = 0;

#if GM_HAVE_SDL3
    SDL_AudioStream* stream = nullptr;
#endif
};

std::optional<AudioDevice> AudioDevice::open(int sample_rate, int channels, bool headless,
                                             std::string& error) {
    if (sample_rate <= 0 || channels <= 0 || channels > 8) {
        error = "audio: bad format " + std::to_string(sample_rate) + " Hz / " +
                std::to_string(channels) + " ch";
        return std::nullopt;
    }
    auto* impl = new Impl();
    impl->headless = headless;
    impl->sample_rate = sample_rate;
    impl->channels = channels;

    if (headless) {
        return AudioDevice(impl);
    }
#if GM_HAVE_SDL3
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error = std::string("SDL_InitSubSystem(audio): ") + SDL_GetError();
        delete impl;
        return std::nullopt;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = channels > 2 ? 2 : channels;
    spec.freq = sample_rate;
    impl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                             nullptr);
    if (impl->stream == nullptr) {
        error = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
        delete impl;
        return std::nullopt;
    }
    SDL_ResumeAudioStreamDevice(impl->stream);
    logf(LogLevel::Info, "audio", sample_rate, " Hz ", spec.channels, " ch");
    return AudioDevice(impl);
#else
    error = "built without SDL3; use --headless";
    delete impl;
    return std::nullopt;
#endif
}

AudioDevice::AudioDevice(Impl* impl) : impl_(impl) {}

AudioDevice::~AudioDevice() {
    if (impl_ == nullptr) {
        return;
    }
#if GM_HAVE_SDL3
    if (impl_->stream != nullptr) {
        SDL_DestroyAudioStream(impl_->stream);
    }
#endif
    delete impl_;
}

AudioDevice::AudioDevice(AudioDevice&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

AudioDevice& AudioDevice::operator=(AudioDevice&& other) noexcept {
    if (this != &other) {
        std::swap(impl_, other.impl_);
    }
    return *this;
}

void AudioDevice::queue(const std::int16_t* samples, std::size_t frames) {
    if (impl_ == nullptr || samples == nullptr || frames == 0) {
        return;
    }
    const std::size_t bytes = frames * static_cast<std::size_t>(impl_->channels) * 2;
    impl_->queued += bytes;
#if GM_HAVE_SDL3
    if (!impl_->headless && impl_->stream != nullptr) {
        SDL_PutAudioStreamData(impl_->stream, samples, static_cast<int>(bytes));
    }
#else
    (void)bytes;
#endif
}

void AudioDevice::pause(bool paused) {
    if (impl_ == nullptr) {
        return;
    }
#if GM_HAVE_SDL3
    if (!impl_->headless && impl_->stream != nullptr) {
        if (paused) {
            SDL_PauseAudioStreamDevice(impl_->stream);
        } else {
            SDL_ResumeAudioStreamDevice(impl_->stream);
        }
    }
#else
    (void)paused;
#endif
}

std::uint64_t AudioDevice::queued_bytes() const {
    return impl_ != nullptr ? impl_->queued : 0;
}

bool AudioDevice::headless() const {
    return impl_ != nullptr && impl_->headless;
}

} // namespace gm
