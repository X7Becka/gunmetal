// Integer upscale and palette fade (`M8.5` slice 2): render/upscale.h.
//
// No game files: a synthetic grid with unique colours pins the nearest-neighbour
// geometry point by point, a 320x200 frame pins the real canvas size and the
// timing, and the fade is checked for its three provable properties (black at
// level 0, identity at max_level, monotone in between) plus a 256-entry
// reference table produced by an independent Python implementation
// (docs/UPSCALE.md §3).
//
// Every check prints its numbers, so a green run is also the record the doc
// quotes.
#include "render/upscale.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return (static_cast<std::uint32_t>(r) << 24u) |
           (static_cast<std::uint32_t>(g) << 16u) |
           (static_cast<std::uint32_t>(b) << 8u) | static_cast<std::uint32_t>(a);
}

std::int64_t millis_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

// ---------------------------------------------------------------------------
// Reference fade tables.
//
// Independent implementation, run before the test was written (docs/UPSCALE.md
// §3 has the exact command):
//
//   lut[c] = (c * level + 11 // 2) // 11        # level 1, 5, 8, 11
//
// The values below are that Python output, pasted verbatim; the test compares
// all 256 entries of the C++ table with them, so the two implementations cannot
// drift unnoticed.
constexpr std::uint8_t kRefLevel1[256] = {
      0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
      1,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,
      3,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   4,   4,   4,   4,
      4,   4,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   6,   6,   6,
      6,   6,   6,   6,   6,   6,   6,   6,   7,   7,   7,   7,   7,   7,   7,   7,
      7,   7,   7,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   9,   9,
      9,   9,   9,   9,   9,   9,   9,   9,   9,  10,  10,  10,  10,  10,  10,  10,
     10,  10,  10,  10,  11,  11,  11,  11,  11,  11,  11,  11,  11,  11,  11,  12,
     12,  12,  12,  12,  12,  12,  12,  12,  12,  12,  13,  13,  13,  13,  13,  13,
     13,  13,  13,  13,  13,  14,  14,  14,  14,  14,  14,  14,  14,  14,  14,  14,
     15,  15,  15,  15,  15,  15,  15,  15,  15,  15,  15,  16,  16,  16,  16,  16,
     16,  16,  16,  16,  16,  16,  17,  17,  17,  17,  17,  17,  17,  17,  17,  17,
     17,  18,  18,  18,  18,  18,  18,  18,  18,  18,  18,  18,  19,  19,  19,  19,
     19,  19,  19,  19,  19,  19,  19,  20,  20,  20,  20,  20,  20,  20,  20,  20,
     20,  20,  21,  21,  21,  21,  21,  21,  21,  21,  21,  21,  21,  22,  22,  22,
     22,  22,  22,  22,  22,  22,  22,  22,  23,  23,  23,  23,  23,  23,  23,  23,
};

constexpr std::uint8_t kRefLevel5[256] = {
      0,   0,   1,   1,   2,   2,   3,   3,   4,   4,   5,   5,   5,   6,   6,   7,
      7,   8,   8,   9,   9,  10,  10,  10,  11,  11,  12,  12,  13,  13,  14,  14,
     15,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  20,  20,  21,  21,
     22,  22,  23,  23,  24,  24,  25,  25,  25,  26,  26,  27,  27,  28,  28,  29,
     29,  30,  30,  30,  31,  31,  32,  32,  33,  33,  34,  34,  35,  35,  35,  36,
     36,  37,  37,  38,  38,  39,  39,  40,  40,  40,  41,  41,  42,  42,  43,  43,
     44,  44,  45,  45,  45,  46,  46,  47,  47,  48,  48,  49,  49,  50,  50,  50,
     51,  51,  52,  52,  53,  53,  54,  54,  55,  55,  55,  56,  56,  57,  57,  58,
     58,  59,  59,  60,  60,  60,  61,  61,  62,  62,  63,  63,  64,  64,  65,  65,
     65,  66,  66,  67,  67,  68,  68,  69,  69,  70,  70,  70,  71,  71,  72,  72,
     73,  73,  74,  74,  75,  75,  75,  76,  76,  77,  77,  78,  78,  79,  79,  80,
     80,  80,  81,  81,  82,  82,  83,  83,  84,  84,  85,  85,  85,  86,  86,  87,
     87,  88,  88,  89,  89,  90,  90,  90,  91,  91,  92,  92,  93,  93,  94,  94,
     95,  95,  95,  96,  96,  97,  97,  98,  98,  99,  99, 100, 100, 100, 101, 101,
    102, 102, 103, 103, 104, 104, 105, 105, 105, 106, 106, 107, 107, 108, 108, 109,
    109, 110, 110, 110, 111, 111, 112, 112, 113, 113, 114, 114, 115, 115, 115, 116,
};

