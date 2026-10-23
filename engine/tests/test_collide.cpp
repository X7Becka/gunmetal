// Collision primitives over `.sbw` geometry (docs/COLLIDE.md).
//
// Synthetic part: an axis-aligned cube, a floor/ceiling/wall set and two
// standalone triangles (vertex and edge contacts), all built from hand-written
// `SbwLevel`s so the pipeline `SbwLevel -> world triangles -> Top` is exercised.
//
// Real part: `level0.sbw` (game install, skipped when absent).  The goldens are
// reproduced by an independent Python parse + triangulation,
// `tools/re/collide_goldens.py`, with the same Möller-Trumbore formulation used
// here.  The object frame is the proven one: the 96-byte header is 12 f64
// (`origin + basis * local`, basis = identity in all shipped objects), so the
// level spans the object origins — the old `origin32 + 1.875 * local` model was
// the high dword of the doubles read as f32.
//
// Pinned values below were cross-checked against a direct all-triangles sweep
// with the reference formulation.  Surfaces are classified by the stored
// `.sbw` polygon normal (`Triangle::data_normal`), not by the winding: the
// shipped levels wind horizontal faces opposite to the authored normal, so the
// winding alone calls every floor a ceiling (docs/COLLIDE.md §1.2).
#include "render/collide.h"

#include "res/sbw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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

// --- synthetic .sbw construction -------------------------------------------

struct TriSpec {
    gm::Vec3d a;
    gm::Vec3d b;
    gm::Vec3d c;
    std::uint32_t sector = 0;
    std::uint16_t texture = 0;
    std::uint16_t material = 0;
    /// Stored polygon normal (`SbwPoly::normal`); zero keeps the old
    /// winding-only behaviour of the synthetic worlds.
    gm::Vec3f normal{};
};

void put_f64(std::uint8_t* p, double value) {
    std::memcpy(p, &value, sizeof(value));
}

/// Identity object transform: origin (0,0,0), basis diag(1,1,1).
/// The header is 12 little-endian `f64`: `origin[3] + basis[3][3]` (row-major),
/// i.e. `world = origin + basis * local`.
void write_identity_header(std::array<std::uint8_t, 96>& header) {
    header.fill(0);
    for (int point = 0; point < 4; ++point) {
        for (int component = 0; component < 3; ++component) {
            const double value = (point == 0) ? 0.0 : (component == point - 1 ? 1.0 : 0.0);
            put_f64(header.data() + 8 * (point * 3 + component), value);
        }
    }
}

/// One object per triangle, identity transform, so local == world.
gm::SbwLevel make_level(const std::vector<TriSpec>& specs) {
    gm::SbwLevel level;
    std::uint32_t sector_count = 1;
    for (const TriSpec& spec : specs) {
        sector_count = std::max(sector_count, spec.sector + 1);
    }
    level.header.sectors.resize(sector_count);
    for (const TriSpec& spec : specs) {
        gm::SbwSectorRef& sector = level.header.sectors.at(spec.sector);
        gm::SbwObjectRef object;
        write_identity_header(object.header);
        sector.objects.push_back(object);

        gm::SbwGeometry geometry;
        geometry.n_verts = 3;
        geometry.vertices = {gm::Vec3f{static_cast<float>(spec.a.x), static_cast<float>(spec.a.y),
                                       static_cast<float>(spec.a.z)},
                             gm::Vec3f{static_cast<float>(spec.b.x), static_cast<float>(spec.b.y),
                                       static_cast<float>(spec.b.z)},
                             gm::Vec3f{static_cast<float>(spec.c.x), static_cast<float>(spec.c.y),
                                       static_cast<float>(spec.c.z)}};
        gm::SbwPoly poly;
        poly.texture = spec.texture;
        poly.material = spec.material;
        poly.normal = spec.normal;
        poly.indices = {0, 1, 2};
        geometry.polys.push_back(poly);
        geometry.n_polys = 1;
        level.geometry.push_back(geometry);
    }
    level.header.object_count = specs.size();
    return level;
}

void add_quad(std::vector<TriSpec>& out, gm::Vec3d p0, gm::Vec3d p1, gm::Vec3d p2, gm::Vec3d p3,
              std::uint32_t sector = 0) {
    out.push_back({p0, p1, p2, sector, 0, 0});
    out.push_back({p0, p2, p3, sector, 0, 0});
}

