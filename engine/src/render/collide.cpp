#include "render/collide.h"

#include <algorithm>
#include <array>
#include <cstring>
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

/// Slab test clipped to `[0, max_t]`; `d` must be finite (it may be zero on an
/// axis, which is handled by the branch).
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

bool sector_allowed(const CollideFilter& filter, std::uint32_t sector) {
    if (filter.sectors.empty()) {
        return true;
    }
    for (const std::uint32_t candidate : filter.sectors) {
        if (candidate == sector) {
            return true;
        }
    }
    return false;
}

double read_f64(const std::uint8_t* p) {
    double value = 0.0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

/// Object header: 12 little-endian `f64` = `origin[3]` + `basis[3][3]`
/// (row-major, `world = origin + basis * local`).  The loader copies the 96
/// bytes unchanged into `obj+0x00/+0x18/+0x30/+0x48` (`0x42ae86`, `0x42aea7`,
/// `0x42aec8`, `0x42aee9`) and the runtime transform `0x437af8` reads them with
/// `FLD double`; the basis is the identity in every shipped object.  Reading
/// the high dword as `f32` (the old model) produced `origin32 + 1.875 * local`
/// because the bits of the double `1.0` (`0x3FF00000`) are the float `1.875`.
struct ObjectTransform {
    Vec3d origin;
    Vec3d bx;
    Vec3d by;
    Vec3d bz;

    bool finite_transform() const {
        return gm::finite(origin) && gm::finite(bx) && gm::finite(by) && gm::finite(bz);
    }

    Vec3d apply(Vec3d local) const {
        return origin + bx * local.x + by * local.y + bz * local.z;
    }
};

ObjectTransform decode_transform(const std::array<std::uint8_t, 96>& header) {
    double v[12];
    for (int i = 0; i < 12; ++i) {
        v[i] = read_f64(header.data() + 8 * i);
    }
    ObjectTransform out;
    out.origin = {v[0], v[1], v[2]};
    out.bx = {v[3], v[4], v[5]};
    out.by = {v[6], v[7], v[8]};
    out.bz = {v[9], v[10], v[11]};
    return out;
}

bool coordinate_ok(Vec3d v) {
    return std::fabs(v.x) <= kCollideMaxCoordinate && std::fabs(v.y) <= kCollideMaxCoordinate &&
           std::fabs(v.z) <= kCollideMaxCoordinate;
}

void set_first_error(std::string* error, std::string message) {
    if (error != nullptr && error->empty()) {
        *error = std::move(message);
    }
}

struct SphereContact {
    double t = 0.0;
    Vec3d point;
    Vec3d normal;
};

/// A ray with the per-axis reciprocal hoisted out of the box loop: the dynamic
/// broad phase tests a dozen block boxes per query, and `ray_aabb` recomputes
/// `1/d` for every one of them.
struct RaySlab {
    Vec3d origin;
    Vec3d inv;
    std::uint8_t parallel = 0; ///< bit `i`: direction component `i` is ~0
};

RaySlab make_slab(const Vec3d& origin, const Vec3d& direction) {
    RaySlab slab;
    slab.origin = origin;
    for (int i = 0; i < 3; ++i) {
        const double d = axis_of(direction, i);
        if (std::fabs(d) < 1e-300) {
            slab.parallel |= static_cast<std::uint8_t>(1u << i);
        } else {
            const double inv = 1.0 / d;
            if (i == 0) {
                slab.inv.x = inv;
            } else if (i == 1) {
                slab.inv.y = inv;
            } else {
                slab.inv.z = inv;
            }
        }
    }
    return slab;
}

/// The same slab test as `ray_aabb`, with the reciprocals of `slab` and the
/// branch chosen from the pre-computed `parallel` mask.  Used only for the
/// dynamic broad phase (the static path keeps `ray_aabb`, bit for bit).
bool ray_slab_aabb(const RaySlab& slab, const Aabb& box, double max_t) {
    double t_min = 0.0;
    double t_max = max_t;
    for (int i = 0; i < 3; ++i) {
        const double o = axis_of(slab.origin, i);
        const double lo = axis_of(box.lo, i);
        const double hi = axis_of(box.hi, i);
        if ((slab.parallel & static_cast<std::uint8_t>(1u << i)) != 0) {
            if (o < lo || o > hi) {
                return false;
            }
            continue;
        }
        const double inv = axis_of(slab.inv, i);
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

/// Closest approach of a swept sphere to one triangle: face, three edges and
/// three vertices.  Each candidate is a genuine contact of the Minkowski sum
/// `triangle (+) sphere`, so the earliest one is the entry time.
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

bool has_data_normal(const Triangle& triangle) {
    // No sqrt: a built `data_normal` is either the zero vector or unit.
    return dot(triangle.data_normal, triangle.data_normal) > 0.0;
}

/// Fill the fields of one dynamic triangle that the caller may have left
/// empty.  `bounds` is detectable (the default AABB is invalid), `normal` too
/// (zero or non-finite); `centroid` and `plane_offset` are recomputed
/// unconditionally because the formula is exact and cheap, which lets a caller
/// pass a bare `Triangle{a, b, c}`.  For a triangle produced by
/// `MovingCollider` the recomputation yields the same bits (same expressions,
/// same order), so this never moves a pose.
void complete_dynamic_triangle(Triangle& triangle) {
    if (!triangle.bounds.valid() || !finite(triangle.bounds.lo) ||
        !finite(triangle.bounds.hi)) {
        triangle.bounds = Aabb{};
        triangle.bounds.expand(triangle.a);
        triangle.bounds.expand(triangle.b);
        triangle.bounds.expand(triangle.c);
    }
    if (!finite(triangle.normal) || dot(triangle.normal, triangle.normal) <= 0.0) {
        const Vec3d face = cross(triangle.b - triangle.a, triangle.c - triangle.a);
        const double area2 = length(face);
        triangle.normal = area2 > 0.0 ? face * (1.0 / area2) : Vec3d{};
    }
    if (!finite(triangle.data_normal)) {
        triangle.data_normal = Vec3d{};
    }
    triangle.plane_offset = dot(triangle.normal, triangle.a);
    triangle.centroid = (triangle.a + triangle.b + triangle.c) * (1.0 / 3.0);
}

} // namespace

Vec3d surface_normal(const Triangle& triangle) {
    // `.sbw` stores the polygon normal as f32; a zero vector means "not
    // authored" and the winding normal is the only information left.
    return has_data_normal(triangle) ? triangle.data_normal : triangle.normal;
}

Surface surface_of(const Triangle& triangle, double min_normal_z) {
    // Two tolerances, because the two sources have different precision:
    //  * the stored polygon normal is f32; the exact author value 0.8 is
    //    -0.7999999928474427 after the f32 round trip, i.e. off by ~7e-9.
    //    The 1e-6 tolerance keeps those hand-authored 45-degree faces on the
    //    same side as tools/re/spawn.py, which compares the unrounded value;
    //  * the winding normal is computed in double; 1e-12 only absorbs one ulp
    //    of normalisation.
    const bool from_data = has_data_normal(triangle);
    constexpr double kWindingEps = 1e-12;
    constexpr double kDataEps = 1e-6;
    const Vec3d n = from_data ? triangle.data_normal : triangle.normal;
    const double eps = from_data ? kDataEps : kWindingEps;
    if (n.z >= min_normal_z - eps) {
        return Surface::floor;
    }
    if (n.z <= -min_normal_z + eps) {
        return Surface::ceiling;
    }
    return Surface::wall;
}

bool CollideFilter::allows(const Triangle& triangle) const {
    switch (surface_of(triangle, min_normal_z)) {
    case Surface::floor:
        if (!floors) {
            return false;
        }
        break;
    case Surface::ceiling:
        if (!ceilings) {
            return false;
        }
        break;
    case Surface::wall:
        if (!walls) {
            return false;
        }
        break;
    }
    return sector_allowed(*this, triangle.sector);
}

Vec3d closest_point_on_triangle(Vec3d p, const Triangle& triangle) {
    const Vec3d a = triangle.a;
    const Vec3d b = triangle.b;
    const Vec3d c = triangle.c;
    const Vec3d ab = b - a;
    const Vec3d ac = c - a;
    const Vec3d ap = p - a;
    const double d1 = dot(ab, ap);
    const double d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return a;
    }
    const Vec3d bp = p - b;
    const double d3 = dot(ab, bp);
    const double d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return b;
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double denom = d1 - d3;
        const double v = denom != 0.0 ? d1 / denom : 0.0;
        return a + ab * v;
    }
    const Vec3d cp = p - c;
    const double d5 = dot(ab, cp);
    const double d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return c;
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double denom = d2 - d6;
        const double w = denom != 0.0 ? d2 / denom : 0.0;
        return a + ac * w;
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double denom = (d4 - d3) + (d5 - d6);
        const double w = denom != 0.0 ? (d4 - d3) / denom : 0.0;
        return b + (c - b) * w;
    }
    const double total = va + vb + vc;
    if (total == 0.0) {
        return a;
    }
    const double inv = 1.0 / total;
    const double v = vb * inv;
    const double w = vc * inv;
    return a + ab * v + ac * w;
}

