#pragma once

// Kinematic colliders for level objects that move: door leaves and lift cabins.
//
// `CollisionWorld` is built once per level from the rest pose in the `.sbw`
// file and never changes.  A door leaf, however, is translated by the engine
// every tick (`0x44dfc0` -> `0x434887`, docs/DOOR_FIELDS.md §1.2): its
// triangles stay the same, only their placement changes.  This module keeps a
// copy of one or more panels at rest and re-poses them from the door state, so
// a caller can cast rays/spheres against the *current* shape of a door without
// rebuilding the whole world.
//
// Model (proven by docs/DOOR_FIELDS.md §1.2 and confirmed on the shipped data):
//   * the `.sbw` object of a door is stored at its closed position: for all
//     1779 shipped doors the object header origin equals `Door::closed`
//     (`D1..D3`, record `+0x00`, already folded with the size vector);
//   * the engine interpolates `center = closed + (open - closed) * phase` and
//     writes `origin = center - basis^T * size`; for the 1422 shipped doors
//     whose three angle operands are zero the basis is the identity, so the
//     leaf is a pure translation by `(open - closed) * fraction`;
//   * `Door::open - Door::closed` is therefore the full travel of a sliding
//     door (`door_travel`); 105 of the 1421 moving sliding doors have a
//     diagonal travel, hence the `DoorAxis::Travel` mode;
//   * 357 doors carry a rotation operand.  A swing leaf rotates, and a pure
//     translation is not its motion; `door_axis` reports `DoorAxis::Rotating`
//     and `set_panels` refuses that mode instead of pretending.
//
// Conventions are the same as `render/collide.h`:
//   * double precision, world units, `world = origin + basis * local`;
//   * `normal` is recomputed from the moved winding, `data_normal` is carried
//     over (a translation does not rotate it) and re-normalised,
//     `plane_offset = dot(normal, a)`, `centroid` and `bounds` follow the
//     moved vertices.  `surface_of`/`surface_normal` see the same two normals
//     as they do on a `CollisionWorld` triangle;
//   * `raycast`/`sphere_cast` are two-sided by default and report the normal
//     against the incoming query, exactly like `CollisionWorld`;
//   * the query filters (`floors`/`ceilings`/`walls`/`backfaces`/sectors) are
//     the same `CollideFilter`, applied by `surface_of(data_normal)`;
//   * queries allocate nothing and return pointers into `triangles()`.
//
// Limitations (docs/MOVING_COLLIDERS.md §6): no rotation, no object-vs-object
// collision, no penetration resolve/push-out, and no integration into
// `CollisionWorld` or the runtime yet.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "render/collide.h"
#include "world/doors.h"

namespace gm {

/// How a panel is moved by `update(open_fraction)`.
///
/// `X`/`Y`/`Z`: the door was authored along one world axis; the component of
/// `MovingPanel::travel` on that axis is used.  `Travel`: diagonally authored
/// door, the whole `travel` vector is used (105 shipped sliding doors).
/// `Rotating`: a swing door; not modelled, `set_panels` refuses it.
enum class DoorAxis : std::uint8_t {
    X = 0,
    Y = 1,
    Z = 2,
    Travel = 3,
    Rotating = 4,
};

const char* door_axis_name(DoorAxis axis);

/// Unit vector of `X`/`Y`/`Z`; the zero vector for `Travel`/`Rotating`.
Vec3d door_axis_unit(DoorAxis axis);

/// Full displacement of a door at `open_fraction == 1`:
/// `Door::open - Door::closed` (record `+0x18` minus `+0x00`, both already
/// folded with `D7..D9`).  This is the exact target of the engine lerp
/// `0x44dfc0`; see docs/DOOR_FIELDS.md §1.2.
Vec3d door_travel(const Door& door);

/// Axis of `door_travel(door)`: `Rotating` when any of the three angle
/// operands is non-zero, `X`/`Y`/`Z` when exactly one travel component is
/// non-zero, `Travel` otherwise (diagonal or static).
DoorAxis door_axis(const Door& door);

/// One moving leaf, in the world frame at rest (`open_fraction == 0`).
struct MovingPanel {
    /// Triangles of the door object as built by `CollisionWorld` (same fan,
    /// winding, `data_normal`, sector/object/texture fields).
    std::vector<Triangle> rest;
    /// `open - closed`, world units.  Zero for a pure lift panel that is moved
    /// only by `update(..., lift_offset)`.
    Vec3d travel{};
};

/// A re-posed set of panels plus ray/sphere queries over the current pose.
class MovingCollider {
public:
    /// Replace the panels and the movement axis.  Re-poses everything at
    /// `open_fraction = 0`, `lift_offset = 0`.  An empty set, an empty panel,
    /// a non-finite travel/vertex or `DoorAxis::Rotating` leaves the collider
    /// not ready (`ready() == false`, `error()` explains why).
    void set_panels(std::vector<MovingPanel> panels, DoorAxis axis);