std::vector<TriSpec> cube_specs(std::uint32_t sector = 0) {
    const gm::Vec3d p000{-1, -1, -1};
    const gm::Vec3d p100{1, -1, -1};
    const gm::Vec3d p110{1, 1, -1};
    const gm::Vec3d p010{-1, 1, -1};
    const gm::Vec3d p001{-1, -1, 1};
    const gm::Vec3d p101{1, -1, 1};
    const gm::Vec3d p111{1, 1, 1};
    const gm::Vec3d p011{-1, 1, 1};
    std::vector<TriSpec> out;
    add_quad(out, p100, p110, p111, p101, sector); // +X
    add_quad(out, p000, p001, p011, p010, sector); // -X
    add_quad(out, p010, p011, p111, p110, sector); // +Y
    add_quad(out, p000, p100, p101, p001, sector); // -Y
    add_quad(out, p001, p101, p111, p011, sector); // +Z (floor)
    add_quad(out, p000, p010, p110, p100, sector); // -Z (ceiling)
    return out;
}

std::vector<TriSpec> floor_specs(double z, std::uint32_t sector = 0, bool up = true) {
    std::vector<TriSpec> out;
    const double s = 50.0;
    if (up) {
        add_quad(out, {-s, -s, z}, {s, -s, z}, {s, s, z}, {-s, s, z}, sector);
    } else {
        add_quad(out, {-s, -s, z}, {-s, s, z}, {s, s, z}, {s, -s, z}, sector);
    }
    return out;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// --- synthetic tests -------------------------------------------------------

void test_cube() {
    const gm::SbwLevel level = make_level(cube_specs());
    std::string error;
    gm::CollisionWorld world;
    CHECK(world.build(level, &error));
    CHECK(error.empty());
    CHECK(world.triangle_count() == 12);
    CHECK(world.sector_count() == 1);
    CHECK_NEAR(world.bounds().lo.x, -1.0, 0.0);
    CHECK_NEAR(world.bounds().hi.z, 1.0, 0.0);

    // Straight down onto the top face.
    {
        const auto hit = world.raycast({0, 0, 5}, {0, 0, -1});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 4.0, 1e-12);
            CHECK_NEAR(hit->point.x, 0.0, 1e-12);
            CHECK_NEAR(hit->point.y, 0.0, 1e-12);
            CHECK_NEAR(hit->point.z, 1.0, 1e-12);
            CHECK_NEAR(hit->normal.x, 0.0, 1e-12);
            CHECK_NEAR(hit->normal.y, 0.0, 1e-12);
            CHECK_NEAR(hit->normal.z, 1.0, 1e-12);
            CHECK(hit->front_face);
            CHECK(hit->sector == 0);
            CHECK(hit->triangle != nullptr);
            CHECK(hit->triangle->bounds.contains({0, 0, 1}));
        }
        CHECK(!world.raycast({0, 0, 5}, {0, 0, -1}, 3.9).has_value());
        const auto exact = world.raycast({0, 0, 5}, {0, 0, -1}, 4.0);
        CHECK(exact.has_value());
    }

    // Non-unit direction: t is a distance in world units.
    {
        const auto hit = world.raycast({-5, 0, 0}, {2, 0, 0});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 4.0, 1e-12);
            CHECK_NEAR(hit->point.x, -1.0, 1e-12);
            CHECK_NEAR(hit->normal.x, -1.0, 1e-12);
            CHECK(hit->front_face);
        }
    }

    // Miss above the cube.
    CHECK(!world.raycast({-5, 0, 3}, {1, 0, 0}).has_value());

    // Origin inside: the exit face is a back face with the normal pointing at
    // the origin, and `backfaces = false` rejects it.
    {
        const auto hit = world.raycast({0, 0, 0}, {1, 0, 0});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 1.0, 1e-12);
            CHECK_NEAR(hit->point.x, 1.0, 1e-12);
            CHECK_NEAR(hit->normal.x, -1.0, 1e-12); // against the ray
            CHECK(!hit->front_face);
        }
        gm::CollideFilter front_only;
        front_only.backfaces = false;
        CHECK(!world.raycast({0, 0, 0}, {1, 0, 0}, gm::kCollideInfinity, front_only).has_value());
        CHECK(world.raycast({-5, 0, 0}, {1, 0, 0}, gm::kCollideInfinity, front_only).has_value());
    }

    // Filter classes of the cube: top floor, bottom ceiling, four walls.
    {
        gm::CollideFilter floors;
        floors.walls = false;
        floors.ceilings = false;
        std::vector<std::uint32_t> out;
        CHECK(world.overlap_aabb(world.bounds(), out, floors) == 2);
        gm::CollideFilter ceilings;
        ceilings.walls = false;
        ceilings.floors = false;
        CHECK(world.overlap_aabb(world.bounds(), out, ceilings) == 2);
        gm::CollideFilter walls;
        walls.floors = false;
        walls.ceilings = false;
        CHECK(world.overlap_aabb(world.bounds(), out, walls) == 8);
        CHECK(world.overlap_aabb(world.bounds(), out) == 12);
    }
}