constexpr std::uint8_t kRefLevel8[256] = {
      0,   1,   1,   2,   3,   4,   4,   5,   6,   7,   7,   8,   9,   9,  10,  11,
     12,  12,  13,  14,  15,  15,  16,  17,  17,  18,  19,  20,  20,  21,  22,  23,
     23,  24,  25,  25,  26,  27,  28,  28,  29,  30,  31,  31,  32,  33,  33,  34,
     35,  36,  36,  37,  38,  39,  39,  40,  41,  41,  42,  43,  44,  44,  45,  46,
     47,  47,  48,  49,  49,  50,  51,  52,  52,  53,  54,  55,  55,  56,  57,  57,
     58,  59,  60,  60,  61,  62,  63,  63,  64,  65,  65,  66,  67,  68,  68,  69,
     70,  71,  71,  72,  73,  73,  74,  75,  76,  76,  77,  78,  79,  79,  80,  81,
     81,  82,  83,  84,  84,  85,  86,  87,  87,  88,  89,  89,  90,  91,  92,  92,
     93,  94,  95,  95,  96,  97,  97,  98,  99, 100, 100, 101, 102, 103, 103, 104,
    105, 105, 106, 107, 108, 108, 109, 110, 111, 111, 112, 113, 113, 114, 115, 116,
    116, 117, 118, 119, 119, 120, 121, 121, 122, 123, 124, 124, 125, 126, 127, 127,
    128, 129, 129, 130, 131, 132, 132, 133, 134, 135, 135, 136, 137, 137, 138, 139,
    140, 140, 141, 142, 143, 143, 144, 145, 145, 146, 147, 148, 148, 149, 150, 151,
    151, 152, 153, 153, 154, 155, 156, 156, 157, 158, 159, 159, 160, 161, 161, 162,
    163, 164, 164, 165, 166, 167, 167, 168, 169, 169, 170, 171, 172, 172, 173, 174,
    175, 175, 176, 177, 177, 178, 179, 180, 180, 181, 182, 183, 183, 184, 185, 185,
};

constexpr std::uint8_t kRefLevel11[256] = {
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
     16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
     32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
     48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
     64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
     80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
     96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
};

constexpr int kReferenceMax = 11;

void check_reference(int level, const std::uint8_t (&reference)[gm::kFadeLutSize]) {
    std::uint8_t lut[gm::kFadeLutSize] = {};
    std::string error;
    CHECK(gm::fade_lut(level, kReferenceMax, lut, &error));
    int mismatches = 0;
    int first = -1;
    for (int value = 0; value < gm::kFadeLutSize; ++value) {
        if (lut[value] != reference[value]) {
            if (first < 0) {
                first = value;
            }
            ++mismatches;
        }
    }
    CHECK_EQ(mismatches, 0);
    if (first >= 0) {
        std::fprintf(stderr, "  level %d first mismatch at %d: %u vs %u\n", level,
                     first, static_cast<unsigned>(lut[first]),
                     static_cast<unsigned>(reference[first]));
    }
    std::printf("fade lut level %d/%d: %d/256 entries equal the Python reference\n",
                level, kReferenceMax, gm::kFadeLutSize - mismatches);
}