bool point_in_triangle(Vec3d p, const Triangle& triangle) {
    const Vec3d v0 = triangle.b - triangle.a;
    const Vec3d v1 = triangle.c - triangle.a;
    const Vec3d v2 = p - triangle.a;
    const double d00 = dot(v0, v0);
    const double d01 = dot(v0, v1);
    const double d11 = dot(v1, v1);
    const double d20 = dot(v2, v0);
    const double d21 = dot(v2, v1);
    const double denom = d00 * d11 - d01 * d01;
    if (!(denom > 0.0)) {
        return false;
    }
    const double v = (d11 * d20 - d01 * d21) / denom;
    const double w = (d00 * d21 - d01 * d20) / denom;
    const double u = 1.0 - v - w;
    constexpr double kBar = 1e-9;
    return u >= -kBar && v >= -kBar && w >= -kBar;
}

std::optional<double> ray_triangle(const Vec3d& origin, const Vec3d& unit_direction,
                                   const Triangle& triangle, double max_t, double* u_out,
                                   double* v_out) {
    const Vec3d e1 = triangle.b - triangle.a;
    const Vec3d e2 = triangle.c - triangle.a;
    const Vec3d p = cross(unit_direction, e2);
    const double det = dot(e1, p);
    if (std::fabs(det) <= 1e-12) {
        return std::nullopt; // parallel or zero-area
    }
    const double inv = 1.0 / det;
    const Vec3d s = origin - triangle.a;
    const double u = dot(s, p) * inv;
    constexpr double kBar = 1e-9;
    if (u < -kBar || u > 1.0 + kBar) {
        return std::nullopt;
    }
    const Vec3d q = cross(s, e1);
    const double v = dot(unit_direction, q) * inv;
    if (v < -kBar || u + v > 1.0 + kBar) {
        return std::nullopt;
    }
    const double t = dot(e2, q) * inv;
    if (!(t >= kCollideRayEps) || t > max_t) {
        return std::nullopt;
    }
    if (u_out != nullptr) {
        *u_out = u;
    }
    if (v_out != nullptr) {
        *v_out = v;
    }
    return t;
}

