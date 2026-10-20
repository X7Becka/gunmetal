// Runtime .soo player (`anim/soo_player.h`, docs/SOO_PLAYER.md).
//
// Synthetic fixtures pin the engine timeline exactly (91 Hz, mode 0 hold,
// mode 1 wrap, last loop frame interpolating back to frame 0), the vertex and
// normal morph, the SOB name cache, the negative paths and the strong load
// guarantee.  The shipped goldens are FNV-1a64 over the 32-bit little-endian
// float components of the interpolated vertices and the renormalised per-face
// normals, generated with gm/soo.py + gm/sob.py at four moments of
// Cultist.soo, Cloud.soo and kingbody.soo.  Each file is skipped cleanly when
// the game is not installed.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "anim/soo_player.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (eps))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.17g, expected %.17g\n", __FILE__,       \
                         __LINE__, #a, va, vb);                                              \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

constexpr std::uint64_t kFnvBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t fnv_float(std::uint64_t hash, float value) {
    std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        hash = (hash ^ (bits & 0xFFu)) * kFnvPrime;
        bits >>= 8;
    }
    return hash;
}

std::uint64_t hash_vertices(const std::vector<gm::Vec3f>& vertices) {
    std::uint64_t hash = kFnvBasis;
    for (const gm::Vec3f& v : vertices) {
        hash = fnv_float(hash, v.x);
        hash = fnv_float(hash, v.y);
        hash = fnv_float(hash, v.z);
    }
    return hash;
}

