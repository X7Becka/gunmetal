#include "platform/audio.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#ifndef GM_HAVE_SDL3
#define GM_HAVE_SDL3 0
#endif

#if GM_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace gm {

// ---------------------------------------------------------------------------
// AudioDevice: append-only stream wrapper kept for the existing --music path.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// AudioOutput
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxFrames = std::size_t{1} << 20;      // per request / config
constexpr std::size_t kMaxQueueFrames = std::size_t{1} << 22; // 16 MiB stereo S16

bool valid_config(const AudioOutput::Config& config) {
    return config.sample_rate >= 1000 && config.sample_rate <= 192000 && config.channels >= 1 &&
           config.channels <= 2 && config.frames >= 1 &&
           static_cast<std::size_t>(config.frames) <= kMaxFrames && config.queue_frames >= 0 &&
           static_cast<std::size_t>(config.queue_frames) <= kMaxQueueFrames;
}

std::string config_error(const AudioOutput::Config& config) {
    return "audio output: bad config (" + std::to_string(config.sample_rate) + " Hz, " +
           std::to_string(config.channels) + " ch, " + std::to_string(config.frames) +
           " frames, queue " + std::to_string(config.queue_frames) + ")";
}

std::size_t ring_capacity(const AudioOutput::Config& config) {
    std::size_t frames = config.queue_frames > 0
                             ? static_cast<std::size_t>(config.queue_frames)
                             : static_cast<std::size_t>(config.frames) * 8;
    frames = std::min(frames, kMaxQueueFrames);
    frames = std::max<std::size_t>(frames, 1);
    return std::bit_ceil(frames);
}

} // namespace

struct AudioOutput::Impl {
    Config cfg{};
    Backend actual = Backend::Null;
    std::string device;
    bool sdl_initialized = false;

    std::size_t capacity = 0;
    std::size_t mask = 0;
    std::vector<std::int16_t> ring;

    std::atomic<std::uint64_t> read{0};  // monotonic frame cursor (consumer)
    std::atomic<std::uint64_t> write{0}; // monotonic frame cursor (producer)

    std::atomic<int> volume{255};
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> underruns{0};
    std::atomic<std::uint64_t> overruns{0};

    PullSource source = nullptr;
    void* source_user = nullptr;

#if GM_HAVE_SDL3
    SDL_AudioStream* stream = nullptr;
    std::vector<std::int16_t> scratch;
    std::size_t scratch_frames = 0;
    static void SDLCALL sdl_callback(void* user, SDL_AudioStream* stream, int additional_amount,
                                     int total_amount);
#endif

    void pull(std::int16_t* out, std::size_t frames);
};

