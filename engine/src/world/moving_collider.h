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
//   * 357 doors carry a rotation operand (`+0x48`, `A1..A3`).  `0x44dfc0`
//     picks the axis by the priority z > x > y, uses the unit vector
//     -x / +y / -z and the angle `(A_i / 65535) * phase` (the record keeps
//     the operand already divided by 65535); `0x434887`
//     builds the basis as a rotation by `-angle` about that axis.  Combining
//     that with `origin = center - basis^T * size` maps a rest world point
//     `p` to `center + R(angle) * (p - closed)`.  This module rotates every
//     vertex and both normals by `R(fraction * max_angle)` about `axis`
//     through `pivot == Door::closed` (`door_swing`), and slides the pivot by
//     `(open - closed) * fraction`; `f = 0` is exactly the rest pose.
//
// Conventions are the same as `render/collide.h`:
//   * double precision, world units, `world = origin + basis * local`;
//   * a translation recomputes `normal` from the moved winding and carries
//     `data_normal` over; a rotation turns both by the same `R(angle)` and
//     keeps their origin (the winding normal is not recomputed).  Both are
//     re-normalised, `plane_offset = dot(normal, a)`, `centroid` and `bounds`
//     follow the moved vertices.  `surface_of`/`surface_normal` see the same
//     two normals as they do on a `CollisionWorld` triangle;
//   * `raycast`/`sphere_cast` are two-sided by default and report the normal
//     against the incoming query, exactly like `CollisionWorld`;
//   * the query filters (`floors`/`ceilings`/`walls`/`backfaces`/sectors) are
//     the same `CollideFilter`, applied by `surface_of(data_normal)`;
//   * queries allocate nothing and return pointers into `triangles()`.
//
// Limitations (docs/MOVING_COLLIDERS.md §6): no object-vs-object collision, no
// penetration resolve/push-out, and no integration into `CollisionWorld` from
// this module itself (the runtime does it, docs/DOOR_COLLISION.md).

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
/// `Rotating`: a swing door; the panel turns about `DoorSwing::pivot` by
/// `fraction * DoorSwing::max_angle` and its pivot slides by
/// `MovingPanel::travel * fraction` (357 shipped doors, 113 of them moving).
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
    /// `open - closed`, world units.  For a swing leaf this is the slide of
    /// the rotation pivot (`center(f) = pivot + travel * f`), not the vertex
    /// displacement.  Zero for a pure lift panel that is moved only by
    /// `update(..., lift_offset)`.
    Vec3d travel{};
};

/// The rotation of a swing leaf, derived from the `DOOR` record
/// (docs/DOOR_FIELDS.md §1.2, `0x44dfc0` + `0x434887`).
///
/// * `pivot` is `Door::closed` (record `+0x00`, already folded with the size
///   vector `+0x30`), the point `0x44dfc0` interpolates into `center` and
///   `0x434887` rotates about.
/// * `axis` is the unit vector the engine picks from `A1..A3` (`+0x48`):
///   `z != 0` -> `(0, 0, -1)`, else `x != 0` -> `(-1, 0, 0)`, else
///   `y != 0` -> `(0, 1, 0)`.
/// * `max_angle` is the rotation at `open_fraction == 1`, in radians:
///   `A_i * 65535` (`door.axis[i]` is the record value already divided by
///   65535, and the geometry phase spans `0..0xffff`).
struct DoorSwing {
    Vec3d pivot{};
    Vec3d axis{};
    double max_angle = 0.0;
};

/// Swing data of `door`: `DoorAxis::Rotating` doors get the pivot, unit axis
/// and full angle; a sliding door yields a zero struct (pivot still
/// `Door::closed`, `max_angle == 0`).
DoorSwing door_swing(const Door& door);

/// A re-posed set of panels plus ray/sphere queries over the current pose.
class MovingCollider {
public:
    /// Replace the panels and the movement axis.  Re-poses everything at
    /// `open_fraction = 0`, `lift_offset = 0`.  `swing` is required (and
    /// validated) for `DoorAxis::Rotating`; sliding modes ignore it.  An empty
    /// set, an empty panel, a non-finite travel/vertex, a non-unit swing axis,
    /// a zero swing angle or a non-finite pivot/angle leaves the collider not
    /// ready (`ready() == false`, `error()` explains why).
    void set_panels(std::vector<MovingPanel> panels, DoorAxis axis,
                    const DoorSwing& swing = {});

    /// Re-pose the panels.  A sliding door moves by
    /// `travel_along_axis * open_fraction`, a `Travel` panel by
    /// `travel * open_fraction`.  A `Rotating` panel moves its pivot by
    /// `travel * open_fraction` and turns every vertex and normal by
    /// `open_fraction * max_angle` about the swing axis through the pivot
    /// (the engine's `0x44dfc0` lerp + `0x434887` basis).  Every panel is
    /// additionally moved by `lift_offset` along +Z.  `open_fraction` is
    /// clamped to `[0, 1]` like the engine phase (`0x44f335..0x44f349`);
    /// `lift_offset` is signed, a cabin can go down.
    ///
    /// At `open_fraction == 0` with no lift the pose is the rest pose
    /// bit-for-bit (the engine transform is the identity there), so a caller
    /// can publish it without perturbing the static triangles.
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
    /// Swing pivot/axis/angle; zero (and `max_angle == 0`) for a sliding door.
    const DoorSwing& swing() const { return swing_; }
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
    DoorSwing swing_;
    double open_fraction_ = 0.0;
    double lift_offset_ = 0.0;
    bool valid_ = false;
    bool update_ok_ = false;
    std::string error_;
};

} // namespace gm
