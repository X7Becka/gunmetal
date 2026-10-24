#include "render/collide_grid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace gm {
namespace {

double axis_of(Vec3d v, int axis) {
    switch (axis) {
    case 0:
        return v.x;
    case 1:
        return v.y;
    default:
        return v.z;
    }
}

/// Same slab test as `CollisionWorld::raycast` (kept local so this translation
/// unit does not depend on the anonymous namespace of collide.cpp).
bool ray_aabb(const Vec3d& origin, const Vec3d& direction, const Aabb& box, double max_t) {
    double t_min = 0.0;
    double t_max = max_t;
    for (int i = 0; i < 3; ++i) {
        const double o = axis_of(origin, i);
        const double d = axis_of(direction, i);
        const double lo = axis_of(box.lo, i);
        const double hi = axis_of(box.hi, i);
        if (std::fabs(d) < 1e-300) {
            if (o < lo || o > hi) {
                return false;
            }
            continue;
        }
        const double inv = 1.0 / d;
        double ta = (lo - o) * inv;
        double tb = (hi - o) * inv;
        if (ta > tb) {
            std::swap(ta, tb);
        }
        if (ta > t_min) {
            t_min = ta;
        }
        if (tb < t_max) {
            t_max = tb;
        }
        if (t_min > t_max) {
            return false;
        }
    }
    return true;
}

void set_first_error(std::string* error, const char* text) {
    if (error != nullptr && error->empty()) {
        *error = text;
    }
}

/// The argument validation of `CollisionWorld::raycast`, factored out so the
/// grid rejects exactly the queries the brute-force path ignores.
struct PreparedRay {
    Vec3d d;
    double limit = 0.0;
};

bool prepare_ray(Vec3d origin, Vec3d direction, double max_t, PreparedRay& out) {
    if (!finite(origin) || !finite(direction)) {
        return false;
    }
    const double len = length(direction);
    if (!(len > 0.0) || !std::isfinite(len)) {
        return false;
    }
    if (!(max_t > 0.0)) {
        return false;
    }
    out.limit = std::isfinite(max_t) ? max_t : kCollideInfinity;
    out.d = direction * (1.0 / len);
    return true;
}

/// The structural half of the narrow phase: suppression, surface/sector flags
/// and the optional one-sided mode.  Shared by the candidate scan and the
/// on-the-fly query so both always agree.
bool structural_allows(const CollisionWorld& world, const Triangle& triangle, Vec3d d,
                       const CollideFilter& filter) {
    if (world.suppressed_count() != 0 && world.is_suppressed(triangle.sector, triangle.object)) {
        return false;
    }
    if (!filter.allows(triangle)) {
        return false;
    }
    if (!filter.backfaces && dot(surface_normal(triangle), d) >= 0.0) {
        return false;
    }
    return true;
}

/// Grow a triangle AABB by this much (world units) before binning.  The grid
/// planes are exact doubles; without the slack a ray running exactly along a
/// plane could walk the cells on one side while the triangle is registered only
/// on the other.  1e-6 is ~1e-9 of a room span and costs no extra cell at the
/// cell sizes the documentation measures.
constexpr double kBinSlack = 1e-6;

} // namespace

std::size_t CollideGrid::axis_cell(double value, int axis) const {
    const std::size_t n = axis == 0 ? dims_.x : (axis == 1 ? dims_.y : dims_.z);
    const double scaled = (value - axis_of(origin_, axis)) / cell_size_;
    if (!(scaled > 0.0)) {
        return 0; // also maps NaN to the first cell
    }
    const double floored = std::floor(scaled);
    const std::size_t index =
        floored >= static_cast<double>(n) ? n - 1 : static_cast<std::size_t>(floored);
    return index;
}

void CollideGrid::clear() {
    cells_ = 0;
    dims_ = Dimensions{};
    cell_size_ = 0.0;
    origin_ = Vec3d{};
    cell_begin_.clear();
    cell_begin_.shrink_to_fit();
    entries_.clear();
    entries_.shrink_to_fit();
    owner_ = nullptr;
    owner_count_ = 0;
}

bool CollideGrid::matches(const CollisionWorld& world) const {
    return owner_ != nullptr && owner_count_ == world.triangle_count() &&
           owner_ == world.triangles().data();
}

