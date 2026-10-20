// Platform audio output tests (`platform/audio.h`, docs/AUDIO_OUTPUT.md).
//
// The null backend is fully deterministic, so every number below is exact:
// ring capacity/overrun accounting, underrun padding, pause, the 0..255 volume
// curve and the on-demand PullSource.  The SDL3 half is opportunistic: it runs
// only when a driver/device is available (the dummy driver counts), otherwise
// it prints an explicit SKIP reason and the test still passes.
#include "audio/mixer.h"
#include "platform/audio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if GM_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

using gm::AudioOutput;

AudioOutput::Config null_config(int rate = 22050, int channels = 2, int frames = 256,
                                int queue = 64) {
    AudioOutput::Config cfg;
    cfg.sample_rate = rate;
    cfg.channels = channels;
    cfg.frames = frames;
    cfg.queue_frames = queue;
    cfg.backend = AudioOutput::Backend::Null;
    return cfg;
}

std::optional<AudioOutput> open_null(int rate = 22050, int channels = 2, int frames = 256,
                                     int queue = 64) {
    std::string error;
    auto output = AudioOutput::open(null_config(rate, channels, frames, queue), error);
    if (!output) {
        std::fprintf(stderr, "FAIL null backend did not open: %s\n", error.c_str());
        ++g_failures;
    }
    return output;
}

std::vector<std::int16_t> pull_vector(AudioOutput& output, std::size_t frames) {
    std::vector<std::int16_t> out(frames * static_cast<std::size_t>(output.config().channels), 0);
    output.pull(out.data(), frames);
    return out;
}

void test_null_mono() {
    auto output = open_null(8000, 1, 64, 16);
    if (!output) {
        return;
    }
    CHECK(output->backend() == AudioOutput::Backend::Null);
    CHECK_EQ(std::string(output->backend_name()), std::string("null"));
    CHECK_EQ(output->stats().device, std::string("null"));
    CHECK_EQ(output->capacity(), std::size_t{16});
    CHECK_EQ(output->queued(), std::size_t{0});
    CHECK_EQ(output->volume(), 255);
    CHECK(output->running()); // null has no device to start
    CHECK(output->start());
    CHECK(output->running());
    CHECK(!output->paused());

    const std::array<std::int16_t, 10> source{-32768, -1000, -1, 0, 1, 1000, 12345, 32766, 32767, 7};
    CHECK_EQ(output->push(source.data(), source.size()), source.size());
    CHECK_EQ(output->queued(), source.size());

    std::array<std::int16_t, 10> out{};
    output->pull(out.data(), out.size());
    CHECK(out == source);
    CHECK_EQ(output->queued(), std::size_t{0});

    const auto stats = output->stats();
    CHECK_EQ(stats.frames, std::uint64_t{10});
    CHECK_EQ(stats.underruns, std::uint64_t{0});
    CHECK_EQ(stats.overruns, std::uint64_t{0});
}

void test_null_stereo() {
    auto output = open_null(32000, 2, 128, 32);
    if (!output) {
        return;
    }
    CHECK(output->start());
    const std::array<std::int16_t, 8> source{1, -1, 32767, -32768, 0, 123, -456, 789};
    CHECK_EQ(output->push(source.data(), 4), std::size_t{4});
    auto out = pull_vector(*output, 4);
    CHECK(out == std::vector<std::int16_t>(source.begin(), source.end()));

    const std::array<std::int16_t, 4> tail{11, 12, -13, -14};
    CHECK_EQ(output->push(tail.data(), 2), std::size_t{2});
    auto one = pull_vector(*output, 1);
    CHECK_EQ(one.size(), std::size_t{2});
    CHECK_EQ(one[0], 11);
    CHECK_EQ(one[1], 12);
    CHECK_EQ(output->queued(), std::size_t{1});
    auto second = pull_vector(*output, 1);
    CHECK_EQ(second[0], -13);
    CHECK_EQ(second[1], -14);
}

void test_zero_and_huge() {
    auto output = open_null(22050, 2, 128, 32);
    if (!output) {
        return;
    }
    CHECK(output->start());

    output->pull(nullptr, 100); // negative: null destination
    std::array<std::int16_t, 2> one{};
    output->pull(one.data(), 0); // negative: zero frames
    CHECK_EQ(output->stats().frames, std::uint64_t{0});
    CHECK_EQ(output->stats().underruns, std::uint64_t{0});

    constexpr std::size_t kHuge = 100000;
    std::vector<std::int16_t> out(kHuge * 2, 1234);
    output->pull(out.data(), kHuge);
    CHECK(std::all_of(out.begin(), out.end(), [](std::int16_t v) { return v == 0; }));
    auto stats = output->stats();
    CHECK_EQ(stats.frames, std::uint64_t{kHuge});
    CHECK_EQ(stats.underruns, std::uint64_t{1});

    output->pull(out.data(), 1);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(output->stats().underruns, std::uint64_t{2});

    CHECK_EQ(output->push(nullptr, 10), std::size_t{0});
    CHECK_EQ(output->push(out.data(), 0), std::size_t{0});
    CHECK_EQ(output->stats().overruns, std::uint64_t{0});
}