bool CollisionWorld::build(const SbwLevel& level, std::string* error) {
    clear();
    if (error != nullptr) {
        error->clear();
    }
    std::string failure;
    const std::size_t expected =
        level.header.object_count != 0
            ? level.header.object_count
            : [&level] {
                  std::size_t n = 0;
                  for (const auto& sector : level.header.sectors) {
                      n += sector.objects.size();
                  }
                  return n;
              }();
    if (level.geometry.size() != expected) {
        set_first_error(&failure, "geometry/object count mismatch: " + std::to_string(level.geometry.size()) +
                                   " geometries for " + std::to_string(expected) + " objects");
        return false;
    }

    stats_.sectors = level.header.sectors.size();
    sectors_.reserve(stats_.sectors);
    std::size_t geometry_index = 0;

    for (std::size_t s = 0; s < level.header.sectors.size(); ++s) {
        const SbwSectorRef& source = level.header.sectors[s];
        Sector sector;
        sector.begin = static_cast<std::uint32_t>(triangles_.size());

        for (std::size_t o = 0; o < source.objects.size(); ++o, ++geometry_index) {
            const SbwObjectRef& object_ref = source.objects[o];
            const SbwGeometry& geometry = level.geometry[geometry_index];
            ++stats_.objects;

            const ObjectTransform transform = decode_transform(object_ref.header);
            if (!transform.finite_transform()) {
                ++stats_.non_finite;
                set_first_error(&failure, "non-finite object transform in sector " + std::to_string(s) +
                                           " object " + std::to_string(o));
                continue;
            }
            if (!coordinate_ok(transform.origin)) {
                ++stats_.out_of_range;
                set_first_error(&failure, "object origin out of range in sector " + std::to_string(s) +
                                           " object " + std::to_string(o));
                continue;
            }

            std::vector<Vec3d> world;
            std::vector<char> valid;
            world.reserve(geometry.vertices.size());
            valid.reserve(geometry.vertices.size());
            for (const Vec3f& local : geometry.vertices) {
                const Vec3d point = transform.apply({local.x, local.y, local.z});
                if (!finite(point)) {
                    ++stats_.non_finite;
                    set_first_error(&failure, "non-finite vertex in sector " + std::to_string(s) +
                                               " object " + std::to_string(o));
                    world.push_back(Vec3d{});
                    valid.push_back(0);
                    continue;
                }
                if (!coordinate_ok(point)) {
                    ++stats_.out_of_range;
                    set_first_error(&failure, "vertex out of range in sector " + std::to_string(s) +
                                               " object " + std::to_string(o));
                    world.push_back(Vec3d{});
                    valid.push_back(0);
                    continue;
                }
                world.push_back(point);
                valid.push_back(1);
            }

            const std::uint32_t vertex_count = static_cast<std::uint32_t>(geometry.vertices.size());
            for (std::size_t pi = 0; pi < geometry.polys.size(); ++pi) {
                const SbwPoly& poly = geometry.polys[pi];
                ++stats_.polygons;
                if (poly.indices.size() < 3) {
                    ++stats_.degenerate;
                    continue;
                }
                for (const std::uint16_t index : poly.indices) {
                    if (index >= vertex_count) {
                        ++stats_.bad_index;
                        set_first_error(&failure, "vertex index " + std::to_string(index) + " out of range (" +
                                                   std::to_string(vertex_count) + ") in sector " +
                                                   std::to_string(s) + " object " + std::to_string(o));
                    }
                }

                for (std::size_t k = 1; k + 1 < poly.indices.size(); ++k) {
                    const std::uint16_t i0 = poly.indices[0];
                    const std::uint16_t i1 = poly.indices[k];
                    const std::uint16_t i2 = poly.indices[k + 1];
                    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
                        continue;
                    }
                    if (!valid[i0] || !valid[i1] || !valid[i2]) {
                        continue;
                    }
                    Triangle triangle;
                    triangle.a = world[i0];
                    triangle.b = world[i1];
                    triangle.c = world[i2];

                    const Vec3d e1 = triangle.b - triangle.a;
                    const Vec3d e2 = triangle.c - triangle.a;
                    const Vec3d face = cross(e1, e2);
                    const double area2 = length(face);
                    const double e_max = std::max({length(e1), length(e2), length(triangle.c - triangle.b)});
                    if (!(area2 > 1e-12 * std::max(1.0, e_max * e_max))) {
                        ++stats_.degenerate;
                        continue;
                    }
                    triangle.normal = face * (1.0 / area2);
                    // The authoring normal travels with the object like a
                    // direction: every shipped basis is the identity, so a
                    // rigid basis is all the data contains.  A zero or
                    // non-finite stored normal leaves the winding as the
                    // fallback (docs/COLLIDE.md §1.2).
                    const Vec3d local{poly.normal.x, poly.normal.y, poly.normal.z};
                    const Vec3d stored =
                        transform.bx * local.x + transform.by * local.y + transform.bz * local.z;
                    if (finite(stored)) {
                        const double stored_length = length(stored);
                        if (stored_length > 0.0) {
                            triangle.data_normal = stored * (1.0 / stored_length);
                        }
                    }
                    triangle.plane_offset = dot(triangle.normal, triangle.a);
                    triangle.centroid = (triangle.a + triangle.b + triangle.c) * (1.0 / 3.0);
                    triangle.bounds.expand(triangle.a);
                    triangle.bounds.expand(triangle.b);
                    triangle.bounds.expand(triangle.c);
                    triangle.sector = static_cast<std::uint32_t>(s);
                    triangle.object = static_cast<std::uint32_t>(o);
                    triangle.texture = poly.texture;
                    triangle.material = poly.material;
                    triangle.lightmap = poly.lightmap;
                    triangle.source_poly = static_cast<std::uint32_t>(pi);

                    sector.bounds.expand(triangle.bounds);
                    triangles_.push_back(triangle);
                }
            }
        }

        sector.count = static_cast<std::uint32_t>(triangles_.size()) - sector.begin;
        bounds_.expand(sector.bounds);
        sectors_.push_back(sector);
    }

    stats_.triangles = triangles_.size();
    if (!failure.empty()) {
        // Keep the counts for diagnostics but do not expose a half-built world.
        triangles_.clear();
        sectors_.clear();
        bounds_ = Aabb{};
        if (error != nullptr) {
            *error = failure;
        }
        return false;
    }
    return true;
}

