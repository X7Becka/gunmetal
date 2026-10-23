// Music transitions and gain (`audio/music.h`, docs/MUSIC.md).
//
// The module is pure DSP on top of a decoded `PcmClip`, so the test needs no
// platform audio.  It covers:
//
//   * linear fades pinned against the documented formula gain(k) = k / N
//     (91 Hz tick clock and a 100-step case where the midpoint is exactly
//     0.5), plus the fade-out mirror;
//   * a linear crossfade: amplitude weights sum to one, old^2 + new^2 <= 1,
//     the output peak stays inside [-1, 1], both tracks are really present at
//     the midpoint (single-bin DFT at 440/660 Hz and RMS per channel), and
//     the fade ends with only the incoming track;
//   * the clip end and a closed fade-out gate -> `finished()`, `play()` after
//     that rewinds to frame 0 and restores the envelope;
//   * negatives: NaN/inf master gain, negative fade/advance, `mix_into`
//     without a loaded/playing track, bad rates/channels/empty clips and a
//     crossfade without playback — all quiet failures with a text, no crash;
//   * real shipped data through the same decoder `--music` uses:
//     `MUSIC/SOU.IDX` entry `L01BONUS` (66304 frames, 16000 in the header but
//     the original music path plays 32000, docs/SOU2.md §2) and the first
//     track of `MUSIC/Dm1A.sog`.
//
// Numbers are printed, not just asserted: clip lengths, fade steps, midpoint
// gain, crossfade RMS/peak and the two DFT bins.

#include "audio/music.h"
#include "res/bigfile.h"
#include "res/sou2.h"
#include "res/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
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

#define CHECK_NEAR(a, b, tol)                                                                \
    do {                                                                                     \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (tol))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s=%.17g != %s=%.17g (tol %.3g)\n", __FILE__,  \
                         __LINE__, #a, va, #b, vb, static_cast<double>(tol));                \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

constexpr double kPi = 3.14159265358979323846;

/// Mono/stereo sine clip.  `amp` is a fraction of int16 full scale.
gm::PcmClip make_tone(double frequency, double seconds, int rate, double amp,
                      int channels = 1) {
    gm::PcmClip clip;
    clip.sample_rate = rate;
    clip.channels = channels;
    const std::size_t frames =
        static_cast<std::size_t>(std::llround(seconds * static_cast<double>(rate)));
    clip.samples.resize(frames * static_cast<std::size_t>(channels));
    for (std::size_t n = 0; n < frames; ++n) {
        const double phase = 2.0 * kPi * frequency * static_cast<double>(n) /
                             static_cast<double>(rate);
        const double value = amp * std::sin(phase) * 32767.0;
        const auto sample = static_cast<std::int16_t>(std::llround(value));
        for (int c = 0; c < channels; ++c) {
            clip.samples[n * static_cast<std::size_t>(channels) +
                         static_cast<std::size_t>(c)] = sample;
        }
    }
    return clip;
}

double rms_channel(const std::vector<float>& out, std::size_t start, std::size_t count,
                   int channel) {
    double sum = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        const double value = out[(start + n) * 2 + static_cast<std::size_t>(channel)];
        sum += value * value;
    }
    return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
}

/// Single-bin DFT magnitude over `count` frames; window-relative phase keeps
/// the magnitude independent of where the window starts.
double bin_magnitude(const std::vector<float>& out, std::size_t start, std::size_t count,
                     int channel, double frequency, int rate) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        const double value = out[(start + n) * 2 + static_cast<std::size_t>(channel)];
        const double phase = 2.0 * kPi * frequency * static_cast<double>(n) /
                             static_cast<double>(rate);
        re += value * std::cos(phase);
        im -= value * std::sin(phase);
    }
    return std::hypot(re, im);
}

double peak_abs(const std::vector<float>& out) {
    double peak = 0.0;
    for (const float value : out) {
        peak = std::max(peak, std::fabs(static_cast<double>(value)));
    }
    return peak;
}

// ---------------------------------------------------------------- fades ----

