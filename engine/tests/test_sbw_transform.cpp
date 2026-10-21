// The `.sbw` object transform: 12 little-endian `f64` = `origin[3]` +
// `basis[3][3]`, `world = origin + basis * local` (docs/WORLD_FIELDS.md §3,
// docs/WORLD_MATERIALS.md §6).
//
// The old model read every component as a `(u32 pad, f32 value)` pair, i.e.
// the *high dword* of each double.  For the double `1.0` that dword is
// `0x3FF00000`, which as a float is `1.875` — hence the imaginary
// `diag(1.875)` basis and the garbage origins (the high dword of a double is a
// crude function of its exponent, never its value).
//
// This test pins the codec down on the shipped data and on a synthetic object
// with a non-identity (rotated + scaled) basis:
//
//   * all 338 304 words of the 28 192 objects in the 36 shipped `.sbw` files
//     have a zero low dword (what an `f64` widened from `f32` looks like);
//   * every basis row is exactly the identity (unit, orthogonal);
//   * `world = origin + basis * local` matches the independent Python
//     implementation (`gm/sbw.py::Object.world_vertices`, values pinned below)
//     to 1e-12, including a rotated basis where the two readings must differ.
//
// The full 36-level sweep runs in the quick suite too: it parses ~950 MB of
// `.sbw` once (~1 s) and is the cheapest guard against a regression to the
// f32 reading.
#include "render/collide.h"
#include "render/render.h"
#include "res/sbw.h"