    /// Re-pose the panels.  A sliding door moves by
    /// `travel_along_axis * open_fraction`, a `Travel` panel by
    /// `travel * open_fraction`, and every panel additionally by `lift_offset`
    /// along +Z.  `open_fraction` is clamped to `[0, 1]` like the engine phase
    /// (`0x44f335..0x44f349`); `lift_offset` is signed, a cabin can go down.
    ///
    /// A NaN/inf argument is refused: the previous pose and `triangles()` stay
    /// untouched, `last_update_ok()` becomes false and `error()` says which
    /// argument was rejected.  The next finite call restores the flag.
    void update(double open_fraction, double lift_offset = 0.0);

    bool ready() const { return valid_ && !triangles_.empty(); }
    /// False when the last `set_panels`/`update` was refused.
    bool last_update_ok() const { return update_ok_; }
    const std::string& error() const { return error_; }

    DoorAxis axis() const { return axis_; }
    std::size_t panel_count() const { return panels_.size(); }
    /// Current pose, flattened panel by panel, in build order.
    const std::vector<Triangle>& triangles() const { return triangles_; }
    std::size_t triangle_count() const { return triangles_.size(); }
    /// Panel index of `triangle_index` (0-based into `triangles()`), or
    /// `panel_count()` for an out-of-range index.
    std::size_t panel_of(std::uint32_t triangle_index) const;
    const Aabb& bounds() const { return bounds_; }
    double open_fraction() const { return open_fraction_; }
    double lift_offset() const { return lift_offset_; }

    /// Nearest triangle along `direction` (need not be unit).  Same input
    /// guards and `t >= kCollideRayEps` rule as `CollisionWorld::raycast`.
    std::optional<RayHit> raycast(const Vec3d& origin, const Vec3d& direction,
                                  double max_t = kCollideInfinity,
                                  const CollideFilter& filter = {}) const;

    /// Sweep a sphere of `radius` from `origin` along `direction`; face, edge
    /// and vertex contacts, an initial overlap at `t = 0` with a push-out
    /// normal.  Same guards as `CollisionWorld::sphere_cast`.
    std::optional<SphereHit> sphere_cast(const Vec3d& origin, const Vec3d& direction,
                                         double radius, double max_t = kCollideInfinity,
                                         const CollideFilter& filter = {}) const;

private:
    void clear_pose();
    void pose(double fraction, double lift_offset);
    void rebuild_bounds();

    std::vector<MovingPanel> panels_;
    std::vector<Triangle> triangles_;
    /// Panel index per `triangles_` entry; index-aligned with `triangles_`.
    std::vector<std::uint32_t> panel_of_;
    Aabb bounds_;
    DoorAxis axis_ = DoorAxis::Travel;
    double open_fraction_ = 0.0;
    double lift_offset_ = 0.0;
    bool valid_ = false;
    bool update_ok_ = false;
    std::string error_;
};

} // namespace gm