void test_fade_in_out() {
    std::printf("fade: clock 91 Hz (the game tick), clip 10 s @ 1000 Hz\n");
    gm::audio::MusicTrack track;
    std::string error;
    const gm::PcmClip clip = make_tone(220.0, 10.0, 1000, 0.5);
    CHECK(track.load(clip, 91, &error));
    CHECK(track.play());

    track.fade_in(1.0); // 91 output frames
    CHECK_NEAR(track.gain(), 0.0, 1e-12);
    double previous = track.gain();
    double first = 0.0;
    double middle = 0.0;
    for (int i = 1; i <= 91; ++i) {
        track.advance(1);
        const double gain = track.gain();
        const double expected = static_cast<double>(i) / 91.0;
        CHECK(gain >= previous - 1e-15); // monotone
        CHECK_NEAR(gain, expected, 1e-9);
        previous = gain;
        if (i == 1) {
            first = gain;
        }
        if (i == 45) {
            middle = gain;
        }
    }
    std::printf("fade_in(1.0) @91: steps=91 first=%.12f mid(45)=%.12f end=%.12f "
                "|mid-0.5|=%.6f\n",
                first, middle, track.gain(), std::fabs(middle - 0.5));
    CHECK_NEAR(track.gain(), 1.0, 1e-9);

    // Fade-out is the mirror image from the current envelope (1.0).
    track.fade_out(1.0);
    previous = track.gain();
    double end = 1.0;
    for (int i = 1; i <= 91; ++i) {
        track.advance(1);
        const double gain = track.gain();
        CHECK(gain <= previous + 1e-15); // monotone
        CHECK_NEAR(gain, 1.0 - static_cast<double>(i) / 91.0, 1e-9);
        previous = gain;
        end = gain;
    }
    std::printf("fade_out(1.0) @91: end=%.12f playing=%d finished=%d\n", end,
                track.playing() ? 1 : 0, track.finished() ? 1 : 0);
    CHECK_NEAR(end, 0.0, 1e-12);
    CHECK(!track.playing());
    CHECK(track.finished());

    // An even number of steps makes the midpoint exactly 0.5.
    gm::audio::MusicTrack exact;
    CHECK(exact.load(make_tone(220.0, 2.0, 1000, 0.5), 100, &error));
    CHECK(exact.play());
    exact.fade_in(1.0); // 100 frames
    exact.advance(50);
    std::printf("fade_in(1.0) @100: gain(50/100)=%.17g\n", exact.gain());
    CHECK_NEAR(exact.gain(), 0.5, 1e-12);

    // The documented pre-play order works too: load -> fade_in -> play.
    gm::audio::MusicTrack pending;
    CHECK(pending.load(make_tone(220.0, 2.0, 1000, 0.5), 100, &error));
    pending.fade_in(0.5);
    CHECK(pending.play());
    CHECK_NEAR(pending.gain(), 0.0, 1e-12);
    pending.advance(25); // 50-frame ramp
    std::printf("fade_in(0.5) before play @100: gain(25/50)=%.12f\n", pending.gain());
    CHECK_NEAR(pending.gain(), 0.5, 1e-12);
}

// ------------------------------------------------------------ crossfade ----