std::uint64_t hash_normals(const std::vector<gm::Vec3f>& normals) {
    std::uint64_t hash = kFnvBasis;
    for (const gm::Vec3f& n : normals) {
        hash = fnv_float(hash, n.x);
        hash = fnv_float(hash, n.y);
        hash = fnv_float(hash, n.z);
    }
    return hash;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool same_pose(const gm::SooPlayerPose& a, const gm::SooPlayerPose& b) {
    if (a.frame_a != b.frame_a || a.frame_b != b.frame_b) {
        return false;
    }
    if (a.alpha != b.alpha || a.tick != b.tick ||
        a.mesh.vertices.size() != b.mesh.vertices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.mesh.vertices.size(); ++i) {
        if (a.mesh.vertices[i].x != b.mesh.vertices[i].x ||
            a.mesh.vertices[i].y != b.mesh.vertices[i].y ||
            a.mesh.vertices[i].z != b.mesh.vertices[i].z) {
            return false;
        }
    }
    return true;
}

// A minimal one-face .sob: 3 vertices, one triangle, plane normal *normal*.
std::string sob_text(const char* vertices, const char* normal) {
    return std::string("3 1\n") + vertices + "\n" +
           "tex none none none none none none none 0 3 0 1 2 "
           "0 0 0 1 0 0 0 1 0 " + normal + " 0 0 0\n";
}

void test_synthetic() {
    const fs::path dir = fs::temp_directory_path() / "gm_soo_player_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "SOBS", ec);
    fs::create_directories(dir / "Sobanim", ec);

    // Frame A lies in z=0 with normal +x; frame B is lifted 10 units with
    // normal +y, so a half-way morph must normalise (0.5, 0.5, 0).
    write_text(dir / "SOBS" / "A.sob",
               sob_text("0 0 0 10 0 0 0 10 0", "1 0 0"));
    write_text(dir / "SOBS" / "B.sob",
               sob_text("0 0 10 10 0 10 0 10 10", "0 1 0"));
    // Different vertex count than A/B.
    write_text(dir / "SOBS" / "C.sob", "4 0\n0 0 0 1 0 0 0 1 0 0 0 1\n");

    write_text(dir / "Sobanim" / "hold.soo", "0\n2\nA 10\nB 10\n");
    write_text(dir / "Sobanim" / "loop.soo", "1\n2\nA 10\nB 10\n");
    write_text(dir / "Sobanim" / "empty.soo", "");
    write_text(dir / "Sobanim" / "badname.soo", "0 1\nNope 10\n");
    write_text(dir / "Sobanim" / "mismatch.soo", "0 2\nA 10\nC 10\n");
    write_text(dir / "Sobanim" / "truncated.soo", "0 3\nA 10\nB 10\n");
    write_text(dir / "Sobanim" / "mode2.soo", "2 1\nA 10\n");
    write_text(dir / "Sobanim" / "broken.soo", "0 1\nBroken 10\n");
    write_text(dir / "SOBS" / "Broken.sob", "not a model\n");

    gm::SooPlayer player(dir);
    std::string error;

    CHECK(!player.loaded());
    CHECK(!player.pose_at(0, error).has_value());
    CHECK(!error.empty());

    CHECK(player.load("hold.soo", error));
    CHECK(error.empty());
    CHECK(player.loaded());
    CHECK(player.loop() == false);
    CHECK(player.animation().mode == 0);
    CHECK(player.animation().declared == 2);
    CHECK(player.duration_ticks() == 20);
    CHECK_NEAR(player.duration_ms(), 20.0 * 1000.0 / 91.0, 1e-12);

    gm::SooPlayerStats stats = player.stats();
    CHECK(stats.soo_loads == 1);
    CHECK(stats.frame_loads == 2);
    CHECK(stats.cache_misses == 2);
    CHECK(stats.cache_hits == 0);
    CHECK(stats.cached_models == 2);

    // t = 0: frame A exactly.
    auto pose = player.pose_at(0, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 0 && pose->frame_b == 1);
        CHECK(pose->name_a == "A" && pose->name_b == "B");
        CHECK(pose->alpha == 0.0);
        CHECK(pose->tick == 0 && pose->elapsed == 0 && pose->total_ticks == 20);
        CHECK(pose->mesh.vertices.size() == 3);
        CHECK(pose->mesh.faces.size() == 1);
        CHECK(pose->normals.size() == 1);
        CHECK(pose->mesh.vertices[0].x == 0.0f && pose->mesh.vertices[0].y == 0.0f &&
              pose->mesh.vertices[0].z == 0.0f);
        CHECK(pose->normals[0].x == 1.0f && pose->normals[0].y == 0.0f &&
              pose->normals[0].z == 0.0f);
    }

    // 54 ms -> 4 ticks of a 10-tick frame: alpha 0.4, frames A -> B.
    pose = player.pose_at(54, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 0 && pose->frame_b == 1);
        CHECK(pose->tick == 4 && pose->elapsed == 4);
        CHECK_NEAR(pose->alpha, 0.4, 1e-15);
        CHECK_NEAR(pose->mesh.vertices[0].x, 0.0, 1e-6);
        CHECK_NEAR(pose->mesh.vertices[0].y, 0.0, 1e-6);
        CHECK_NEAR(pose->mesh.vertices[0].z, 4.0, 1e-6);
        CHECK_NEAR(pose->mesh.vertices[1].x, 10.0, 1e-6);
        CHECK_NEAR(pose->mesh.vertices[1].z, 4.0, 1e-6);
        const gm::Vec3f& n = pose->normals[0];
        const double length = std::sqrt(static_cast<double>(n.x) * n.x +
                                        static_cast<double>(n.y) * n.y +
                                        static_cast<double>(n.z) * n.z);
        CHECK_NEAR(length, 1.0, 1e-6);
        CHECK_NEAR(n.x, 0.8320502943378437, 1e-6);
        CHECK_NEAR(n.y, 0.5547001962252291, 1e-6);
        CHECK_NEAR(n.z, 0.0, 1e-6);
    }

    // 110 ms -> tick 10: frame B held (next == itself), alpha 0.
    pose = player.pose_at(110, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 1 && pose->frame_b == 1);
        CHECK(pose->tick == 10 && pose->elapsed == 0);
        CHECK(pose->alpha == 0.0);
        CHECK(pose->mesh.vertices[0].z == 10.0f);
        CHECK(pose->normals[0].x == 0.0f && pose->normals[0].y == 1.0f);
    }

    // 220 ms is past the end: mode 0 clamps to total - 1 = tick 19.
    pose = player.pose_at(220, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 1 && pose->frame_b == 1);
        CHECK(pose->tick == 19 && pose->elapsed == 9);
        CHECK_NEAR(pose->alpha, 0.9, 1e-15);
        CHECK(pose->mesh.vertices[0].z == 10.0f); // holding, not extrapolating
    }

    // Negative time is clamped to 0, like the engine at 0x433e84.
    const auto at_zero = player.pose_at(0, error);
    const auto at_negative = player.pose_at(-1000, error);
    CHECK(at_zero.has_value() && at_negative.has_value());
    if (at_zero && at_negative) {
        CHECK(same_pose(*at_zero, *at_negative));
    }
    // A huge clock is safe (64-bit conversion) and still holds the last frame.
    pose = player.pose_at(std::numeric_limits<int>::max(), error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 1 && pose->frame_b == 1);
        CHECK(pose->tick == 19);
    }

    // set_loop overrides mode 0 -> 1: tick 20 wraps to frame A.
    player.set_loop(true);
    CHECK(player.loop());
    pose = player.pose_at(220, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 0 && pose->frame_b == 1);
        CHECK(pose->tick == 0);
        CHECK(pose->mesh.vertices[0].z == 0.0f);
    }
    player.set_loop(false);
    CHECK(!player.loop());

    // Reloading loop.soo reuses both cached models and must stay exact.
    CHECK(player.load("loop.soo", error));
    stats = player.stats();
    CHECK(stats.soo_loads == 2);
    CHECK(stats.frame_loads == 4);
    CHECK(stats.cache_misses == 2);
    CHECK(stats.cache_hits == 2);
    CHECK(stats.cached_models == 2);
    CHECK(player.loop());
    CHECK(player.animation().mode == 1);
    CHECK(player.duration_ticks() == 20);

    // Mode 1 at t >= total wraps: tick 20 % 20 == 0.
    pose = player.pose_at(220, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 0 && pose->frame_b == 1);
        CHECK(pose->tick == 0 && pose->alpha == 0.0);
        CHECK(pose->mesh.vertices[0].z == 0.0f);
    }
    // Tick 14: last frame interpolates back towards frame 0 (alpha 0.4).
    pose = player.pose_at(164, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 1 && pose->frame_b == 0);
        CHECK(pose->tick == 14 && pose->elapsed == 4);
        CHECK_NEAR(pose->alpha, 0.4, 1e-15);
        CHECK_NEAR(pose->mesh.vertices[0].z, 6.0, 1e-6);
    }
    // Tick 9: frame 0 towards frame 1, alpha 0.9.
    pose = player.pose_at(109, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 0 && pose->frame_b == 1);
        CHECK(pose->tick == 9);
        CHECK_NEAR(pose->mesh.vertices[0].z, 9.0, 1e-6);
    }
    // Override loop off while loaded: mode 0 now holds the last frame.
    player.set_loop(false);
    pose = player.pose_at(220, error);
    CHECK(pose.has_value());
    if (pose) {
        CHECK(pose->frame_a == 1 && pose->frame_b == 1);
        CHECK(pose->tick == 19);
    }
    player.set_loop(true);

    // Negative paths: every failed load reports a clean error.
    struct Negative {
        const char* path;
        const char* needle;
    };
    const Negative negatives[] = {
        {"does-not-exist.soo", "not found"},
        {"empty.soo", "empty"},
        {"badname.soo", "model not found"},
        {"broken.soo", ""},
        {"mismatch.soo", "topology"},
        {"truncated.soo", "declared"},
        {"mode2.soo", "mode"},
    };
    for (const auto& negative : negatives) {
        error.clear();
        CHECK(!player.load(negative.path, error));
        CHECK(!error.empty());
        if (negative.needle[0] != '\0') {
            CHECK(contains(error, negative.needle));
        }
    }
    CHECK(contains(error, "mode")); // the last one was mode2
    // A failed load leaves the previous animation intact and playable.
    CHECK(player.loaded());
    CHECK(player.loop());
    CHECK(player.pose_at(0, error).has_value());

    // Absolute paths and extension-less names are accepted too.
    CHECK(player.load(dir / "Sobanim" / "hold.soo", error));
    CHECK(player.animation().mode == 0);
    CHECK(player.load("hold", error));
    const auto reloaded = player.pose_at(54, error);
    CHECK(reloaded.has_value());
    if (reloaded) {
        CHECK(reloaded->frame_a == 0 && reloaded->frame_b == 1);
    }

    fs::remove_all(dir, ec);
}