void test_floor_filters() {
    std::vector<TriSpec> specs = floor_specs(0.0, 0);
    const std::vector<TriSpec> ceiling = floor_specs(10.0, 1, false);
    specs.insert(specs.end(), ceiling.begin(), ceiling.end());
    // Vertical wall at x = 5, spanning y/z, normal -X.
    add_quad(specs, {5, -10, 0}, {5, 10, 0}, {5, 10, 10}, {5, -10, 10}, 2);

    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_level(specs), &error));
    CHECK(world.sector_count() == 3);

    CHECK(world.floor_at(0, 0, 5).has_value());
    if (const auto floor = world.floor_at(0, 0, 5)) {
        CHECK_NEAR(*floor, 0.0, 1e-12);
    }
    CHECK(!world.floor_at(0, 0, -5).has_value()); // nothing below the floor
    CHECK(!world.floor_at(0, 0, 5, 4.0).has_value());
    CHECK(world.floor_at(0, 0, 5, 5.0).has_value());
    CHECK(!world.floor_at(std::numeric_limits<double>::quiet_NaN(), 0, 5).has_value());

    // Wall at x = 5.
    const auto wall = world.raycast({0, 0, 5}, {1, 0, 0});
    CHECK(wall.has_value());
    if (wall) {
        CHECK_NEAR(wall->t, 5.0, 1e-12);
        CHECK(wall->sector == 2);
        CHECK(gm::surface_of(*wall->triangle) == gm::Surface::wall);
        CHECK_NEAR(wall->normal.x, -1.0, 1e-12);
    }

    gm::CollideFilter ceilings;
    ceilings.floors = false;
    ceilings.walls = false;
    const auto up = world.raycast({0, 0, 5}, {0, 0, 1}, gm::kCollideInfinity, ceilings);
    CHECK(up.has_value());
    if (up) {
        CHECK_NEAR(up->t, 5.0, 1e-12);
        CHECK(gm::surface_of(*up->triangle) == gm::Surface::ceiling);
    }
    gm::CollideFilter floors;
    floors.ceilings = false;
    floors.walls = false;
    CHECK(!world.raycast({0, 0, 5}, {0, 0, 1}, gm::kCollideInfinity, floors).has_value());

    // Sector restriction narrows the broad phase.
    const std::uint32_t only_wall[1] = {2};
    gm::CollideFilter sector_wall;
    sector_wall.sectors = only_wall;
    CHECK(!world.raycast({0, 0, 5}, {0, 0, -1}, gm::kCollideInfinity, sector_wall).has_value());
    std::vector<std::uint32_t> sector_out;
    CHECK(world.overlap_aabb(world.bounds(), sector_out, sector_wall) == 2);
    const std::uint32_t only_floor[1] = {0};
    gm::CollideFilter sector_floor;
    sector_floor.sectors = only_floor;
    CHECK(world.raycast({0, 0, 5}, {0, 0, -1}, gm::kCollideInfinity, sector_floor).has_value());

    // A slanted triangle is a wall by the 0.8 threshold; lower the threshold and
    // it becomes a floor.
    gm::Triangle slanted;
    slanted.a = {0, 0, 0};
    slanted.b = {1, 0, 0};
    slanted.c = {0, 1, 1};
    slanted.normal = gm::normalized(gm::cross(slanted.b - slanted.a, slanted.c - slanted.a));
    CHECK(gm::surface_of(slanted) == gm::Surface::wall);
    CHECK(gm::surface_of(slanted, 0.5) == gm::Surface::floor);
}

