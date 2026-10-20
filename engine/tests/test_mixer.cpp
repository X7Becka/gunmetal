// Software mixer tests (`audio/mixer.h`, docs/AUDIO_MIXER.md).
//
// Covers the four things the mixer has to get right independently of the
// engine: a deterministic 16-bit stereo output, source-rate conversion for the
// three SOU codecs, the fixed 32-voice pool, and the 3D gain/pan model.
//
// Synthetic fixtures pin exact integer results (the mixer is fixed-point, so
// "about 100" is not good enough).  The real SOU1 goldens are FNV-1a 64 over
// the first 4096 stereo frames (8192 little-endian int16 values) of ADON
// (8 kHz mono, the whole clip fits in 4140 output frames) and INTRO (32 kHz
// stereo), decoded from sounds/SOU.IDX and mixed at the default 22050 Hz.
#include "audio/mixer.h"
#include "res/bigfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

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

constexpr std::uint64_t kFnvBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t fnv_i16(std::uint64_t hash, std::int16_t value) {
    const auto v = static_cast<std::uint16_t>(value);
    hash = (hash ^ (v & 0xFFu)) * kFnvPrime;
    hash = (hash ^ (v >> 8)) * kFnvPrime;
    return hash;
}

std::uint64_t fnv_stereo(const std::vector<std::int16_t>& samples) {
    std::uint64_t hash = kFnvBasis;
    for (const std::int16_t value : samples) {
        hash = fnv_i16(hash, value);
    }
    return hash;
}

std::vector<std::int16_t> mix_to_vector(gm::audio::Mixer& mixer, std::size_t frames) {
    std::vector<std::int16_t> out(frames * 2, 0);
    mixer.mix(out.data(), frames);
    return out;
}

// A sound made of a literal 16-bit pattern at the mixer's own rate, so the
// resampler step is exactly one frame and no interpolation rounding is
// involved.
struct Fixture {
    std::vector<std::int16_t> samples;
    std::uint32_t rate = 22050;
    std::uint8_t channels = 1;
    gm::audio::SoundRef ref() const {
        return {samples.data(), static_cast<std::uint32_t>(samples.size() / channels), rate,
                channels};
    }
};

void test_formats() {
    std::string error;

    // SOU0: raw unsigned 8-bit PCM -> int16, central byte 128 is silence.
    const std::array<std::uint8_t, 3> raw{0, 128, 255};
    auto sou0 = gm::audio::make_sound_u8(raw, 8000, 1, error);
    CHECK(sou0.has_value());
    if (sou0) {
        CHECK(sou0->sample_rate == 8000);
        CHECK(sou0->channels == 1);
        CHECK(sou0->samples.size() == 3);
        CHECK(sou0->samples[0] == -32768);
        CHECK(sou0->samples[1] == 0);
        CHECK(sou0->samples[2] == 32512);
        CHECK(sou0->ref().frames == 3);
    }
    CHECK(!gm::audio::make_sound_u8(raw, 0, 1, error));
    CHECK(!gm::audio::make_sound_u8(raw, 8000, 3, error));
    std::vector<std::uint8_t> odd{1, 2, 3};
    CHECK(!gm::audio::make_sound_u8(odd, 8000, 2, error));

    // SOU2: already 16-bit stereo, copied verbatim.
    gm::PcmClip clip;
    clip.sample_rate = 32000;
    clip.channels = 2;
    clip.samples = {100, -100, 200, -200};
    auto sou2 = gm::audio::make_sound(clip, error);
    CHECK(sou2.has_value());
    if (sou2) {
        CHECK(sou2->sample_rate == 32000);
        CHECK(sou2->channels == 2);
        CHECK(sou2->frames() == 2);
        CHECK(sou2->samples == clip.samples);
    }
    clip.channels = 3;
    CHECK(!gm::audio::make_sound(clip, error));

    // SOU1: four duplicated 8 kHz slots collapse to one source frame.
    gm::audio::Sou1Sound sou1;
    sou1.sample_rate = 8000;
    sou1.channels = 1;
    sou1.samples = {3200, 3200, 3200, 3200, 6400, 6400, 6400, 6400, -3200, -3200, -3200,
                    -3200};
    auto canonical = gm::audio::make_sound(sou1, error);
    CHECK(canonical.has_value());
    if (canonical) {
        CHECK(canonical->sample_rate == 8000);
        CHECK(canonical->channels == 1);
        CHECK(canonical->frames() == 3);
        CHECK(canonical->samples[0] == 100);
        CHECK(canonical->samples[1] == 200);
        CHECK(canonical->samples[2] == -100);
    }
    // 16 kHz uses two copies, 32 kHz one.
    sou1.sample_rate = 16000;
    sou1.samples = {1600, 1600, 3200, 3200};
    canonical = gm::audio::make_sound(sou1, error);
    CHECK(canonical.has_value());
    if (canonical) {
        CHECK(canonical->sample_rate == 16000);
        CHECK(canonical->frames() == 2);
        CHECK(canonical->samples[0] == 50);
        CHECK(canonical->samples[1] == 100);
    }
    sou1.sample_rate = 22050; // not a SOU1 rate: 32000/22050 is not integral
    CHECK(!gm::audio::make_sound(sou1, error));
    sou1.sample_rate = 8000;
    sou1.samples.resize(6); // not a whole number of 4-slot frames
    CHECK(!gm::audio::make_sound(sou1, error));
}

