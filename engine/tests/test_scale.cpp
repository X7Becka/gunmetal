// Interface scaling (M4.3.3): fit_letterbox + blit_scaled.
//
// No game files: a synthetic image with known pixels.  Aspect handling is
// checked on the real 320x200 frame in common window sizes, the filters on a
// 2x2 source, so both properties are pinned without a GPU or SDL.
#include "render/scale.h"

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
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", __FILE__,  \
                         __LINE__, #a, #b, static_cast<long long>(va),               \
                         static_cast<long long>(vb));                                \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

std::uint32_t rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return 0xff000000u | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | b;
}

void test_letterbox() {
    // 320x200 (1.6) into 16:9 -> full height, bars left and right.
    gm::ScaleRect r = gm::fit_letterbox(320, 200, 1920, 1080);
    CHECK_EQ(r.height, 1080);
    CHECK_EQ(r.width, 1728); // 1080 * 1.6
    CHECK_EQ(r.x, 96);
    CHECK_EQ(r.y, 0);

    // 320x200 into a taller 4:3 window -> full width, bars top and bottom.
    r = gm::fit_letterbox(320, 200, 800, 600);
    CHECK_EQ(r.width, 800);
    CHECK_EQ(r.height, 500);
    CHECK_EQ(r.x, 0);
    CHECK_EQ(r.y, 50);

    // Exact multiple: no bars at all.
    r = gm::fit_letterbox(320, 200, 1280, 800);
    CHECK_EQ(r.width, 1280);
    CHECK_EQ(r.height, 800);
    CHECK_EQ(r.x, 0);
    CHECK_EQ(r.y, 0);

    // Degenerate sizes never produce an empty box.
    r = gm::fit_letterbox(0, 200, 800, 600);
    CHECK_EQ(r.width, 0);
    r = gm::fit_letterbox(320, 200, 1, 1);
    CHECK_EQ(r.width, 1);
    CHECK_EQ(r.height, 1);
}

void test_nearest() {
    const std::uint8_t src[3 * 2 * 2] = {
        255, 0,   0,   0,   255, 0,   // red, green
        0,   0,   255, 255, 255, 255, // blue, white
    };
    std::vector<std::uint32_t> dst(4 * 4, 0);
    gm::blit_scaled(src, 2, 2, dst.data(), 4, 4, gm::ScaleFilter::Nearest);
    // 2x2 -> 4x4 stretches both axes by two, every source pixel is a 2x2 block.
    CHECK_EQ(dst[0], rgb(255, 0, 0));
    CHECK_EQ(dst[1], rgb(255, 0, 0));
    CHECK_EQ(dst[2], rgb(0, 255, 0));
    CHECK_EQ(dst[4], rgb(255, 0, 0));
    CHECK_EQ(dst[8], rgb(0, 0, 255));
    CHECK_EQ(dst[15], rgb(255, 255, 255));
}

void test_bilinear() {
    // 2x2 -> 4x4 (same aspect, so no bars): a horizontal black -> white ramp
    // in every row.  Edge clamping keeps the ends at 0 and 255.
    const std::uint8_t src[3 * 2 * 2] = {
        0,   0,   0,   255, 255, 255,
        0,   0,   0,   255, 255, 255,
    };
    std::vector<std::uint32_t> dst(4 * 4, 0);
    gm::blit_scaled(src, 2, 2, dst.data(), 4, 4, gm::ScaleFilter::Bilinear);
    CHECK_EQ(dst[0], rgb(0, 0, 0));
    CHECK_EQ(dst[3], rgb(255, 255, 255));
    const int mid1 = static_cast<int>((dst[1] >> 16) & 0xff);
    const int mid2 = static_cast<int>((dst[2] >> 16) & 0xff);
    CHECK(mid1 > 40 && mid1 < 90);
    CHECK(mid2 > 165 && mid2 < 215);
    // Vertical interpolation of an identical ramp changes nothing.
    CHECK_EQ(dst[4], dst[0]);
    CHECK_EQ(dst[7], dst[3]);
    CHECK_EQ(dst[12], dst[0]);
}

void test_bars_are_black() {
    std::uint8_t src[3 * 4 * 4] = {0};
    for (std::size_t i = 0; i < sizeof(src); i += 3) {
        src[i] = 200; // dark red 4x4
    }
    std::vector<std::uint32_t> dst(8 * 4, 0xdeadbeefu);
    gm::blit_scaled(src, 4, 4, dst.data(), 8, 4, gm::ScaleFilter::Nearest);
    // 1:1 aspect -> the 4x4 image is centred, columns 0..1 and 6..7 are bars.
    CHECK_EQ(dst[0], 0xff000000u);
    CHECK_EQ(dst[7], 0xff000000u);
    CHECK_EQ(dst[2], rgb(200, 0, 0));
    CHECK_EQ(dst[5], rgb(200, 0, 0));
}

} // namespace

int main() {
    test_letterbox();
    test_nearest();
    test_bilinear();
    test_bars_are_black();
    if (g_failures != 0) {
        std::fprintf(stderr, "scale: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("scale: ok\n");
    return 0;
}
