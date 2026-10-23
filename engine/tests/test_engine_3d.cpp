// Engine 3D audio formulas (`M7.3`, docs/AUDIO_3D.md).
//
// The vectors below are produced by the Python reference:
//   python3 tools/re/audio_3d.py --calc <case> --json
// so a drift on either side fails with a concrete number.
#include "audio/engine_3d.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                        \
    do {                                                                             \
        const double va = (a);                                                       \
        const double vb = (b);                                                       \
        if (!(std::fabs(va - vb) <= (eps))) {                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s (%.9f) != %s (%.9f)\n", __FILE__,   \
                         __LINE__, #a, va, #b, vb);                                   \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

void test_distance() {
    // 0x4344df: max + 0.25*(mid+min), not sqrt.
    CHECK_NEAR(gm::audio::length_approx(3.0, 4.0, 0.0), 4.75, 1e-12);
    CHECK_NEAR(gm::audio::length_approx(100.0, 100.0, 100.0), 150.0, 1e-12);
    CHECK_NEAR(gm::audio::length_approx(-7.0, 0.0, 0.0), 7.0, 1e-12);
    CHECK_NEAR(gm::audio::clamped_distance(0.5), 1.0, 1e-12);
    CHECK_NEAR(gm::audio::clamped_distance(2.5), 2.5, 1e-12);
}

void test_level0_first_emitter() {
    // tools/re/audio_3d.py --calc --call level0:0 --listener 0,3200,48,0
    //   d = (48, 8, -184), right = (1, 0, 0), volume 64, atten 0.03
    const double distance = gm::audio::length_approx(48.0, 8.0, -184.0);
    CHECK_NEAR(distance, 198.0, 1e-9);

    const int gain = gm::audio::gain_of(64, 0.03, distance);
    CHECK(gain == 58);

    const auto right = gm::audio::basis_right(0.0, 0.0, 0.0);
    CHECK_NEAR(right[0], 1.0, 1e-12);
    CHECK_NEAR(std::fabs(right[1]), 0.0, 1e-12);
    CHECK_NEAR(std::fabs(right[2]), 0.0, 1e-12);

    const int pan = gm::audio::pan_of(48.0, 8.0, -184.0, right, distance);
    CHECK(pan == 159);

    const int volume_index = gm::audio::request_volume(gain);
    CHECK(volume_index == 29);
    const auto channels = gm::audio::channel_gains(volume_index, pan);
    CHECK(channels.has_value());
    if (channels) {
        CHECK(channels->left == 23);
        CHECK(channels->right == 32); // 38 clamped to the 5-bit maximum
    }

    // Same source with the listener turned +90 degrees: right = (0, -1, 0),
    // so dot = -dy = -8 and the pan sits at 122.
    const auto turned = gm::audio::basis_right(0.0, 0.0, M_PI / 2.0);
    CHECK_NEAR(turned[0], 0.0, 1e-12);
    CHECK_NEAR(turned[1], -1.0, 1e-12);
    const int pan_turned =
        gm::audio::pan_of(48.0, 8.0, -184.0, turned, distance);
    CHECK(pan_turned == 122);
    const auto turned_channels = gm::audio::channel_gains(volume_index, pan_turned);
    CHECK(turned_channels.has_value());
    if (turned_channels) {
        CHECK(turned_channels->left == 32);
        CHECK(turned_channels->right == 29);
    }
}

void test_explicit_position() {
    // tools/re/audio_3d.py --calc --pos 100,100,100 --listener 0,0,0 --json
    const double distance = gm::audio::length_approx(100.0, 100.0, 100.0);
    const int gain = gm::audio::gain_of(64, 0.03, distance);
    CHECK(gain == 59);
    const auto right = gm::audio::basis_right(0.0, 0.0, 0.0);
    const int pan = gm::audio::pan_of(100.0, 100.0, 100.0, right, distance);
    CHECK(pan == 213);
    const auto channels = gm::audio::channel_gains(gm::audio::request_volume(gain), pan);
    CHECK(channels.has_value());
    if (channels) {
        CHECK(channels->left == 10);
        CHECK(channels->right == 32);
    }
}

void test_muffle_bits() {
    // --call level0:0 --listener 0,3200,48,0 --muffle 1:
    // gain = trunc(64 - 0.03*2*198) = 52, >> 2 = 13, v = 6, L/R = 5/8.
    const double distance = gm::audio::length_approx(48.0, 8.0, -184.0);
    const int gain = gm::audio::gain_of(64, 0.03, distance, 1);
    CHECK(gain == 13);
    const int volume_index = gm::audio::request_volume(gain);
    CHECK(volume_index == 6);
    const auto channels = gm::audio::channel_gains(volume_index, 159);
    CHECK(channels.has_value());
    if (channels) {
        CHECK(channels->left == 5);
        CHECK(channels->right == 8);
    }
    // n = 2 (both sector bits) shifts by four.
    CHECK(gm::audio::gain_of(64, 0.03, 198.0, 2) ==
          (static_cast<int>(64 - 0.03 * 4.0 * 198.0) >> 4));
}

void test_silence_threshold() {
    // gain < 2 means the engine never creates the request (0x40be58).
    CHECK(gm::audio::request_volume(0) == 0);
    CHECK(gm::audio::request_volume(1) == 0);   // 1*32>>6 = 0
    CHECK(gm::audio::request_volume(3) == 1);
    CHECK(gm::audio::request_volume(4) == 2);
    CHECK(gm::audio::request_volume(64) == 32);
    CHECK(gm::audio::request_volume(1000) == 32); // clamped
    CHECK(!gm::audio::channel_gains(0, 128).has_value());
    // A source inside one unit sits at the centre; the distance is clamped.
    CHECK(gm::audio::pan_of(0.2, 0.0, 0.0, {1.0, 0.0, 0.0}, 0.2) == 128);
    CHECK_NEAR(gm::audio::clamped_distance(0.2), 1.0, 1e-12);
}

} // namespace

int main() {
    test_distance();
    test_level0_first_emitter();
    test_explicit_position();
    test_muffle_bits();
    test_silence_threshold();
    if (g_failures != 0) {
        std::fprintf(stderr, "engine_3d: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("engine_3d: ok\n");
    return 0;
}