void test_basic() {
    gm::audio::Mixer mixer;
    CHECK(mixer.init());
    CHECK(mixer.ready());
    CHECK_EQ(mixer.sample_rate(), 22050u);
    CHECK_EQ(mixer.voices(), 0);

    // One voice, one frame: an impulse is reproduced exactly at the same rate.
    Fixture impulse{{1000, 0, 0, 0}};
    const int id = mixer.play(impulse.ref());
    CHECK(id > 0);
    CHECK_EQ(mixer.voices(), 1);
    auto out = mix_to_vector(mixer, 4);
    CHECK(out[0] == 1000);
    CHECK(out[1] == 1000);
    CHECK(out[2] == 0 && out[3] == 0);
    CHECK(out[6] == 0 && out[7] == 0);
    CHECK_EQ(mixer.voices(), 0); // sound ended
    CHECK_EQ(mixer.stats().voices_finished, 1u);

    // Two voices sum in slot order; order of play does not matter.
    Fixture a{{1000, 1000, 1000, 1000}};
    Fixture b{{2000, 2000, 2000, 2000}};
    CHECK(mixer.play(a.ref()) > 0);
    CHECK(mixer.play(b.ref()) > 0);
    out = mix_to_vector(mixer, 2);
    CHECK(out[0] == 3000 && out[1] == 3000 && out[2] == 3000 && out[3] == 3000);
    mixer.stop_all(); // drop the two-frame tails before the pan scenarios

    // Pan: hard left, centre, hard right on a DC fixture.
    Fixture dc{{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000}};
    CHECK(mixer.play(dc.ref(), {.pan = -1.0f}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 1000 && out[1] == 0);
    mixer.stop_all();

    CHECK(mixer.play(dc.ref(), {.pan = 0.0f}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 1000 && out[1] == 1000);
    mixer.stop_all();

    CHECK(mixer.play(dc.ref(), {.pan = 1.0f}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 0 && out[1] == 1000);
    mixer.stop_all();

    // Loop: a four-frame ramp wraps exactly at one frame per output frame.
    Fixture ramp{{100, 200, 300, 400}};
    CHECK(mixer.play(ramp.ref(), {.loop = true}) > 0);
    out = mix_to_vector(mixer, 8);
    const std::array<std::int16_t, 8> want{100, 200, 300, 400, 100, 200, 300, 400};
    for (std::size_t i = 0; i < want.size(); ++i) {
        CHECK(out[2 * i] == want[i]);
        CHECK(out[2 * i + 1] == want[i]);
    }
    CHECK_EQ(mixer.voices(), 1); // loops never finish
    mixer.stop_all(); // the loop above would otherwise keep playing

    // Stop by id silences the voice and invalidates the id.
    const std::uint64_t stopped_before = mixer.stats().voices_stopped;
    const int loop_id = mixer.play(dc.ref(), {.loop = true});
    CHECK(loop_id > 0);
    out = mix_to_vector(mixer, 2);
    CHECK(out[0] == 1000);
    CHECK(mixer.stop(loop_id));
    CHECK(!mixer.stop(loop_id));
    out = mix_to_vector(mixer, 2);
    CHECK(out[0] == 0 && out[1] == 0);
    CHECK_EQ(mixer.stats().voices_stopped, stopped_before + 1);
    mixer.stop_all();
    CHECK_EQ(mixer.voices(), 0);
}

void test_resample() {
    gm::audio::Mixer mixer;
    CHECK(mixer.init(22050));

    // 11025 Hz -> 22050 Hz is exactly 0.5 source frames per output frame:
    // 1000, interpolated 2000, 3000, then the last-frame hold.
    Fixture half{{1000, 3000}, 11025};
    CHECK(mixer.play(half.ref()) > 0);
    auto out = mix_to_vector(mixer, 6);
    CHECK(out[0] == 1000 && out[1] == 1000);
    CHECK(out[2] == 2000 && out[3] == 2000);
    CHECK(out[4] == 3000 && out[5] == 3000);
    CHECK(out[6] == 3000 && out[7] == 3000);
    CHECK(out[8] == 0 && out[9] == 0);
    CHECK_EQ(mixer.voices(), 0);

    // 2:1 downsample: 44100 -> 22050 takes every second source frame.
    Fixture double_rate{{100, 200, 300, 400}, 44100};
    CHECK(mixer.play(double_rate.ref()) > 0);
    out = mix_to_vector(mixer, 4);
    CHECK(out[0] == 100);
    CHECK(out[2] == 300);
    CHECK(out[4] == 0);
}

void test_3d() {
    gm::audio::Mixer mixer;
    CHECK(mixer.init(22050));
    Fixture dc{{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000}};

    // Reference distance 1: no attenuation; beyond it inverse falloff.
    // 10 units on the right at yaw 0: right gain 1/(1+9) = 0.1 -> 100.
    CHECK(mixer.play(dc.ref(), {.pan = 0.0f, .position = gm::audio::Vec3{10.0f, 0.0f, 0.0f}}) >
          0);
    auto out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 0);
    CHECK(out[1] >= 99 && out[1] <= 100);
    mixer.stop_all();

    // Same distance on the left: mirrored.
    CHECK(mixer.play(dc.ref(), {.position = gm::audio::Vec3{-10.0f, 0.0f, 0.0f}}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] >= 99 && out[0] <= 100);
    CHECK(out[1] == 0);
    mixer.stop_all();

    // Directly ahead: no directional pan, still attenuated.
    CHECK(mixer.play(dc.ref(), {.position = gm::audio::Vec3{0.0f, 0.0f, 10.0f}}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 100 && out[1] == 100);
    mixer.stop_all();

    // At the reference distance the gain is exactly 1.
    CHECK(mixer.play(dc.ref(), {.position = gm::audio::Vec3{1.0f, 0.0f, 0.0f}}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 0 && out[1] == 1000);
    mixer.stop_all();

    // Listener yaw turns the right vector: yaw pi/2 looks along +X, so a
    // source at -Z is on the right.
    gm::audio::Listener listener;
    listener.yaw = 1.5707963267948966f;
    mixer.set_listener(listener);
    CHECK(mixer.play(dc.ref(), {.position = gm::audio::Vec3{0.0f, 0.0f, -10.0f}}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[1] > out[0]);
    mixer.stop_all();

    // Height does not pan, and the distance is still three-dimensional.
    CHECK(mixer.play(dc.ref(), {.position = gm::audio::Vec3{0.0f, 10.0f, 0.0f}}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == out[1]);
    CHECK(out[0] > 0 && out[0] < 1000);
    mixer.stop_all();

    // NaNs in the listener/params must not poison the output.
    listener.yaw = std::numeric_limits<float>::quiet_NaN();
    listener.position = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    mixer.set_listener(listener);
    CHECK(mixer.play(dc.ref(), {.gain = std::numeric_limits<float>::quiet_NaN(),
                                .pan = std::numeric_limits<float>::quiet_NaN(),
                                .position = gm::audio::Vec3{
                                    std::numeric_limits<float>::infinity(), 0.0f, 0.0f}}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] >= -32768 && out[0] <= 32767);
    CHECK(out[1] >= -32768 && out[1] <= 32767);
}

void test_limits() {
    gm::audio::Mixer mixer;
    CHECK(mixer.init(22050));
    Fixture dc{{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000}};

    // 32 voices, then the pool refuses the 33rd.  2000 per voice makes the
    // sum 64 000, so both output channels must clip.
    Fixture loud{{2000, 2000, 2000, 2000}};
    for (int i = 0; i < gm::audio::Mixer::kMaxVoices; ++i) {
        CHECK(mixer.play(loud.ref(), {.loop = true}) > 0);
    }
    CHECK_EQ(mixer.voices(), 32);
    CHECK(mixer.play(loud.ref(), {.loop = true}) == -1);
    CHECK_EQ(mixer.voices(), 32);
    CHECK_EQ(mixer.stats().voices_dropped, 1u);

    auto out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 32767); // 32 * 2000 clipped
    CHECK_EQ(mixer.stats().max_active, 32u);
    CHECK(mixer.stats().clipped == 2u);
    mixer.stop_all();
    CHECK_EQ(mixer.voices(), 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 0 && out[1] == 0);

    // Determinism: the same script on two mixers is bit-identical.
    Fixture sine;
    sine.samples.resize(128);
    for (std::size_t i = 0; i < sine.samples.size(); ++i) {
        sine.samples[i] = static_cast<std::int16_t>(
            std::lround(12000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 *
                                          static_cast<double>(i) / 22050.0)));
    }
    const auto run = [&](gm::audio::Mixer& m) {
        CHECK(m.init(22050));
        CHECK(m.play(sine.ref(), {.loop = true, .gain = 0.5f, .pan = -0.25f}) > 0);
        CHECK(m.play(dc.ref(), {.gain = 0.75f, .pan = 0.5f,
                                .position = gm::audio::Vec3{2.0f, 0.0f, 3.0f}}) > 0);
        return mix_to_vector(m, 1000);
    };
    gm::audio::Mixer first;
    gm::audio::Mixer second;
    const auto a = run(first);
    const auto b = run(second);
    CHECK(a == b);
    CHECK(first.stats().frames == 1000);
    CHECK(fnv_stereo(a) == fnv_stereo(b));

    // Re-initialising the same mixer and replaying the same script is also
    // bit-identical (no state leaked between runs).
    const auto c = run(first);
    CHECK(a == c);
}

void test_negatives() {
    gm::audio::Mixer mixer;
    CHECK(!mixer.init(0));
    CHECK(!mixer.ready());
    CHECK(!mixer.init(999));
    CHECK(!mixer.init(192001));
    CHECK(mixer.init(22050));
    CHECK(!mixer.init(0));
    CHECK_EQ(mixer.sample_rate(), 22050u); // failed init leaves the mixer intact

    Fixture dc{{1000, 1000, 1000, 1000}};

    // Bad sounds.
    CHECK(mixer.play(gm::audio::SoundRef{}) == -1);
    CHECK(mixer.play(gm::audio::SoundRef{dc.samples.data(), 0, 22050, 1}) == -1);
    CHECK(mixer.play(gm::audio::SoundRef{dc.samples.data(), 4, 0, 1}) == -1);
    CHECK(mixer.play(gm::audio::SoundRef{dc.samples.data(), 4, 22050, 3}) == -1);
    CHECK(mixer.play(gm::audio::SoundRef{dc.samples.data(), 4, 22050, 0}) == -1);
    CHECK(mixer.play(gm::audio::SoundRef{dc.samples.data(), 4, 22050, 1,
                                         static_cast<gm::audio::SampleFormat>(99)}) == -1);
    CHECK_EQ(mixer.voices(), 0);

    // Null out and zero frames are no-ops.
    mixer.play(dc.ref(), {.loop = true});
    mixer.mix(nullptr, 8);
    CHECK_EQ(mixer.stats().frames, 0u);
    std::int16_t one[2] = {0, 0};
    mixer.mix(one, 0);
    CHECK_EQ(mixer.stats().frames, 0u);
    mixer.mix(one, 1);
    CHECK_EQ(mixer.stats().frames, 1u);
    CHECK(one[0] == 1000 && one[1] == 1000);

    // Huge finite gain saturates, non-finite gain is silence, NaN pan is
    // centred; none of them produce NaN or undefined output.
    Fixture loud{{10000, 10000, 10000, 10000}};
    mixer.stop_all();
    CHECK(mixer.play(loud.ref(), {.gain = 1.0e30f}) > 0);
    auto out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 32767 && out[1] == 32767);
    CHECK(mixer.stats().clipped >= 2u);

    mixer.stop_all();
    CHECK(mixer.play(loud.ref(), {.gain = std::numeric_limits<float>::infinity()}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 0 && out[1] == 0);

    mixer.stop_all();
    CHECK(mixer.play(loud.ref(),
                     {.gain = 1.0f,
                      .pan = std::numeric_limits<float>::quiet_NaN()}) > 0);
    out = mix_to_vector(mixer, 1);
    CHECK(out[0] == 10000 && out[1] == 10000);
}

std::optional<gm::BigFile> open_archive() {
    const fs::path idx = "/mnt/d/games/Gunmetal 3dfx/sounds/SOU.IDX";
    std::error_code ec;
    if (!fs::is_regular_file(idx, ec)) {
        std::printf("mixer: game data not present, SOU1 goldens skipped\n");
        return std::nullopt;
    }
    std::string error;
    auto big = gm::BigFile::open(idx, error);
    if (!big) {
        std::printf("mixer: %s\n", error.c_str());
        return std::nullopt;
    }
    return big;
}

const gm::BigEntry* find(const gm::BigFile& big, const char* name) {
    for (const auto& entry : big.entries()) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

// FNV-1a 64 of the first 4096 stereo frames at 22050 Hz, one loop off, gain 1,
// pan centre, no listener offset.
struct Golden {
    const char* name;
    std::uint64_t hash;
    std::uint32_t frames; // source frames (informational)
    std::uint32_t rate;
    int channels;
};

const Golden kGoldens[] = {
    {"ADON", 0x08ac82e8766481a9ull, 3004, 8000, 1},
    {"INTRO", 0xafd8dc5a7461c163ull, 576000, 32000, 2},
};

void test_real_sou1() {
    auto archive = open_archive();
    if (!archive) {
        return;
    }
    std::string error;
    for (const Golden& golden : kGoldens) {
        const gm::BigEntry* entry = find(*archive, golden.name);
        CHECK(entry != nullptr);
        if (entry == nullptr) {
            continue;
        }
        const auto bytes = archive->read(*entry, error);
        CHECK(bytes.has_value());
        if (!bytes) {
            continue;
        }
        const auto decoded = gm::audio::decode(*bytes, error);
        CHECK(decoded.has_value());
        if (!decoded) {
            std::fprintf(stderr, "FAIL decode %s: %s\n", golden.name, error.c_str());
            continue;
        }
        const auto sound = gm::audio::make_sound(*decoded, error);
        CHECK(sound.has_value());
        if (!sound) {
            std::fprintf(stderr, "FAIL adapter %s: %s\n", golden.name, error.c_str());
            continue;
        }
        CHECK_EQ(sound->sample_rate, golden.rate);
        CHECK_EQ(sound->frames(), golden.frames);
        CHECK_EQ(static_cast<int>(sound->channels), golden.channels);

        gm::audio::Mixer mixer;
        CHECK(mixer.init(22050));
        CHECK(mixer.play(*sound, {.loop = false, .gain = 1.0f, .pan = 0.0f}) > 0);
        auto out = mix_to_vector(mixer, 4096);
        const std::uint64_t hash = fnv_stereo(out);
        std::printf("mixer: %s %u Hz %d ch, %u source frames, first 4096 stereo FNV %016llx\n",
                    golden.name, sound->sample_rate, sound->channels, sound->frames(),
                    static_cast<unsigned long long>(hash));
        if (golden.hash != 0) {
            CHECK_EQ(hash, golden.hash);
        } else {
            std::printf("mixer: %s golden not pinned yet (run once and update kGoldens)\n",
                        golden.name);
        }
    }
}

} // namespace

int main() {
    test_formats();
    test_basic();
    test_resample();
    test_3d();
    test_limits();
    test_negatives();
    test_real_sou1();
    if (g_failures != 0) {
        std::fprintf(stderr, "mixer: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mixer: ok\n");
    return 0;
}