bool CollideGrid::build(const CollisionWorld& world, double cell_size, std::string* error) {
    clear();
    if (error != nullptr) {
        error->clear();
    }
    if (world.triangle_count() == 0) {
        set_first_error(error, "collide_grid: collision world has no static triangles");
        return false;
    }
    if (!std::isfinite(cell_size) || !(cell_size > 0.0)) {
        set_first_error(error, "collide_grid: cell_size must be finite and > 0");
        return false;
    }
    const Aabb& bounds = world.bounds();
    if (!bounds.valid() || !finite(bounds.lo) || !finite(bounds.hi)) {
        set_first_error(error, "collide_grid: world bounds are not finite");
        return false;
    }

    const Vec3d span = bounds.size();
    auto cells_on = [cell_size](double extent) {
        const double scaled = extent / cell_size;
        if (!(scaled > 0.0)) {
            return std::size_t{1};
        }
        if (scaled > static_cast<double>(kMaxCells)) {
            return kMaxCells + 1; // saturate: the cap check below refuses it
        }
        return std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(scaled)));
    };
    dims_.x = cells_on(span.x);
    dims_.y = cells_on(span.y);
    dims_.z = cells_on(span.z);
    if (dims_.x > kMaxCells || dims_.y > kMaxCells / dims_.x ||
        dims_.z > kMaxCells / (dims_.x * dims_.y)) {
        set_first_error(error, "collide_grid: cell_size too fine: grid exceeds the cell cap");
        dims_ = Dimensions{};
        return false;
    }
    cells_ = dims_.x * dims_.y * dims_.z;
    if (cells_ == 0 || cells_ > kMaxCells) {
        set_first_error(error, "collide_grid: cell_size too fine: grid exceeds the cell cap");
        dims_ = Dimensions{};
        cells_ = 0;
        return false;
    }
    // `axis_cell` needs the grid frame for the binning pass below.
    origin_ = bounds.lo;
    cell_size_ = cell_size;

    // Pass 1: how many cells every triangle AABB overlaps, and the total number
    // of references (one per triangle/cell pair).
    std::vector<std::uint32_t> counts(cells_, 0);
    std::size_t total = 0;
    for (const Triangle& triangle : world.triangles()) {
        const std::size_t x0 = axis_cell(triangle.bounds.lo.x - kBinSlack, 0);
        const std::size_t x1 = axis_cell(triangle.bounds.hi.x + kBinSlack, 0);
        const std::size_t y0 = axis_cell(triangle.bounds.lo.y - kBinSlack, 1);
        const std::size_t y1 = axis_cell(triangle.bounds.hi.y + kBinSlack, 1);
        const std::size_t z0 = axis_cell(triangle.bounds.lo.z - kBinSlack, 2);
        const std::size_t z1 = axis_cell(triangle.bounds.hi.z + kBinSlack, 2);
        const std::size_t span_cells =
            (x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1);
        if (total + span_cells > kMaxReferences) {
            set_first_error(error,
                            "collide_grid: cell_size too small: triangle references exceed the cap");
            clear();
            return false;
        }
        total += span_cells;
        for (std::size_t z = z0; z <= z1; ++z) {
            for (std::size_t y = y0; y <= y1; ++y) {
                for (std::size_t x = x0; x <= x1; ++x) {
                    ++counts[cell_index(x, y, z)];
                }
            }
        }
    }

    // CSR layout; the per-cell tails become the fill cursors.
    cell_begin_.assign(cells_ + 1, 0);
    for (std::size_t c = 0; c < cells_; ++c) {
        cell_begin_[c + 1] = cell_begin_[c] + counts[c];
    }
    entries_.resize(total);
    std::vector<std::uint32_t> cursor(cell_begin_.begin(), cell_begin_.end() - 1);
    for (std::size_t index = 0; index < world.triangles().size(); ++index) {
        const Triangle& triangle = world.triangles()[index];
        const std::size_t x0 = axis_cell(triangle.bounds.lo.x - kBinSlack, 0);
        const std::size_t x1 = axis_cell(triangle.bounds.hi.x + kBinSlack, 0);
        const std::size_t y0 = axis_cell(triangle.bounds.lo.y - kBinSlack, 1);
        const std::size_t y1 = axis_cell(triangle.bounds.hi.y + kBinSlack, 1);
        const std::size_t z0 = axis_cell(triangle.bounds.lo.z - kBinSlack, 2);
        const std::size_t z1 = axis_cell(triangle.bounds.hi.z + kBinSlack, 2);
        for (std::size_t z = z0; z <= z1; ++z) {
            for (std::size_t y = y0; y <= y1; ++y) {
                for (std::size_t x = x0; x <= x1; ++x) {
                    const std::size_t cell = cell_index(x, y, z);
                    entries_[cursor[cell]++] = static_cast<std::uint32_t>(index);
                }
            }
        }
    }

    origin_ = bounds.lo;
    cell_size_ = cell_size;
    owner_ = world.triangles().data();
    owner_count_ = world.triangles().size();
    return true;
}

