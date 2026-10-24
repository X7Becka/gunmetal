// Broad-phase grid over the static triangles of a `CollisionWorld`
// (docs/COLLIDE_GRID.md).
//
// The claim under test is *parity*: `CollideGrid::raycast` must return exactly
// the `RayHit` of `CollisionWorld::raycast` -- same triangle, index, sector, t,
// point, normal and front-face flag, bit for bit, not to a tolerance.  The grid
// only changes how many triangles reach the narrow phase, never the answer.
//
// Real levels: `level0.sbw` (the campaign hub) and `dm1.sbw` (the largest death
// match arena, 11 781 triangles) from the game install or its mirror.  Rays are
// deterministic: a fixed LCG seed, printed below, so a failure is reproducible
// by re-running the binary.
//
// Also covered: every combination of the four `CollideFilter` booleans and a
// `sectors` restriction; the same hit on a repeated query (the grid is
// immutable, queries are const); the candidate count versus a full scan; and
// `build` refusing an empty world, a non-positive/NaN/inf cell size and a grid
// finer than the caps.
#include "render/collide_grid.h"

#include "res/sbw.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
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

const fs::path kInstall = "/mnt/d/games/Gunmetal 3dfx";
constexpr std::uint64_t kSeed = 0x5eed'c0de'1234'abcdULL;
constexpr int kParityRays = 500;
constexpr int kFilterRays = 32;
constexpr int kSweepRays = 200;

constexpr double kPi = 3.14159265358979323846;

// --- setup -----------------------------------------------------------------

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

/// Game install first, then the warm mirror used by `regress.py --mirror`.
fs::path find_sbw(const char* name) {
    const fs::path installed = kInstall / name;
    if (fs::exists(installed)) {
        return installed;
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        const fs::path mirrored = fs::path(home) / ".cache/gunmetal-re/game" / name;
        if (fs::exists(mirrored)) {
            return mirrored;
        }
    }
    return {};
}

bool load_level(const char* name, gm::CollisionWorld& world) {
    const fs::path sbw = find_sbw(name);
    if (sbw.empty()) {
        std::printf("collide_grid: %s not installed, real-level part skipped\n", name);
        return false;
    }
    std::string error;
    const std::vector<std::uint8_t> raw = read_file(sbw);
    CHECK(!raw.empty());
    const std::optional<gm::SbwLevel> level = gm::parse_sbw(raw, error);
    CHECK(level.has_value());
    if (!level) {
        std::fprintf(stderr, "collide_grid: parse_sbw(%s): %s\n", name, error.c_str());
        return false;
    }
    error.clear();
    CHECK(world.build(*level, &error));
    CHECK(error.empty());
    std::printf("collide_grid: %s triangles=%zu sectors=%zu span=(%.1f %.1f %.1f)\n", name,
                world.triangle_count(), world.sector_count(), world.bounds().size().x,
                world.bounds().size().y, world.bounds().size().z);
    return true;
}

// --- deterministic rays -----------------------------------------------------

/// SplitMix64: tiny, portable, and the seed is printed so any case can be
/// replayed.  `unit()` is uniform on [0, 1).
struct Lcg {
    std::uint64_t state = 0;

