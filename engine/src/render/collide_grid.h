#pragma once

// Broad-phase acceleration for `CollisionWorld::raycast`: a uniform 3D grid of
// the static triangles, traversed by a 3D-DDA ray walk, with a narrow phase that
// reproduces the clauses, epsilons and tie rule of `CollisionWorld::raycast`
// so the returned `RayHit` is bit-for-bit identical (docs/COLLIDE_GRID.md).
//
// Why a grid and not a tree: the level geometry is a static triangle soup in a
// room-scale world, the build is one pass over the triangles, and the ray walk
// touches only the cells the ray crosses.  The grid is an index over
// `CollisionWorld::triangles()`; it never copies geometry, never mutates the
// world and holds no state that a query changes, so a grid can be shared by
// threads as long as the world outlives it.
//
// Parity rule (docs/COLLIDE_GRID.md §2): the brute-force `raycast` visits the
// static triangles in ascending index order and keeps the first triangle on an
// exact `t` tie.  The grid walks the cells in ray order and runs the identical
// slab/`ray_triangle` sequence; on an exact `t` tie it keeps the *lowest*
// triangle index, which is the same triangle the ascending-index loop keeps.
// The walk stops at the current best hit, but the cell boundary at exactly
// `best` is still stepped into, so a tied triangle is never skipped.  Every
// accepted hit, its `t`, `point`, `normal`, `front_face`, `triangle` pointer and
// `triangle_index` are therefore equal exactly (not to a tolerance).  Only the
// set of triangles *examined* differs, and it is always a superset of the
// triangles the brute-force path can accept.
//
// Scope and limits:
//   * static triangles only -- a world carrying a dynamic set (`set_dynamic`)
//     is answered by `CollisionWorld::raycast` (correct, not accelerated);
//   * the grid is tied to the `CollisionWorld` it was built from; if the world
//     changed (triangle count or storage moved, dynamic set present) the query
//     falls back to the brute-force path instead of returning a stale hit;
//   * a triangle is entered in every cell its (slightly grown) AABB overlaps,
//     so a large floor slab is stored in many cells.  `references()` reports
//     the total, `memory_bytes()` the backing arrays; `build` refuses a grid
//     that would exceed the fixed cell/reference caps;
//   * no dynamic rebalancing: moving geometry needs a rebuild (or a separate
//     `CollideGrid` per pose);
//   * a query uses a per-thread scratch buffer for its candidate list (grown on
//     first use, reused afterwards), so the steady state allocates nothing; the
//     caller-visible semantics do not depend on it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "render/collide.h"

namespace gm {

class CollideGrid {
public:
    struct Dimensions {
        std::size_t x = 0;
        std::size_t y = 0;
        std::size_t z = 0;
    };

    /// Index every static triangle of `world` in a uniform grid of `cell_size`
    /// world units (must be finite and > 0).  Returns false and fills `error`
    /// with one line for an empty world, a non-finite world AABB, a bad cell
    /// size and a grid beyond the cell/reference caps; a failed build leaves an
    /// empty grid (`cells() == 0`).
    bool build(const CollisionWorld& world, double cell_size, std::string* error = nullptr);

    void clear();

    bool empty() const { return cells_ == 0; }

    /// True when `world` is still the one the grid was built from (same
    /// triangle storage and count).  A mismatch is not an error here: queries
    /// fall back to `CollisionWorld::raycast`.
    bool matches(const CollisionWorld& world) const;

    /// Nearest hit along `direction`, identical to
    /// `world.raycast(origin, direction, max_distance, filter)`.  Delegates to
    /// that function when the grid cannot speak for the world (mismatch, or a
    /// non-empty dynamic set).  `max_distance <= 0` / non-finite inputs are a
    /// miss, exactly as in `CollisionWorld`.
    std::optional<RayHit> raycast(const CollisionWorld& world, Vec3d origin, Vec3d direction,
                                  const CollideFilter& filter = {},
                                  double max_distance = kCollideInfinity) const;

    /// Number of triangles the broad phase hands to the narrow phase for this
    /// query: unique triangles of the cells the ray crosses on its whole
    /// `[0, max_distance]` segment that also pass the filter (suppression,
    /// surface flags, sector list and `backfaces`).  It is an upper bound of
    /// what `raycast` examines, because `raycast` stops walking once a hit is
    /// closer than the next cell boundary.  Returns 0 for an invalid query and
    /// for a world the grid does not match.
    std::size_t candidates_for_ray(const CollisionWorld& world, Vec3d origin, Vec3d direction,
                                   const CollideFilter& filter = {},
                                   double max_distance = kCollideInfinity) const;

    /// Allocated cell count (`dimensions().x * y * z`), 0 before a build.
    std::size_t cells() const { return cells_; }
    Dimensions dimensions() const { return dims_; }
    /// Cell size in world units as passed to `build` (0 before a build).
    double cell_size() const { return cell_size_; }
    /// Static triangles indexed at build time.
    std::size_t triangle_count() const { return owner_count_; }
    /// Grid entries: sum over triangles of the cells its AABB touches.
    std::size_t references() const { return entries_.size(); }
    /// Backing arrays of the index (`cell_begin_` + `entries_`), bytes.
    std::size_t memory_bytes() const {
        return cell_begin_.capacity() * sizeof(std::uint32_t) +
               entries_.capacity() * sizeof(std::uint32_t);
    }

private:
    static constexpr std::size_t kMaxCells = 1u << 24;    ///< 16 777 216 cells
    static constexpr std::size_t kMaxReferences = 1u << 25; ///< 33 554 432 entries

    /// Cells whose AABB is `[origin_ + i * cell_size_, ...]`, x fastest.
    std::size_t cell_index(std::size_t x, std::size_t y, std::size_t z) const {
        return x + dims_.x * (y + dims_.y * z);
    }

    /// `(coordinate - origin_) / cell_size_`, clamped to the grid on `axis`.
    std::size_t axis_cell(double value, int axis) const;

    /// Clip the ray to the grid and walk the cells it crosses, calling
    /// `visit(cell)` once per cell.  The callable returns the distance the walk
    /// may still reach (the current best hit); the walk stops as soon as the
    /// next cell boundary is farther than that, so a hit shortens the walk but
    /// a candidate at exactly the same `t` is still reached.
    template <typename Visit>
    void walk_cells(Vec3d origin, Vec3d d, double limit, Visit&& visit) const;

    /// Clip the ray to the grid and walk the cells it crosses, appending the
    /// unique triangle indices that survive the structural filter to `out`
    /// (ascending, no duplicates).  Returns the number appended.  `d` must be
    /// unit length and `limit` positive/finite.
    std::size_t gather(const CollisionWorld& world, Vec3d origin, Vec3d d, double limit,
                       const CollideFilter& filter, std::vector<std::uint32_t>& out) const;

    std::size_t cells_ = 0;
    Dimensions dims_;
    double cell_size_ = 0.0;
    Vec3d origin_;
    /// CSR index: cell `c` owns `entries_[cell_begin_[c] .. cell_begin_[c + 1])`.
    std::vector<std::uint32_t> cell_begin_;
    std::vector<std::uint32_t> entries_;
    /// Triangle storage the grid was built from (`triangles().data()`); a
    /// rebuilt world with the same count but a new buffer is detected here.
    const Triangle* owner_ = nullptr;
    std::size_t owner_count_ = 0;
};

} // namespace gm
