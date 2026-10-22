// Effect animation frames (M4.2.6): `0x41de7d` and the `.sbw` effect table.
//
// Synthetic checks pin the four branches (one-shot hold, cycle, special mode,
// refused negative time), then the shipped worlds are scanned: every negative
// polygon texture must resolve to an entry.  The raw count matches
// `tools/re/effects.py --summary` exactly (6 117 polygons with ``texture<0``);
// the tool's *distinct* total (48) is smaller because it applies the setup's
// ``n_idx > 2`` gate and the overlay rule, so this test only requires the
// broader invariants.
#include "core/bytes.h"
#include "game/effect_anim.h"
#include "game_path.h"
#include "res/sbw.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

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

constexpr const char* kGameDir = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

void test_formula() {
    // 8 frames, period 100 ticks.
    CHECK_EQ(*gm::effect_frame_index(1, 100, 8, 0), std::size_t{0});
    CHECK_EQ(*gm::effect_frame_index(1, 100, 8, 12), std::size_t{0});
    CHECK_EQ(*gm::effect_frame_index(1, 100, 8, 13), std::size_t{1});
    CHECK_EQ(*gm::effect_frame_index(1, 100, 8, 50), std::size_t{4});
    CHECK_EQ(*gm::effect_frame_index(1, 100, 8, 99), std::size_t{7});
    CHECK_EQ(*gm::effect_frame_index(1, 100, 8, 100), std::size_t{0}); // loops
    // One-shot holds the last frame once the period is over.
    CHECK_EQ(*gm::effect_frame_index(0, 100, 8, 99), std::size_t{7});
    CHECK_EQ(*gm::effect_frame_index(0, 100, 8, 100), std::size_t{7});
    CHECK_EQ(*gm::effect_frame_index(0, 100, 8, 10'000), std::size_t{7});
    // Special modes (100/101/102) always show frame 0.
    CHECK_EQ(*gm::effect_frame_index(100, 100, 8, 50), std::size_t{0});
    CHECK_EQ(*gm::effect_frame_index(102, 0, 8, 50), std::size_t{0});
    // Refusals: negative time, empty frame list, cyclic period 0.
    CHECK(!gm::effect_frame_index(1, 100, 8, -1).has_value());
    CHECK(!gm::effect_frame_index(0, 100, 0, 5).has_value());
    CHECK(!gm::effect_frame_index(1, 0, 8, 5).has_value());

    // A single-frame effect is always frame 0.
    CHECK_EQ(*gm::effect_frame_index(1, 100, 1, 77), std::size_t{0});
}

void test_mapping() {
    std::vector<gm::SbwEffectRef> effects(3);
    effects[0].name = "ONE";
    effects[0].mode = 1;
    effects[0].period = 40;
    effects[0].ids = {10, 11, 12, 13};
    effects[1].name = "TWO";
    effects[1].mode = 0;
    effects[1].period = 90;
    effects[1].ids = {20};
    effects[2].name = "THREE";

    CHECK(gm::effect_for_texture(effects, -1) == &effects[0]);
    CHECK(gm::effect_for_texture(effects, -2) == &effects[1]);
    CHECK(gm::effect_for_texture(effects, -3) == &effects[2]);
    CHECK(gm::effect_for_texture(effects, -4) == nullptr); // past the table
    CHECK(gm::effect_for_texture(effects, 0) == nullptr);  // no effect
    CHECK(gm::effect_for_texture(effects, 7) == nullptr);  // a real texture

    // start_tick is subtracted (poly+0x68): at t=10 the 4-frame cycle is 1.
    CHECK_EQ(*gm::effect_frame_texture(effects[0], 100, 110), std::uint16_t{11});
    CHECK_EQ(*gm::effect_frame_texture(effects[0], 100, 100), std::uint16_t{10});
    // Before the start tick the engine's remainder is negative — refused.
    CHECK(!gm::effect_frame_texture(effects[0], 100, 90).has_value());
}

/// Count the negative-texture polygons of one world and resolve them.
std::pair<std::size_t, std::set<std::string>> scan_world(const fs::path& path) {
    const auto bytes = read_file(path);
    std::string error;
    auto header = gm::parse_sbw_header(bytes, error);
    CHECK(header.has_value());
    if (!header) {
        std::fprintf(stderr, "  %s: %s\n", path.filename().string().c_str(),
                     error.c_str());
        return {0, {}};
    }
    // The polygon stream follows the header; `parse_sbw` is the full reader.
    auto world = gm::parse_sbw(bytes, error);
    CHECK(world.has_value());
    if (!world) {
        return {0, {}};
    }
    std::set<std::string> names;
    std::size_t hits = 0;
    for (const gm::SbwGeometry& geometry : world->geometry) {
        for (const gm::SbwPoly& poly : geometry.polys) {
            // .sbw stores the index as u16; negatives are two's complement.
            const int texture = static_cast<std::int16_t>(poly.texture);
            if (texture >= 0) {
                continue;
            }
            ++hits;
            const gm::SbwEffectRef* effect =
                gm::effect_for_texture(header->effects, texture);
            CHECK(effect != nullptr);
            if (effect != nullptr && !effect->ids.empty()) {
                names.insert(effect->name);
                // Shipped data always has t >= 0, so a frame exists.
                CHECK(gm::effect_frame_texture(*effect, 0, 0).has_value());
            }
        }
    }
    return {hits, names};
}

} // namespace

int main() {
    test_formula();
    test_mapping();

    if (!fs::is_directory(kGameDir)) {
        std::printf("effect_anim: game directory not found, formula only\n");
    } else {
        std::set<std::string> distinct;
        std::size_t pairs = 0;
        std::size_t polys = 0;
        for (const auto& entry : fs::directory_iterator(kGameDir)) {
            if (!entry.is_regular_file() ||
                entry.path().extension() != ".sbw") {
                continue;
            }
            const auto [hits, names] = scan_world(entry.path());
            polys += hits;
            if (!names.empty()) {
                ++pairs; // one (level, effect) pair per used name set
            }
            distinct.insert(names.begin(), names.end());
        }
        std::printf("effect_anim: %zu negative-texture polygons, %zu distinct "
                    "effects, %zu worlds using them\n",
                    polys, distinct.size(), pairs);
        // docs/EFFECTS.md: exactly 6 117 polygons with a negative texture.
        CHECK_EQ(polys, std::size_t{6117});
        CHECK(distinct.size() >= 40);
        CHECK(pairs > 20);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "effect_anim: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("effect_anim: ok\n");
    return 0;
}