void CollisionWorld::clear() {
    triangles_.clear();
    sectors_.clear();
    bounds_ = Aabb{};
    stats_ = BuildStats{};
    clear_dynamic();
}

void CollisionWorld::clear_dynamic() {
    dynamic_.clear();
    suppressed_.clear();
    suppressed_keys_.clear();
    suppressed_triangles_.clear();
    dynamic_bounds_ = Aabb{};
    dynamic_chunks_.clear();
}

bool CollisionWorld::is_suppressed(std::uint32_t sector, std::uint32_t object) const {
    if (suppressed_keys_.empty()) {
        return false;
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(sector) << 32) |
                              static_cast<std::uint64_t>(object);
    return std::binary_search(suppressed_keys_.begin(), suppressed_keys_.end(), key);
}

/// Mark every static triangle whose `(sector, object)` is in `suppressed_`.
///
/// The scan is O(static triangles) and runs only when the pair list changes
/// (the runtime republishes the same list on every door pose).  Membership is a
/// per-sector bitmask, so the scan does not degrade into pairs × triangles.
void CollisionWorld::rebuild_suppressed_mask() {
    suppressed_triangles_.assign(triangles_.size(), 0);
    if (suppressed_.empty()) {
        return;
    }
    std::vector<std::vector<std::uint64_t>> masks(sectors_.size());
    for (const auto& pair : suppressed_) {
        if (pair.first >= masks.size()) {
            continue;
        }
        std::vector<std::uint64_t>& mask = masks[pair.first];
        const std::size_t word = pair.second >> 6;
        if (mask.size() <= word) {
            mask.resize(word + 1, 0);
        }
        mask[word] |= std::uint64_t{1} << (pair.second & 63);
    }
    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        const Triangle& triangle = triangles_[i];
        if (triangle.sector >= masks.size()) {
            continue;
        }
        const std::vector<std::uint64_t>& mask = masks[triangle.sector];
        const std::size_t word = triangle.object >> 6;
        if (word < mask.size() &&
            ((mask[word] >> (triangle.object & 63)) & std::uint64_t{1}) != 0) {
            suppressed_triangles_[i] = 1;
        }
    }
}