void AudioOutput::Impl::pull(std::int16_t* out, std::size_t frames) {
    if (out == nullptr || frames == 0) {
        return;
    }
    const std::size_t channels = static_cast<std::size_t>(cfg.channels);
    if (frames > std::numeric_limits<std::size_t>::max() / (channels * sizeof(std::int16_t))) {
        return; // impossible request; refuse instead of wrapping the byte count
    }
    const std::size_t samples = frames * channels;

    if (!running.load(std::memory_order_acquire) || paused.load(std::memory_order_acquire)) {
        std::memset(out, 0, samples * sizeof(std::int16_t));
        this->frames.fetch_add(frames, std::memory_order_relaxed);
        return;
    }

    const std::uint64_t r = read.load(std::memory_order_relaxed);
    const std::uint64_t w = write.load(std::memory_order_acquire);
    std::size_t available = static_cast<std::size_t>(w - r);
    if (available > capacity) {
        available = capacity; // defensive: cursors are only moved by push/pull
    }
    const std::size_t take = std::min(frames, available);

    if (take > 0) {
        const std::size_t start = static_cast<std::size_t>(r) & mask;
        const std::size_t first = std::min(take, capacity - start);
        std::memcpy(out, ring.data() + start * channels, first * channels * sizeof(std::int16_t));
        if (take > first) {
            std::memcpy(out + first * channels, ring.data(),
                        (take - first) * channels * sizeof(std::int16_t));
        }
        read.store(r + take, std::memory_order_release);
    }

    if (take < frames) {
        if (source != nullptr) {
            source(source_user, out + take * channels, frames - take);
        } else {
            std::memset(out + take * channels, 0, (frames - take) * channels * sizeof(std::int16_t));
            this->underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const int gain = volume.load(std::memory_order_relaxed);
    if (gain <= 0) {
        std::memset(out, 0, samples * sizeof(std::int16_t));
    } else if (gain < 255) {
        for (std::size_t i = 0; i < samples; ++i) {
            out[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(out[i]) * gain) / 255);
        }
    }

    this->frames.fetch_add(frames, std::memory_order_relaxed);
}

#if GM_HAVE_SDL3
void SDLCALL AudioOutput::Impl::sdl_callback(void* user, SDL_AudioStream* stream,
                                             int additional_amount, int total_amount) {
    (void)total_amount;
    if (user == nullptr || stream == nullptr || additional_amount <= 0) {
        return;
    }
    auto* impl = static_cast<Impl*>(user);
    const std::size_t frame_bytes = static_cast<std::size_t>(impl->cfg.channels) * 2;
    std::size_t remaining = static_cast<std::size_t>(additional_amount) / frame_bytes;
    while (remaining > 0) {
        const std::size_t chunk = std::min(remaining, impl->scratch_frames);
        impl->pull(impl->scratch.data(), chunk);
        SDL_PutAudioStreamData(stream, impl->scratch.data(),
                               static_cast<int>(chunk * frame_bytes));
        remaining -= chunk;
    }
}
#endif

std::optional<AudioOutput> AudioOutput::open(const Config& config, std::string& error) {
    error.clear();
    if (!valid_config(config)) {
        error = config_error(config);
        return std::nullopt;
    }

    const std::size_t capacity = ring_capacity(config);

    auto make_null = [&](std::string device) -> std::optional<AudioOutput> {
        auto* impl = new Impl();
        impl->cfg = config;
        impl->actual = Backend::Null;
        impl->device = std::move(device);
        impl->capacity = capacity;
        impl->mask = capacity - 1;
        impl->ring.assign(capacity * static_cast<std::size_t>(config.channels), 0);
        // A null sink has no device to resume: the caller drives pull directly.
        impl->running.store(true, std::memory_order_relaxed);
        error.clear();
        return AudioOutput(impl);
    };

    if (config.backend == Backend::Null) {
        return make_null("null");
    }

#if GM_HAVE_SDL3
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const std::string reason = std::string("SDL_InitSubSystem(SDL_INIT_AUDIO): ") +
                                   SDL_GetError();
        if (config.backend == Backend::SDL) {
            error = "audio output: " + reason;
            return std::nullopt;
        }
        return make_null("null (" + reason + ")");
    }

    auto* impl = new Impl();
    impl->cfg = config;
    impl->actual = Backend::SDL;
    impl->capacity = capacity;
    impl->mask = capacity - 1;
    impl->ring.assign(capacity * static_cast<std::size_t>(config.channels), 0);
    impl->scratch_frames = std::min<std::size_t>(
        std::max<std::size_t>(static_cast<std::size_t>(config.frames), 256), 8192);
    impl->scratch.assign(impl->scratch_frames * static_cast<std::size_t>(config.channels), 0);
    impl->sdl_initialized = true;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = config.channels;
    spec.freq = config.sample_rate;
    impl->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                             &Impl::sdl_callback, impl);
    if (impl->stream == nullptr) {
        const std::string reason =
            std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
        delete impl;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        if (config.backend == Backend::SDL) {
            error = "audio output: " + reason;
            return std::nullopt;
        }
        return make_null("null (" + reason + ")");
    }

    std::string device = "SDL";
    if (const char* driver = SDL_GetCurrentAudioDriver(); driver != nullptr) {
        device += ": ";
        device += driver;
    }
    const SDL_AudioDeviceID device_id = SDL_GetAudioStreamDevice(impl->stream);
    if (device_id != 0) {
        if (const char* name = SDL_GetAudioDeviceName(device_id); name != nullptr) {
            device += " (";
            device += name;
            device += ")";
        }
    }
    impl->device = std::move(device);
    return AudioOutput(impl);
#else
    if (config.backend == Backend::SDL) {
        error = "audio output: built without SDL3 (GM_HAVE_SDL3=0)";
        return std::nullopt;
    }
    return make_null("null (built without SDL3)");
#endif
}

AudioOutput::AudioOutput(Impl* impl) : impl_(impl) {}

AudioOutput::~AudioOutput() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->running.store(false, std::memory_order_release);
#if GM_HAVE_SDL3
    if (impl_->stream != nullptr) {
        SDL_PauseAudioStreamDevice(impl_->stream);
        SDL_DestroyAudioStream(impl_->stream); // also waits for an in-flight callback
        impl_->stream = nullptr;
    }
    if (impl_->sdl_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#endif
    delete impl_;
}