void test_stored_normal() {
    // Down-wound triangle (`normal.z = -1`) whose stored polygon normal is
    // `+z`: the shipped level bug in miniature.  Classification and the
    // back-face test must follow the stored normal, collision stays
    // two-sided.
    std::vector<TriSpec> specs;
    specs.push_back({{-10, -10, 0}, {-10, 10, 0}, {10, 10, 0}, 0, 0, 0, {0, 0, 1}});
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_level(specs), &error));
    CHECK(world.triangle_count() == 1);
    const gm::Triangle& triangle = world.triangles()[0];
    CHECK_NEAR(triangle.normal.z, -1.0, 1e-12);
    CHECK_NEAR(triangle.data_normal.z, 1.0, 1e-12);
    CHECK_NEAR(gm::surface_normal(triangle).z, 1.0, 1e-12);
    CHECK(gm::surface_of(triangle) == gm::Surface::floor);

    // From above: hit, authored front side, normal towards the ray.
    const auto down = world.raycast({0, 0, 5}, {0, 0, -1});
    CHECK(down.has_value());
    if (down) {
        CHECK_NEAR(down->t, 5.0, 1e-12);
        CHECK_NEAR(down->normal.z, 1.0, 1e-12);
        CHECK(down->front_face);
    }
    // From below: still a hit (two-sided even though the winding is away
    // from this side), reported against the ray, not a front face.
    const auto up = world.raycast({0, 0, -5}, {0, 0, 1});
    CHECK(up.has_value());
    if (up) {
        CHECK_NEAR(up->t, 5.0, 1e-12);
        CHECK_NEAR(up->normal.z, -1.0, 1e-12);
        CHECK(!up->front_face); // `dot(data_normal, +z) > 0`
    }

    // The filter classes follow `surface_of` from either side.
    gm::CollideFilter floors;
    floors.walls = false;
    floors.ceilings = false;
    CHECK(world.raycast({0, 0, 5}, {0, 0, -1}, gm::kCollideInfinity, floors).has_value());
    gm::CollideFilter ceilings;
    ceilings.floors = false;
    ceilings.walls = false;
    CHECK(!world.raycast({0, 0, 5}, {0, 0, -1}, gm::kCollideInfinity, ceilings).has_value());

    // `backfaces = false` is the opt-in one-sided mode: the authored side
    // passes, the other does not.
    gm::CollideFilter front_only;
    front_only.backfaces = false;
    CHECK(world.raycast({0, 0, 5}, {0, 0, -1}, gm::kCollideInfinity, front_only).has_value());
    CHECK(!world.raycast({0, 0, -5}, {0, 0, 1}, gm::kCollideInfinity, front_only).has_value());

    // No stored normal: the winding remains the source (up-wound floor).
    {
        gm::CollisionWorld plain;
        std::string plain_error;
        CHECK(plain.build(make_level(floor_specs(0.0)), &plain_error));
        const gm::Triangle& up_triangle = plain.triangles()[0];
        CHECK_NEAR(gm::length(up_triangle.data_normal), 0.0, 0.0);
        CHECK(gm::surface_of(up_triangle) == gm::Surface::floor);
        CHECK(plain.floor_at(0, 0, 5).has_value());
    }
    // ... and a down-wound synthetic floor is still a ceiling, exactly as
    // before the stored-normal change.
    {
        gm::CollisionWorld plain;
        std::string plain_error;
        CHECK(plain.build(make_level(floor_specs(0.0, 0, false)), &plain_error));
        CHECK(gm::surface_of(plain.triangles()[0]) == gm::Surface::ceiling);
        CHECK(!plain.floor_at(0, 0, 5).has_value());
    }
}