void test_pause() {
    auto output = open_null(22050, 1, 64, 16);
    if (!output) {
        return;
    }
    CHECK(output->start());
    const std::array<std::int16_t, 4> source{100, 200, 300, 400};
    CHECK_EQ(output->push(source.data(), source.size()), source.size());

    output->pause(true);
    CHECK(output->paused());
    auto paused = pull_vector(*output, 4);
    CHECK(std::all_of(paused.begin(), paused.end(), [](std::int16_t v) { return v == 0; }));
    CHECK_EQ(output->queued(), std::size_t{4});
    CHECK_EQ(output->stats().underruns, std::uint64_t{0});

    output->pause(false);
    CHECK(!output->paused());
    auto resumed = pull_vector(*output, 4);
    CHECK(resumed == std::vector<std::int16_t>(source.begin(), source.end()));
    CHECK_EQ(output->stats().frames, std::uint64_t{8});
}

void test_volume() {
    auto output = open_null(22050, 1, 64, 32);
    if (!output) {
        return;
    }
    CHECK(output->start());
    const std::array<std::int16_t, 4> source{1000, -2000, 32767, -32768};

    CHECK_EQ(output->volume(), 255);
    CHECK_EQ(output->push(source.data(), source.size()), source.size());
    auto unity = pull_vector(*output, source.size());
    CHECK(unity == std::vector<std::int16_t>(source.begin(), source.end()));

    output->set_volume(128);
    CHECK_EQ(output->volume(), 128);
    CHECK_EQ(output->push(source.data(), source.size()), source.size());
    auto half = pull_vector(*output, source.size());
    // (v * 128) / 255 with truncation toward zero.
    CHECK_EQ(half[0], 501);
    CHECK_EQ(half[1], -1003);
    CHECK_EQ(half[2], 16447);
    CHECK_EQ(half[3], -16448);

    output->set_volume(0);
    CHECK_EQ(output->push(source.data(), source.size()), source.size());
    auto silent = pull_vector(*output, source.size());
    CHECK(std::all_of(silent.begin(), silent.end(), [](std::int16_t v) { return v == 0; }));

    output->set_volume(-7);
    CHECK_EQ(output->volume(), 0);
    output->set_volume(999);
    CHECK_EQ(output->volume(), 255);
}

void test_underrun_overrun() {
    auto output = open_null(22050, 1, 16, 32);
    if (!output) {
        return;
    }
    CHECK(output->start());

    std::vector<std::int16_t> first(40, 7);
    CHECK_EQ(output->push(first.data(), first.size()), std::size_t{32});
    CHECK_EQ(output->queued(), std::size_t{32});
    CHECK_EQ(output->stats().overruns, std::uint64_t{8});

    auto full = pull_vector(*output, 32);
    CHECK(std::all_of(full.begin(), full.end(), [](std::int16_t v) { return v == 7; }));
    CHECK_EQ(output->stats().underruns, std::uint64_t{0});

    auto short_pull = pull_vector(*output, 3);
    CHECK(std::all_of(short_pull.begin(), short_pull.end(), [](std::int16_t v) { return v == 0; }));
    CHECK_EQ(output->stats().underruns, std::uint64_t{1});
    CHECK_EQ(output->stats().frames, std::uint64_t{35});

    std::vector<std::int16_t> fives(20, 5);
    std::vector<std::int16_t> nines(20, 9);
    CHECK_EQ(output->push(fives.data(), fives.size()), std::size_t{20});
    CHECK_EQ(output->push(nines.data(), nines.size()), std::size_t{12});
    CHECK_EQ(output->queued(), std::size_t{32});
    CHECK_EQ(output->stats().overruns, std::uint64_t{16});

    auto mixed = pull_vector(*output, 32);
    CHECK(std::all_of(mixed.begin(), mixed.begin() + 20,
                      [](std::int16_t v) { return v == 5; }));
    CHECK(std::all_of(mixed.begin() + 20, mixed.end(),
                      [](std::int16_t v) { return v == 9; }));
    CHECK_EQ(output->stats().underruns, std::uint64_t{1});
}

