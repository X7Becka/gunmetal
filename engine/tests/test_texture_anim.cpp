// Texture scroll and s16 animation of a world polygon (`M4.2.2`, slice 2):
// `render/texture_anim.*`.
//
// Synthetic cases pin the s16/u16 conversion, the proven scroll formula of
// `0x434c11`/`0x434c80` and the `0x4342f0` 16-bit lerp (both the two-key
// engine form and the documented one-polygon model), plus every refusal.
// Real cases read `level0`/`dm1` and sweep all 36 shipped `.sbw` files:
// 2917 polygons with a non-zero scroll weight (1.24 % of 235 069), all 235 069
// with a non-zero `anim*` slot, max |scroll_u| = 15224 (dm2), max |scroll_v| =
// 10000 (level17).  Numbers and the proven/model split: docs/TEXTURE_ANIM.md.

#include "render/texture_anim.h"

#include "res/sbw.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

bool close(double a, double b, double tol = 1e-12) {
    const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
    return std::fabs(a - b) <= tol * scale;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

/// Mirror first (drvfs is slow), the installation second.
std::optional<fs::path> find_game_dir() {
    std::vector<fs::path> candidates;
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        candidates.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    candidates.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_directory(candidate, ec) && fs::is_regular_file(candidate / "level0.sbw", ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Synthetic: conversion, formula, refusals.

void synthetic_cases() {
    using gm::Vec3d;
    const Vec3d unit_x{1.0, 0.0, 0.0};
    const Vec3d unit_y{0.0, 1.0, 0.0};

    // Raw -> signed/unsigned slots: 0xFFFF is -1 for scroll (short) and 65535
    // for the anim halves (ushort), exactly as 0x434c11 / 0x4342f0 read them.
    {
        const auto raw = gm::texanim::PolyAnim::from_raw(0xFFFF, 0xFFF8, 0xFFFF, 0x0001);
        CHECK_EQ(raw.scroll_u, -1);
        CHECK_EQ(raw.scroll_v, -8);
        CHECK_EQ(raw.anim0, 65535);
        CHECK_EQ(raw.anim1, 1);
        CHECK(gm::texanim::has_scroll(raw));
        CHECK(gm::texanim::has_anim(raw));

        const auto zero = gm::texanim::PolyAnim::from_raw(0, 0, 0, 0);
        CHECK(!gm::texanim::has_scroll(zero));
        CHECK(!gm::texanim::has_anim(zero));

        const auto min16 = gm::texanim::PolyAnim::from_raw(0x8000, 0x7FFF, 0, 0);
        CHECK_EQ(min16.scroll_u, -32768);
        CHECK_EQ(min16.scroll_v, 32767);
        CHECK(gm::texanim::has_scroll(min16));
    }

    // Scroll: t = 0 is exactly zero; t = 1/91 is the weight along the axis.
    {
        gm::texanim::PolyAnim anim;
        anim.scroll_u = 256; // one world unit per tick along u
        anim.scroll_v = -512;

        const Vec3d at0 = gm::texanim::scroll_offset(anim, unit_x, unit_y, 0.0);
        CHECK(close(at0.x, 0.0) && close(at0.y, 0.0) && close(at0.z, 0.0));

        const Vec3d at1 = gm::texanim::scroll_offset(anim, unit_x, unit_y, 1.0);
        CHECK(close(at1.x, 1.0));
        CHECK(close(at1.y, -2.0));
        CHECK(close(at1.z, 0.0));

        const Vec3d at91 = gm::texanim::scroll_offset(anim, unit_x, unit_y, 91.0);
        CHECK(close(at91.x, 91.0));
        CHECK(close(at91.y, -182.0));

        // Direction: the offset stays a combination of the two axes, with the
        // weight's sign, and the projections scale linearly with t.
        gm::texanim::PolyAnim mixed;
        mixed.scroll_u = 256;
        mixed.scroll_v = 128;
        const Vec3d at2 = gm::texanim::scroll_offset(mixed, unit_x, unit_y, 2.0);
        CHECK(close(at2.x, 2.0));
        CHECK(close(at2.y, 1.0));
        CHECK(close(at2.x * unit_x.x + at2.y * unit_x.y + at2.z * unit_x.z, 2.0));
        CHECK(close(at2.x * unit_y.x + at2.y * unit_y.y + at2.z * unit_y.z, 1.0));

        // A negative tick is the engine's own sign (`* (float)-param_5`).
        const Vec3d back = gm::texanim::scroll_offset(anim, unit_x, unit_y, -1.0);
        CHECK(close(back.x, -1.0));
        CHECK(close(back.y, 2.0));

        // Non-unit axes are used verbatim (the documented formula); the
        // engine would normalise by |axis|^2 - see docs/TEXTURE_ANIM.md §2.
        const Vec3d long_u{2.0, 0.0, 0.0};
        const Vec3d stretched = gm::texanim::scroll_offset(anim, long_u, unit_y, 1.0);
        CHECK(close(stretched.x, 2.0));
        CHECK(close(stretched.y, -2.0));
    }

    // Scroll refusals: zero/non-finite axes, non-finite tick, s16 overflow.
    {
        using gm::texanim::scroll_offset;
        const gm::texanim::PolyAnim anim{100, 0, 0, 0};
        std::string error = "stale";

        const Vec3d zero_u = scroll_offset(anim, Vec3d{0.0, 0.0, 0.0}, unit_y, 1.0, &error);
        CHECK(close(zero_u.x, 0.0) && close(zero_u.y, 0.0) && close(zero_u.z, 0.0));
        CHECK(error.find("u_axis is zero") != std::string::npos);

        CHECK(close(scroll_offset(anim, unit_x, Vec3d{}, 1.0, &error).x, 0.0));
        CHECK(error.find("v_axis is zero") != std::string::npos);

        const double nan = std::nan("");
        const double inf = std::numeric_limits<double>::infinity();
        CHECK(close(scroll_offset(anim, unit_x, unit_y, nan, &error).x, 0.0));
        CHECK(error.find("tick is not finite") != std::string::npos);
        CHECK(close(scroll_offset(anim, unit_x, unit_y, inf, &error).x, 0.0));
        CHECK(error.find("tick is not finite") != std::string::npos);
        CHECK(close(scroll_offset(anim, Vec3d{nan, 0.0, 0.0}, unit_y, 1.0, &error).x, 0.0));
        CHECK(error.find("u_axis is not finite") != std::string::npos);
        CHECK(close(scroll_offset(anim, unit_x, Vec3d{0.0, inf, 0.0}, 1.0, &error).x, 0.0));
        CHECK(error.find("v_axis is not finite") != std::string::npos);

        // 32767 fits s16 and does not overflow in double; 32768 does not fit.
        gm::texanim::PolyAnim big;
        big.scroll_u = 32767;
        big.scroll_v = -32768;
        error = "stale";
        const Vec3d maxed = scroll_offset(big, unit_x, unit_y, 1.0, &error);
        CHECK(error.empty());
        CHECK(close(maxed.x, 32767.0 / 256.0));
        CHECK(close(maxed.y, -32768.0 / 256.0));
        CHECK(std::isfinite(maxed.x) && std::isfinite(maxed.y));
        CHECK_EQ(std::string(error), std::string());

        gm::texanim::PolyAnim too_big;
        too_big.scroll_u = 32768;
        CHECK(close(scroll_offset(too_big, unit_x, unit_y, 1.0, &error).x, 0.0));
        CHECK(error.find("must fit s16") != std::string::npos);

        // A tick so large that the product leaves double range is refused, not
        // returned as infinity.
        gm::texanim::PolyAnim huge;
        huge.scroll_u = 32767;
        CHECK(close(scroll_offset(huge, unit_x, unit_y, 1e308, &error).x, 0.0));
        CHECK(error.find("not finite") != std::string::npos);

        // A null error pointer must not crash.
        CHECK(close(scroll_offset(anim, Vec3d{}, unit_y, 1.0, nullptr).x, 0.0));
    }

    // The proven two-key lerp: float32 arithmetic, truncation toward zero.
    {
        gm::texanim::PolyAnim a{0, 0, 1000, 60000};
        gm::texanim::PolyAnim b{0, 0, 2000, 0};
        std::string error = "stale";

        const auto t0 = gm::texanim::anim_step(a, b, 0.0, &error);
        CHECK(error.empty());
        CHECK_EQ(t0.first, 1000);
        CHECK_EQ(t0.second, 60000);

        const auto half = gm::texanim::anim_step(a, b, 0.5, &error);
        CHECK_EQ(half.first, 1500);
        CHECK_EQ(half.second, 30000);

        const auto full = gm::texanim::anim_step(a, b, 1.0, &error);
        CHECK_EQ(full.first, 2000);
        CHECK_EQ(full.second, 0);

        // Truncation, not rounding: 1000 + 3*0.5 = 1001.5 -> 1001.
        gm::texanim::PolyAnim a1{0, 0, 1000, 0};
        gm::texanim::PolyAnim b1{0, 0, 1003, 0};
        CHECK_EQ(gm::texanim::anim_step(a1, b1, 0.5, &error).first, 1001);
        // ... and the same for a descending half: 1003 + (1000-1003)*0.5 =
        // 1001.5 -> 1001.
        CHECK_EQ(gm::texanim::anim_step(b1, a1, 0.5, &error).first, 1001);

        // 16-bit halves are independent: one may wrap through zero, the other
        // may stay put.
        gm::texanim::PolyAnim wrap_a{0, 0, 65535, 7};
        gm::texanim::PolyAnim wrap_b{0, 0, 0, 7};
        const auto wrapped = gm::texanim::anim_step(wrap_a, wrap_b, 0.5, &error);
        CHECK_EQ(wrapped.first, 32767); // (0 - 65535)*0.5 + 65535 = 32767.5
        CHECK_EQ(wrapped.second, 7);
    }

    // The one-polygon model: t = 0 reproduces the file pair, t = 1 swaps it.
    {
        gm::texanim::PolyAnim key{0, 0, 15595, 15617};
        std::string error = "stale";
        const auto start = gm::texanim::anim_step(key, 0.0, &error);
        CHECK(error.empty());
        CHECK_EQ(start.first, 15595);
        CHECK_EQ(start.second, 15617);
        const auto middle = gm::texanim::anim_step(key, 0.5, &error);
        CHECK_EQ(middle.first, 15606);
        CHECK_EQ(middle.second, 15606);
        const auto end = gm::texanim::anim_step(key, 1.0, &error);
        CHECK_EQ(end.first, 15617);
        CHECK_EQ(end.second, 15595);
    }

    // Anim refusals.
    {
        gm::texanim::PolyAnim a{0, 0, 10, 20};
        gm::texanim::PolyAnim b{0, 0, 30, 40};
        std::string error;
        const double nan = std::nan("");
        const double inf = std::numeric_limits<double>::infinity();

        CHECK_EQ(gm::texanim::anim_step(a, b, nan, &error).first, 0);
        CHECK(error.find("t is not finite") != std::string::npos);
        CHECK_EQ(gm::texanim::anim_step(a, b, inf, &error).first, 0);
        CHECK(error.find("t is not finite") != std::string::npos);
        CHECK_EQ(gm::texanim::anim_step(a, b, -0.25, &error).first, 0);
        CHECK(error.find("in [0, 1]") != std::string::npos);
        CHECK_EQ(gm::texanim::anim_step(a, b, 1.25, &error).first, 0);
        CHECK(error.find("in [0, 1]") != std::string::npos);

        gm::texanim::PolyAnim broken{0, 0, 65536, 0};
        CHECK_EQ(gm::texanim::anim_step(broken, b, 0.5, &error).first, 0);
        CHECK(error.find("must fit the u16 slot") != std::string::npos);
        CHECK_EQ(gm::texanim::anim_step(a, broken, 0.5, &error).first, 0);
        CHECK(error.find("must fit the u16 slot") != std::string::npos);
        CHECK_EQ(gm::texanim::anim_step(broken, 0.5, &error).first, 0);
        CHECK(error.find("must fit the u16 slot") != std::string::npos);

        // Null error pointers must not crash.
        CHECK_EQ(gm::texanim::anim_step(a, b, 0.5, nullptr).first, 20);
        CHECK_EQ(gm::texanim::anim_step(a, 0.5, nullptr).first, 15);
    }
}

// ---------------------------------------------------------------------------
// Real data.

struct FoundScroll {
    std::size_t object = 0;
    std::size_t poly = 0;
    gm::SbwPoly raw;
};

struct LevelStat {
    std::string name;
    std::size_t polys = 0;
    std::size_t scroll = 0;
    std::size_t anim = 0;
    std::size_t both = 0;
    std::size_t only_u = 0;
    std::size_t only_v = 0;
    int max_abs_u = 0;
    int max_abs_v = 0;
    std::vector<FoundScroll> scrolled;

    bool operator==(const LevelStat& other) const {
        return name == other.name && polys == other.polys && scroll == other.scroll &&
               anim == other.anim && both == other.both && only_u == other.only_u &&
               only_v == other.only_v && max_abs_u == other.max_abs_u &&
               max_abs_v == other.max_abs_v && scrolled.size() == other.scrolled.size();
    }
};

std::optional<gm::SbwLevel> load_level(const fs::path& path) {
    std::string error;
    const auto bytes = read_file(path);
    const auto level = gm::parse_sbw(bytes, error);
    if (!level) {
        std::fprintf(stderr, "FAIL %s: %s\n", path.string().c_str(), error.c_str());
        ++g_failures;
        return std::nullopt;
    }
    return level;
}

LevelStat scan_level(const fs::path& path) {
    LevelStat stat;
    stat.name = path.stem().string();
    const auto level = load_level(path);
    if (!level) {
        return stat;
    }
    for (std::size_t oi = 0; oi < level->geometry.size(); ++oi) {
        const gm::SbwGeometry& geometry = level->geometry[oi];
        for (std::size_t pi = 0; pi < geometry.polys.size(); ++pi) {
            const gm::SbwPoly& poly = geometry.polys[pi];
            const gm::texanim::PolyAnim anim = gm::texanim::from_poly(poly);
            ++stat.polys;
            if (gm::texanim::has_scroll(anim)) {
                ++stat.scroll;
                if (anim.scroll_u != 0 && anim.scroll_v != 0) {
                    ++stat.both;
                } else if (anim.scroll_u != 0) {
                    ++stat.only_u;
                } else {
                    ++stat.only_v;
                }
                stat.max_abs_u = std::max(stat.max_abs_u, std::abs(anim.scroll_u));
                stat.max_abs_v = std::max(stat.max_abs_v, std::abs(anim.scroll_v));
                stat.scrolled.push_back(FoundScroll{oi, pi, poly});
            }
            if (gm::texanim::has_anim(anim)) {
                ++stat.anim;
            }
        }
    }
    return stat;
}

void real_level_cases(const fs::path& game) {
    const auto level0 = scan_level(game / "level0.sbw");
    const auto dm1 = scan_level(game / "dm1.sbw");

    // level0: exactly one scrolled polygon, geometry[186] local 6, weights
    // 2000/400, unit orthogonal axes (0,0,-1) and (0,-1,0).
    CHECK_EQ(level0.name, std::string("level0"));
    CHECK_EQ(level0.polys, std::size_t{1449});
    CHECK_EQ(level0.scroll, std::size_t{1});
    CHECK_EQ(level0.anim, std::size_t{1449});
    CHECK_EQ(level0.max_abs_u, 2000);
    CHECK_EQ(level0.max_abs_v, 400);
    if (level0.scrolled.size() == 1) {
        const FoundScroll& found = level0.scrolled.front();
        CHECK_EQ(found.object, std::size_t{186});
        CHECK_EQ(found.poly, std::size_t{6});
        const gm::texanim::PolyAnim anim = gm::texanim::from_poly(found.raw);
        CHECK_EQ(anim.scroll_u, 2000);
        CHECK_EQ(anim.scroll_v, 400);
        CHECK_EQ(anim.anim0, 15531);
        CHECK_EQ(anim.anim1, 15531);

        const gm::Vec3d u{found.raw.u_axis.x, found.raw.u_axis.y, found.raw.u_axis.z};
        const gm::Vec3d v{found.raw.v_axis.x, found.raw.v_axis.y, found.raw.v_axis.z};
        CHECK(close(u.z, -1.0));
        CHECK(close(v.y, -1.0));

        std::string error = "stale";
        const gm::Vec3d at0 = gm::texanim::scroll_offset(anim, u, v, 0.0, &error);
        CHECK(error.empty());
        CHECK(close(at0.x, 0.0) && close(at0.y, 0.0) && close(at0.z, 0.0));

        const gm::Vec3d at1 = gm::texanim::scroll_offset(anim, u, v, 1.0, &error);
        CHECK(close(at1.x, 0.0));
        CHECK(close(at1.y, -1.5625));
        CHECK(close(at1.z, -7.8125));

        const gm::Vec3d at91 = gm::texanim::scroll_offset(anim, u, v, 91.0, &error);
        CHECK(close(at91.x, 0.0));
        CHECK(close(at91.y, -142.1875));
        CHECK(close(at91.z, -710.9375));

        // Direction: both projections are positive and equal to t*weight/256,
        // i.e. the offset runs along +u_axis and +v_axis together.
        const double proj_u = at1.x * u.x + at1.y * u.y + at1.z * u.z;
        const double proj_v = at1.x * v.x + at1.y * v.y + at1.z * v.z;
        CHECK(close(proj_u, 2000.0 / 256.0));
        CHECK(close(proj_v, 400.0 / 256.0));
        CHECK(proj_u > 0.0 && proj_v > 0.0);

        // The one-polygon model is constant for this pair (anim0 == anim1).
        const auto step = gm::texanim::anim_step(anim, 0.5, &error);
        CHECK_EQ(step.first, 15531);
        CHECK_EQ(step.second, 15531);

        std::printf("level0 scroll: object=%zu poly=%zu scroll=%d/%d uv_axes=(%g,%g,%g)/(%g,%g,%g)\n",
                    found.object, found.poly, anim.scroll_u, anim.scroll_v,
                    static_cast<double>(u.x), static_cast<double>(u.y),
                    static_cast<double>(u.z), static_cast<double>(v.x),
                    static_cast<double>(v.y), static_cast<double>(v.z));
        std::printf("level0 offset: t=1 (%.6f, %.6f, %.6f) t=91 (%.6f, %.6f, %.6f) "
                    "|o(1)|=%.6f proj=(%.6f, %.6f)\n",
                    at1.x, at1.y, at1.z, at91.x, at91.y, at91.z,
                    std::sqrt(at1.x * at1.x + at1.y * at1.y + at1.z * at1.z), proj_u, proj_v);
    }

    // dm1: 48 scrolled polygons, all scroll_u = 300 and scroll_v = 0; the
    // first two are the real pair used for the two-key lerp below.
    CHECK_EQ(dm1.name, std::string("dm1"));
    CHECK_EQ(dm1.polys, std::size_t{5786});
    CHECK_EQ(dm1.scroll, std::size_t{48});
    CHECK_EQ(dm1.anim, std::size_t{5786});
    CHECK_EQ(dm1.max_abs_u, 300);
    CHECK_EQ(dm1.max_abs_v, 0);
    CHECK_EQ(dm1.only_u, std::size_t{48});
    CHECK_EQ(dm1.both, std::size_t{0});
    for (const FoundScroll& found : dm1.scrolled) {
        const gm::texanim::PolyAnim anim = gm::texanim::from_poly(found.raw);
        CHECK_EQ(anim.scroll_u, 300);
        CHECK_EQ(anim.scroll_v, 0);
    }

    if (dm1.scrolled.size() >= 2) {
        const FoundScroll& first = dm1.scrolled[0];
        const gm::texanim::PolyAnim a = gm::texanim::from_poly(first.raw);
        CHECK_EQ(a.anim0, 15595);
        CHECK_EQ(a.anim1, 15617);

        const gm::Vec3d u{first.raw.u_axis.x, first.raw.u_axis.y, first.raw.u_axis.z};
        const gm::Vec3d v{first.raw.v_axis.x, first.raw.v_axis.y, first.raw.v_axis.z};
        std::string error;
        const gm::Vec3d at1 = gm::texanim::scroll_offset(a, u, v, 1.0, &error);
        CHECK(error.empty());
        // u is NOT unit here: |u| = 0.4008249..., so the documented formula
        // gives 300*u/256 rather than the engine's normalised 300/256.
        CHECK(close(at1.x, 0.18827130552381277));
        CHECK(close(at1.y, 0.0));
        CHECK(close(at1.z, -0.43033441761508584));
        const double norm_u = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
        CHECK(close(norm_u, 0.40082493296498817));
        const double proj_u = at1.x * u.x + at1.y * u.y + at1.z * u.z;
        CHECK(close(proj_u, (300.0 / 256.0) * norm_u * norm_u));
        CHECK(proj_u > 0.0);

        const gm::texanim::PolyAnim b = gm::texanim::from_poly(dm1.scrolled[1].raw);
        CHECK_EQ(b.anim0, 15528);
        CHECK_EQ(b.anim1, 15540);
        const auto t0 = gm::texanim::anim_step(a, b, 0.0, &error);
        const auto half = gm::texanim::anim_step(a, b, 0.5, &error);
        const auto t1 = gm::texanim::anim_step(a, b, 1.0, &error);
        CHECK_EQ(t0.first, 15595);
        CHECK_EQ(t0.second, 15617);
        CHECK_EQ(half.first, 15561);  // 15595 + (15528-15595)*0.5 = 15561.5 -> 15561
        CHECK_EQ(half.second, 15578); // 15617 + (15540-15617)*0.5 = 15578.5 -> 15578
        CHECK_EQ(t1.first, 15528);
        CHECK_EQ(t1.second, 15540);

        const auto model = gm::texanim::anim_step(a, 0.5, &error);
        CHECK_EQ(model.first, 15606);
        CHECK_EQ(model.second, 15606);

        std::printf("dm1 scroll: 48 polygons, first object=%zu poly=%zu scroll=%d/%d "
                    "|u|=%.9f offset(1)=(%.9f, %.9f, %.9f)\n",
                    first.object, first.poly, a.scroll_u, a.scroll_v, norm_u, at1.x, at1.y,
                    at1.z);
        std::printf("dm1 anim: first pair (anim0,anim1)=(%d,%d) second=(%d,%d) "
                    "two-key t=0.5 -> (%d,%d)\n",
                    a.anim0, a.anim1, b.anim0, b.anim1, half.first, half.second);
    }
}

/// One pass over the 36 shipped `.sbw` files.
std::vector<LevelStat> corpus_pass(const fs::path& game) {
    std::vector<fs::path> worlds;
    for (const auto& entry : fs::directory_iterator(game)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sbw") {
            worlds.push_back(entry.path());
        }
    }
    std::sort(worlds.begin(), worlds.end());
    std::vector<LevelStat> stats;
    stats.reserve(worlds.size());
    for (const fs::path& path : worlds) {
        stats.push_back(scan_level(path));
    }
    return stats;
}

struct Totals {
    std::size_t levels = 0;
    std::size_t polys = 0;
    std::size_t scroll = 0;
    std::size_t anim = 0;
    std::size_t both = 0;
    std::size_t only_u = 0;
    std::size_t only_v = 0;
    int max_abs_u = 0;
    int max_abs_v = 0;
    std::string max_u_level;
    std::string max_v_level;
};

Totals sum_stats(const std::vector<LevelStat>& stats) {
    Totals totals;
    totals.levels = stats.size();
    for (const LevelStat& stat : stats) {
        totals.polys += stat.polys;
        totals.scroll += stat.scroll;
        totals.anim += stat.anim;
        totals.both += stat.both;
        totals.only_u += stat.only_u;
        totals.only_v += stat.only_v;
        if (stat.max_abs_u > totals.max_abs_u) {
            totals.max_abs_u = stat.max_abs_u;
            totals.max_u_level = stat.name;
        }
        if (stat.max_abs_v > totals.max_abs_v) {
            totals.max_abs_v = stat.max_abs_v;
            totals.max_v_level = stat.name;
        }
    }
    return totals;
}

void corpus_case(const fs::path& game) {
    const std::vector<LevelStat> stats = corpus_pass(game);
    const Totals totals = sum_stats(stats);

    std::printf("corpus: %zu levels, %zu polygons, has_scroll=%zu (%.3f %%), "
                "has_anim=%zu (%.3f %%)\n",
                totals.levels, totals.polys, totals.scroll,
                100.0 * static_cast<double>(totals.scroll) / static_cast<double>(totals.polys),
                totals.anim,
                100.0 * static_cast<double>(totals.anim) / static_cast<double>(totals.polys));
    std::printf("corpus: both=%zu only_u=%zu only_v=%zu max|scroll_u|=%d (%s) "
                "max|scroll_v|=%d (%s)\n",
                totals.both, totals.only_u, totals.only_v, totals.max_abs_u,
                totals.max_u_level.c_str(), totals.max_abs_v, totals.max_v_level.c_str());

    // docs/TEXTURE_ANIM.md §4: measured on all 36 worlds.
    CHECK_EQ(totals.levels, std::size_t{36});
    CHECK_EQ(totals.polys, std::size_t{235069});
    CHECK_EQ(totals.scroll, std::size_t{2917});
    CHECK_EQ(totals.anim, std::size_t{235069});
    CHECK_EQ(totals.both, std::size_t{93});
    CHECK_EQ(totals.only_u, std::size_t{2513});
    CHECK_EQ(totals.only_v, std::size_t{311});
    CHECK_EQ(totals.max_abs_u, 15224);
    CHECK_EQ(totals.max_abs_v, 10000);

    // Per-level rows for the extremes and the two named levels.
    for (const LevelStat& stat : stats) {
        if (stat.name == "level0" || stat.name == "dm1" || stat.name == "dm2" ||
            stat.name == "level17" || stat.name == "level0b") {
            std::printf("  %-8s polys=%-6zu scroll=%-4zu anim=%-6zu max|u|=%-6d max|v|=%d\n",
                        stat.name.c_str(), stat.polys, stat.scroll, stat.anim, stat.max_abs_u,
                        stat.max_abs_v);
        }
    }

    // Determinism: a second pass over the same files must give the same rows.
    const std::vector<LevelStat> again = corpus_pass(game);
    CHECK_EQ(again.size(), stats.size());
    bool identical = again.size() == stats.size();
    for (std::size_t i = 0; identical && i < stats.size(); ++i) {
        identical = again[i] == stats[i];
    }
    CHECK(identical);
    std::printf("corpus: second pass identical=%s\n", identical ? "yes" : "no");
}

} // namespace

int main() {
    synthetic_cases();

    const auto game = find_game_dir();
    if (!game) {
        std::printf("texture_anim: game directory not found, synthetic cases only\n");
    } else {
        std::printf("texture_anim: game=%s\n", game->string().c_str());
        real_level_cases(*game);
        corpus_case(*game);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "texture_anim: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("texture_anim: all checks passed\n");
    return 0;
}