void test_crossfade() {
    constexpr int kRate = 1000;
    constexpr int kRamp = 10000; // 10 s
    constexpr double kToneA = 440.0;
    constexpr double kToneB = 660.0;
    constexpr double kAmpA = 1.0;
    constexpr double kAmpB = 0.5;

    gm::audio::MusicTrack track;
    std::string error;
    CHECK(track.load(make_tone(kToneA, 20.0, kRate, kAmpA), kRate, &error));
    CHECK(track.play());
    track.advance(100);

    CHECK(track.crossfade_to_at(make_tone(kToneB, 20.0, kRate, kAmpB), kRate, 10.0));
    CHECK(track.crossfade().active);

    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(kRamp) * 2);
    double max_weights_sq = 0.0;
    for (int block = 0; block < kRamp / 100; ++block) {
        std::array<float, 200> buffer{};
        const int written = track.mix_into(buffer, 2);
        CHECK(written == 100);
        out.insert(out.end(), buffer.begin(), buffer.end());
        const auto info = track.crossfade();
        CHECK_NEAR(info.old_gain + info.new_gain, 1.0, 1e-9); // weights sum to 1
        max_weights_sq =
            std::max(max_weights_sq, info.old_gain * info.old_gain +
                                         info.new_gain * info.new_gain);
    }

    // At half of the ramp the module itself says both envelopes are 0.5.
    const auto middle = [&]() {
        gm::audio::MusicTrack probe;
        std::string err;
        probe.load(make_tone(kToneA, 20.0, kRate, kAmpA), kRate, &err);
        probe.play();
        probe.advance(100);
        probe.crossfade_to_at(make_tone(kToneB, 20.0, kRate, kAmpB), kRate, 10.0);
        probe.advance(kRamp / 2);
        return probe.crossfade();
    }();
    std::printf("crossfade: ramp=%d frames, mid old=%.12f new=%.12f progress=%.12f\n",
                kRamp, middle.old_gain, middle.new_gain, middle.progress);
    CHECK_NEAR(middle.old_gain, 0.5, 1e-9);
    CHECK_NEAR(middle.new_gain, 0.5, 1e-9);
    CHECK(max_weights_sq <= 1.0 + 1e-12);
    std::printf("crossfade: max(old^2+new^2)=%.12f (linear law, <= 1)\n", max_weights_sq);

    const double peak = peak_abs(out);
    std::printf("crossfade: peak=%.9f (must be <= 1)\n", peak);
    CHECK(peak <= 1.0 + 1e-6);

    // Window inside the ramp: 100 frames at 1000 Hz make 440 Hz (44 cycles)
    // and 660 Hz (66 cycles) exact DFT bins, orthogonal to each other.
    const std::size_t start = 4950; // symmetric around the 10000-frame midpoint
    const std::size_t count = 100;
    const double bin_a_l = bin_magnitude(out, start, count, 0, kToneA, kRate);
    const double bin_b_l = bin_magnitude(out, start, count, 0, kToneB, kRate);
    const double bin_a_r = bin_magnitude(out, start, count, 1, kToneA, kRate);
    const double bin_b_r = bin_magnitude(out, start, count, 1, kToneB, kRate);
    const double rms_l = rms_channel(out, start, count, 0);
    const double rms_r = rms_channel(out, start, count, 1);
    const auto window = middle; // both envelopes ~0.5 at this point
    const double predicted_rms =
        std::sqrt(window.old_gain * window.old_gain * (kAmpA * kAmpA / 2.0) +
                  window.new_gain * window.new_gain * (kAmpB * kAmpB / 2.0));
    std::printf("crossfade window [%zu,%zu): bin440=%.3f/%.3f bin660=%.3f/%.3f "
                "rms=%.6f/%.6f predicted=%.6f\n",
                start, start + count, bin_a_l, bin_a_r, bin_b_l, bin_b_r, rms_l, rms_r,
                predicted_rms);
    // 50 * gain * amplitude for a pure sine in an exact bin.
    CHECK(bin_a_l > 0.2 * 50.0 * window.old_gain * kAmpA);
    CHECK(bin_b_l > 0.2 * 50.0 * window.new_gain * kAmpB);
    CHECK_NEAR(rms_l, predicted_rms, 0.02 * predicted_rms + 1e-9);
    CHECK_NEAR(rms_r, predicted_rms, 0.02 * predicted_rms + 1e-9);

    const auto after = track.crossfade();
    std::printf("crossfade end: active=%d old=%.12f new=%.12f playing=%d\n",
                after.active ? 1 : 0, after.old_gain, after.new_gain,
                track.playing() ? 1 : 0);
    CHECK(!after.active);
    CHECK_NEAR(after.old_gain, 0.0, 1e-12);
    CHECK_NEAR(after.new_gain, 1.0, 1e-9);
    CHECK(track.playing());
}

// ------------------------------------------------------- end and rewind ----