void test_ring_wrap() {
    auto output = open_null(22050, 1, 16, 8);
    if (!output) {
        return;
    }
    CHECK(output->start());
    const std::array<std::int16_t, 6> a{1, 2, 3, 4, 5, 6};
    CHECK_EQ(output->push(a.data(), a.size()), std::size_t{6});
    auto head = pull_vector(*output, 4);
    CHECK(head == std::vector<std::int16_t>({1, 2, 3, 4}));
    CHECK_EQ(output->queued(), std::size_t{2});

    const std::array<std::int16_t, 6> b{7, 8, 9, 10, 11, 12};
    CHECK_EQ(output->push(b.data(), b.size()), std::size_t{6});
    CHECK_EQ(output->queued(), std::size_t{8});
    auto rest = pull_vector(*output, 8);
    CHECK(rest == std::vector<std::int16_t>({5, 6, 7, 8, 9, 10, 11, 12}));
}

void test_start_stop_repeat() {
    auto output = open_null(22050, 1, 32, 8);
    if (!output) {
        return;
    }
    output->stop(); // exercise the explicit stopped state of the null sink
    CHECK(!output->running());
    const std::array<std::int16_t, 3> first{1, 2, 3};
    CHECK_EQ(output->push(first.data(), first.size()), first.size());
    auto stopped = pull_vector(*output, 3);
    CHECK(std::all_of(stopped.begin(), stopped.end(), [](std::int16_t v) { return v == 0; }));
    CHECK_EQ(output->queued(), std::size_t{3});

    CHECK(output->start());
    CHECK(output->start()); // repeated start is harmless
    auto playing = pull_vector(*output, 3);
    CHECK(playing == std::vector<std::int16_t>({1, 2, 3}));

    output->stop();
    CHECK(!output->running());
    output->stop(); // repeated stop is harmless
    const std::array<std::int16_t, 2> second{4, 5};
    CHECK_EQ(output->push(second.data(), second.size()), second.size());
    auto paused_by_stop = pull_vector(*output, 2);
    CHECK(std::all_of(paused_by_stop.begin(), paused_by_stop.end(),
                      [](std::int16_t v) { return v == 0; }));
    CHECK_EQ(output->queued(), std::size_t{2});

    CHECK(output->start());
    auto again = pull_vector(*output, 2);
    CHECK(again == std::vector<std::int16_t>({4, 5}));

    const auto stats = output->stats();
    CHECK_EQ(stats.frames, std::uint64_t{10});
    CHECK_EQ(stats.underruns, std::uint64_t{0});
    CHECK_EQ(stats.overruns, std::uint64_t{0});
}

struct RampSource {
    std::int16_t next = 1;
    std::size_t calls = 0;
};

void ramp_source(void* user, std::int16_t* out, std::size_t frames) {
    auto* source = static_cast<RampSource*>(user);
    ++source->calls;
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = source->next++;
    }
}

void mixer_source(void* user, std::int16_t* out, std::size_t frames) {
    static_cast<gm::audio::Mixer*>(user)->mix(out, frames);
}

void test_source() {
    auto output = open_null(22050, 1, 64, 8);
    if (!output) {
        return;
    }
    CHECK(output->start());
    RampSource ramp;
    output->set_source(&ramp_source, &ramp);

    auto first = pull_vector(*output, 5);
    CHECK(first == std::vector<std::int16_t>({1, 2, 3, 4, 5}));
    CHECK_EQ(ramp.calls, std::size_t{1});
    CHECK_EQ(output->stats().underruns, std::uint64_t{0});

    const std::array<std::int16_t, 3> queued{100, 101, 102};
    CHECK_EQ(output->push(queued.data(), queued.size()), queued.size());
    auto mixed = pull_vector(*output, 5);
    CHECK(mixed == std::vector<std::int16_t>({100, 101, 102, 6, 7}));
    CHECK_EQ(ramp.next, 8);

    output->pause(true);
    auto paused = pull_vector(*output, 2);
    CHECK(std::all_of(paused.begin(), paused.end(), [](std::int16_t v) { return v == 0; }));
    CHECK_EQ(ramp.next, 8);
    output->pause(false);
    auto resumed = pull_vector(*output, 1);
    CHECK_EQ(resumed[0], 8);

    // The real mixer is a valid source: a one-frame DC sound, held by the
    // mixer at the end of a non-looping voice, fills every requested frame.
    gm::audio::Mixer mixer;
    CHECK(mixer.init(22050));
    gm::audio::Sound sound;
    sound.samples = {1000};
    sound.sample_rate = 22050;
    sound.channels = 1;
    CHECK(mixer.play(sound, {.loop = true, .gain = 1.0f}) > 0);
    auto mixed_output = open_null(22050, 2, 64, 8);
    if (mixed_output) {
        CHECK(mixed_output->start());
        mixed_output->set_source(&mixer_source, &mixer);
        auto frames = pull_vector(*mixed_output, 4);
        CHECK_EQ(frames.size(), std::size_t{8});
        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK_EQ(frames[i], 1000);
        }
        CHECK_EQ(mixed_output->stats().underruns, std::uint64_t{0});
    }
}