bool CollisionWorld::set_dynamic(
    std::vector<Triangle> triangles,
    std::vector<std::pair<std::uint32_t, std::uint32_t>> suppressed, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    std::string failure;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        Triangle& triangle = triangles[i];
        const std::string where = "dynamic triangle " + std::to_string(i);
        if (!finite(triangle.a) || !finite(triangle.b) || !finite(triangle.c)) {
            set_first_error(&failure, where + ": non-finite vertex");
            break;
        }
        if (!coordinate_ok(triangle.a) || !coordinate_ok(triangle.b) ||
            !coordinate_ok(triangle.c)) {
            set_first_error(&failure, where + ": vertex out of range");
            break;
        }
        if (!finite(triangle.normal) || !finite(triangle.data_normal) ||
            !finite(triangle.centroid)) {
            set_first_error(&failure, where + ": non-finite normal/centroid");
            break;
        }
        if (triangle.bounds.valid() &&
            (!finite(triangle.bounds.lo) || !finite(triangle.bounds.hi))) {
            set_first_error(&failure, where + ": non-finite bounds");
            break;
        }
        complete_dynamic_triangle(triangle);
    }
    if (!failure.empty()) {
        // The previous set survives a rejected call: a NaN frame must not
        // teleport or remove the door (docs/DOOR_COLLISION.md §4).
        if (error != nullptr) {
            *error = failure;
        }
        return false;
    }

    dynamic_ = std::move(triangles);
    // Note the comparison before the move: the mask only has to be rebuilt
    // when the pair list changes (the runtime republishes the same list on
    // every door pose).
    const bool same_suppression = suppressed == suppressed_;
    suppressed_ = std::move(suppressed);
    dynamic_bounds_ = Aabb{};
    dynamic_chunks_.clear();
    for (std::size_t i = 0; i < dynamic_.size(); ++i) {
        const Triangle& triangle = dynamic_[i];
        dynamic_bounds_.expand(triangle.bounds);
        if (i % kDynamicChunk == 0) {
            dynamic_chunks_.push_back(Aabb{});
        }
        dynamic_chunks_.back().expand(triangle.bounds);
    }
    // Lookup structures of the suppression list: a sorted key vector (for the
    // public `is_suppressed`) and a byte mask over the static triangles (for
    // the query loops).
    suppressed_keys_.clear();
    suppressed_keys_.reserve(suppressed_.size());
    for (const auto& pair : suppressed_) {
        suppressed_keys_.push_back((static_cast<std::uint64_t>(pair.first) << 32) |
                                   static_cast<std::uint64_t>(pair.second));
    }
    std::sort(suppressed_keys_.begin(), suppressed_keys_.end());
    if (!same_suppression) {
        rebuild_suppressed_mask();
    }
    return true;
}

