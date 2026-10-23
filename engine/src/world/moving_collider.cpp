#include "world/moving_collider.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gm {
namespace {

bool coordinate_ok(Vec3d v) {
    return std::fabs(v.x) <= kCollideMaxCoordinate && std::fabs(v.y) <= kCollideMaxCoordinate &&
           std::fabs(v.z) <= kCollideMaxCoordinate;
}

bool triangle_ok(const Triangle& triangle) {
    return finite(triangle.a) && finite(triangle.b) && finite(triangle.c) &&
           coordinate_ok(triangle.a) && coordinate_ok(triangle.b) && coordinate_ok(triangle.c);
}

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

/// Slab test clipped to `[0, max_t]`, the same formulation as
/// `render/collide.cpp` (kept here because that helper is file-local).
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

/// Translation of one rest triangle plus the dependent fields.  A translation
/// preserves the winding and the area, so the geometric normal is recomputed
/// from the moved vertices (degenerate input keeps its rest normal) and the
/// authored normal is re-normalised; the plane, centroid and AABB follow.
Triangle moved_triangle(const Triangle& rest, Vec3d delta) {
    Triangle triangle = rest;
    triangle.a = rest.a + delta;
    triangle.b = rest.b + delta;
    triangle.c = rest.c + delta;

    const Vec3d face = cross(triangle.b - triangle.a, triangle.c - triangle.a);
    const double area2 = length(face);
    if (area2 > 0.0) {
        triangle.normal = face * (1.0 / area2);
    }

    const double data_length = length(triangle.data_normal);
    if (data_length > 0.0) {
        triangle.data_normal = triangle.data_normal * (1.0 / data_length);
    }

    triangle.plane_offset = dot(triangle.normal, triangle.a);
    triangle.centroid = (triangle.a + triangle.b + triangle.c) * (1.0 / 3.0);
    triangle.bounds = Aabb{};
    triangle.bounds.expand(triangle.a);
    triangle.bounds.expand(triangle.b);
    triangle.bounds.expand(triangle.c);
    return triangle;
}

/// `R(angle)` about the unit vector `axis` (Rodrigues).  `0x434887` builds the
/// basis rows as the *transpose* of this matrix (`basis = R(-angle)`), and
/// `world = origin + basis^T * local` turns into `center + R(angle) * (p -
/// closed)` once `origin = center - basis^T * size` is folded in (see
/// docs/DOOR_FIELDS.md §1.2).  `axis` is validated to be unit in
/// `set_panels`, so no normalisation happens per query.
Vec3d rotate_about(Vec3d v, Vec3d axis, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

Vec3d normalized_or(Vec3d v, Vec3d fallback) {
    const double len = length(v);
    return len > 0.0 ? v * (1.0 / len) : fallback;
}

/// One rest triangle turned by `R(angle)` about `axis` through `pivot`, with
/// the pivot slid by `center_delta` and the whole leaf lifted by `lift` along
/// +Z.  Both normals are rotated by the same matrix and re-normalised (their
/// origin is kept: the winding normal is not recomputed from the moved
/// vertices), then the plane/centroid/bounds follow the vertices.
Triangle swung_triangle(const Triangle& rest, Vec3d pivot, Vec3d center_delta, Vec3d axis,
                        double angle, double lift) {
    Triangle triangle = rest;
    const Vec3d offset{center_delta.x, center_delta.y, center_delta.z + lift};
    triangle.a = pivot + offset + rotate_about(rest.a - pivot, axis, angle);
    triangle.b = pivot + offset + rotate_about(rest.b - pivot, axis, angle);
    triangle.c = pivot + offset + rotate_about(rest.c - pivot, axis, angle);

    triangle.normal = normalized_or(rotate_about(rest.normal, axis, angle), rest.normal);
    triangle.data_normal =
        normalized_or(rotate_about(rest.data_normal, axis, angle), rest.data_normal);

    triangle.plane_offset = dot(triangle.normal, triangle.a);
    triangle.centroid = (triangle.a + triangle.b + triangle.c) * (1.0 / 3.0);
    triangle.bounds = Aabb{};
    triangle.bounds.expand(triangle.a);
    triangle.bounds.expand(triangle.b);
    triangle.bounds.expand(triangle.c);
    return triangle;
}

struct SphereContact {
    double t = 0.0;
    Vec3d point;
    Vec3d normal;
};

/// Closest approach of a swept sphere to one triangle: face, three edges and
/// three vertices.  This mirrors `sphere_vs_triangle` in `render/collide.cpp`;
/// `test_moving_collider` checks the two agree on a real panel, so the two
/// implementations cannot drift apart silently.
bool sphere_vs_triangle(const Vec3d& origin, const Vec3d& direction, double radius,
                        const Triangle& triangle, double max_t, SphereContact& out) {
    bool found = false;
    double best = max_t;

    auto consider = [&](double t, Vec3d normal, Vec3d point) {
        if (!(t >= 0.0) || t > best) {
            return;
        }
        if (!found || t < best) {
            best = t;
            out.t = t;
            out.normal = normal;
            out.point = point;
            found = true;
        }
    };

    // Already overlapping at t = 0.
    const Vec3d start_point = closest_point_on_triangle(origin, triangle);
    const Vec3d start_delta = origin - start_point;
    const double start_distance = length(start_delta);
    if (start_distance <= radius) {
        Vec3d normal;
        if (start_distance > 1e-12) {
            normal = start_delta * (1.0 / start_distance);
        } else if (dot(triangle.normal, direction) > 0.0) {
            normal = -triangle.normal;
        } else {
            normal = triangle.normal;
        }
        out = SphereContact{0.0, start_point, normal};
        return true;
    }

    const Vec3d n = triangle.normal;
    const double dir_dot_n = dot(n, direction);
    const double distance0 = dot(n, origin - triangle.a);
    double face_t = kCollideInfinity;
    if (distance0 > radius && dir_dot_n < -1e-12) {
        face_t = (radius - distance0) / dir_dot_n;
    } else if (distance0 < -radius && dir_dot_n > 1e-12) {
        face_t = (-radius - distance0) / dir_dot_n;
    }
    if (face_t >= kCollideRayEps && face_t <= best) {
        const Vec3d centre = origin + direction * face_t;
        const Vec3d on_plane = centre - n * dot(n, centre - triangle.a);
        if (point_in_triangle(on_plane, triangle)) {
            const Vec3d normal = distance0 > 0.0 ? n : -n;
            consider(face_t, normal, centre - normal * radius);
        }
    }

    const Vec3d verts[3] = {triangle.a, triangle.b, triangle.c};
    for (int i = 0; i < 3; ++i) {
        const Vec3d p0 = verts[i];
        const Vec3d p1 = verts[(i + 1) % 3];
        const Vec3d edge = p1 - p0;
        const double edge_len2 = dot(edge, edge);
        if (edge_len2 <= 1e-18) {
            continue;
        }
        const Vec3d m = origin - p0;
        const double b = dot(direction, edge);
        const double e = dot(edge, m);
        const double a = 1.0 - b * b / edge_len2;
        if (a <= 1e-12) {
            continue; // ray parallel to the edge: the end caps cover it
        }
        const double bb = 2.0 * (dot(m, direction) - e * b / edge_len2);
        const double c = dot(m, m) - e * e / edge_len2 - radius * radius;
        const double disc = bb * bb - 4.0 * a * c;
        if (disc < 0.0) {
            continue;
        }
        const double root = std::sqrt(disc);
        const double ts[2] = {(-bb - root) / (2.0 * a), (-bb + root) / (2.0 * a)};
        for (const double t : ts) {
            if (t < kCollideRayEps || t > best) {
                continue;
            }
            double s = (e + t * b) / edge_len2;
            if (s < -1e-9 || s > 1.0 + 1e-9) {
                continue;
            }
            s = std::clamp(s, 0.0, 1.0);
            const Vec3d feature = p0 + edge * s;
            const Vec3d centre = origin + direction * t;
            const Vec3d delta = centre - feature;
            const double distance = length(delta);
            if (distance <= 1e-12) {
                continue;
            }
            consider(t, delta * (1.0 / distance), feature);
        }
    }

    for (const Vec3d& vertex : verts) {
        const Vec3d m = origin - vertex;
        const double b = 2.0 * dot(m, direction);
        const double c = dot(m, m) - radius * radius;
        const double disc = b * b - 4.0 * c;
        if (disc < 0.0) {
            continue;
        }
        const double root = std::sqrt(disc);
        const double ts[2] = {(-b - root) * 0.5, (-b + root) * 0.5};
        for (const double t : ts) {
            if (t < kCollideRayEps || t > best) {
                continue;
            }
            const Vec3d centre = origin + direction * t;
            const Vec3d delta = centre - vertex;
            const double distance = length(delta);
            if (distance <= 1e-12) {
                continue;
            }
            consider(t, delta * (1.0 / distance), vertex);
        }
    }
    return found;
}

} // namespace

const char* door_axis_name(DoorAxis axis) {
    switch (axis) {
    case DoorAxis::X:
        return "x";
    case DoorAxis::Y:
        return "y";
    case DoorAxis::Z:
        return "z";
    case DoorAxis::Travel:
        return "travel";
    case DoorAxis::Rotating:
        break;
    }
    return "rotating";
}

Vec3d door_axis_unit(DoorAxis axis) {
    switch (axis) {
    case DoorAxis::X:
        return {1.0, 0.0, 0.0};
    case DoorAxis::Y:
        return {0.0, 1.0, 0.0};
    case DoorAxis::Z:
        return {0.0, 0.0, 1.0};
    case DoorAxis::Travel:
    case DoorAxis::Rotating:
        break;
    }
    return {};
}

Vec3d door_travel(const Door& door) {
    return {door.open[0] - door.closed[0], door.open[1] - door.closed[1],
            door.open[2] - door.closed[2]};
}

DoorAxis door_axis(const Door& door) {
    // 0x44dfc0 tests delta[2], then delta[0], then delta[1]; any non-zero
    // operand makes the leaf rotate about that axis.
    if (door.axis[0] != 0.0 || door.axis[1] != 0.0 || door.axis[2] != 0.0) {
        return DoorAxis::Rotating;
    }
    const Vec3d travel = door_travel(door);
    const int x = travel.x != 0.0 ? 1 : 0;
    const int y = travel.y != 0.0 ? 1 : 0;
    const int z = travel.z != 0.0 ? 1 : 0;
    if (x + y + z == 1) {
        return x != 0 ? DoorAxis::X : (y != 0 ? DoorAxis::Y : DoorAxis::Z);
    }
    // Diagonal (105 of the 1421 moving sliding doors), or a static panel that
    // does not move at all: the caller's panel travel decides, not an axis.
    return DoorAxis::Travel;
}

DoorSwing door_swing(const Door& door) {
    DoorSwing swing;
    swing.pivot = {door.closed[0], door.closed[1], door.closed[2]};
    const int axis = door_rotation_axis(door);
    if (axis < 0) {
        return swing;
    }
    // 0x44dfc0: the unit vector is -x, +y or -z (docs/DOOR_FIELDS.md §1.2).
    swing.axis = axis == 0 ? Vec3d{-1.0, 0.0, 0.0}
                           : (axis == 1 ? Vec3d{0.0, 1.0, 0.0} : Vec3d{0.0, 0.0, -1.0});
    // angle at phase p is A_i / 65535 * p; f = p / 65535.
    swing.max_angle =
        door.axis[static_cast<std::size_t>(axis)] * static_cast<double>(kDoorPhaseMax);
    return swing;
}

void MovingCollider::clear_pose() {
    triangles_.clear();
    panel_of_.clear();
    bounds_ = Aabb{};
}

void MovingCollider::set_panels(std::vector<MovingPanel> panels, DoorAxis axis,
                                const DoorSwing& swing) {
    clear_pose();
    panels_ = std::move(panels);
    axis_ = axis;
    swing_ = swing;
    open_fraction_ = 0.0;
    lift_offset_ = 0.0;
    valid_ = false;
    update_ok_ = false;
    error_.clear();

    if (axis_ == DoorAxis::Rotating) {
        // The engine reads the operands blindly (`0x44dfc0`); the model refuses
        // data that would poison the basis or silently not move.
        if (!finite(swing_.pivot) || !coordinate_ok(swing_.pivot)) {
            error_ = "swing pivot is non-finite/out of range";
            panels_.clear();
            return;
        }
        if (!finite(swing_.axis)) {
            error_ = "swing axis is not finite";
            panels_.clear();
            return;
        }
        if (!(std::fabs(length(swing_.axis) - 1.0) <= 1e-9)) {
            error_ = "swing axis is not a unit vector";
            panels_.clear();
            return;
        }
        if (!std::isfinite(swing_.max_angle)) {
            error_ = "swing angle is not finite";
            panels_.clear();
            return;
        }
        if (swing_.max_angle == 0.0) {
            // A Rotating panel with a zero angle would stand still; the caller
            // should have used a sliding mode (or `door_axis` would not have
            // reported Rotating for a real door).
            error_ = "swing angle is zero: nothing rotates";
            panels_.clear();
            return;
        }
    }
    if (panels_.empty()) {
        error_ = "no panels";
        return;
    }

    std::size_t total = 0;
    for (std::size_t i = 0; i < panels_.size(); ++i) {
        const MovingPanel& panel = panels_[i];
        if (!finite(panel.travel)) {
            error_ = "non-finite travel in panel " + std::to_string(i);
            panels_.clear();
            return;
        }
        for (std::size_t k = 0; k < panel.rest.size(); ++k) {
            if (!triangle_ok(panel.rest[k])) {
                error_ = "non-finite/out-of-range rest triangle " + std::to_string(k) +
                         " in panel " + std::to_string(i);
                panels_.clear();
                return;
            }
            ++total;
        }
    }
    if (total == 0) {
        error_ = "panels carry no triangles";
        return;
    }

    valid_ = true;
    pose(0.0, 0.0);
    update_ok_ = true;
}

void MovingCollider::update(double open_fraction, double lift_offset) {
    if (!std::isfinite(open_fraction)) {
        update_ok_ = false;
        error_ = "open_fraction is not finite";
        return; // keep the last good pose
    }
    if (!std::isfinite(lift_offset)) {
        update_ok_ = false;
        error_ = "lift_offset is not finite";
        return;
    }
    if (!valid_) {
        update_ok_ = false;
        return; // error_ already names the reason set_panels refused
    }
    const double fraction = std::clamp(open_fraction, 0.0, 1.0);
    pose(fraction, lift_offset);
    open_fraction_ = fraction;
    lift_offset_ = lift_offset;
    update_ok_ = true;
    error_.clear();
}

void MovingCollider::pose(double fraction, double lift_offset) {
    clear_pose();
    std::size_t total = 0;
    for (const MovingPanel& panel : panels_) {
        total += panel.rest.size();
    }
    triangles_.reserve(total);
    panel_of_.reserve(total);

    const bool rotating = axis_ == DoorAxis::Rotating;
    const double angle = rotating ? swing_.max_angle * fraction : 0.0;
    // `f = 0, lift = 0` is the identity transform of `0x44dfc0`/`0x434887`
    // (`center == closed`, basis identity): copy the rest triangles so the
    // closed pose is bit-identical to the static `.sbw` geometry.
    const bool identity = rotating && fraction == 0.0 && lift_offset == 0.0;
    for (std::size_t i = 0; i < panels_.size(); ++i) {
        const MovingPanel& panel = panels_[i];
        if (rotating) {
            // center(f) = closed + (open - closed) * f; the rotation is about
            // that point (the `size`-folded closed corner) along the engine's
            // unit axis.
            const Vec3d center_delta = panel.travel * fraction;
            for (const Triangle& rest : panel.rest) {
                if (identity) {
                    triangles_.push_back(rest);
                } else {
                    triangles_.push_back(swung_triangle(rest, swing_.pivot, center_delta,
                                                        swing_.axis, angle, lift_offset));
                }
                panel_of_.push_back(static_cast<std::uint32_t>(i));
            }
            continue;
        }
        Vec3d slide;
        if (axis_ == DoorAxis::Travel) {
            slide = panel.travel * fraction;
        } else {
            const Vec3d unit = door_axis_unit(axis_);
            slide = unit * (dot(panel.travel, unit) * fraction);
        }
        const Vec3d delta{slide.x, slide.y, slide.z + lift_offset};
        for (const Triangle& rest : panel.rest) {
            triangles_.push_back(moved_triangle(rest, delta));
            panel_of_.push_back(static_cast<std::uint32_t>(i));
        }
    }
    rebuild_bounds();
}

void MovingCollider::rebuild_bounds() {
    for (const Triangle& triangle : triangles_) {
        bounds_.expand(triangle.bounds);
    }
}

std::size_t MovingCollider::panel_of(std::uint32_t triangle_index) const {
    return triangle_index < panel_of_.size() ? panel_of_[triangle_index] : panels_.size();
}

std::optional<RayHit> MovingCollider::raycast(const Vec3d& origin, const Vec3d& direction,
                                              double max_t, const CollideFilter& filter) const {
    if (!ready() || !finite(origin) || !finite(direction)) {
        return std::nullopt;
    }
    const double len = length(direction);
    if (!(len > 0.0) || !std::isfinite(len)) {
        return std::nullopt;
    }
    if (!(max_t > 0.0)) {
        return std::nullopt;
    }
    const double limit = std::isfinite(max_t) ? max_t : kCollideInfinity;
    const Vec3d d = direction * (1.0 / len);

    double best = limit;
    const Triangle* best_triangle = nullptr;
    std::uint32_t best_index = 0;

    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        const Triangle& triangle = triangles_[i];
        if (!filter.allows(triangle)) {
            continue;
        }
        // Same rule as `CollisionWorld`: the authored normal (or the winding
        // when absent) defines the back side, and the default is two-sided.
        if (!filter.backfaces && dot(surface_normal(triangle), d) >= 0.0) {
            continue;
        }
        if (!ray_aabb(origin, d, triangle.bounds, best)) {
            continue;
        }
        const std::optional<double> t = ray_triangle(origin, d, triangle, best);
        if (!t.has_value() || *t > best) {
            continue;
        }
        if (best_triangle != nullptr && !(*t < best)) {
            continue; // keep the first triangle on an exact tie
        }
        best = *t;
        best_triangle = &triangle;
        best_index = static_cast<std::uint32_t>(i);
    }