void test_robustness() {
    // Degenerate triangle: skipped, counted, world stays usable.
    {
        std::vector<TriSpec> specs = floor_specs(0.0);
        specs.push_back({{1, 1, 1}, {2, 2, 2}, {3, 3, 3}, 0, 0, 0}); // collinear
        specs.push_back({{1, 1, 1}, {1, 1, 1}, {2, 2, 2}, 0, 0, 0}); // zero edge
        gm::SbwLevel level = make_level(specs);
        gm::CollisionWorld world;
        std::string error;
        CHECK(world.build(level, &error));
        CHECK(world.triangle_count() == 2);
        CHECK(world.stats().degenerate == 2);
        CHECK(!world.raycast({500, 500, 500}, {0, 0, -1}).has_value());
        CHECK(world.raycast({0, 0, 5}, {0, 0, -1}).has_value());
    }
    // Polygon with fewer than three indices is degenerate too.
    {
        gm::SbwLevel level = make_level(floor_specs(0.0));
        level.geometry[0].polys[0].indices = {0, 1};
        gm::CollisionWorld world;
        std::string error;
        CHECK(world.build(level, &error));
        CHECK(world.stats().degenerate == 1);
        CHECK(world.triangle_count() == 1);
    }
    // NaN vertex: clean error, no partially built world.
    {
        gm::SbwLevel level = make_level(floor_specs(0.0));
        level.geometry[0].vertices[0].x = std::numeric_limits<float>::quiet_NaN();
        gm::CollisionWorld world;
        std::string error;
        CHECK(!world.build(level, &error));
        CHECK(error.find("non-finite") != std::string::npos);
        CHECK(world.empty());
        CHECK(world.stats().non_finite == 1);
        CHECK(!world.raycast({0, 0, 5}, {0, 0, -1}).has_value());
    }
    // Infinite vertex.
    {
        gm::SbwLevel level = make_level(floor_specs(0.0));
        level.geometry[0].vertices[1].y = std::numeric_limits<float>::infinity();
        gm::CollisionWorld world;
        std::string error;
        CHECK(!world.build(level, &error));
        CHECK(error.find("non-finite") != std::string::npos);
    }
    // Huge (but finite) coordinate is rejected as corrupt data.
    {
        gm::SbwLevel level = make_level(floor_specs(0.0));
        level.geometry[0].vertices[2].z = 1.0e12f;
        gm::CollisionWorld world;
        std::string error;
        CHECK(!world.build(level, &error));
        CHECK(error.find("out of range") != std::string::npos);
        CHECK(world.stats().out_of_range == 1);
    }
    // Out-of-range vertex index.
    {
        gm::SbwLevel level = make_level(floor_specs(0.0));
        level.geometry[0].polys[0].indices[0] = 99;
        gm::CollisionWorld world;
        std::string error;
        CHECK(!world.build(level, &error));
        CHECK(error.find("index") != std::string::npos);
        CHECK(world.stats().bad_index >= 1);
    }
    // Ray exactly in the floor plane: parallel, no hit (no NaN, no epsilon hit).
    {
        gm::CollisionWorld flat;
        std::string flat_error;
        CHECK(flat.build(make_level(floor_specs(0.0)), &flat_error));
        CHECK(!flat.raycast({-10, 0, 0}, {1, 0, 0}).has_value());
        CHECK(!flat.raycast({0, 0, 0}, {0, 0, 1}).has_value());
        CHECK(flat.raycast({0, 0, 0}, {0, 0, -1}).has_value() == false);
        CHECK(flat.raycast({0, 0, 1e-6}, {0, 0, -1}).has_value()); // above the epsilon
    }
    // Bad query parameters never crash and cleanly miss.
    {
        gm::CollisionWorld world;
        std::string error;
        CHECK(world.build(make_level(floor_specs(0.0)), &error));
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        CHECK(!world.raycast({nan, 0, 5}, {0, 0, -1}).has_value());
        CHECK(!world.raycast({0, 0, 5}, {nan, 0, 0}).has_value());
        CHECK(!world.raycast({0, 0, 5}, {0, 0, 0}).has_value());
        CHECK(!world.raycast({0, 0, 5}, {inf, 0, 0}).has_value());
        CHECK(!world.raycast({0, inf, 5}, {0, 0, -1}).has_value());
        CHECK(!world.raycast({0, 0, 5}, {0, 0, -1}, nan).has_value());
        CHECK(!world.raycast({0, 0, 5}, {0, 0, -1}, 0.0).has_value());
        CHECK(!world.raycast({0, 0, 5}, {0, 0, -1}, -1.0).has_value());
        CHECK(!world.sphere_cast({0, 0, 5}, {0, 0, -1}, -1.0).has_value());
        CHECK(!world.sphere_cast({0, 0, 5}, {0, 0, -1}, nan).has_value());
        CHECK(!world.sphere_cast({nan, 0, 5}, {0, 0, -1}, 1.0).has_value());
        CHECK(!world.floor_at(0, 0, 5, nan).has_value());
        // Empty world.
        gm::CollisionWorld empty;
        empty.clear();
        CHECK(empty.empty());
        CHECK(!empty.raycast({0, 0, 5}, {0, 0, -1}).has_value());
        std::vector<std::uint32_t> empty_out;
        CHECK(empty.overlap_aabb(gm::Aabb{}, empty_out) == 0);
    }
}