void test_reference_tables() {
    check_reference(1, kRefLevel1);
    check_reference(5, kRefLevel5);
    check_reference(8, kRefLevel8);
    check_reference(11, kRefLevel11);
    // Boundaries of the curve, spelled out (they are what the engine's own
    // index table shows at levels 0 and 11, docs/UPSCALE.md §3).
    std::uint8_t lut[gm::kFadeLutSize] = {};
    std::string error;
    CHECK(gm::fade_lut(0, kReferenceMax, lut, &error));
    CHECK_EQ(lut[0], 0);
    CHECK_EQ(lut[255], 0);
    CHECK(gm::fade_lut(kReferenceMax, kReferenceMax, lut, &error));
    int identity = 0;
    for (int value = 0; value < gm::kFadeLutSize; ++value) {
        identity += lut[value] == value ? 1 : 0;
    }
    CHECK_EQ(identity, gm::kFadeLutSize);
    std::printf("fade lut 0/%d: 0 x 256 (black); %d/%d: %d/256 identity\n",
                kReferenceMax, kReferenceMax, kReferenceMax, identity);
}

// ---------------------------------------------------------------------------
// Nearest-neighbour geometry: a 3x2 grid with six unique colours, every
// destination pixel checked against its source pixel.

void test_nearest_grid() {
    constexpr int width = 3;
    constexpr int height = 2;
    const std::uint32_t source[width * height] = {
        rgba(255, 0, 0, 255),   rgba(0, 255, 0, 255),   rgba(0, 0, 255, 255),
        rgba(255, 255, 0, 128), rgba(0, 255, 255, 200), rgba(255, 0, 255, 0),
    };
    for (const int scale : {1, 2, 3, 8}) {
        std::vector<std::uint32_t> out;
        gm::UpscaleStats stats;
        std::string error = "stale";
        const bool ok = gm::upscale_nearest(source, width, height, scale, out, &error,
                                            &stats);
        CHECK(ok);
        CHECK(error.empty());
        if (!ok) {
            continue;
        }
        const std::size_t expected = static_cast<std::size_t>(width) * height *
                                     static_cast<std::size_t>(scale) *
                                     static_cast<std::size_t>(scale);
        CHECK_EQ(out.size(), expected);
        CHECK_EQ(stats.scale, scale);
        CHECK_EQ(stats.pixels_in, static_cast<std::size_t>(width) * height);
        CHECK_EQ(stats.pixels_out, expected);
        CHECK_EQ(stats.pixels_out,
                 stats.pixels_in * static_cast<std::size_t>(scale) * scale);

        int bad = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        const std::size_t index =
                            static_cast<std::size_t>(y * scale + dy) *
                                static_cast<std::size_t>(width * scale) +
                            static_cast<std::size_t>(x * scale + dx);
                        if (out[index] != source[y * width + x]) {
                            ++bad;
                        }
                    }
                }
            }
        }
        CHECK_EQ(bad, 0);
        std::printf("nearest %dx%d scale %d -> %dx%d, pixels %zu -> %zu, "
                    "mismatched destination pixels %d\n",
                    width, height, scale, width * scale, height * scale,
                    stats.pixels_in, stats.pixels_out, bad);
    }
}

void test_nearest_canvas() {
    // The real front-end canvas: 320x200 (ui/pager_render.h kPagerCanvasWidth,
    // ui/menu_render.h kMenuCanvasWidth), every pixel distinct.
    constexpr int width = 320;
    constexpr int height = 200;
    std::vector<std::uint32_t> source(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            source[static_cast<std::size_t>(y) * width + x] =
                rgba(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y),
                     static_cast<std::uint8_t>((x * 7 + y * 13) & 0xFF), 255);
        }
    }
    for (const int scale : {1, 2, 4, 8}) {
        std::vector<std::uint32_t> out;
        gm::UpscaleStats stats;
        std::string error;
        CHECK(gm::upscale_nearest(source.data(), width, height, scale, out, &error,
                                  &stats));
        CHECK_EQ(out.size(), static_cast<std::size_t>(width) * scale * height * scale);
        int bad = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::uint32_t want = source[static_cast<std::size_t>(y) * width + x];
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        const std::size_t index =
                            static_cast<std::size_t>(y * scale + dy) *
                                static_cast<std::size_t>(width * scale) +
                            static_cast<std::size_t>(x * scale + dx);
                        bad += out[index] == want ? 0 : 1;
                    }
                }
            }
        }
        CHECK_EQ(bad, 0);
        std::printf("nearest canvas 320x200 scale %d -> %dx%d, pixels %zu -> %zu "
                    "(bad %d)\n",
                    scale, width * scale, height * scale, stats.pixels_in,
                    stats.pixels_out, bad);
    }
}