void test_finish_and_rewind() {
    gm::audio::MusicTrack track;
    std::string error;
    CHECK(track.load(make_tone(300.0, 0.1, 1000, 0.5), 1000, &error));
    CHECK(track.play());
    track.fade_out(0.05); // 50 frames, before the 100-frame clip ends

    std::vector<float> buffer(400, 0.0f); // 200 stereo frames
    const int written = track.mix_into(buffer, 2);
    std::printf("finish: clip=%.3f s (%llu frames), gate=50 frames, mix_into=%d frames, "
                "playing=%d finished=%d gain=%.12f\n",
                track.duration_seconds(),
                static_cast<unsigned long long>(track.frames()), written,
                track.playing() ? 1 : 0, track.finished() ? 1 : 0, track.gain());
    CHECK(written == 50);
    CHECK(!track.playing());
    CHECK(track.finished());
    CHECK_NEAR(track.gain(), 0.0, 1e-12);
    // The buffer past the stop must stay untouched.
    for (std::size_t i = 50 * 2; i < buffer.size(); ++i) {
        CHECK(buffer[i] == 0.0f);
    }

    CHECK(track.play()); // rewind after finished()
    std::printf("rewind: playing=%d finished=%d gain=%.12f position=%.3f s\n",
                track.playing() ? 1 : 0, track.finished() ? 1 : 0, track.gain(),
                track.position_seconds());
    CHECK(track.playing());
    CHECK(!track.finished());
    CHECK_NEAR(track.gain(), 1.0, 1e-12);
    CHECK_NEAR(track.position_seconds(), 0.0, 1e-12);

    // Natural end, no fade: 100 frames is the whole clip.
    gm::audio::MusicTrack natural;
    CHECK(natural.load(make_tone(300.0, 0.1, 1000, 0.5), 1000, &error));
    CHECK(natural.play());
    natural.advance(100);
    std::printf("natural end: playing=%d finished=%d position=%.3f s\n",
                natural.playing() ? 1 : 0, natural.finished() ? 1 : 0,
                natural.position_seconds());
    CHECK(!natural.playing());
    CHECK(natural.finished());

    // Explicit stop is not "finished": it can be played again.
    gm::audio::MusicTrack stopped;
    CHECK(stopped.load(make_tone(300.0, 1.0, 1000, 0.5), 1000, &error));
    CHECK(stopped.play());
    stopped.stop();
    CHECK(!stopped.playing());
    CHECK(!stopped.finished());
}

// --------------------------------------------------------------- negatives ----