void test_sphere() {
    // Sphere over a floor.
    {
        gm::CollisionWorld world;
        std::string error;
        CHECK(world.build(make_level(floor_specs(0.0)), &error));
        const auto hit = world.sphere_cast({0, 0, 5}, {0, 0, -1}, 2.0);
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 3.0, 1e-12);
            CHECK_NEAR(hit->centre.z, 2.0, 1e-12);
            CHECK_NEAR(hit->point.z, 0.0, 1e-12);
            CHECK_NEAR(hit->normal.z, 1.0, 1e-12);
        }
        const auto zero = world.sphere_cast({0, 0, 5}, {0, 0, -1}, 0.0);
        CHECK(zero.has_value());
        if (zero) {
            CHECK_NEAR(zero->t, 5.0, 1e-12);
        }
        CHECK(!world.sphere_cast({0, 0, 5}, {0, 0, -1}, 2.0, 2.9).has_value());
        // Already overlapping: t = 0 and a push-out normal.
        const auto overlap = world.sphere_cast({0, 0, 1}, {0, 0, -1}, 2.0);
        CHECK(overlap.has_value());
        if (overlap) {
            CHECK_NEAR(overlap->t, 0.0, 0.0);
            CHECK_NEAR(overlap->normal.z, 1.0, 1e-12);
            CHECK_NEAR(overlap->point.z, 0.0, 1e-12);
        }
    }
    // Sphere against a cube face.
    {
        gm::CollisionWorld world;
        std::string error;
        CHECK(world.build(make_level(cube_specs()), &error));
        const auto hit = world.sphere_cast({-5, 0, 0}, {1, 0, 0}, 1.0);
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 3.0, 1e-12);
            CHECK_NEAR(hit->centre.x, -2.0, 1e-12);
            CHECK_NEAR(hit->point.x, -1.0, 1e-12);
            CHECK_NEAR(hit->normal.x, -1.0, 1e-12);
        }
    }
    // Vertex contact: triangle (0,0,0)-(1,0,0)-(0,1,0), sphere from +X at z=0.
    {
        std::vector<TriSpec> specs;
        specs.push_back({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 0, 0, 0});
        gm::CollisionWorld world;
        std::string error;
        CHECK(world.build(make_level(specs), &error));
        const auto hit = world.sphere_cast({5, 0, 0}, {-1, 0, 0}, 1.0);
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 3.0, 1e-12);
            CHECK_NEAR(hit->centre.x, 2.0, 1e-12);
            CHECK_NEAR(hit->point.x, 1.0, 1e-12);
            CHECK_NEAR(hit->normal.x, 1.0, 1e-12);
        }
    }
    // Edge contact: long edge (-10,0,0)-(10,0,0); the vertices are 10 units
    // away and the motion is in the triangle plane, so the face test is
    // parallel and the edge interior is the first contact.
    {
        std::vector<TriSpec> specs;
        specs.push_back({{-10, 0, 0}, {10, 0, 0}, {0, 10, 0}, 0, 0, 0});
        gm::CollisionWorld world;
        std::string error;
        CHECK(world.build(make_level(specs), &error));
        const auto hit = world.sphere_cast({0, -2, 0}, {0, 1, 0}, 0.5);
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 1.5, 1e-12);
            CHECK_NEAR(hit->centre.y, -0.5, 1e-12);
            CHECK_NEAR(hit->point.x, 0.0, 1e-12);
            CHECK_NEAR(hit->point.y, 0.0, 1e-12);
            CHECK_NEAR(hit->normal.y, -1.0, 1e-12); // pushes back against the motion
        }
    }
}

void test_aabb() {
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_level(cube_specs()), &error));
    std::vector<std::uint32_t> out;
    gm::Aabb box;
    box.expand(gm::Vec3d{-2, -2, -2});
    box.expand(gm::Vec3d{2, 2, 2});
    CHECK(world.overlap_aabb(box, out) == 12);
    CHECK(out.size() == 12);
    CHECK(world.overlap_aabb(box, out) == 12); // reused vector is cleared

    // Strictly inside the cube: triangle AABBs are on the faces, so nothing.
    gm::Aabb inside;
    inside.expand(gm::Vec3d{-0.5, -0.5, -0.5});
    inside.expand(gm::Vec3d{0.5, 0.5, 0.5});
    CHECK(world.overlap_aabb(inside, out) == 0);

    gm::Aabb far;
    far.expand(gm::Vec3d{10, 10, 10});
    far.expand(gm::Vec3d{11, 11, 11});
    CHECK(world.overlap_aabb(far, out) == 0);

    // Inverted / NaN boxes are rejected rather than matched.
    gm::Aabb inverted;
    inverted.lo = {1, 1, 1};
    inverted.hi = {-1, -1, -1};
    CHECK(world.overlap_aabb(inverted, out) == 0);
    gm::Aabb nan_box;
    nan_box.lo = {0, 0, 0};
    nan_box.hi = {std::numeric_limits<double>::quiet_NaN(), 1, 1};
    CHECK(world.overlap_aabb(nan_box, out) == 0);

    // Every triangle sits inside its own sector box (broad-phase invariant).
    for (const gm::Triangle& triangle : world.triangles()) {
        const gm::Aabb& sector = world.sector_bounds(triangle.sector);
        CHECK(sector.valid());
        CHECK(sector.contains(triangle.a));
        CHECK(sector.contains(triangle.b));
        CHECK(sector.contains(triangle.c));
    }
}

// --- level0 goldens --------------------------------------------------------