// ---------------------------------------------------------------------------
// Fade: boundaries, monotonicity, alpha, determinism.

std::vector<std::uint32_t> fade_sample() {
    // One pixel per channel value: r = c, g = 255 - c, b = 7c, alpha = c.
    std::vector<std::uint32_t> frame(gm::kFadeLutSize);
    for (int c = 0; c < gm::kFadeLutSize; ++c) {
        frame[static_cast<std::size_t>(c)] = rgba(
            static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(255 - c),
            static_cast<std::uint8_t>((c * 7) & 0xFF), static_cast<std::uint8_t>(c));
    }
    return frame;
}

void test_fade_boundaries() {
    const std::vector<std::uint32_t> original = fade_sample();
    std::string error = "stale";

    std::vector<std::uint32_t> black = original;
    CHECK(gm::fade_rgba(black, 0, kReferenceMax, &error));
    CHECK(error.empty());
    int rgb_nonzero = 0;
    int alpha_kept = 0;
    for (std::size_t i = 0; i < black.size(); ++i) {
        rgb_nonzero += (black[i] >> 8u) != 0u ? 1 : 0;
        alpha_kept += (black[i] & 0xFFu) == (original[i] & 0xFFu) ? 1 : 0;
    }
    CHECK_EQ(rgb_nonzero, 0);
    CHECK_EQ(alpha_kept, static_cast<int>(original.size()));
    std::printf("fade level 0/%d: nonzero RGB pixels %d/256, alpha preserved "
                "%d/256 (0x%08x -> 0x%08x)\n",
                kReferenceMax, rgb_nonzero, alpha_kept, original[200], black[200]);

    std::vector<std::uint32_t> identity = original;
    CHECK(gm::fade_rgba(identity, kReferenceMax, kReferenceMax, &error));
    CHECK(std::memcmp(identity.data(), original.data(),
                      original.size() * sizeof(std::uint32_t)) == 0);
    std::printf("fade level %d/%d: %zu/%zu pixels byte-identical (0x%08x -> 0x%08x)\n",
                kReferenceMax, kReferenceMax, identity.size(), original.size(),
                original[200], identity[200]);

    // Levels above max clamp to the identity (model, docs/UPSCALE.md §3): the
    // engine's own tables above 11 are not identity, see the doc.
    std::vector<std::uint32_t> clamped_high = original;
    CHECK(gm::fade_rgba(clamped_high, 15, kReferenceMax, &error));
    CHECK(std::memcmp(clamped_high.data(), original.data(),
                      original.size() * sizeof(std::uint32_t)) == 0);
    std::vector<std::uint32_t> clamped_low = original;
    CHECK(gm::fade_rgba(clamped_low, -7, kReferenceMax, &error));
    CHECK_EQ(clamped_low[200] >> 8u, 0u);
    std::printf("fade clamp: level 15 -> identity, level -7 -> black\n");

    std::vector<std::uint32_t> frame(static_cast<std::size_t>(320) * 200);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        frame[i] = original[i % original.size()];
    }
    for (const int level : {1, 5, 7, 10}) {
        std::vector<std::uint32_t> once = frame;
        std::vector<std::uint32_t> twice = frame;
        CHECK(gm::fade_rgba(once, level, kReferenceMax, &error));
        CHECK(gm::fade_rgba(twice, level, kReferenceMax, &error));
        CHECK(once == twice);
        int grew = 0;
        for (std::size_t i = 0; i < once.size(); ++i) {
            for (int shift : {24, 16, 8}) {
                const std::uint32_t before = (frame[i] >> shift) & 0xFFu;
                const std::uint32_t after = (once[i] >> shift) & 0xFFu;
                grew += after > before ? 1 : 0;
            }
        }
        CHECK_EQ(grew, 0);
        std::printf("fade level %d/%d: deterministic on %zu px, channels that grew %d, "
                    "0x%08x -> 0x%08x\n",
                    level, kReferenceMax, once.size(), grew, frame[200], once[200]);
    }
}