void test_negatives() {
    gm::audio::MusicTrack empty;
    std::array<float, 8> buffer{};
    buffer.fill(0.25f);
    const std::array<float, 8> before = buffer;
    const int written = empty.mix_into(buffer, 2);
    std::printf("negative mix_into without load: written=%d error=\"%s\"\n", written,
                empty.last_error().c_str());
    CHECK(written == 0);
    CHECK(buffer == before);
    CHECK(!empty.last_error().empty());
    CHECK(!empty.play());

    std::string error;
    gm::audio::MusicTrack track;
    CHECK(!track.load(gm::PcmClip{}, 32000, &error));
    std::printf("negative load empty clip: \"%s\"\n", error.c_str());
    CHECK(!error.empty());

    gm::PcmClip bad_rate = make_tone(440.0, 0.1, 32000, 0.5);
    bad_rate.sample_rate = 0;
    CHECK(!track.load(bad_rate, 32000, &error));
    std::printf("negative load source rate 0: \"%s\"\n", error.c_str());

    gm::PcmClip bad_channels = make_tone(440.0, 0.1, 32000, 0.5);
    bad_channels.channels = 3;
    CHECK(!track.load(bad_channels, 32000, &error));
    std::printf("negative load 3 channels: \"%s\"\n", error.c_str());

    CHECK(!track.load(make_tone(440.0, 0.1, 32000, 0.5), 0, &error));
    std::printf("negative load output rate 0: \"%s\"\n", error.c_str());

    CHECK(track.load(make_tone(440.0, 1.0, 1000, 0.5), 1000, &error));
    CHECK(track.play());
    const double gain_before = track.master_gain();
    track.set_gain(std::numeric_limits<double>::quiet_NaN());
    std::printf("negative set_gain(NaN): master=%.12f error=\"%s\"\n", track.master_gain(),
                track.last_error().c_str());
    CHECK_NEAR(track.master_gain(), gain_before, 0.0);
    CHECK(!track.last_error().empty());

    track.set_gain(std::numeric_limits<double>::infinity());
    CHECK_NEAR(track.master_gain(), gain_before, 0.0);

    track.set_gain(2.0);
    CHECK_NEAR(track.master_gain(), 1.0, 0.0);
    track.set_gain(-1.0);
    CHECK_NEAR(track.master_gain(), 0.0, 0.0);
    track.set_gain(1.0);

    const double fade_gain = track.gain();
    track.fade_in(-1.0);
    std::printf("negative fade_in(-1): gain=%.12f error=\"%s\"\n", track.gain(),
                track.last_error().c_str());
    CHECK_NEAR(track.gain(), fade_gain, 1e-12);
    CHECK(!track.last_error().empty());

    track.fade_out(-0.5);
    CHECK(!track.last_error().empty());

    track.advance(-5);
    CHECK(!track.last_error().empty());
    CHECK(track.playing());

    std::array<float, 6> stereo{};
    CHECK(track.mix_into(stereo, 3) == 0);
    CHECK(!track.last_error().empty());

    gm::audio::MusicTrack idle;
    idle.crossfade_to(make_tone(440.0, 0.1, 1000, 0.5), 0.5);
    std::printf("negative crossfade without playback: active=%d error=\"%s\"\n",
                idle.crossfade().active ? 1 : 0, idle.last_error().c_str());
    CHECK(!idle.last_error().empty());

    CHECK(track.load(make_tone(440.0, 1.0, 1000, 0.5), 1000, &error));
    CHECK(track.play());
    CHECK(!track.crossfade_to_at(make_tone(660.0, 1.0, 1000, 0.5), 1000,
                                 std::numeric_limits<double>::quiet_NaN()));
    CHECK(!track.crossfade_to_at(make_tone(660.0, 1.0, 1000, 0.5), 0, 0.5));
    CHECK(track.playing());
}

// ------------------------------------------------------------- real data ----

std::optional<fs::path> game_dir() {
    const std::array<fs::path, 2> candidates{
        fs::path("/mnt/d/games/Gunmetal 3dfx"),
        fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".cache/gunmetal-re/game"};
    for (const auto& path : candidates) {
        if (fs::exists(path / "MUSIC" / "SOU.IDX")) {
            return path;
        }
    }
    return std::nullopt;
}

const gm::BigEntry* find_entry(const gm::BigFile& archive, const std::string& name) {
    for (const auto& entry : archive.entries()) {
        if (entry.name.size() == name.size() &&
            std::equal(entry.name.begin(), entry.name.end(), name.begin(),
                       [](char a, char b) {
                           return std::toupper(static_cast<unsigned char>(a)) ==
                                  std::toupper(static_cast<unsigned char>(b));
                       })) {
            return &entry;
        }
    }
    return nullptr;
}