template <typename Visit>
void CollideGrid::walk_cells(Vec3d origin, Vec3d d, double limit, Visit&& visit) const {
    if (cells_ == 0) {
        return;
    }

    // Clip the ray to the grid AABB with the same slab test the narrow phase
    // uses, so a ray that misses the world never walks a cell.
    const Aabb box{origin_, origin_ + Vec3d{static_cast<double>(dims_.x) * cell_size_,
                                            static_cast<double>(dims_.y) * cell_size_,
                                            static_cast<double>(dims_.z) * cell_size_}};
    double t_enter = 0.0;
    double t_exit = limit;
    for (int i = 0; i < 3; ++i) {
        const double o = axis_of(origin, i);
        const double dir = axis_of(d, i);
        const double lo = axis_of(box.lo, i);
        const double hi = axis_of(box.hi, i);
        if (std::fabs(dir) < 1e-300) {
            if (o < lo || o > hi) {
                return;
            }
            continue;
        }
        const double inv = 1.0 / dir;
        double ta = (lo - o) * inv;
        double tb = (hi - o) * inv;
        if (ta > tb) {
            std::swap(ta, tb);
        }
        if (ta > t_enter) {
            t_enter = ta;
        }
        if (tb < t_exit) {
            t_exit = tb;
        }
        if (t_enter > t_exit) {
            return;
        }
    }

    const Vec3d entry = origin + d * t_enter;
    std::size_t index[3];
    long long step[3];
    double t_next[3];
    double t_delta[3];
    const double lo[3] = {origin_.x, origin_.y, origin_.z};
    const std::size_t count[3] = {dims_.x, dims_.y, dims_.z};
    const double o[3] = {origin.x, origin.y, origin.z};
    const double dir[3] = {d.x, d.y, d.z};
    for (int i = 0; i < 3; ++i) {
        index[i] = axis_cell(axis_of(entry, i), i);
        if (dir[i] > 0.0) {
            step[i] = 1;
            t_next[i] = (lo[i] + static_cast<double>(index[i] + 1) * cell_size_ - o[i]) / dir[i];
            t_delta[i] = cell_size_ / dir[i];
        } else if (dir[i] < 0.0) {
            step[i] = -1;
            t_next[i] = (lo[i] + static_cast<double>(index[i]) * cell_size_ - o[i]) / dir[i];
            t_delta[i] = -cell_size_ / dir[i];
        } else {
            step[i] = 0;
            t_next[i] = std::numeric_limits<double>::infinity();
            t_delta[i] = std::numeric_limits<double>::infinity();
        }
    }

    const std::size_t max_steps = dims_.x + dims_.y + dims_.z + 8;
    for (std::size_t guard = 0; guard < max_steps; ++guard) {
        const std::size_t cell = cell_index(index[0], index[1], index[2]);
        // The visitor returns the distance the walk may still cover: the
        // current best hit for a query, a constant for a candidate scan.  A
        // cell boundary at exactly that distance is still stepped into, so a
        // triangle at exactly `best` is never skipped (the tie rule needs it).
        const double reach = visit(cell);
        // Advance along the axis whose next plane is nearest; ties go to x then
        // y.  Visiting an extra cell is harmless (duplicates are dropped or
        // re-tested identically), skipping one is not.
        int axis = 0;
        if (!(t_next[0] <= t_next[1])) {
            axis = 1;
        }
        if (!(t_next[axis] <= t_next[2])) {
            axis = 2;
        }
        if (!(t_next[axis] <= reach) || !(t_next[axis] <= t_exit)) {
            break;
        }
        if (step[axis] == 0) {
            break;
        }
        if (step[axis] > 0) {
            if (index[axis] + 1 >= count[axis]) {
                break;
            }
            ++index[axis];
        } else {
            if (index[axis] == 0) {
                break;
            }
            --index[axis];
        }
        t_next[axis] += t_delta[axis];
    }
}