    explicit Lcg(std::uint64_t seed) : state(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

    double between(double lo, double hi) { return lo + (hi - lo) * unit(); }
};

struct Ray {
    gm::Vec3d origin;
    gm::Vec3d direction;
    double max_t = 0.0;
};

/// Half of the origins sit near a triangle centroid (so the ray starts inside
/// the playable volume and has a realistic chance to hit), half are uniform in
/// the world AABB.  Directions are uniform on the sphere; `max_t` cycles
/// through infinity, a quarter, a half and the full diagonal, so both "hit"
/// and "miss" and both finite and infinite limits are exercised.
std::vector<Ray> make_rays(const gm::CollisionWorld& world, int count, std::uint64_t seed) {
    Lcg rng(seed);
    const gm::Aabb& bounds = world.bounds();
    const gm::Vec3d span = bounds.size();
    const double diagonal = gm::length(span);
    const std::array<double, 4> limits = {gm::kCollideInfinity, diagonal * 0.25, diagonal * 0.5,
                                          diagonal};
    std::vector<Ray> rays;
    rays.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Ray ray;
        if (i % 2 == 0 && !world.triangles().empty()) {
            const gm::Triangle& triangle =
                world.triangles()[static_cast<std::size_t>(rng.next() % world.triangle_count())];
            const gm::Vec3d offset = {rng.between(-4.0, 4.0), rng.between(-4.0, 4.0),
                                      rng.between(-4.0, 4.0)};
            ray.origin = triangle.centroid + offset;
        } else {
            ray.origin = {rng.between(bounds.lo.x, bounds.hi.x),
                          rng.between(bounds.lo.y, bounds.hi.y),
                          rng.between(bounds.lo.z, bounds.hi.z)};
        }
        const double cos_phi = rng.between(-1.0, 1.0);
        const double sin_phi = std::sqrt(std::max(0.0, 1.0 - cos_phi * cos_phi));
        const double theta = rng.between(0.0, 2.0 * kPi);
        ray.direction = {sin_phi * std::cos(theta), sin_phi * std::sin(theta), cos_phi};
        ray.max_t = limits[static_cast<std::size_t>(i) % limits.size()];
        rays.push_back(ray);
    }
    return rays;
}

// --- comparison -------------------------------------------------------------

bool same_hit(const std::optional<gm::RayHit>& brute, const std::optional<gm::RayHit>& grid,
              double& distance_delta) {
    if (brute.has_value() != grid.has_value()) {
        return false;
    }
    if (!brute) {
        return true;
    }
    distance_delta = std::fabs(brute->t - grid->t);
    return brute->t == grid->t && brute->point.x == grid->point.x &&
           brute->point.y == grid->point.y && brute->point.z == grid->point.z &&
           brute->normal.x == grid->normal.x && brute->normal.y == grid->normal.y &&
           brute->normal.z == grid->normal.z && brute->triangle_index == grid->triangle_index &&
           brute->triangle == grid->triangle && brute->sector == grid->sector &&
           brute->front_face == grid->front_face && brute->dynamic == grid->dynamic;
}

struct ParityTotals {
    std::size_t compared = 0;
    std::size_t same = 0;
    std::size_t hits = 0;
    double max_distance_delta = 0.0;
};

ParityTotals compare_rays(const gm::CollisionWorld& world, const gm::CollideGrid& grid,
                          const std::vector<Ray>& rays, const gm::CollideFilter& filter,
                          const char* label) {
    ParityTotals totals;
    for (const Ray& ray : rays) {
        const std::optional<gm::RayHit> brute =
            world.raycast(ray.origin, ray.direction, ray.max_t, filter);
        const std::optional<gm::RayHit> fast =
            grid.raycast(world, ray.origin, ray.direction, filter, ray.max_t);
        double delta = 0.0;
        const bool equal = same_hit(brute, fast, delta);
        ++totals.compared;
        if (equal) {
            ++totals.same;
        } else if (totals.compared - totals.same <= 5) {
            std::fprintf(stderr,
                         "collide_grid: %s mismatch ray %zu: brute=%s t=%.17g idx=%u, "
                         "grid=%s t=%.17g idx=%u\n",
                         label, totals.compared - 1, brute ? "hit" : "miss",
                         brute ? brute->t : 0.0, brute ? brute->triangle_index : 0u,
                         fast ? "hit" : "miss", fast ? fast->t : 0.0,
                         fast ? fast->triangle_index : 0u);
        }
        if (brute) {
            ++totals.hits;
            totals.max_distance_delta = std::max(totals.max_distance_delta, delta);
        }
    }
    CHECK(totals.same == totals.compared);
    std::printf("collide_grid: %s same=%zu/%zu hits=%zu max_distance_delta=%.3e\n", label,
                totals.same, totals.compared, totals.hits, totals.max_distance_delta);
    return totals;
}

