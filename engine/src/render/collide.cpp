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

} // namespace

Surface surface_of(const Triangle& triangle, double min_normal_z) {
    // Normalisation (and the f32 -> double world transform) can move an exact
    // 0.8 by one ulp; the tolerance keeps hand-authored 45-degree faces on the
    // same side as tools/re/spawn.py, which compares the unrounded value.
    constexpr double kSurfaceEps = 1e-12;
    if (triangle.normal.z >= min_normal_z - kSurfaceEps) {
        return Surface::floor;
    }
    if (triangle.normal.z <= -min_normal_z + kSurfaceEps) {
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
            if (!filter.allows(triangle)) {
                continue;
            }
            if (!filter.backfaces && dot(triangle.normal, d) >= 0.0) {
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
    const double facing_final = dot(best_triangle->normal, d);
    hit.front_face = facing_final < 0.0;
    hit.normal = facing_final > 0.0 ? -best_triangle->normal : best_triangle->normal;
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
    SphereContact best_contact;

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
                dot(triangle.normal, (origin + d * contact.t) - triangle.a) < 0.0) {
                continue; // the sphere touches the back side of the triangle
            }
            if (best_triangle == nullptr || contact.t < best) {
                best = contact.t;
                best_contact = contact;
                best_triangle = &triangle;
                best_index = i;
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