void test_fade_monotone() {
    for (int level = 1; level < kReferenceMax; ++level) {
        std::uint8_t lut[gm::kFadeLutSize] = {};
        std::string error;
        CHECK(gm::fade_lut(level, kReferenceMax, lut, &error));
        int decreasing = 0;
        int grew = 0;
        for (int value = 0; value < gm::kFadeLutSize; ++value) {
            decreasing += value + 1 < gm::kFadeLutSize && lut[value + 1] < lut[value];
            grew += lut[value] > value ? 1 : 0;
        }
        CHECK_EQ(decreasing, 0);
        CHECK_EQ(grew, 0);
        std::printf("fade lut level %d/%d: monotone (decreasing %d), no channel grew "
                    "(%d), 0->%u 128->%u 255->%u\n",
                    level, kReferenceMax, decreasing, grew,
                    static_cast<unsigned>(lut[0]), static_cast<unsigned>(lut[128]),
                    static_cast<unsigned>(lut[255]));
    }
}

// ---------------------------------------------------------------------------
// Errors and their contract: false + text, `out` emptied, buffer untouched.

void test_errors() {
    const std::uint32_t pixel = rgba(1, 2, 3, 4);
    std::vector<std::uint32_t> out(4, 0xdeadbeefu);
    std::string error;

    for (const int scale : {0, -1, 9, 1000}) {
        error.clear();
        out.assign(4, 0xdeadbeefu);
        const bool ok = gm::upscale_nearest(&pixel, 1, 1, scale, out, &error);
        CHECK(!ok);
        CHECK(!error.empty());
        CHECK(out.empty());
        std::printf("error scale %d: \"%s\"\n", scale, error.c_str());
    }
    for (const int w : {0, -3}) {
        error.clear();
        const bool ok = gm::upscale_nearest(&pixel, w, 1, 2, out, &error);
        CHECK(!ok);
        CHECK(!error.empty());
        CHECK(out.empty());
        std::printf("error source %dx1: \"%s\"\n", w, error.c_str());
    }
    error.clear();
    CHECK(!gm::upscale_nearest(nullptr, 1, 1, 2, out, &error));
    CHECK(!error.empty());
    CHECK(out.empty());
    std::printf("error null source: \"%s\"\n", error.c_str());

    // 2^30 * 2^30 pixels at scale 8: the destination count no longer fits in
    // `std::size_t`, and nothing is allocated.
    error.clear();
    CHECK(!gm::upscale_nearest(&pixel, 1 << 30, 1 << 30, 8, out, &error));
    CHECK(!error.empty());
    CHECK(out.empty());
    std::printf("error overflow 2^30x2^30 at scale 8: \"%s\"\n", error.c_str());

    // A successful 1x1 upscale still works after all those failures.
    error.clear();
    CHECK(gm::upscale_nearest(&pixel, 1, 1, 3, out, &error));
    CHECK(error.empty());
    CHECK_EQ(out.size(), static_cast<std::size_t>(9));
    CHECK_EQ(out[0], pixel);
    CHECK_EQ(out[8], pixel);

    // fade: only max_level < 1 is refused; the buffer is left untouched then.
    std::vector<std::uint32_t> frame = fade_sample();
    const std::vector<std::uint32_t> before = frame;
    for (const int max_level : {0, -5}) {
        error.clear();
        CHECK(!gm::fade_rgba(frame, 3, max_level, &error));
        CHECK(!error.empty());
        CHECK(frame == before);
        std::printf("error fade max_level %d: \"%s\" (buffer untouched)\n", max_level,
                    error.c_str());
    }
    CHECK(!gm::fade_rgba(frame, 3, 0, nullptr)); // null error is allowed
    std::uint8_t lut[gm::kFadeLutSize] = {};
    CHECK(!gm::fade_lut(0, 0, lut, nullptr));

    // Empty buffers: an upscale is impossible (no size), a fade is a no-op.
    std::vector<std::uint32_t> empty;
    error.clear();
    CHECK(!gm::upscale_nearest(&pixel, 0, 0, 1, empty, &error));
    CHECK(gm::fade_rgba(empty, 5, kReferenceMax, &error));
    CHECK(error.empty());
    CHECK(empty.empty());
    std::printf("empty buffer: upscale refused, fade accepted as a no-op\n");

    // Error text of a successful call is cleared, not left stale.
    std::vector<std::uint32_t> ok_out;
    error = "stale";
    CHECK(gm::upscale_nearest(&pixel, 1, 1, 1, ok_out, &error));
    CHECK(error.empty());
}