struct GoldenMoment {
    int ms;
    int frame_a;
    int frame_b;
    double alpha;
    std::uint64_t vertices;
    std::uint64_t normals;
};

struct GoldenFile {
    const char* name;
    int mode;
    int frames;
    int total_ticks;
    std::size_t vertices;
    std::size_t faces;
    GoldenMoment moments[4];
};

// Generated by /tmp/opencode/gen_soo_golden.py: gm/soo.py frame_at(ms) and
// gm/sob.py + the same float32 morph/normalisation, FNV-1a64 over the
// little-endian float components.
const GoldenFile kGolden[] = {
    {"Cultist.soo", 1, 4, 400, 58, 46,
     {{0, 0, 1, 0.0, 0xbd880b4a1c64baacull, 0x70ab2adcede28445ull},
      {1098, 1, 2, 0.32666666666666666, 0x292b2c7fed76dc48ull, 0x70ab2adcede28445ull},
      {2197, 1, 2, 0.99333333333333329, 0xf7e2960a337c60f4ull, 0x70ab2adcede28445ull},
      {3956, 3, 0, 0.72666666666666668, 0xb3234830da277fe4ull, 0x70ab2adcede28445ull}}},
    {"Cloud.soo", 0, 9, 640, 18, 14,
     {{0, 0, 1, 0.0, 0x8f29afc49df2f8e5ull, 0x35da762063936645ull},
      {1758, 2, 3, 0.36249999999999999, 0xfc35fa2d62a49692ull, 0x35da762063936645ull},
      {3516, 4, 5, 0.36249999999999999, 0xfc35fa2d62a49692ull, 0x35da762063936645ull},
      {6329, 7, 8, 0.5625, 0xc49e7ec895945593ull, 0x35da762063936645ull}}},
    {"kingbody.soo", 1, 3, 450, 129, 95,
     {{0, 0, 1, 0.0, 0x7d7771bf6e7dca4aull, 0xbc630bb52c3aa644ull},
      {1230, 0, 1, 0.73999999999999999, 0x33a794e6b4a4e428ull, 0xbc630bb52c3aa644ull},
      {2472, 1, 2, 0.49333333333333335, 0x16238e863516476cull, 0xbc630bb52c3aa644ull},
      {4450, 2, 0, 0.69333333333333336, 0x7d7771bf6e7dca4aull, 0xbc630bb52c3aa644ull}}},
};