void test_negatives() {
    auto attempt = [](int rate, int channels, int frames, int queue,
                      AudioOutput::Backend backend) {
        AudioOutput::Config cfg;
        cfg.sample_rate = rate;
        cfg.channels = channels;
        cfg.frames = frames;
        cfg.queue_frames = queue;
        cfg.backend = backend;
        std::string error;
        auto output = AudioOutput::open(cfg, error);
        return std::pair{std::move(output), error};
    };

    const auto bad_rate = attempt(0, 2, 256, 0, AudioOutput::Backend::Null);
    CHECK(!bad_rate.first);
    CHECK(!bad_rate.second.empty());
    CHECK(!attempt(-1, 2, 256, 0, AudioOutput::Backend::Null).first);
    CHECK(!attempt(22050, 0, 256, 0, AudioOutput::Backend::Null).first);
    CHECK(!attempt(22050, 3, 256, 0, AudioOutput::Backend::Null).first);
    CHECK(!attempt(22050, 2, 0, 0, AudioOutput::Backend::Null).first);
    CHECK(!attempt(22050, 2, 256, -1, AudioOutput::Backend::Null).first);
    CHECK(!attempt(22050, 2, 256, (1 << 22) + 1, AudioOutput::Backend::Null).first);
    CHECK(!attempt(22050, 2, (1 << 20) + 1, 0, AudioOutput::Backend::Null).first);

    // A generous but valid rate is accepted.
    const auto high = attempt(192000, 1, 1, 1, AudioOutput::Backend::Null);
    CHECK(high.first.has_value());
    if (high.first) {
        CHECK_EQ(high.first->capacity(), std::size_t{1});
    }
    const auto huge = attempt(22050, 1, 1 << 20, 1, AudioOutput::Backend::Null);
    CHECK(huge.first.has_value());

#if !GM_HAVE_SDL3
    const auto sdl = attempt(22050, 2, 256, 0, AudioOutput::Backend::SDL);
    CHECK(!sdl.first);
    CHECK(sdl.second.find("SDL") != std::string::npos);

    const auto automatic = attempt(22050, 2, 256, 0, AudioOutput::Backend::Auto);
    CHECK(automatic.first.has_value());
    if (automatic.first) {
        CHECK(automatic.first->backend() == AudioOutput::Backend::Null);
        CHECK(automatic.first->stats().device.find("SDL") != std::string::npos);
    }
#endif
}

struct Scenario {
    std::vector<std::int16_t> pcm;
    std::uint64_t frames = 0;
    std::uint64_t underruns = 0;
    std::uint64_t overruns = 0;
    std::string device;
};

Scenario run_scenario() {
    Scenario result;
    auto output = open_null(11025, 2, 64, 32);
    if (!output) {
        return result;
    }
    CHECK(output->start());
    std::vector<std::int16_t> source(80);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::int16_t>((i * 37) % 1000 - 500);
    }
    CHECK_EQ(output->push(source.data(), 20), std::size_t{20});
    auto a = pull_vector(*output, 12);
    result.pcm.insert(result.pcm.end(), a.begin(), a.end());
    output->set_volume(201);
    CHECK_EQ(output->push(source.data() + 20, 20), std::size_t{20});
    auto b = pull_vector(*output, 30);
    result.pcm.insert(result.pcm.end(), b.begin(), b.end());
    output->pause(true);
    auto c = pull_vector(*output, 6);
    result.pcm.insert(result.pcm.end(), c.begin(), c.end());
    output->pause(false);
    CHECK_EQ(output->push(source.data() + 40, 40), std::size_t{32});
    auto d = pull_vector(*output, 40);
    result.pcm.insert(result.pcm.end(), d.begin(), d.end());
    const auto stats = output->stats();
    result.frames = stats.frames;
    result.underruns = stats.underruns;
    result.overruns = stats.overruns;
    result.device = stats.device;
    return result;
}