AudioOutput::AudioOutput(AudioOutput&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {}

AudioOutput& AudioOutput::operator=(AudioOutput&& other) noexcept {
    if (this != &other) {
        std::swap(impl_, other.impl_);
    }
    return *this;
}

bool AudioOutput::start() {
    if (impl_ == nullptr) {
        return false;
    }
    impl_->paused.store(false, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
#if GM_HAVE_SDL3
    if (impl_->stream != nullptr && !SDL_ResumeAudioStreamDevice(impl_->stream)) {
        impl_->running.store(false, std::memory_order_release);
        return false;
    }
#endif
    return true;
}

void AudioOutput::stop() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->running.store(false, std::memory_order_release);
    impl_->paused.store(false, std::memory_order_release);
#if GM_HAVE_SDL3
    if (impl_->stream != nullptr) {
        SDL_PauseAudioStreamDevice(impl_->stream);
    }
#endif
}

void AudioOutput::pause(bool paused) {
    if (impl_ == nullptr) {
        return;
    }
    impl_->paused.store(paused, std::memory_order_release);
#if GM_HAVE_SDL3
    if (impl_->stream != nullptr) {
        if (paused) {
            SDL_PauseAudioStreamDevice(impl_->stream);
        } else if (impl_->running.load(std::memory_order_acquire)) {
            SDL_ResumeAudioStreamDevice(impl_->stream);
        }
    }
#endif
}

void AudioOutput::set_volume(int volume) {
    if (impl_ == nullptr) {
        return;
    }
    impl_->volume.store(std::clamp(volume, 0, 255), std::memory_order_release);
}

int AudioOutput::volume() const {
    return impl_ != nullptr ? impl_->volume.load(std::memory_order_acquire) : 0;
}

bool AudioOutput::running() const {
    return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

bool AudioOutput::paused() const {
    return impl_ != nullptr && impl_->paused.load(std::memory_order_acquire);
}

void AudioOutput::set_source(PullSource source, void* user) {
    if (impl_ == nullptr) {
        return;
    }
    impl_->source = source;
    impl_->source_user = user;
}

std::size_t AudioOutput::push(const std::int16_t* samples, std::size_t frames) {
    if (impl_ == nullptr || samples == nullptr || frames == 0) {
        return 0;
    }
    const std::size_t channels = static_cast<std::size_t>(impl_->cfg.channels);
    if (frames > std::numeric_limits<std::size_t>::max() / (channels * sizeof(std::int16_t))) {
        return 0;
    }

    const std::uint64_t w = impl_->write.load(std::memory_order_relaxed);
    const std::uint64_t r = impl_->read.load(std::memory_order_acquire);
    std::size_t used = static_cast<std::size_t>(w - r);
    if (used > impl_->capacity) {
        used = impl_->capacity;
    }
    const std::size_t space = impl_->capacity - used;
    const std::size_t take = std::min(frames, space);

    if (take > 0) {
        const std::size_t start = static_cast<std::size_t>(w) & impl_->mask;
        const std::size_t first = std::min(take, impl_->capacity - start);
        std::memcpy(impl_->ring.data() + start * channels, samples,
                    first * channels * sizeof(std::int16_t));
        if (take > first) {
            std::memcpy(impl_->ring.data(), samples + first * channels,
                        (take - first) * channels * sizeof(std::int16_t));
        }
        impl_->write.store(w + take, std::memory_order_release);
    }

    if (take < frames) {
        impl_->overruns.fetch_add(frames - take, std::memory_order_relaxed);
    }
    return take;
}

void AudioOutput::pull(std::int16_t* out, std::size_t frames) {
    if (impl_ != nullptr) {
        impl_->pull(out, frames);
    }
}

std::size_t AudioOutput::capacity() const {
    return impl_ != nullptr ? impl_->capacity : 0;
}

std::size_t AudioOutput::queued() const {
    if (impl_ == nullptr) {
        return 0;
    }
    const std::uint64_t w = impl_->write.load(std::memory_order_acquire);
    const std::uint64_t r = impl_->read.load(std::memory_order_acquire);
    const std::size_t used = static_cast<std::size_t>(w - r);
    return std::min(used, impl_->capacity);
}

AudioOutput::Backend AudioOutput::backend() const {
    return impl_ != nullptr ? impl_->actual : Backend::Null;
}

const char* AudioOutput::backend_name() const {
    return backend() == Backend::SDL ? "sdl" : "null";
}

const AudioOutput::Config& AudioOutput::config() const {
    static const Config fallback{};
    return impl_ != nullptr ? impl_->cfg : fallback;
}

AudioOutput::Stats AudioOutput::stats() const {
    Stats snapshot;
    if (impl_ == nullptr) {
        return snapshot;
    }
    snapshot.frames = impl_->frames.load(std::memory_order_relaxed);
    snapshot.underruns = impl_->underruns.load(std::memory_order_relaxed);
    snapshot.overruns = impl_->overruns.load(std::memory_order_relaxed);
    snapshot.device = impl_->device;
    return snapshot;
}

} // namespace gm