// ---------------------------------------------------------------------------
// Performance: LUT against the per-channel formula, and the canvas upscale.

void fade_per_channel(std::vector<std::uint32_t>& rgba, int level, int max_level) {
    for (std::uint32_t& colour : rgba) {
        std::uint32_t channels[3] = {(colour >> 24u) & 0xFFu, (colour >> 16u) & 0xFFu,
                                     (colour >> 8u) & 0xFFu};
        for (std::uint32_t& value : channels) {
            value = (value * static_cast<std::uint32_t>(level) +
                     static_cast<std::uint32_t>(max_level / 2)) /
                    static_cast<std::uint32_t>(max_level);
        }
        colour = (channels[0] << 24u) | (channels[1] << 16u) | (channels[2] << 8u) |
                 (colour & 0xFFu);
    }
}

void test_performance() {
    constexpr int width = 320;
    constexpr int height = 200;
    std::vector<std::uint32_t> source(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::uint32_t>(i * 2654435761u) | 0xFFu;
    }

    std::vector<std::uint32_t> out;
    std::string error;
    const auto upscale_start = std::chrono::steady_clock::now();
    constexpr int upscale_runs = 5;
    for (int i = 0; i < upscale_runs; ++i) {
        CHECK(gm::upscale_nearest(source.data(), width, height, 8, out, &error));
    }
    const std::int64_t upscale_ms = millis_since(upscale_start);
    const double out_pixels = static_cast<double>(out.size()) * upscale_runs / 1.0e6;
    std::printf("perf upscale 320x200 x8 -> %zux%zu px, %d runs in %lld ms (%.1f Mpx/s)\n",
                static_cast<std::size_t>(width) * 8, static_cast<std::size_t>(height) * 8,
                upscale_runs, static_cast<long long>(upscale_ms),
                upscale_ms > 0 ? out_pixels / (static_cast<double>(upscale_ms) / 1000.0)
                               : 0.0);

    // Same input for both paths: the LUT must equal the formula, and be faster.
    std::vector<std::uint32_t> frames = source;
    std::vector<std::uint32_t> reference = source;
    constexpr int level = 7;
    CHECK(gm::fade_rgba(frames, level, kReferenceMax, &error));
    fade_per_channel(reference, level, kReferenceMax);
    CHECK(frames == reference);

    constexpr int fade_runs = 100;
    const auto lut_start = std::chrono::steady_clock::now();
    for (int i = 0; i < fade_runs; ++i) {
        CHECK(gm::fade_rgba(frames, level, kReferenceMax, &error));
    }
    const std::int64_t lut_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - lut_start)
            .count();
    std::vector<std::uint32_t> per_pixel = source;
    const auto per_start = std::chrono::steady_clock::now();
    for (int i = 0; i < fade_runs; ++i) {
        fade_per_channel(per_pixel, level, kReferenceMax);
    }
    const std::int64_t per_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - per_start)
            .count();
    std::printf("perf fade 64000 px level %d/%d: LUT %lld us, per-channel %lld us "
                "(%d runs, %.1f vs %.1f ns/px)\n",
                level, kReferenceMax, static_cast<long long>(lut_us),
                static_cast<long long>(per_us), fade_runs,
                static_cast<double>(lut_us) * 1000.0 / fade_runs / 64000.0,
                static_cast<double>(per_us) * 1000.0 / fade_runs / 64000.0);
    std::printf("perf fade: LUT equal to the per-channel formula on 64000 px\n");
}

} // namespace

int main() {
    test_nearest_grid();
    test_nearest_canvas();
    test_fade_boundaries();
    test_fade_monotone();
    test_reference_tables();
    test_errors();
    test_performance();
    if (g_failures != 0) {
        std::fprintf(stderr, "upscale: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("upscale: ok\n");
    return 0;
}
