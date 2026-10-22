// Full-screen haze fade (`SET_CAMERA_HAZE`, docs/SCREEN_FX.md).
//
// The blend must match gm/render.py::fade_rgb byte for byte, so the checks use
// exact integer expectations (alpha truncates: 0.5 -> 127, not 128).
#include "render/postfx.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va = (a);                                                         \
        const auto vb = (b);                                                         \
        if (!(va == vb)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a,   \
                         #b);                                                            \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

void test_alpha() {
    // ftol truncates (0x418c7c..0x418c90).
    CHECK_EQ(gm::haze_alpha(0.0), std::uint8_t{0});
    CHECK_EQ(gm::haze_alpha(-1.0), std::uint8_t{0});
    CHECK_EQ(gm::haze_alpha(0.001), std::uint8_t{0});
    CHECK_EQ(gm::haze_alpha(0.5), std::uint8_t{127});
    CHECK_EQ(gm::haze_alpha(1.0), std::uint8_t{255});
    CHECK_EQ(gm::haze_alpha(2.0), std::uint8_t{255});
    // The shipped fade values: 1.0, 0.35, 0.0 (docs/SCREEN_FX.md §2).
    CHECK_EQ(gm::haze_alpha(0.35), std::uint8_t{89});
}

void test_blend() {
    std::vector<std::uint8_t> frame = {10, 20, 30, 200, 100, 50};
    // haze 0: untouched (the default pipeline).
    gm::fade_white(frame, 0.0);
    CHECK_EQ(frame[0], std::uint8_t{10});
    CHECK_EQ(frame[5], std::uint8_t{50});

    // haze 1: white everywhere.
    gm::fade_white(frame, 1.0);
    for (std::uint8_t value : frame) {
        CHECK_EQ(value, std::uint8_t{255});
    }

    // alpha 127: out = (v * 128 + 255 * 127) / 255, floor.
    std::vector<std::uint8_t> black = {0, 0, 0};
    gm::fade_white(black, 0.5);
    CHECK_EQ(black[0], std::uint8_t{(255 * 127) / 255});
    CHECK_EQ(black[0], std::uint8_t{127});
    std::vector<std::uint8_t> white = {255, 255, 255};
    gm::fade_white(white, 0.5);
    CHECK_EQ(white[0], std::uint8_t{255});

    // Monotonic, and the neutral grey stays grey (equal channels stay equal).
    std::vector<std::uint8_t> low = {64, 64, 64};
    std::vector<std::uint8_t> high = {64, 64, 64};
    gm::fade_white(low, 0.25);
    gm::fade_white(high, 0.75);
    CHECK(low[0] < high[0]);
    CHECK_EQ(low[0], low[1]);
    CHECK_EQ(low[1], low[2]);

    // Trailing bytes that do not form a pixel are left alone (the full-white
    // shortcut, like gm/render.py, fills every byte).
    std::vector<std::uint8_t> odd = {0, 0, 0, 7};
    gm::fade_white(odd, 0.5);
    CHECK_EQ(odd[0], std::uint8_t{127});
    CHECK_EQ(odd[3], std::uint8_t{7});
}

} // namespace

int main() {
    test_alpha();
    test_blend();
    if (g_failures != 0) {
        std::fprintf(stderr, "postfx: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("postfx: ok\n");
    return 0;
}