    if (best_triangle == nullptr) {
        return std::nullopt;
    }

    RayHit hit;
    hit.t = best;
    hit.point = origin + d * best;
    hit.triangle = best_triangle;
    hit.triangle_index = best_index;
    hit.sector = best_triangle->sector;
    const Vec3d reference = surface_normal(*best_triangle);
    const double facing = dot(reference, d);
    hit.front_face = facing < 0.0;
    hit.normal = facing > 0.0 ? -reference : reference;
    return hit;
}

std::optional<SphereHit> MovingCollider::sphere_cast(const Vec3d& origin, const Vec3d& direction,
                                                     double radius, double max_t,
                                                     const CollideFilter& filter) const {
    if (!ready() || !finite(origin) || !finite(direction) || !std::isfinite(radius) ||
        radius < 0.0) {
        return std::nullopt;
    }
    const double len = length(direction);
    if (!(len > 0.0) || !std::isfinite(len)) {
        return std::nullopt;
    }
    if (!(max_t > 0.0)) {
        return std::nullopt;
    }
    const double limit = std::isfinite(max_t) ? max_t : kCollideInfinity;
    const Vec3d d = direction * (1.0 / len);
    const double grow = radius;

    double best = limit;
    const Triangle* best_triangle = nullptr;
    std::uint32_t best_index = 0;
    SphereContact best_contact;

    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        const Triangle& triangle = triangles_[i];
        if (!filter.allows(triangle)) {
            continue;
        }
        Aabb tri_broad = triangle.bounds;
        tri_broad.grow(grow);
        if (!ray_aabb(origin, d, tri_broad, best)) {
            continue;
        }
        SphereContact contact;
        if (!sphere_vs_triangle(origin, d, radius, triangle, best, contact)) {
            continue;
        }
        if (!filter.backfaces &&
            dot(surface_normal(triangle), (origin + d * contact.t) - triangle.a) < 0.0) {
            continue; // the sphere touches the authored back side
        }
        if (best_triangle == nullptr || contact.t < best) {
            best = contact.t;
            best_contact = contact;
            best_triangle = &triangle;
            best_index = static_cast<std::uint32_t>(i);
        }
    }

    if (best_triangle == nullptr) {
        return std::nullopt;
    }
    SphereHit hit;
    hit.t = best_contact.t;
    hit.point = best_contact.point;
    hit.centre = origin + d * best_contact.t;
    hit.normal = best_contact.normal;
    hit.triangle = best_triangle;
    hit.triangle_index = best_index;
    hit.sector = best_triangle->sector;
    return hit;
}

} // namespace gm
