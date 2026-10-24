#pragma once

// Line of sight (LOS) for a hitscan-style hit test: "is there level geometry
// between the eye and a spherical target?" (`M6.2.5` slice 2).
//
// This is a *new* query layer over `CollisionWorld`, not a port of the original
// engine's visibility code.  The original hitscan target scan
// (`0x477f90`/`0x4779f0`, docs/WEAPONS_CPP.md §6) is a cone/range test and does
// not trace the world for an obstruction; the only previous LOS check in the
// C++ engine is `Ai::line_of_sight` (a private point-to-point ray with a skin
// margin, docs/AI_CPP.md).  This module formalises the target-aware form:
//
//   * one *central* ray from `eye` to `target.center`; no cone, no edge or
//     tangent rays.  The target's size enters as a documented margin: a hit
//     blocks only when it is strictly closer than `distance - radius`
//     (docs/LINE_OF_SIGHT.md §1);
//   * because the ray is limited to that distance, geometry that belongs to
//     the target itself (anything inside its bounding sphere) is never cast
//     against, and neither is anything behind it.  A target standing right
//     against a wall is therefore visible, which is the intended semantics of
//     "nothing between me and the target";
//   * the filter (`CollideFilter`) is the caller's: "only floors", a sector
//     list, one-sided clipping and so on all apply unchanged, so a filter that
//     removes the blocking surface turns the answer into `visible`;
//   * the query is pure: no allocation in the single form, no state in
//     `CollisionWorld`, results are values plus a pointer/index into the world;
//   * the ray and the collision math are `double` and deterministic
//     (`CollisionWorld::raycast` order, docs/COLLIDE.md §8).
//
// Errors are refusals, not "visible": a non-finite eye/center, a negative or
// non-finite radius, or a world with no static triangles
// (`triangle_count() == 0`) returns `visible = false` with a one-line `error`.
// The alternative (an empty world means "nothing in between") would silently
// report every target visible on a level that failed to build, which is the
// dangerous direction for a gameplay check.  `line_of_sight_all` keeps that
// per-target: one bad target does not stop the batch, the first message is
// copied to the batch `error`.
//
// Not wired into `resolve_hitscan_impact`/`fire()`: the caller decides when a
// target is relevant and asks this module; see docs/LINE_OF_SIGHT.md §4 for
// the limits (no cone/spread, no transparent materials, no broad phase of its
// own beyond `raycast`, no dynamic-target occlusion).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "render/collide.h" // Vec3d, Triangle, CollideFilter, CollisionWorld

namespace gm {

/// `LosResult::blocker` when level geometry stopped the ray.
inline constexpr std::uint32_t kLosWorldBlocker = 0;
/// `LosResult::blocker` when the target is visible (nothing blocked it).
inline constexpr std::uint32_t kLosNoBlocker = 0xffffffffu;

/// One target: a bounding sphere in the engine world frame.  `id` is opaque to
/// this module (a thing id, a player slot, ...) and is echoed in `LosResult::id`
/// so a batch answer can be matched to its request without relying on order.
struct LosTarget {
    std::uint32_t id = 0;
    Vec3d center{};
    double radius = 0.0; ///< bounding-sphere radius, >= 0 (world units)
};

/// Answer for one target.
struct LosResult {
    bool visible = false;
    /// Euclidean `|target.center - eye|`; always filled for a valid request,
    /// including when the answer is "blocked".
    double distance = 0.0;
    /// The blocking triangle, or nullptr when `visible` (or on a refused
    /// request).  Valid while the `CollisionWorld` is alive.
    const Triangle* triangle = nullptr;
    /// Index of the blocking triangle: into `triangles()` for a static hit and
    /// into `dynamic_triangles()` when `dynamic` is true (same convention as
    /// `RayHit::triangle_index`, docs/DOOR_COLLISION.md).
    std::size_t triangle_index = 0;
    /// `kLosWorldBlocker` when level geometry blocked the ray,
    /// `kLosNoBlocker` otherwise.  Target ids are reserved for a future
    /// target-vs-target pass; this module never sets one (docs §4).
    std::uint32_t blocker = kLosNoBlocker;
    /// Echo of `LosTarget::id`.
    std::uint32_t id = 0;
    /// The blocking triangle came from the dynamic set (`set_dynamic`).
    bool dynamic = false;
    /// Ray parameter of the blocking hit (`raycast().t`): strictly less than
    /// `distance - radius` whenever `visible == false`.  0 when nothing hit.
    double blocker_distance = 0.0;
};

/// Trace one central ray from `eye` to `target.center` and report whether any
/// filtered triangle lies strictly closer to the eye than `distance - radius`.
///
/// A target centred on the eye (`distance == 0`) or one whose radius reaches
/// the eye (`distance - radius <= 0`) is visible by definition: no surface can
/// be between the two points.
///
/// Returns a refused result (`visible = false`, `triangle == nullptr`, `error`
/// set when the pointer is non-null) for:
///   * a non-finite `eye` or `target.center`;
///   * `target.radius < 0`, NaN or infinite (a malformed bounding sphere);
///   * `world.triangle_count() == 0` (no static level geometry to test
///     against; the dynamic set alone is not accepted, see the header comment).
///
/// The animation/rest-pose suppression of `CollisionWorld::set_dynamic` is
/// honoured exactly as by `raycast`.  `error` is cleared on success.
LosResult line_of_sight(const CollisionWorld& world, Vec3d eye,
                        const LosTarget& target, const CollideFilter& filter = {},
                        std::string* error = nullptr);

/// Batch form: `line_of_sight` for every target, in input order, so
/// `result[i]` answers `targets[i]` (`LosResult::id` makes that explicit).
///
/// No target is tested against another: each result is bit-identical to the
/// single-target call with the same arguments, which keeps the batch and the
/// single form interchangeable and avoids an order-dependent answer.
/// A refused target stays in the vector (invisible, `distance == 0`) instead of
/// aborting the batch; `error` receives the first refusal message, or is
/// cleared when every request was valid.
std::vector<LosResult> line_of_sight_all(const CollisionWorld& world, Vec3d eye,
                                         std::span<const LosTarget> targets,
                                         const CollideFilter& filter = {},
                                         std::string* error = nullptr);

} // namespace gm