void test_level0() {
    const fs::path sbw = kGame / "level0.sbw";
    if (!fs::exists(sbw)) {
        std::printf("collide: level0.sbw not installed, real-level tests skipped\n");
        return;
    }
    std::string error;
    const std::vector<std::uint8_t> raw = read_file(sbw);
    CHECK(!raw.empty());
    auto level = gm::parse_sbw(raw, error);
    CHECK(level.has_value());
    if (!level) {
        std::fprintf(stderr, "collide: parse_sbw: %s\n", error.c_str());
        return;
    }

    gm::CollisionWorld world;
    error.clear();
    CHECK(world.build(*level, &error));
    CHECK(error.empty());
    CHECK(world.sector_count() == 58);
    CHECK(world.stats().objects == 192);
    CHECK(world.stats().polygons == 1449);
    CHECK(world.stats().triangles == 3162);
    CHECK(world.stats().degenerate == 2); // two collinear polygons in sector 5
    CHECK(world.triangle_count() == 3162);

    // Surface split by the 0.8 threshold on the stored polygon normal
    // (`surface_normal()`); the winding normal gives the horizontal faces the
    // opposite class (522 / 2082 / 558), which is the bug this pins.
    // The eight triangles stored at exactly `n.z = -0.8` are `-0.79999999`
    // after the f32 round trip, hence the 1e-6 tolerance for data normals.
    std::size_t floors = 0;
    std::size_t walls = 0;
    std::size_t ceilings = 0;
    for (const gm::Triangle& triangle : world.triangles()) {
        switch (gm::surface_of(triangle)) {
        case gm::Surface::floor:
            ++floors;
            break;
        case gm::Surface::ceiling:
            ++ceilings;
            break;
        case gm::Surface::wall:
            ++walls;
            break;
        }
    }
    CHECK(floors == 558);
    CHECK(walls == 2082);
    CHECK(ceilings == 522);

    // World AABB and sector 10 AABB, produced by the Python oracle
    // (`tools/re/collide_goldens.py`, an independent parse + triangulation).
    // The frame is the proven one: the 96-byte header is 12 f64
    // (`origin + basis * local`, basis = identity), so the world spans the
    // object origins, not `origin32 + 1.875 * local`.
    const gm::Aabb& bounds = world.bounds();
    CHECK_NEAR(bounds.lo.x, -3080.0, 1e-9);
    CHECK_NEAR(bounds.lo.y, -408.0, 1e-9);
    CHECK_NEAR(bounds.lo.z, -1920.0, 1e-9);
    CHECK_NEAR(bounds.hi.x, 1616.0, 1e-9);
    CHECK_NEAR(bounds.hi.y, 8836.0, 1e-9);
    CHECK_NEAR(bounds.hi.z, 288.0, 1e-9);

    const gm::Aabb& sector10 = world.sector_bounds(10);
    CHECK_NEAR(sector10.lo.x, 776.0, 1e-9);
    CHECK_NEAR(sector10.lo.y, 364.0, 1e-9);
    CHECK_NEAR(sector10.lo.z, 128.0, 1e-9);
    CHECK_NEAR(sector10.hi.x, 1048.0, 1e-9);
    CHECK_NEAR(sector10.hi.y, 988.0, 1e-9);
    CHECK_NEAR(sector10.hi.z, 176.0, 1e-9);

    // Spawn eye (24, -208, 58): the alcove floor at z = 16 is the highest
    // authored floor below (stored `n.z = +1`, winding `-1`).  The larger room
    // plane at z = 0 is stored with `n.z = -1`, i.e. it is an authored ceiling
    // (`surface_of`), so `floor_at` stops at 16.
    CHECK(world.floor_at(24, -208, 58).has_value());
    if (const auto z = world.floor_at(24, -208, 58)) {
        CHECK_NEAR(*z, 16.0, 1e-9);
    }
    {
        // Straight down the first surface is the alcove floor at z = 16.
        const auto hit = world.raycast({24, -208, 58}, {0, 0, -1});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 42.0, 1e-9);
            CHECK_NEAR(hit->point.z, 16.0, 1e-9);
            CHECK_NEAR(hit->normal.z, 1.0, 1e-12);
            CHECK(hit->sector == 56);
            CHECK(hit->triangle->object == 1);
            CHECK(hit->triangle->texture == 3);
            CHECK(hit->front_face); // the ray arrives on the authored +z side
            CHECK(gm::surface_of(*hit->triangle) == gm::Surface::floor);
            CHECK_NEAR(hit->triangle->normal.z, -1.0, 1e-12); // winding is opposite
            CHECK_NEAR(hit->triangle->data_normal.z, 1.0, 1e-12);
        }
    }

    // Horizontal rays at eye height, all-triangle sweep.
    {
        const auto hit = world.raycast({24, -208, 58}, {1, 0, 0});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 56.0, 1e-9);
            CHECK_NEAR(hit->point.x, 80.0, 1e-9);
            CHECK_NEAR(hit->normal.x, -1.0, 1e-12);
            CHECK(hit->sector == 56);
            CHECK(hit->triangle->texture == 34);
            CHECK(gm::surface_of(*hit->triangle) == gm::Surface::wall);
        }
    }
    {
        const auto hit = world.raycast({24, -208, 58}, {-1, 0, 0});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 72.0, 1e-9);
            CHECK_NEAR(hit->point.x, -48.0, 1e-9);
            CHECK_NEAR(hit->normal.x, 1.0, 1e-12);
            CHECK(hit->sector == 47);
        }
    }
    {
        const auto hit = world.raycast({24, -208, 58}, {0, 1, 0});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 336.0, 1e-9);
            CHECK_NEAR(hit->point.y, 128.0, 1e-9);
            CHECK_NEAR(hit->normal.y, -1.0, 1e-12);
            CHECK(hit->sector == 14);
            CHECK(gm::surface_of(*hit->triangle) == gm::Surface::wall);
        }
    }
    {
        const auto hit = world.raycast({24, -208, 58}, {0, -1, 0});
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 80.0, 1e-9);
            CHECK_NEAR(hit->point.y, -288.0, 1e-9);
            CHECK_NEAR(hit->normal.y, 1.0, 1e-12);
            CHECK(hit->sector == 1);
        }
    }

    // Filtered sweeps change where the first hit lands (same Python oracle).
    {
        // Ceilings only: past the floor, the room plane at z = 0 is an
        // authored ceiling (stored n.z = -1), hit by the same downwards ray.
        gm::CollideFilter ceilings;
        ceilings.floors = false;
        ceilings.walls = false;
        const auto hit = world.raycast({24, -208, 58}, {0, 0, -1}, gm::kCollideInfinity, ceilings);
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 58.0, 1e-9);
            CHECK_NEAR(hit->point.z, 0.0, 1e-9);
            CHECK(hit->sector == 56);
            CHECK(hit->triangle->texture == 0);
            CHECK(!hit->front_face); // authored `-z` side faces the incoming ray
            CHECK(gm::surface_of(*hit->triangle) == gm::Surface::ceiling);
        }
    }
    {
        // Floors only, straight down: the alcove floor at z = 16.
        gm::CollideFilter floors;
        floors.walls = false;
        floors.ceilings = false;
        const auto hit = world.raycast({24, -208, 58}, {0, 0, -1}, gm::kCollideInfinity, floors);
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 42.0, 1e-9);
            CHECK_NEAR(hit->point.z, 16.0, 1e-9);
            CHECK_NEAR(hit->normal.z, 1.0, 1e-12);
            CHECK(hit->sector == 56);
            CHECK(hit->triangle->texture == 3);
        }
    }
    {
        // The only surface along +y is a wall, so a floors+ceilings sweep misses.
        gm::CollideFilter no_walls;
        no_walls.walls = false;
        CHECK(!world.raycast({24, -208, 58}, {0, 1, 0}, gm::kCollideInfinity, no_walls).has_value());
    }

    // Sector restriction: the floor at (24, -208) lives in sector 56, not 7.
    {
        const std::uint32_t only56[1] = {56};
        gm::CollideFilter filter;
        filter.sectors = only56;
        CHECK(world.raycast({24, -208, 58}, {0, 0, -1}, gm::kCollideInfinity, filter).has_value());
        const std::uint32_t only7[1] = {7};
        filter.sectors = only7;
        CHECK(!world.raycast({24, -208, 58}, {0, 0, -1}, gm::kCollideInfinity, filter).has_value());
    }

    // Broad phase over the whole world.
    {
        std::vector<std::uint32_t> out;
        CHECK(world.overlap_aabb(bounds, out) == 3162);
        gm::CollideFilter walls;
        walls.floors = false;
        walls.ceilings = false;
        CHECK(world.overlap_aabb(bounds, out, walls) == 2082);
    }

    // Sphere of radius 10 over the first surface below z = 300 (the straight
    // down raycast from the same origin lands at z = 112, so face contact is at
    // t = 300 - 112 - 10).
    {
        const auto hit = world.sphere_cast({24, -208, 300}, {0, 0, -1}, 10.0);
        CHECK(hit.has_value());
        if (hit) {
            CHECK_NEAR(hit->t, 178.0, 1e-9);
            CHECK_NEAR(hit->centre.z, 122.0, 1e-9);
            CHECK_NEAR(hit->point.z, 112.0, 1e-9);
            CHECK_NEAR(hit->normal.z, 1.0, 1e-12);
            CHECK(hit->sector == 56);
        }
    }
}

} // namespace

int main() {
    test_cube();
    test_floor_filters();
    test_stored_normal();
    test_robustness();
    test_sphere();
    test_aabb();
    test_level0();
    if (g_failures != 0) {
        std::fprintf(stderr, "collide: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("collide: ok\n");
    return 0;
}