const Aabb& CollisionWorld::sector_bounds(std::uint32_t sector) const {
    static const Aabb empty;
    return sector < sectors_.size() ? sectors_[sector].bounds : empty;
}

std::optional<RayHit> CollisionWorld::raycast(const Vec3d& origin, const Vec3d& direction,
                                              double max_t, const CollideFilter& filter) const {
    if (!finite(origin) || !finite(direction)) {
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
    bool best_dynamic = false;

    // The static loop repeats the dynamic candidate test on purpose: Debug
    // builds are -O0, where a per-triangle lambda call is measurable in the
    // tens-of-thousands of casts a tick makes.  The two bodies below must stay
    // in lockstep (same clause order, same epsilons).
    //
    // `skip` is the one-byte-per-static-triangle suppression mask; it is null
    // when nothing is suppressed, so a plain world pays one register test per
    // sector and no per-triangle lookup at all.
    const std::uint8_t* const skip =
        suppressed_triangles_.size() == triangles_.size() ? suppressed_triangles_.data()
                                                          : nullptr;
    for (std::size_t s = 0; s < sectors_.size(); ++s) {
        const Sector& sector = sectors_[s];
        if (sector.count == 0 || !sector_allowed(filter, static_cast<std::uint32_t>(s))) {
            continue;
        }
        if (!ray_aabb(origin, d, sector.bounds, best)) {
            continue;
        }
        for (std::uint32_t i = sector.begin; i < sector.begin + sector.count; ++i) {
            const Triangle& triangle = triangles_[i];
            if (skip != nullptr && skip[i] != 0) {
                continue; // stale rest pose of a moving object
            }
            if (!filter.allows(triangle)) {
                continue;
            }
            // The winding never decides passability: the back-side test uses
            // the authored normal (or the winding when there is none), and the
            // default `backfaces = true` lets the ray through from either
            // side (docs/COLLIDE.md §3).
            if (!filter.backfaces && dot(surface_normal(triangle), d) >= 0.0) {
                continue; // back face, rejected by the filter
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
            best_index = i;
            best_dynamic = false;
        }
    }

    // Dynamic set: no sector broad phase (it holds the panels of a handful of
    // doors), so the pre-computed block boxes are the broad phase: the union
    // box first, then one box per `kDynamicChunk` triangles, with the ray
    // reciprocals hoisted out of the loop.  Both tests only skip triangles the
    // per-triangle tests below would reject as well.  Static wins a tie.
    if (!dynamic_.empty()) {
        const RaySlab slab = make_slab(origin, d);
        if (ray_slab_aabb(slab, dynamic_bounds_, best)) {
            for (std::size_t c = 0; c < dynamic_chunks_.size(); ++c) {
                if (!ray_slab_aabb(slab, dynamic_chunks_[c], best)) {
                    continue;
                }
                const std::size_t begin = c * kDynamicChunk;
                const std::size_t end = std::min(begin + kDynamicChunk, dynamic_.size());
                for (std::size_t i = begin; i < end; ++i) {
                    const Triangle& triangle = dynamic_[i];
                    if (!filter.allows(triangle)) {
                        continue;
                    }
                    if (!filter.backfaces && dot(surface_normal(triangle), d) >= 0.0) {
                        continue;
                    }
                    if (!ray_aabb(origin, d, triangle.bounds, best)) {
                        continue;
                    }
                    const std::optional<double> t =
                        ray_triangle(origin, d, triangle, best);
                    if (!t.has_value() || *t > best) {
                        continue;
                    }
                    if (best_triangle != nullptr && !(*t < best)) {
                        continue;
                    }
                    best = *t;
                    best_triangle = &triangle;
                    best_index = static_cast<std::uint32_t>(i);
                    best_dynamic = true;
                }
            }
        }
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
    hit.dynamic = best_dynamic;
    // `front_face` is about the authored side of the polygon; `normal` is
    // always reported against the incoming ray so a caller can slide on it.
    // Flipping a unit normal and the sign of its dot product yields the same
    // "against the ray" vector, so `normal` is unchanged by the new source.
    const Vec3d reference = surface_normal(*best_triangle);
    const double facing_final = dot(reference, d);
    hit.front_face = facing_final < 0.0;
    hit.normal = facing_final > 0.0 ? -reference : reference;
    return hit;
}

std::optional<SphereHit> CollisionWorld::sphere_cast(const Vec3d& origin, const Vec3d& direction,
                                                     double radius, double max_t,
                                                     const CollideFilter& filter) const {
    if (!finite(origin) || !finite(direction) || !std::isfinite(radius) || radius < 0.0) {
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
    bool best_dynamic = false;
    SphereContact best_contact;

    // Same deliberate duplication as `raycast`: at -O0 a per-triangle helper
    // call is measurable.  Static first, dynamic after, static wins an exact
    // tie (the order of the two loops is the tie-break).
    const std::uint8_t* const skip =
        suppressed_triangles_.size() == triangles_.size() ? suppressed_triangles_.data()
                                                          : nullptr;
    for (std::size_t s = 0; s < sectors_.size(); ++s) {
        const Sector& sector = sectors_[s];
        if (sector.count == 0 || !sector_allowed(filter, static_cast<std::uint32_t>(s))) {
            continue;
        }
        Aabb broad = sector.bounds;
        broad.grow(grow);
        if (!ray_aabb(origin, d, broad, best)) {
            continue;
        }
        for (std::uint32_t i = sector.begin; i < sector.begin + sector.count; ++i) {
            const Triangle& triangle = triangles_[i];
            if (skip != nullptr && skip[i] != 0) {
                continue; // stale rest pose of a moving object
            }
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
                best_index = i;
                best_dynamic = false;
            }
        }
    }

    if (!dynamic_.empty()) {
        Aabb union_box = dynamic_bounds_;
        union_box.grow(grow);
        const RaySlab slab = make_slab(origin, d);
        if (ray_slab_aabb(slab, union_box, best)) {
            for (std::size_t c = 0; c < dynamic_chunks_.size(); ++c) {
                Aabb chunk = dynamic_chunks_[c];
                chunk.grow(grow);
                if (!ray_slab_aabb(slab, chunk, best)) {
                    continue;
                }
                const std::size_t begin = c * kDynamicChunk;
                const std::size_t end = std::min(begin + kDynamicChunk, dynamic_.size());
                for (std::size_t i = begin; i < end; ++i) {
                    const Triangle& triangle = dynamic_[i];
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
                        dot(surface_normal(triangle), (origin + d * contact.t) -
                                                          triangle.a) < 0.0) {
                        continue;
                    }
                    if (best_triangle == nullptr || contact.t < best) {
                        best = contact.t;
                        best_contact = contact;
                        best_triangle = &triangle;
                        best_index = static_cast<std::uint32_t>(i);
                        best_dynamic = true;
                    }
                }
            }
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
    hit.dynamic = best_dynamic;
    return hit;
}

std::optional<double> CollisionWorld::floor_at(double x, double y, double z, double max_drop,
                                               const CollideFilter& filter) const {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return std::nullopt;
    }
    if (!(max_drop > 0.0)) {
        return std::nullopt;
    }
    CollideFilter floors = filter;
    floors.floors = true;
    floors.walls = false;
    floors.ceilings = false;
    const std::optional<RayHit> hit =
        raycast({x, y, z}, {0.0, 0.0, -1.0}, std::isfinite(max_drop) ? max_drop : kCollideInfinity,
                floors);
    if (!hit.has_value()) {
        return std::nullopt;
    }
    return hit->point.z;
}

std::size_t CollisionWorld::overlap_aabb(const Aabb& box, std::vector<std::uint32_t>& out,
                                         const CollideFilter& filter) const {
    out.clear();
    if (!box.valid() || !finite(box.lo) || !finite(box.hi)) {
        return 0;
    }
    const std::uint8_t* const skip =
        suppressed_triangles_.size() == triangles_.size() ? suppressed_triangles_.data()
                                                          : nullptr;
    for (std::size_t s = 0; s < sectors_.size(); ++s) {
        const Sector& sector = sectors_[s];
        if (sector.count == 0 || !sector_allowed(filter, static_cast<std::uint32_t>(s))) {
            continue;
        }
        if (!overlaps(sector.bounds, box)) {
            continue;
        }
        for (std::uint32_t i = sector.begin; i < sector.begin + sector.count; ++i) {
            const Triangle& triangle = triangles_[i];
            if (skip != nullptr && skip[i] != 0) {
                continue; // stale rest pose of a moving object
            }
            if (!filter.allows(triangle)) {
                continue;
            }
            if (overlaps(triangle.bounds, box)) {
                out.push_back(i);
            }
        }
    }
    return out.size();
}

} // namespace gm