void test_real_data() {
    const auto dir = game_dir();
    if (!dir) {
        std::printf("real SOU2: SKIP (game not installed)\n");
        return;
    }

    std::string error;
    auto archive = gm::BigFile::open(*dir / "MUSIC" / "SOU.IDX", error);
    if (!archive) {
        std::printf("real SOU2: SKIP (SOU.IDX: %s)\n", error.c_str());
        return;
    }

    // `L01BONUS`: the shipped 16000-header track that the original music path
    // still plays at 32000 (docs/SOU2.md §2).
    const gm::BigEntry* bonus = find_entry(*archive, "L01BONUS");
    CHECK(bonus != nullptr);
    if (bonus != nullptr) {
        const auto blob = archive->read(*bonus, error);
        CHECK(blob.has_value());
        if (blob) {
            auto clip = gm::decode_sou2(*blob, error);
            CHECK(clip.has_value());
            if (clip) {
                std::printf("real L01BONUS: archive=%u bytes, header=%d Hz, %zu samples "
                            "(%.6f frames/ch), %.6f s at header rate\n",
                            bonus->size, clip->sample_rate, clip->samples.size(),
                            static_cast<double>(clip->samples.size()) /
                                static_cast<double>(clip->channels),
                            static_cast<double>(clip->samples.size()) /
                                static_cast<double>(clip->channels) /
                                static_cast<double>(clip->sample_rate));

                gm::audio::MusicTrack track;
                CHECK(track.load_at(*clip, 32000, 32000, &error));
                CHECK(track.play());
                std::printf("real L01BONUS: load_at(32000, 32000) frames=%llu "
                            "duration=%.6f s (header duration would be %.6f s)\n",
                            static_cast<unsigned long long>(track.frames()),
                            track.duration_seconds(),
                            static_cast<double>(track.frames()) /
                                static_cast<double>(clip->sample_rate));
                CHECK_NEAR(track.duration_seconds(),
                           static_cast<double>(track.frames()) / 32000.0, 1e-12);
                // The 32000 Hz rule halves the header-rate duration.
                const double header_duration =
                    static_cast<double>(track.frames()) /
                    static_cast<double>(clip->sample_rate);
                CHECK_NEAR(track.duration_seconds() * 2.0, header_duration, 1e-9);
                std::printf("real L01BONUS: at 32000 it is %.6f s, at the header rate "
                            "%.6f s\n",
                            track.duration_seconds(), header_duration);

                std::vector<float> out(4096 * 2, 0.0f);
                const int written = track.mix_into(out, 2);
                CHECK(written == 4096);
                const double peak = peak_abs(out);
                const double rms = rms_channel(out, 0, 4096, 0);
                std::printf("real L01BONUS: mixed=%d frames peak=%.6f rms(L)=%.6f "
                            "playing=%d\n",
                            written, peak, rms, track.playing() ? 1 : 0);
                CHECK(peak > 0.01);
                CHECK(peak <= 1.0 + 1e-6);
                CHECK(rms > 0.001);
            }
        }
    }

    // A real playlist file: the first name of MUSIC/Dm1A.sog, then that entry.
    const auto sog_path = *dir / "MUSIC" / "Dm1A.sog";
    if (fs::exists(sog_path)) {
        std::FILE* file = std::fopen(sog_path.string().c_str(), "rb");
        CHECK(file != nullptr);
        if (file != nullptr) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max(0L, size)));
            const std::size_t got =
                bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
            CHECK(got == bytes.size());
            const auto sog = gm::parse_sog(bytes, error);
            CHECK(sog.has_value());
            if (sog && !sog->names.empty()) {
                const std::string& first = sog->names.front();
                std::printf("real .sog: Dm1A.sog has %zu tracks, first=%s\n",
                            sog->names.size(), first.c_str());
                const gm::BigEntry* entry = find_entry(*archive, first);
                CHECK(entry != nullptr);
                if (entry != nullptr) {
                    const auto blob = archive->read(*entry, error);
                    CHECK(blob.has_value());
                    if (blob) {
                        auto clip = gm::decode_sou2(*blob, error);
                        CHECK(clip.has_value());
                        if (clip) {
                            gm::audio::MusicTrack track;
                            CHECK(track.load(*clip, clip->sample_rate, &error));
                            CHECK(track.play());
                            std::printf("real .sog track %s: header=%d Hz, frames=%llu, "
                                        "duration=%.3f s\n",
                                        first.c_str(), clip->sample_rate,
                                        static_cast<unsigned long long>(track.frames()),
                                        track.duration_seconds());
                            std::vector<float> out(512 * 2, 0.0f);
                            CHECK(track.mix_into(out, 2) == 512);
                            CHECK(peak_abs(out) > 0.0001);
                        }
                    }
                }
            }
        }
    } else {
        std::printf("real .sog: SKIP (Dm1A.sog missing)\n");
    }
}

} // namespace

int main() {
    test_fade_in_out();
    test_crossfade();
    test_finish_and_rewind();
    test_negatives();
    test_real_data();

    if (g_failures == 0) {
        std::printf("music: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "music: %d failure(s)\n", g_failures);
    return 1;
}