void test_determinism() {
    const Scenario first = run_scenario();
    const Scenario second = run_scenario();
    CHECK(!first.pcm.empty());
    CHECK(first.pcm == second.pcm);
    CHECK_EQ(first.frames, second.frames);
    CHECK_EQ(first.underruns, second.underruns);
    CHECK_EQ(first.overruns, second.overruns);
    CHECK_EQ(first.device, second.device);
    CHECK_EQ(first.frames, std::uint64_t{12 + 30 + 6 + 40});
    CHECK_EQ(first.underruns, std::uint64_t{2}); // pulls of 30 and 40 ran dry
    CHECK_EQ(first.overruns, std::uint64_t{8});  // 40 pushed into a 32-frame ring
}

#if GM_HAVE_SDL3
void test_sdl_fallback_hint() {
    // Force a driver name that cannot exist: Auto must return the null sink
    // with the SDL reason in Stats::device, never throw or crash.
    const char* previous = SDL_getenv("SDL_AUDIODRIVER");
    const std::string saved = previous != nullptr ? previous : "";
    SDL_setenv_unsafe("SDL_AUDIODRIVER", "gm-no-such-audio-driver", 1);

    AudioOutput::Config cfg;
    cfg.backend = AudioOutput::Backend::Auto;
    std::string error;
    auto output = AudioOutput::open(cfg, error);
    CHECK(output.has_value());
    if (output) {
        CHECK(output->backend() == AudioOutput::Backend::Null);
        CHECK(output->stats().device.find("SDL") != std::string::npos);
        CHECK(output->stats().device.find("gm-no-such-audio-driver") != std::string::npos);
    }

    if (previous != nullptr) {
        SDL_setenv_unsafe("SDL_AUDIODRIVER", saved.c_str(), 1);
    } else {
        SDL_unsetenv_unsafe("SDL_AUDIODRIVER");
    }
}
#endif

void test_sdl() {
#if GM_HAVE_SDL3
    test_sdl_fallback_hint();

    auto open_sdl = [] {
        AudioOutput::Config cfg;
        cfg.sample_rate = 22050;
        cfg.channels = 2;
        cfg.frames = 512;
        cfg.queue_frames = 8192;
        cfg.backend = AudioOutput::Backend::SDL;
        std::string error;
        auto output = AudioOutput::open(cfg, error);
        return std::pair{std::move(output), error};
    };

    auto [output, error] = open_sdl();
    bool dummy = false;
    if (!output) {
        std::fprintf(stderr, "audio_output: SDL skip: %s\n", error.c_str());
        SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
        auto retry = open_sdl();
        output = std::move(retry.first);
        error = retry.second;
        if (!output) {
            std::fprintf(stderr, "audio_output: SDL skip (dummy driver): %s\n", error.c_str());
            return;
        }
        dummy = true;
    }
    if (!output) {
        return;
    }

    CHECK(output->backend() == AudioOutput::Backend::SDL);
    CHECK_EQ(std::string(output->backend_name()), std::string("sdl"));
    CHECK(output->stats().device.find("SDL") != std::string::npos);

    std::vector<std::int16_t> frames(2048 * 2);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i] = static_cast<std::int16_t>((i * 13) % 2000 - 1000);
    }
    CHECK_EQ(output->push(frames.data(), 2048), std::size_t{2048});
    CHECK(output->start());
    CHECK(output->running());

    if (!dummy) {
        // A real device consumes frames in the background.  Do not pin exact
        // counts (the OS mixers differ); only require forward progress.
        for (int i = 0; i < 50 && output->stats().frames == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(output->stats().frames > 0);
    } else {
        // The dummy driver also has a device thread.  Give it a moment and
        // require that the callback ran; the exact count is timing-dependent.
        for (int i = 0; i < 100 && output->stats().frames == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(output->stats().frames > 0);
    }

    output->set_volume(100);
    output->pause(true);
    CHECK(output->paused());
    output->pause(false);
    CHECK(!output->paused());
    output->stop();
    CHECK(!output->running());
    CHECK(output->start());
    output->stop();

    const auto stats = output->stats();
    std::printf("audio_output: SDL ok (%s, %s, %llu frames pulled)\n",
                dummy ? "dummy driver" : "device", stats.device.c_str(),
                static_cast<unsigned long long>(stats.frames));
#else
    std::printf("audio_output: SDL skip: built without SDL3 (GM_HAVE_SDL3=0)\n");
#endif
}

} // namespace

int main() {
    test_null_mono();
    test_null_stereo();
    test_zero_and_huge();
    test_pause();
    test_volume();
    test_underrun_overrun();
    test_ring_wrap();
    test_start_stop_repeat();
    test_source();
    test_negatives();
    test_determinism();
    test_sdl();
    if (g_failures != 0) {
        std::fprintf(stderr, "audio_output: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("audio_output: ok\n");
    return 0;
}