void test_shipped() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    std::error_code ec;
    if (!fs::is_directory(game / "Sobanim", ec) || !fs::is_directory(game / "SOBS", ec)) {
        std::printf("soo_player: game not installed, shipped goldens skipped\n");
        return;
    }
    gm::SooPlayer player(game);
    std::string error;
    for (const GoldenFile& file : kGolden) {
        const std::string label = std::string(file.name) + ": ";
        error.clear();
        CHECK(player.load(file.name, error));
        if (!player.loaded()) {
            std::fprintf(stderr, "soo_player: %s%s\n", label.c_str(), error.c_str());
            continue;
        }
        CHECK(player.animation().mode == file.mode);
        CHECK(player.animation().declared == file.frames);
        CHECK(player.duration_ticks() == file.total_ticks);
        for (const GoldenMoment& moment : file.moments) {
            const auto pose = player.pose_at(moment.ms, error);
            CHECK(pose.has_value());
            if (!pose) {
                std::fprintf(stderr, "soo_player: %s%d ms: %s\n", label.c_str(),
                             moment.ms, error.c_str());
                continue;
            }
            if (pose->frame_a != moment.frame_a || pose->frame_b != moment.frame_b ||
                pose->mesh.vertices.size() != file.vertices ||
                pose->mesh.faces.size() != file.faces ||
                pose->normals.size() != file.faces) {
                std::fprintf(stderr,
                             "soo_player: %s%d ms: frames %d/%d, %zu/%zu v, %zu/%zu f\n",
                             label.c_str(), moment.ms, pose->frame_a, pose->frame_b,
                             pose->mesh.vertices.size(), file.vertices,
                             pose->mesh.faces.size(), file.faces);
                ++g_failures;
                continue;
            }
            CHECK_NEAR(pose->alpha, moment.alpha, 1e-15);
            const std::uint64_t vertices = hash_vertices(pose->mesh.vertices);
            const std::uint64_t normals = hash_normals(pose->normals);
            if (vertices != moment.vertices) {
                std::fprintf(stderr, "soo_player: %s%d ms: vertex hash %016llx != %016llx\n",
                             label.c_str(), moment.ms,
                             static_cast<unsigned long long>(vertices),
                             static_cast<unsigned long long>(moment.vertices));
                ++g_failures;
            }
            if (normals != moment.normals) {
                std::fprintf(stderr, "soo_player: %s%d ms: normal hash %016llx != %016llx\n",
                             label.c_str(), moment.ms,
                             static_cast<unsigned long long>(normals),
                             static_cast<unsigned long long>(moment.normals));
                ++g_failures;
            }
            CHECK(!pose->mesh.faces[0].texture().empty());
        }
    }

    // Repeated load of the same animation: same pose, models parsed once.
    gm::SooPlayer repeat(game);
    CHECK(repeat.load("Cultist.soo", error));
    const auto first = repeat.pose_at(2197, error);
    const auto first_stats = repeat.stats();
    CHECK(repeat.load("Cultist.soo", error));
    const auto second = repeat.pose_at(2197, error);
    const auto second_stats = repeat.stats();
    CHECK(first.has_value() && second.has_value());
    if (first && second) {
        CHECK(same_pose(*first, *second));
        CHECK(hash_vertices(first->mesh.vertices) == hash_vertices(second->mesh.vertices));
    }
    CHECK(second_stats.cached_models == first_stats.cached_models);
    CHECK(second_stats.cached_models == 2); // cult1a, cult1b
    CHECK(second_stats.cache_misses == 2);
    CHECK(second_stats.cache_hits == 6); // 2 in the first load, 4 in the second
    CHECK(second_stats.soo_loads == 2);
    // Case-insensitive bare name resolution (Windows install).
    CHECK(repeat.load("cultist", error));
    CHECK(repeat.loaded());
}

} // namespace

int main() {
    test_synthetic();
    test_shipped();
    if (g_failures != 0) {
        std::fprintf(stderr, "soo_player: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("soo_player: all checks passed\n");
    return 0;
}