std::size_t CollideGrid::gather(const CollisionWorld& world, Vec3d origin, Vec3d d, double limit,
                                const CollideFilter& filter,
                                std::vector<std::uint32_t>& out) const {
    out.clear();
    walk_cells(origin, d, limit, [&](std::size_t cell) {
        for (std::uint32_t e = cell_begin_[cell]; e < cell_begin_[cell + 1]; ++e) {
            out.push_back(entries_[e]);
        }
        return limit; // scan the whole segment
    });

    // Deduplicate, then apply the structural part of the filter.  The result
    // stays sorted by triangle index, so the report is independent of the cell
    // traversal order.
    std::sort(out.begin(), out.end());
    const auto& triangles = world.triangles();
    std::size_t kept = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i != 0 && out[i] == out[i - 1]) {
            continue;
        }
        const std::uint32_t index_value = out[i];
        const Triangle& triangle = triangles[index_value];
        if (!structural_allows(world, triangle, d, filter)) {
            continue;
        }
        out[kept++] = index_value;
    }
    out.resize(kept);
    return kept;
}

std::optional<RayHit> CollideGrid::raycast(const CollisionWorld& world, Vec3d origin,
                                           Vec3d direction, const CollideFilter& filter,
                                           double max_distance) const {
    // A grid speaks only for the exact static world it was built from.  A
    // dynamic set (moving doors) and a rebuilt world are answered by the
    // reference path: correct, just not accelerated.
    if (!matches(world) || world.dynamic_triangle_count() != 0) {
        return world.raycast(origin, direction, max_distance, filter);
    }
    PreparedRay query;
    if (!prepare_ray(origin, direction, max_distance, query)) {
        return std::nullopt;
    }

    // Narrow phase with the clauses, epsilons and tie rule of
    // `CollisionWorld::raycast`.  The cells are walked in ray order, which is
    // not triangle order, so the tie rule is stated explicitly: on an exact
    // `t` tie the lowest triangle index wins -- the same triangle the
    // brute-force loop keeps as "first in ascending index order".
    const auto& triangles = world.triangles();
    double best = query.limit;
    const Triangle* best_triangle = nullptr;
    std::uint32_t best_index = 0;
    walk_cells(origin, query.d, query.limit, [&](std::size_t cell) {
        for (std::uint32_t e = cell_begin_[cell]; e < cell_begin_[cell + 1]; ++e) {
            const std::uint32_t index = entries_[e];
            const Triangle& triangle = triangles[index];
            if (!structural_allows(world, triangle, query.d, filter)) {
                continue;
            }
            if (!ray_aabb(origin, query.d, triangle.bounds, best)) {
                continue;
            }
            const std::optional<double> t = ray_triangle(origin, query.d, triangle, best);
            if (!t.has_value() || *t > best) {
                continue;
            }
            if (best_triangle != nullptr && !(*t < best) && index >= best_index) {
                continue; // exact tie: keep the lower triangle index
            }
            best = *t;
            best_triangle = &triangle;
            best_index = index;
        }
        return best; // no cell boundary farther than the hit is visited
    });
    if (best_triangle == nullptr) {
        return std::nullopt;
    }

    RayHit hit;
    hit.t = best;
    hit.point = origin + query.d * best;
    hit.triangle = best_triangle;
    hit.triangle_index = best_index;
    hit.sector = best_triangle->sector;
    hit.dynamic = false;
    const Vec3d reference = surface_normal(*best_triangle);
    const double facing = dot(reference, query.d);
    hit.front_face = facing < 0.0;
    hit.normal = facing > 0.0 ? -reference : reference;
    return hit;
}

std::size_t CollideGrid::candidates_for_ray(const CollisionWorld& world, Vec3d origin,
                                            Vec3d direction, const CollideFilter& filter,
                                            double max_distance) const {
    if (!matches(world) || world.dynamic_triangle_count() != 0) {
        return 0;
    }
    PreparedRay query;
    if (!prepare_ray(origin, direction, max_distance, query)) {
        return 0;
    }
    static thread_local std::vector<std::uint32_t> candidates;
    return gather(world, origin, query.d, query.limit, filter, candidates);
}

} // namespace gm