// --- build helpers ----------------------------------------------------------

/// Cell size for a level: a fixed fraction of the longest world extent, so the
/// same divisor means "at most N cells along the level" on every level.
double cell_size_for(const gm::CollisionWorld& world, int divisor) {
    const gm::Vec3d span = world.bounds().size();
    const double longest = std::max(span.x, std::max(span.y, span.z));
    return longest / static_cast<double>(divisor);
}

std::size_t total_candidates(const gm::CollisionWorld& world, const gm::CollideGrid& grid,
                             const std::vector<Ray>& rays) {
    std::size_t total = 0;
    for (const Ray& ray : rays) {
        total += grid.candidates_for_ray(world, ray.origin, ray.direction, {}, ray.max_t);
    }
    return total;
}

double elapsed_ms(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// --- world state: suppression and the dynamic fallback ----------------------

/// The grid indexes static triangles only.  Two `CollisionWorld` states must
/// still return exactly the reference answer: a suppression list (the grid
/// consults `is_suppressed`) and a non-empty dynamic set (the grid delegates,
/// docs/COLLIDE_GRID.md §4).
void test_state_paths(const char* name, gm::CollisionWorld& world, const gm::CollideGrid& grid,
                      const std::vector<Ray>& rays) {
    const std::vector<Ray> subset(rays.begin(), rays.begin() + kFilterRays);
    char label[128];

    // 1) Suppress the (sector, object) of the first ray that hits something.
    std::uint32_t sector = 0;
    std::uint32_t object = 0;
    bool found = false;
    for (const Ray& ray : subset) {
        const std::optional<gm::RayHit> hit = world.raycast(ray.origin, ray.direction, ray.max_t);
        if (hit) {
            sector = hit->sector;
            object = hit->triangle->object;
            found = true;
            break;
        }
    }
    CHECK(found);
    if (found) {
        std::string error;
        CHECK(world.set_dynamic({}, {{sector, object}}, &error));
        CHECK(error.empty());
        std::snprintf(label, sizeof(label), "%s suppressed(%u,%u)", name, sector, object);
        compare_rays(world, grid, subset, gm::CollideFilter{}, label);
        world.clear_dynamic();
    }

    // 2) A dynamic panel in front of the first probe: the static grid cannot
    // see it, so the query must fall back to `CollisionWorld::raycast`.
    gm::Triangle panel;
    const gm::Vec3d anchor = subset[0].origin;
    panel.a = anchor + gm::Vec3d{40.0, -20.0, -20.0};
    panel.b = anchor + gm::Vec3d{40.0, 20.0, -20.0};
    panel.c = anchor + gm::Vec3d{40.0, 0.0, 20.0};
    std::string error;
    CHECK(world.set_dynamic({panel}, {}, &error));
    CHECK(error.empty());
    std::snprintf(label, sizeof(label), "%s dynamic", name);
    compare_rays(world, grid, subset, gm::CollideFilter{}, label);
    world.clear_dynamic();

    // 3) Plain static world again: the grid is back in charge.
    std::snprintf(label, sizeof(label), "%s static again", name);
    compare_rays(world, grid, subset, gm::CollideFilter{}, label);
}

// --- per-level test ---------------------------------------------------------

void test_level(const char* name) {
    gm::CollisionWorld world;
    if (!load_level(name, world)) {
        return;
    }

    // Spatial sweep: finer cells mean fewer candidates per ray but more cells
    // traversed and more references.  The measured time optimum on both shipped
    // levels is the /128 grid (docs/COLLIDE_GRID.md §3), so that is the size
    // used for the parity run; the sweep prints the other points.
    const std::vector<Ray> sweep_rays = make_rays(world, kSweepRays, kSeed ^ 0xa5a5a5a5ULL);
    const std::vector<int> divisors = {32, 64, 128, 256};
    constexpr int kChosenDivisor = 128;
    const double best_cell = cell_size_for(world, kChosenDivisor);
    std::printf("collide_grid: %s cell sweep (%d rays)\n", name, kSweepRays);
    const auto sweep_brute_start = std::chrono::steady_clock::now();
    for (const Ray& ray : sweep_rays) {
        world.raycast(ray.origin, ray.direction, ray.max_t);
    }
    const double sweep_brute_ms = elapsed_ms(sweep_brute_start);
    for (const int divisor : divisors) {
        const double cell = cell_size_for(world, divisor);
        gm::CollideGrid grid;
        std::string error;
        if (!grid.build(world, cell, &error)) {
            std::printf("collide_grid: %s  cell=%8.2f  refused: %s\n", name, cell, error.c_str());
            continue;
        }
        const auto grid_start = std::chrono::steady_clock::now();
        for (const Ray& ray : sweep_rays) {
            grid.raycast(world, ray.origin, ray.direction, {}, ray.max_t);
        }
        const double sweep_grid_ms = elapsed_ms(grid_start);
        const std::size_t candidates = total_candidates(world, grid, sweep_rays);
        const double per_ray =
            static_cast<double>(candidates) / static_cast<double>(sweep_rays.size());
        const double ratio = per_ray / static_cast<double>(world.triangle_count());
        std::printf("collide_grid: %s  cell=%8.2f  cells=%9zu refs=%9zu  cand/ray=%8.1f "
                    "(%5.2f%% of %zu)  time=%6.2f ms (brute %.2f ms)\n",
                    name, cell, grid.cells(), grid.references(), per_ray, 100.0 * ratio,
                    world.triangle_count(), sweep_grid_ms, sweep_brute_ms);
    }
    CHECK(best_cell > 0.0);
    if (best_cell == 0.0) {
        return;
    }

    gm::CollideGrid grid;
    std::string error;
    CHECK(grid.build(world, best_cell, &error));
    CHECK(error.empty());
    CHECK(grid.matches(world));
    CHECK(grid.cells() > 0);
    CHECK(grid.triangle_count() == world.triangle_count());
    CHECK(grid.dimensions().x * grid.dimensions().y * grid.dimensions().z == grid.cells());
    std::printf("collide_grid: %s chosen cell=%.2f cells=%zu refs=%zu memory=%.1f KiB\n", name,
                best_cell, grid.cells(), grid.references(),
                static_cast<double>(grid.memory_bytes()) / 1024.0);

    // --- parity on 500 rays, default filter -------------------------------
    const std::vector<Ray> rays = make_rays(world, kParityRays, kSeed);
    char label[96];
    std::snprintf(label, sizeof(label), "%s parity(seed=0x%016llx)", name,
                  static_cast<unsigned long long>(kSeed));
    compare_rays(world, grid, rays, gm::CollideFilter{}, label);

    // --- parity under every filter combination ----------------------------
    // `CollideFilter` has four booleans (floors, ceilings, walls, backfaces);
    // there is no separate "objects" switch, so the 2^4 grid is the full
    // combination space, and a `sectors` restriction is checked after it.
    const std::vector<Ray> filter_rays =
        std::vector<Ray>(rays.begin(), rays.begin() + kFilterRays);
    std::size_t combos_compared = 0;
    std::size_t combos_same = 0;
    for (int mask = 0; mask < 16; ++mask) {
        gm::CollideFilter filter;
        filter.floors = (mask & 1) != 0;
        filter.ceilings = (mask & 2) != 0;
        filter.walls = (mask & 4) != 0;
        filter.backfaces = (mask & 8) != 0;
        for (const Ray& ray : filter_rays) {
            const std::optional<gm::RayHit> brute =
                world.raycast(ray.origin, ray.direction, ray.max_t, filter);
            const std::optional<gm::RayHit> fast =
                grid.raycast(world, ray.origin, ray.direction, filter, ray.max_t);
            double delta = 0.0;
            ++combos_compared;
            if (same_hit(brute, fast, delta)) {
                ++combos_same;
            }
        }
    }
    CHECK(combos_same == combos_compared);
    std::printf("collide_grid: %s filters combos=16 same=%zu/%zu\n", name, combos_same,
                combos_compared);

    // One sector list per chosen sector over the same ray subset.
    std::size_t sector_compared = 0;
    std::size_t sector_same = 0;
    for (std::uint32_t sector = 0; sector < world.sector_count(); ++sector) {
        gm::CollideFilter filter;
        filter.sectors = std::span<const std::uint32_t>(&sector, 1);
        for (const Ray& ray : filter_rays) {
            const std::optional<gm::RayHit> brute =
                world.raycast(ray.origin, ray.direction, ray.max_t, filter);
            const std::optional<gm::RayHit> fast =
                grid.raycast(world, ray.origin, ray.direction, filter, ray.max_t);
            double delta = 0.0;
            ++sector_compared;
            if (same_hit(brute, fast, delta)) {
                ++sector_same;
            }
        }
    }
    CHECK(sector_same == sector_compared);
    std::printf("collide_grid: %s sectors same=%zu/%zu\n", name, sector_same, sector_compared);

    // --- acceleration ------------------------------------------------------
    const std::size_t candidates = total_candidates(world, grid, rays);
    const std::size_t full_scan = rays.size() * world.triangle_count();
    // The gate: a grid that does not cut the narrow phase by at least 10x is
    // not worth its build time.  Measured values are far below (see
    // docs/COLLIDE_GRID.md); the assertion only pins the order of magnitude.
    CHECK(candidates * 10 < full_scan);
    std::printf("collide_grid: %s candidates=%zu full_scan=%zu (%.2f%%), cand/ray=%.1f\n", name,
                candidates, full_scan,
                100.0 * static_cast<double>(candidates) / static_cast<double>(full_scan),
                static_cast<double>(candidates) / static_cast<double>(rays.size()));

    // Wall clock on the same rays (informational: Debug builds measure the
    // constant factor badly, the candidate count above is the hard number).
    std::size_t brute_hits = 0;
    const auto brute_start = std::chrono::steady_clock::now();
    for (const Ray& ray : rays) {
        if (world.raycast(ray.origin, ray.direction, ray.max_t)) {
            ++brute_hits;
        }
    }
    const double brute_ms = elapsed_ms(brute_start);
    std::size_t fast_hits = 0;
    const auto fast_start = std::chrono::steady_clock::now();
    for (const Ray& ray : rays) {
        if (grid.raycast(world, ray.origin, ray.direction, {}, ray.max_t)) {
            ++fast_hits;
        }
    }
    const double fast_ms = elapsed_ms(fast_start);
    CHECK(brute_hits == fast_hits);
    std::printf("collide_grid: %s time brute=%.1f ms grid=%.1f ms speedup=%.2fx hits=%zu\n",
                name, brute_ms, fast_ms, brute_ms / std::max(fast_ms, 1e-6), fast_hits);

    // --- immutability / determinism ----------------------------------------
    const std::size_t cells_before = grid.cells();
    const std::size_t references_before = grid.references();
    const Ray& probe = rays[0];
    const std::optional<gm::RayHit> first =
        grid.raycast(world, probe.origin, probe.direction, {}, probe.max_t);
    const std::size_t candidates_first =
        grid.candidates_for_ray(world, probe.origin, probe.direction, {}, probe.max_t);
    const std::optional<gm::RayHit> second =
        grid.raycast(world, probe.origin, probe.direction, {}, probe.max_t);
    const std::size_t candidates_second =
        grid.candidates_for_ray(world, probe.origin, probe.direction, {}, probe.max_t);
    double delta = 0.0;
    CHECK(same_hit(first, second, delta));
    CHECK(candidates_first == candidates_second);
    CHECK(grid.cells() == cells_before);
    CHECK(grid.references() == references_before);
    std::printf("collide_grid: %s repeat candidates=%zu deterministic=1\n", name,
                candidates_first);

    test_state_paths(name, world, grid, rays);

    // --- refused queries and refused builds ---------------------------------
    CHECK(!grid.raycast(world, {std::nan(""), 0.0, 0.0}, {1.0, 0.0, 0.0}));
    CHECK(!grid.raycast(world, probe.origin, {0.0, 0.0, 0.0}));
    CHECK(!grid.raycast(world, probe.origin, probe.direction, {}, 0.0));
    CHECK(grid.candidates_for_ray(world, probe.origin, {0.0, 0.0, 0.0}) == 0);

    const std::array<double, 4> bad_cells = {0.0, -1.0, std::nan(""),
                                             std::numeric_limits<double>::infinity()};
    for (const double bad : bad_cells) {
        gm::CollideGrid broken;
        std::string broken_error;
        const bool ok = broken.build(world, bad, &broken_error);
        CHECK(!ok);
        CHECK(!broken_error.empty());
        CHECK(broken.cells() == 0);
        if (ok || broken_error.empty()) {
            std::fprintf(stderr, "collide_grid: %s bad cell_size accepted\n", name);
        }
    }
    {
        gm::CollideGrid broken;
        std::string broken_error;
        const bool ok = broken.build(world, 1e-7, &broken_error);
        CHECK(!ok); // beyond the cell/reference cap, not a silent OOM
        CHECK(!broken_error.empty());
        CHECK(broken.cells() == 0);
    }
}

// --- synthetic: cell-plane and tie edge cases -------------------------------

void put_f64(std::uint8_t* p, double value) { std::memcpy(p, &value, sizeof(value)); }

/// One sector, one object per triangle, identity transform: local == world.
gm::SbwLevel make_level(const std::vector<std::array<gm::Vec3d, 3>>& triangles) {
    gm::SbwLevel level;
    level.header.sectors.resize(1);
    for (const std::array<gm::Vec3d, 3>& triangle : triangles) {
        gm::SbwObjectRef object;
        object.header.fill(0);
        for (int point = 0; point < 4; ++point) {
            for (int component = 0; component < 3; ++component) {
                const double value =
                    point == 0 ? 0.0 : (component == point - 1 ? 1.0 : 0.0);
                put_f64(object.header.data() + 8 * (point * 3 + component), value);
            }
        }
        level.header.sectors[0].objects.push_back(object);

        gm::SbwGeometry geometry;
        geometry.n_verts = 3;
        for (const gm::Vec3d& vertex : triangle) {
            geometry.vertices.push_back(
                gm::Vec3f{static_cast<float>(vertex.x), static_cast<float>(vertex.y),
                          static_cast<float>(vertex.z)});
        }
        gm::SbwPoly poly;
        poly.indices = {0, 1, 2};
        geometry.polys.push_back(poly);
        geometry.n_polys = 1;
        level.geometry.push_back(geometry);
    }
    level.header.object_count = triangles.size();
    return level;
}

void add_quad(std::vector<std::array<gm::Vec3d, 3>>& out, gm::Vec3d a, gm::Vec3d b, gm::Vec3d c,
              gm::Vec3d d) {
    out.push_back({a, b, c});
    out.push_back({a, c, d});
}

/// A floor plus two *identical* wall triangles at x = 0.  The world AABB is
/// [-100, 100]^2 x [-100, 10]; with a cell size of 20 the grid planes are at
/// -100 + 20k, i.e. x = 0, y = 0 and z = 0 are all cell planes.  The probe ray
/// therefore travels exactly along two cell planes and hits a triangle that
/// lies exactly in a third, and the two triangles produce a bit-identical `t`
/// so the "lowest index on an exact tie" rule is exercised.
void test_synthetic() {
    std::vector<std::array<gm::Vec3d, 3>> triangles;
    add_quad(triangles, {-100, -100, -100}, {100, -100, -100}, {100, 100, -100},
             {-100, 100, -100});
    const std::array<gm::Vec3d, 3> wall = {gm::Vec3d{0, -10, -10}, gm::Vec3d{0, 10, -10},
                                           gm::Vec3d{0, 0, 10}};
    triangles.push_back(wall);
    triangles.push_back(wall); // duplicate geometry, next index: a tie candidate

    const gm::SbwLevel level = make_level(triangles);
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(level, &error));
    CHECK(error.empty());
    CHECK(world.triangle_count() == 4);

    for (const double cell : {20.0, 5.0}) {
        gm::CollideGrid grid;
        CHECK(grid.build(world, cell, &error));
        CHECK(error.empty());
        CHECK(grid.matches(world));
        const int rays = 200;
        const std::vector<Ray> batch = make_rays(world, rays, kSeed ^ 0x51u);
        char label[64];
        std::snprintf(label, sizeof(label), "synthetic(cell=%.1f)", cell);
        compare_rays(world, grid, batch, gm::CollideFilter{}, label);

        // Axis ray along the y = 0 and z = 0 cell planes, hitting the x = 0 wall.
        const gm::Vec3d origin{-50.0, 0.0, 0.0};
        const gm::Vec3d direction{1.0, 0.0, 0.0};
        const std::optional<gm::RayHit> brute =
            world.raycast(origin, direction, gm::kCollideInfinity);
        const std::optional<gm::RayHit> fast =
            grid.raycast(world, origin, direction, {}, gm::kCollideInfinity);
        double delta = 0.0;
        CHECK(same_hit(brute, fast, delta));
        CHECK(brute.has_value());
        if (brute) {
            CHECK(brute->triangle_index == 2); // the lower index wins the exact tie
            CHECK(fast && fast->triangle_index == 2);
            // `t` exactly at the limit is accepted (t <= max_t) by both paths;
            // one ulp below it is a miss by both.
            const std::optional<gm::RayHit> at_limit =
                grid.raycast(world, origin, direction, {}, brute->t);
            CHECK(at_limit.has_value());
            const std::optional<gm::RayHit> below =
                grid.raycast(world, origin, direction, {}, std::nextafter(brute->t, 0.0));
            CHECK(!below.has_value());
            CHECK(!world.raycast(origin, direction, std::nextafter(brute->t, 0.0)));
        }

        // Oblique, non-unit direction through the wall centre.
        const gm::Vec3d oblique = {-50.0, -5.0, -5.0};
        const gm::Vec3d oblique_dir{1.0, 0.1, 0.1};
        const std::optional<gm::RayHit> ob = world.raycast(oblique, oblique_dir);
        const std::optional<gm::RayHit> of = grid.raycast(world, oblique, oblique_dir);
        CHECK(same_hit(ob, of, delta));
        CHECK(ob.has_value());
    }
    std::printf("collide_grid: synthetic cell-plane/tie cases ok (triangles=4)\n");
}

// --- empty world ------------------------------------------------------------

void test_empty_world() {
    gm::CollisionWorld world;
    gm::CollideGrid grid;
    std::string error;
    CHECK(!grid.build(world, 16.0, &error));
    CHECK(!error.empty());
    CHECK(grid.cells() == 0);
    CHECK(!grid.matches(world));
    // The fallback path still answers exactly like the world itself.
    CHECK(!grid.raycast(world, {0, 0, 0}, {1, 0, 0}));
    std::printf("collide_grid: empty world refused: %s\n", error.c_str());
}

} // namespace

int main() {
    std::printf("collide_grid: seed=0x%016llx parity_rays=%d filter_rays=%d\n",
                static_cast<unsigned long long>(kSeed), kParityRays, kFilterRays);
    test_synthetic();
    test_empty_world();
    test_level("level0.sbw");
    test_level("dm1.sbw");
    if (g_failures != 0) {
        std::fprintf(stderr, "collide_grid: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("collide_grid: ok\n");
    return 0;
}