#include "core/path.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
            std::fprintf(stderr, "FAIL %s:%d: %s=%.17g != %s=%.17g\n", __FILE__, __LINE__,   \
                         #a, va, #b, vb);                                                    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

/// The codec under test: 12 `f64`, exactly what the loader copies
/// (`0x42ae86..0x42aee9`) and what `0x437af8` consumes with `FLD double`.
struct Transform {
    double origin[3]{};
    double basis[3][3]{};

    static Transform decode(const std::array<std::uint8_t, 96>& header) {
        Transform out;
        double values[12];
        for (int i = 0; i < 12; ++i) {
            std::memcpy(&values[i], header.data() + 8 * static_cast<std::size_t>(i),
                        sizeof(double));
        }
        std::memcpy(out.origin, values, sizeof(out.origin));
        std::memcpy(out.basis, values + 3, sizeof(out.basis));
        return out;
    }

    /// The old, wrong reading: the high dword of every word as `f32`.
    static Transform decode_high_f32(const std::array<std::uint8_t, 96>& header) {
        Transform out;
        float values[12];
        for (int i = 0; i < 12; ++i) {
            std::memcpy(&values[i], header.data() + 8 * static_cast<std::size_t>(i) + 4,
                        sizeof(float));
        }
        for (int k = 0; k < 3; ++k) {
            out.origin[k] = static_cast<double>(values[k]);
            out.basis[0][k] = static_cast<double>(values[3 + k]);
            out.basis[1][k] = static_cast<double>(values[6 + k]);
            out.basis[2][k] = static_cast<double>(values[9 + k]);
        }
        return out;
    }

    gm::Vec3d apply(gm::Vec3d local) const {
        return {origin[0] + basis[0][0] * local.x + basis[1][0] * local.y +
                    basis[2][0] * local.z,
                origin[1] + basis[0][1] * local.x + basis[1][1] * local.y +
                    basis[2][1] * local.z,
                origin[2] + basis[0][2] * local.x + basis[1][2] * local.y +
                    basis[2][2] * local.z};
    }
};

void put_f64(std::array<std::uint8_t, 96>& header, int index, double value) {
    std::memcpy(header.data() + 8 * static_cast<std::size_t>(index), &value, sizeof(double));
}

// --- shipped data ----------------------------------------------------------

void test_shipped_headers() {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kGame, ec)) {
        if (entry.is_regular_file() &&
            gm::iequals_ascii(entry.path().extension().string(), ".sbw")) {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        std::printf("sbw_transform: game not installed, shipped checks skipped\n");
        return;
    }
    std::size_t objects = 0;
    std::size_t words = 0;
    std::size_t low_dword_nonzero = 0;
    std::size_t non_identity = 0;
    std::size_t non_unit = 0;
    std::size_t non_orthogonal = 0;
    std::size_t non_finite = 0;
    std::size_t old_scale_hits = 0;  // high dword of 1.0 reads as 1.875

    for (const fs::path& path : files) {
        const std::vector<std::uint8_t> bytes = read_file(path);
        CHECK(!bytes.empty());
        std::string error;
        const auto level = gm::parse_sbw(bytes, error);
        if (!level) {
            std::fprintf(stderr, "sbw_transform: parse_sbw(%s): %s\n",
                         path.filename().string().c_str(), error.c_str());
            ++g_failures;
            continue;
        }
        for (const gm::SbwSectorRef& sector : level->header.sectors) {
            for (const gm::SbwObjectRef& object : sector.objects) {
                ++objects;
                for (int i = 0; i < 12; ++i) {
                    ++words;
                    const std::size_t off = 8 * static_cast<std::size_t>(i);
                    if (object.header[off] != 0 || object.header[off + 1] != 0 ||
                        object.header[off + 2] != 0 || object.header[off + 3] != 0) {
                        ++low_dword_nonzero;
                    }
                }
                const Transform t = Transform::decode(object.header);
                for (int axis = 0; axis < 3; ++axis) {
                    if (!std::isfinite(t.origin[axis]) || !std::isfinite(t.basis[axis][0]) ||
                        !std::isfinite(t.basis[axis][1]) || !std::isfinite(t.basis[axis][2])) {
                        ++non_finite;
                    }
                }
                static constexpr double kIdentity[3][3] = {
                    {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
                bool identity = true;
                for (int row = 0; row < 3; ++row) {
                    for (int col = 0; col < 3; ++col) {
                        if (std::fabs(t.basis[row][col] - kIdentity[row][col]) > 1e-12) {
                            identity = false;
                        }
                    }
                }
                if (!identity) {
                    ++non_identity;
                }
                for (int row = 0; row < 3; ++row) {
                    const double* r = t.basis[row];
                    const double len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                    if (std::fabs(len - 1.0) > 1e-12) {
                        ++non_unit;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    for (int j = i + 1; j < 3; ++j) {
                        const double dot = t.basis[i][0] * t.basis[j][0] +
                                           t.basis[i][1] * t.basis[j][1] +
                                           t.basis[i][2] * t.basis[j][2];
                        if (std::fabs(dot) > 1e-12) {
                            ++non_orthogonal;
                        }
                    }
                }
                // The defect this test guards against: the f32 reading of an
                // identity row is 1.875, not 1.0.
                const Transform old = Transform::decode_high_f32(object.header);
                for (int row = 0; row < 3; ++row) {
                    if (std::fabs(old.basis[row][row] - 1.875) <= 1e-6) {
                        ++old_scale_hits;
                    }
                }
            }
        }
    }

    std::printf("sbw_transform: %zu files, %zu objects, %zu header words\n", files.size(),
                objects, words);
    CHECK(files.size() == 36);
    CHECK(objects == 28192);
    CHECK(words == 338304);
    CHECK(low_dword_nonzero == 0);
    CHECK(non_finite == 0);
    CHECK(non_identity == 0);
    CHECK(non_unit == 0);
    CHECK(non_orthogonal == 0);
    CHECK(old_scale_hits == 3 * objects); // every row would read as 1.875
}

// --- synthetic object with a non-identity basis ----------------------------

void test_synthetic_basis() {
    // Rotation by 30 degrees about Z, uniform scale 2, plus a translation.
    const double c = std::cos(30.0 * 3.14159265358979323846 / 180.0);
    const double s = std::sin(30.0 * 3.14159265358979323846 / 180.0);
    const double k = 2.0;
    const double bx[3] = {k * c, k * s, 0.0};
    const double by[3] = {-k * s, k * c, 0.0};
    const double bz[3] = {0.0, 0.0, k};
    const double origin[3] = {1234.5, -678.25, 42.125};

    std::array<std::uint8_t, 96> header{};
    header.fill(0);
    for (int axis = 0; axis < 3; ++axis) {
        put_f64(header, axis, origin[axis]);
        put_f64(header, 3 + axis, bx[axis]);
        put_f64(header, 6 + axis, by[axis]);
        put_f64(header, 9 + axis, bz[axis]);
    }

    const Transform t = Transform::decode(header);
    CHECK_NEAR(t.origin[0], 1234.5, 0.0);
    CHECK_NEAR(t.basis[0][0], bx[0], 0.0);
    CHECK_NEAR(t.basis[1][1], by[1], 0.0);
    CHECK_NEAR(t.basis[2][2], 2.0, 0.0);

    // Python: gm/sbw.py::Object.world_vertices on the same header
    //   world = origin + basis * local
    //   (10.5, -3.25, 7.75)  -> (1255.9365334794732, -673.3791651245989, 57.625)
    //   (0, 0, 0)            -> (1234.5, -678.25, 42.125)
    //   (-184, 184, 128)     -> (731.80265140732649, -543.55265140732649, 298.125)
    struct Golden {
        gm::Vec3d local;
        gm::Vec3d world;
    };
    const Golden goldens[] = {
        {{10.5, -3.25, 7.75}, {1255.9365334794732, -673.3791651245989, 57.625}},
        {{0.0, 0.0, 0.0}, {1234.5, -678.25, 42.125}},
        {{-184.0, 184.0, 128.0}, {731.80265140732649, -543.55265140732649, 298.125}},
    };
    for (const Golden& golden : goldens) {
        const gm::Vec3d world = t.apply(golden.local);
        CHECK_NEAR(world.x, golden.world.x, 1e-12);
        CHECK_NEAR(world.y, golden.world.y, 1e-12);
        CHECK_NEAR(world.z, golden.world.z, 1e-12);
    }

    // The same header through the old reading: a basis of 0.0078125-ish scale
    // and an origin that is a function of the exponent, not the value.
    const Transform old = Transform::decode_high_f32(header);
    CHECK(std::fabs(old.origin[0] - 1234.5) > 1.0);
    CHECK(std::fabs(old.basis[0][0] - bx[0]) > 1e-3);

    // CollisionWorld (render/collide.cpp) and build_level_meshes
    // (render/render.cpp) must agree with the codec above.
    gm::SbwLevel level;
    level.header.sectors.resize(1);
    gm::SbwObjectRef object;
    object.header = header;
    level.header.sectors[0].objects.push_back(object);
    level.header.object_count = 1;
    gm::SbwGeometry geometry;
    for (const Golden& golden : goldens) {
        geometry.vertices.push_back(gm::Vec3f{static_cast<float>(golden.local.x),
                                              static_cast<float>(golden.local.y),
                                              static_cast<float>(golden.local.z)});
    }
    geometry.n_verts = static_cast<std::uint16_t>(geometry.vertices.size());
    gm::SbwPoly poly;
    poly.indices = {0, 1, 2};
    geometry.polys.push_back(poly);
    geometry.n_polys = 1;
    level.geometry.push_back(geometry);

    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(level, &error));
    CHECK(error.empty());
    CHECK(world.triangle_count() == 1);
    if (world.triangle_count() == 1) {
        const gm::Triangle& triangle = world.triangles()[0];
        // The three vertices go through decode_transform + apply.
        CHECK_NEAR(triangle.a.x, goldens[0].world.x, 1e-9); // f32 local input
        CHECK_NEAR(triangle.b.y, goldens[1].world.y, 1e-9);
        CHECK_NEAR(triangle.c.z, goldens[2].world.z, 1e-9);
    }

    const std::vector<gm::RenderMesh> meshes = gm::build_level_meshes(level, nullptr);
    CHECK(meshes.size() == 1);
    if (meshes.size() == 1) {
        CHECK(meshes[0].vertices.size() == 3);
        if (meshes[0].vertices.size() == 3) {
            CHECK_NEAR(meshes[0].vertices[0].x, goldens[0].world.x, 1e-9);
            CHECK_NEAR(meshes[0].vertices[1].y, goldens[1].world.y, 1e-9);
            CHECK_NEAR(meshes[0].vertices[2].z, goldens[2].world.z, 1e-9);
        }
        // place_scale is the basis scale (2.0), so the world -> local map
        // keeps the polygon frame exact: local = (world - origin) / 2.
        CHECK_NEAR(meshes[0].place_scale, 2.0, 1e-12);
        CHECK_NEAR(meshes[0].place_origin.x, 1234.5, 0.0);
    }
}

} // namespace

int main() {
    test_synthetic_basis();
    test_shipped_headers();
    if (g_failures != 0) {
        std::fprintf(stderr, "sbw_transform: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sbw_transform: ok\n");
    return 0;
}
