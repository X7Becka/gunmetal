#include "render/render.h"

#include "core/path.h"
#include "res/bmp.h"
#include "res/sbm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace gm {
namespace {

// The reference renderer (gm/render.py) runs on Python floats, i.e. IEEE-754
// doubles.  Everything below is double for the same reason: on the tiny 8x8
// lightmap pages a 1-texel sampling shift changes the shade by tens of levels,
// so float32 rounding is visible.  Operation order follows the Python code.

constexpr double kDegToRad = 0.017453292519943295; // math.radians' multiplier
constexpr double kAmbient = 0.28;
constexpr double kBrightnessGain = 0.5;

using V3 = Vec3d;

struct V2 {
    double u = 0.0;
    double v = 0.0;
};

V3 v3(Vec3f v) { return {v.x, v.y, v.z}; }
V3 v3(const Vec3d& v) { return v; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 norm(V3 v) {
    // Divide per component (not multiply by 1/len): gm/render.py's _norm does
    // `a[i] / l`, and the two round differently in the last bit.
    const double len = std::sqrt(dot(v, v));
    return len > 0.0 ? V3{v.x / len, v.y / len, v.z / len} : V3{};
}

const V3 kLight = norm({0.42, -0.5, 0.75});

const char* const kMaterials[] = {"none", "reflective", "translucent", "varadd", "add", "shade"};
const double kMaterialIntensity[] = {0.0, 64.0, 64.0, 100.0, 127.0, -128.0};

struct Camera {
    int width = 0;
    int height = 0;
    V3 eye;
    V3 fwd;
    V3 right;
    V3 up;
    double scale = 1.0;
    double cx = 0.0;
    double cy = 0.0;
    double near = 0.01;

    Camera(V3 center, double radius, int w, int h, double az, double el, double zoom,
           double fov)
        : width(w), height(h) {
        const double a = az * kDegToRad;
        const double e = el * kDegToRad;
        const V3 dir{std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
        const double dist = radius / std::sin(fov * kDegToRad / 2.0) / std::max(1e-3, zoom);
        eye = add(center, mul(norm(dir), dist));
        fwd = norm(sub(center, eye));
        V3 world_up{0.0, 0.0, 1.0};
        if (std::fabs(dot(fwd, world_up)) > 0.99) {
            world_up = {0.0, 1.0, 0.0};
        }
        right = norm(cross(fwd, world_up));
        up = cross(right, fwd);
        scale = (static_cast<double>(height) / 2.0) / std::tan(fov * kDegToRad / 2.0);
        cx = static_cast<double>(width) / 2.0;
        cy = static_cast<double>(height) / 2.0;
        near = std::max(0.02, std::min(dist, radius) * 0.05);
    }

    /// Explicit eye/target camera (gm/render.py ``pos``/``look``).  *radius* is
    /// the scene bounding radius, only used for the near plane.
    Camera(V3 eye_, V3 target, int w, int h, double fov, double radius)
        : width(w), height(h) {
        eye = eye_;
        const V3 to_target = sub(target, eye);
        fwd = norm(to_target);
        const double dist = std::sqrt(dot(to_target, to_target));
        V3 world_up{0.0, 0.0, 1.0};
        if (std::fabs(dot(fwd, world_up)) > 0.99) {
            world_up = {0.0, 1.0, 0.0};
        }
        right = norm(cross(fwd, world_up));
        up = cross(right, fwd);
        scale = (static_cast<double>(height) / 2.0) / std::tan(fov * kDegToRad / 2.0);
        cx = static_cast<double>(width) / 2.0;
        cy = static_cast<double>(height) / 2.0;
        near = std::max(0.02, std::min(dist, radius) * 0.05);
    }

    V3 view(V3 p) const {
        const V3 d = sub(p, eye);
        return {dot(d, right), dot(d, up), dot(d, fwd)};
    }

    bool project_view(V3 v, double& sx, double& sy, double& invz) const {
        if (v.z <= 0.0) {
            return false;
        }
        sx = cx + v.x / v.z * scale;
        sy = cy - v.y / v.z * scale;
        invz = 1.0 / v.z;
        return true;
    }

    double plane_kx() const { return (static_cast<double>(width) / 2.0) / scale; }
    double plane_ky() const { return (static_cast<double>(height) / 2.0) / scale; }

    bool aabb_visible(V3 lo, V3 hi) const {
        const double kx = plane_kx();
        const double ky = plane_ky();
        if (hi.z < near) {
            return false;
        }
        return !(lo.x - kx * hi.z > 0 || hi.x + kx * hi.z < 0 || lo.y - ky * hi.z > 0 ||
                 hi.y + ky * hi.z < 0);
    }

    bool world_aabb_visible(V3 lo, V3 hi) const {
        V3 vlo{1e30, 1e30, 1e30};
        V3 vhi{-1e30, -1e30, -1e30};
        const double xs[2] = {lo.x, hi.x};
        const double ys[2] = {lo.y, hi.y};
        const double zs[2] = {lo.z, hi.z};
        for (double x : xs) {
            for (double y : ys) {
                for (double z : zs) {
                    const V3 v = view({x, y, z});
                    vlo = {std::min(vlo.x, v.x), std::min(vlo.y, v.y), std::min(vlo.z, v.z)};
                    vhi = {std::max(vhi.x, v.x), std::max(vhi.y, v.y), std::max(vhi.z, v.z)};
                }
            }
        }
        return aabb_visible(vlo, vhi);
    }
};

struct Projected {
    double sx = 0.0;
    double sy = 0.0;
    double invz = 0.0;
};

struct Blend {
    bool active = false;
    bool mix = false;
    double weight = 0.0;
};

Blend face_blend(const RenderFace& face, bool materials) {
    Blend out;
    if (!materials) {
        return out;
    }
    const int index = face.material;
    if (index <= 0 || index >= 6) {
        return out;
    }
    if (index == 2) { // translucent
        out.active = true;
        out.mix = true;
        out.weight = 0.5;
        return out;
    }
    out.active = true;
    out.weight = kMaterialIntensity[index] / 127.0;
    return out;
}

int clamp_byte(double v) {
    if (v < 0.0) {
        return 0;
    }
    if (v > 255.0) {
        return 255;
    }
    return static_cast<int>(v);
}

struct Fit {
    double u0 = 0.0;
    double v0 = 0.0;
    double su = 1.0;
    double sv = 1.0;
};

Fit uv_fit(const std::vector<V2>& uvs) {
    Fit fit;
    if (uvs.empty()) {
        return fit;
    }
    double min_u = uvs[0].u;
    double max_u = uvs[0].u;
    double min_v = uvs[0].v;
    double max_v = uvs[0].v;
    for (const auto& uv : uvs) {
        min_u = std::min(min_u, uv.u);
        max_u = std::max(max_u, uv.u);
        min_v = std::min(min_v, uv.v);
        max_v = std::max(max_v, uv.v);
    }
    fit.u0 = min_u;
    fit.v0 = min_v;
    fit.su = std::max(1e-9, max_u - min_u);
    fit.sv = std::max(1e-9, max_v - min_v);
    return fit;
}

void clip_near(std::vector<V3>& view, std::vector<std::vector<V2>>& groups, double near) {
    std::vector<V3> out_v;
    std::vector<std::vector<V2>> out_g(groups.size());
    const std::size_t n = view.size();
    for (std::size_t i = 0; i < n; ++i) {
        const V3 cur = view[i];
        const V3 nxt = view[(i + 1) % n];
        const bool cin = cur.z >= near;
        const bool nin = nxt.z >= near;
        if (cin) {
            out_v.push_back(cur);
            for (std::size_t k = 0; k < groups.size(); ++k) {
                out_g[k].push_back(groups[k][i]);
            }
        }
        if (cin != nin) {
            const double t = (near - cur.z) / (nxt.z - cur.z);
            out_v.push_back({cur.x + (nxt.x - cur.x) * t, cur.y + (nxt.y - cur.y) * t,
                             cur.z + (nxt.z - cur.z) * t});
            for (std::size_t k = 0; k < groups.size(); ++k) {
                const V2 a = groups[k][i];
                const V2 b = groups[k][(i + 1) % n];
                out_g[k].push_back({a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t});
            }
        }
    }
    view = std::move(out_v);
    groups = std::move(out_g);
}

double shade_face(const RenderFace& face, bool lit) {
    if (!lit) {
        return 1.0;
    }
    V3 n = v3(face.normal);
    if (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) < 1e-9) {
        n = cross(v3(face.u), v3(face.v));
    }
    const double lam = std::fabs(dot(norm(n), kLight));
    return kAmbient + (1.0 - kAmbient) * lam;
}

void face_color(const RenderFace& face, int& r, int& g, int& b) {
    std::uint32_t h = 2166136261u;
    for (char ch : face.texture) {
        h = (h ^ static_cast<std::uint8_t>(ch)) * 16777619u;
    }
    r = 110 + static_cast<int>(h & 0x3F);
    g = 110 + static_cast<int>((h >> 6) & 0x3F);
    b = 120 + static_cast<int>((h >> 12) & 0x3F);
}

double face_brightness(const RenderFace& face) {
    return face.brightness ? (static_cast<double>(face.brightness) / 127.0) * kBrightnessGain
                           : 0.0;
}

void draw_triangle(std::vector<std::uint8_t>& buf, std::vector<double>& zbuf, int width,
                   int height, const Projected* tri, const V2* tuv, const Texture* tex,
                   double shade, int cr, int cg, int cb, double u0, double v0, double su,
                   double sv, const Blend& blend, bool z_write, const SbcRecord* lm_page,
                   const V2* lmuv, double lm_base) {
    const double x0 = tri[0].sx, y0 = tri[0].sy, w0 = tri[0].invz;
    const double x1 = tri[1].sx, y1 = tri[1].sy, w1 = tri[1].invz;
    const double x2 = tri[2].sx, y2 = tri[2].sy, w2 = tri[2].invz;
    const int minx = std::max(0, static_cast<int>(std::min({x0, x1, x2})));
    const int maxx = std::min(width - 1, static_cast<int>(std::max({x0, x1, x2})) + 1);
    const int miny = std::max(0, static_cast<int>(std::min({y0, y1, y2})));
    const int maxy = std::min(height - 1, static_cast<int>(std::max({y0, y1, y2})) + 1);
    if (maxx < minx || maxy < miny) {
        return;
    }
    const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area) < 1e-9) {
        return;
    }
    const double inv = 1.0 / area;
    const int tex_w = tex ? tex->width : 1;
    const int tex_h = tex ? tex->height : 1;
    const int lm_w = lm_page ? lm_page->width : 1;
    const int lm_h = lm_page ? lm_page->height : 1;

    for (int y = miny; y <= maxy; ++y) {
        const double py = static_cast<double>(y) + 0.5;
        for (int x = minx; x <= maxx; ++x) {
            const double px = static_cast<double>(x) + 0.5;
            const double l0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
            if (l0 < 0.0) {
                continue;
            }
            const double l1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
            if (l1 < 0.0) {
                continue;
            }
            const double l2 = 1.0 - l0 - l1;
            if (l2 < 0.0) {
                continue;
            }
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            const double iz = l0 * w0 + l1 * w1 + l2 * w2;
            if (iz <= zbuf[i]) {
                continue;
            }
            if (z_write) {
                zbuf[i] = iz;
            }
            double r = 0.0, g = 0.0, b = 0.0;
            if (tex == nullptr) {
                r = static_cast<double>(cr);
                g = static_cast<double>(cg);
                b = static_cast<double>(cb);
            } else {
                const double uu = (l0 * tuv[0].u * w0 + l1 * tuv[1].u * w1 +
                                   l2 * tuv[2].u * w2) / iz;
                const double vv = (l0 * tuv[0].v * w0 + l1 * tuv[1].v * w1 +
                                   l2 * tuv[2].v * w2) / iz;
                int tx = static_cast<int>((uu - u0) / su * static_cast<double>(tex_w)) % tex_w;
                int ty = static_cast<int>((vv - v0) / sv * static_cast<double>(tex_h)) % tex_h;
                tx = ((tx % tex_w) + tex_w) % tex_w;
                ty = ((ty % tex_h) + tex_h) % tex_h;
                const std::size_t o = (static_cast<std::size_t>(ty) * tex_w + tx) * 3;
                r = static_cast<double>(tex->rgb[o]);
                g = static_cast<double>(tex->rgb[o + 1]);
                b = static_cast<double>(tex->rgb[o + 2]);
            }
            double f = shade;
            if (lm_page != nullptr && !lm_page->pixels.empty()) {
                const double lu = (l0 * lmuv[0].u * w0 + l1 * lmuv[1].u * w1 +
                                   l2 * lmuv[2].u * w2) / iz;
                const double lv = (l0 * lmuv[0].v * w0 + l1 * lmuv[1].v * w1 +
                                   l2 * lmuv[2].v * w2) / iz;
                const int iu = static_cast<int>(lu) & (lm_w - 1);
                const int iv = static_cast<int>(lv) & (lm_h - 1);
                const std::size_t lo = static_cast<std::size_t>(iv) * lm_w + iu;
                double light = lm_base;
                if (lo < lm_page->pixels.size()) {
                    light += static_cast<double>(lm_page->pixels[lo]);
                }
                light = light > 127.0 ? 127.0 : (light < 0.0 ? 0.0 : light);
                f = light / 127.0;
            }
            r *= f;
            g *= f;
            b *= f;
            const std::size_t o2 = i * 3;
            if (blend.active && blend.mix) {
                buf[o2] = static_cast<std::uint8_t>(clamp_byte(
                    static_cast<double>(buf[o2]) * (1.0 - blend.weight) + r * blend.weight));
                buf[o2 + 1] = static_cast<std::uint8_t>(clamp_byte(
                    static_cast<double>(buf[o2 + 1]) * (1.0 - blend.weight) +
                    g * blend.weight));
                buf[o2 + 2] = static_cast<std::uint8_t>(clamp_byte(
                    static_cast<double>(buf[o2 + 2]) * (1.0 - blend.weight) +
                    b * blend.weight));
            } else if (blend.active) {
                buf[o2] = static_cast<std::uint8_t>(
                    clamp_byte(static_cast<double>(buf[o2]) + r * blend.weight));
                buf[o2 + 1] = static_cast<std::uint8_t>(
                    clamp_byte(static_cast<double>(buf[o2 + 1]) + g * blend.weight));
                buf[o2 + 2] = static_cast<std::uint8_t>(
                    clamp_byte(static_cast<double>(buf[o2 + 2]) + b * blend.weight));
            } else {
                buf[o2] = static_cast<std::uint8_t>(clamp_byte(r));
                buf[o2 + 1] = static_cast<std::uint8_t>(clamp_byte(g));
                buf[o2 + 2] = static_cast<std::uint8_t>(clamp_byte(b));
            }
        }
    }
}

void draw_poly(std::vector<std::uint8_t>& buf, std::vector<double>& zbuf, int width,
               int height, const std::vector<Projected>& pts, const std::vector<V2>& uvs,
               const Texture* tex, double shade, int cr, int cg, int cb, bool fit_enabled,
               const Fit& fit, const Blend& blend, const SbcRecord* lm_page,
               const std::vector<V2>& lmuv, double lm_base, bool cull) {
    if (pts.size() < 3) {
        return;
    }
    if (cull) {
        const double area = (pts[1].sx - pts[0].sx) * (pts[2].sy - pts[0].sy) -
                            (pts[2].sx - pts[0].sx) * (pts[1].sy - pts[0].sy);
        if (area >= 0.0) {
            return;
        }
    }
    const bool z_write = !blend.active;
    const Fit f = fit_enabled ? fit : Fit{0.0, 0.0, 1.0, 1.0};
    for (std::size_t k = 1; k + 1 < pts.size(); ++k) {
        const Projected tri[3] = {pts[0], pts[k], pts[k + 1]};
        const V2 tuv[3] = {uvs[0], uvs[k], uvs[k + 1]};
        V2 tlm[3];
        const V2* lmp = nullptr;
        if (lm_page != nullptr) {
            tlm[0] = lmuv[0];
            tlm[1] = lmuv[k];
            tlm[2] = lmuv[k + 1];
            lmp = tlm;
        }
        draw_triangle(buf, zbuf, width, height, tri, tuv, tex, shade, cr, cg, cb, f.u0, f.v0,
                      f.su, f.sv, blend, z_write, lm_page, lmp, lm_base);
    }
}

void draw_line(std::vector<std::uint8_t>& buf, int width, int height, Projected a,
               Projected b) {
    int x0 = static_cast<int>(a.sx);
    int y0 = static_cast<int>(a.sy);
    const int x1 = static_cast<int>(b.sx);
    const int y1 = static_cast<int>(b.sy);
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        if (x0 >= 0 && x0 < width && y0 >= 0 && y0 < height) {
            const std::size_t o = (static_cast<std::size_t>(y0) * width + x0) * 3;
            buf[o] = 16;
            buf[o + 1] = 16;
            buf[o + 2] = 20;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

std::array<V3, 4> header_points(const std::array<std::uint8_t, 96>& header) {
    std::array<V3, 4> out{};
    for (int vec = 0; vec < 4; ++vec) {
        double xyz[3];
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t off = static_cast<std::size_t>(vec * 3 + axis) * 8 + 4;
            float value = 0.0f;
            std::memcpy(&value, header.data() + off, 4);
            xyz[axis] = value;
        }
        out[vec] = {xyz[0], xyz[1], xyz[2]};
    }
    return out;
}

int material_index(const std::string& name) {
    for (int i = 0; i < 6; ++i) {
        if (name == kMaterials[i]) {
            return i;
        }
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// mesh builders
// ---------------------------------------------------------------------------

std::vector<RenderMesh> build_level_meshes(const SbwLevel& level, const Sbc* lightmaps) {
    (void)lightmaps;
    std::vector<RenderMesh> out;
    std::vector<std::string> names;
    names.reserve(level.header.textures.size());
    for (const auto& tex : level.header.textures) {
        names.push_back(tex.name);
    }

    std::size_t geometry_index = 0;
    for (const auto& sector : level.header.sectors) {
        for (const auto& object : sector.objects) {
            if (geometry_index >= level.geometry.size()) {
                break;
            }
            const SbwGeometry& geometry = level.geometry[geometry_index++];
            if (geometry.vertices.empty()) {
                continue;
            }
            const auto points = header_points(object.header);
            RenderMesh mesh;
            mesh.placed = true;
            mesh.place_origin = points[0];
            mesh.place_scale = std::sqrt(dot(points[1], points[1]));
            mesh.local.reserve(geometry.vertices.size());
            for (const auto& local : geometry.vertices) {
                mesh.local.push_back(v3(local));
            }
            mesh.vertices.reserve(geometry.vertices.size());
            for (const auto& local : geometry.vertices) {
                const V3 l = v3(local);
                const V3 world{
                    points[0].x + points[1].x * l.x + points[2].x * l.y + points[3].x * l.z,
                    points[0].y + points[1].y * l.x + points[2].y * l.y + points[3].y * l.z,
                    points[0].z + points[1].z * l.x + points[2].z * l.y + points[3].z * l.z};
                mesh.vertices.push_back(world);
            }
            mesh.faces.reserve(geometry.polys.size());
            for (const auto& poly : geometry.polys) {
                if (poly.indices.size() < 3) {
                    continue;
                }
                RenderFace face;
                face.indices = poly.indices;
                face.texture = poly.texture < names.size() ? names[poly.texture] : "none";
                face.material = poly.material < 6 ? poly.material : 0;
                face.brightness = poly.brightness;
                face.lightmap = poly.lightmap > 0 ? static_cast<int>(poly.lightmap) - 1 : -1;
                face.origin = poly.centre;
                face.u = poly.u_axis;
                face.v = poly.v_axis;
                face.normal = poly.normal;
                face.uv_scale = 1.0f;
                face.uv_u_fp = static_cast<std::int32_t>(poly.uv_u_fp);
                face.uv_v_fp = static_cast<std::int32_t>(poly.uv_v_fp);
                mesh.faces.push_back(std::move(face));
            }
            if (!mesh.faces.empty()) {
                out.push_back(std::move(mesh));
            }
        }
    }
    return out;
}

std::vector<RenderMesh> build_model_meshes(const Sob& model) {
    RenderMesh mesh;
    mesh.vertices.reserve(model.vertices.size());
    for (const auto& v : model.vertices) {
        mesh.vertices.push_back(v3(v));
    }
    mesh.local = mesh.vertices;
    mesh.faces.reserve(model.faces.size());
    for (const auto& face : model.faces) {
        if (face.indices.size() < 3) {
            continue;
        }
        RenderFace out;
        out.texture = face.textures[0];
        out.indices.reserve(face.indices.size());
        for (int index : face.indices) {
            out.indices.push_back(static_cast<std::uint16_t>(index));
        }
        for (int slot = 1; slot < 8; ++slot) {
            const int material = material_index(face.textures[slot]);
            if (material > 0) {
                out.material = material;
                break;
            }
        }
        out.origin = face.origin;
        out.u = face.u;
        out.v = face.v;
        out.normal = face.normal;
        out.uv_scale = 1.0f / 65536.0f;
        mesh.faces.push_back(std::move(out));
    }
    return {std::move(mesh)};
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------

TexturePool::TexturePool(std::filesystem::path game, AssetCache* cache)
    : game_(std::move(game)), cache_(cache) {
    for (const char* sub : {"PICS", "sprites"}) {
        if (const auto idx = resolve_ci(game_ / sub, "SBM.IDX")) {
            archives_.push_back(*idx);
        }
    }
    if (cache_ == nullptr) {
        cache_ = &owned_;
    }
}

const Texture* TexturePool::get(const std::string& name) {
    if (name.empty() || name == "none") {
        return nullptr;
    }
    if (const auto it = textures_.find(name); it != textures_.end()) {
        return it->second.get();
    }
    auto tex = load(name);
    const Texture* raw = tex.get();
    textures_.emplace(name, std::move(tex));
    return raw;
}

std::unique_ptr<Texture> TexturePool::load(const std::string& name) {
    if (!palette_ready_) {
        std::string error;
        if (const auto palette = installed_palette(game_, error)) {
            palette_ = *palette;
        }
        palette_ready_ = true;
    }
    for (const auto& idx : archives_) {
        std::string error;
        const auto blob = cache_->get(idx, name, error);
        if (!blob) {
            continue;
        }
        const auto sbm = parse_sbm(*blob, error);
        if (!sbm || sbm->mips.empty() || palette_.empty()) {
            continue;
        }
        const SbmLevel& level = sbm->mips.front();
        const auto rgba = sbm_to_rgba(level, palette_);
        Texture tex;
        tex.width = level.width;
        tex.height = level.height;
        tex.rgb.resize(static_cast<std::size_t>(level.width) * level.height * 3);
        for (int i = 0; i < level.width * level.height; ++i) {
            tex.rgb[static_cast<std::size_t>(i) * 3 + 0] =
                rgba[static_cast<std::size_t>(i) * 4 + 0];
            tex.rgb[static_cast<std::size_t>(i) * 3 + 1] =
                rgba[static_cast<std::size_t>(i) * 4 + 1];
            tex.rgb[static_cast<std::size_t>(i) * 3 + 2] =
                rgba[static_cast<std::size_t>(i) * 4 + 2];
        }
        ++loads_;
        return std::make_unique<Texture>(std::move(tex));
    }
    ++misses_;
    return nullptr;
}

// ---------------------------------------------------------------------------
// renderer
// ---------------------------------------------------------------------------

RenderResult render_scene(const std::vector<RenderMesh>& meshes,
                          const RenderOptions& options, TexturePool* pool,
                          const Sbc* lightmaps) {
    RenderResult result;
    result.width = options.width;
    result.height = options.height;
    result.rgb.assign(static_cast<std::size_t>(options.width) * options.height * 3, 0);
    if (options.width <= 0 || options.height <= 0) {
        return result;
    }

    if (options.background) {
        const std::uint8_t top[3] = {34, 38, 52};
        const std::uint8_t bottom[3] = {12, 12, 16};
        for (int y = 0; y < options.height; ++y) {
            const double t = static_cast<double>(y) /
                             static_cast<double>(std::max(1, options.height - 1));
            std::uint8_t row[3];
            for (int k = 0; k < 3; ++k) {
                row[k] = static_cast<std::uint8_t>(static_cast<int>(
                    static_cast<double>(top[k]) + (static_cast<double>(bottom[k]) - top[k]) * t));
            }
            for (int x = 0; x < options.width; ++x) {
                const std::size_t o = (static_cast<std::size_t>(y) * options.width + x) * 3;
                result.rgb[o] = row[0];
                result.rgb[o + 1] = row[1];
                result.rgb[o + 2] = row[2];
            }
        }
    }

    if (meshes.empty()) {
        return result;
    }

    V3 lo{1e30, 1e30, 1e30};
    V3 hi{-1e30, -1e30, -1e30};
    for (const auto& mesh : meshes) {
        for (const auto& v : mesh.vertices) {
            lo = {std::min(lo.x, static_cast<double>(v.x)), std::min(lo.y, static_cast<double>(v.y)),
                  std::min(lo.z, static_cast<double>(v.z))};
            hi = {std::max(hi.x, static_cast<double>(v.x)), std::max(hi.y, static_cast<double>(v.y)),
                  std::max(hi.z, static_cast<double>(v.z))};
        }
    }
    V3 center{0.0, 0.0, 0.0};
    double radius = 1.0;
    if (lo.x <= hi.x) {
        center = {(lo.x + hi.x) / 2.0, (lo.y + hi.y) / 2.0, (lo.z + hi.z) / 2.0};
        radius = std::max(1e-6, 0.5 * std::sqrt(dot(sub(hi, lo), sub(hi, lo))));
    }
    const Camera cam = options.camera_override
                           ? Camera(V3{options.eye.x, options.eye.y, options.eye.z},
                                    V3{options.target.x, options.target.y, options.target.z},
                                    options.width, options.height, options.fov, radius)
                           : Camera(center, radius, options.width, options.height, options.az,
                                    options.el, options.zoom, options.fov);

    std::vector<double> zbuf(static_cast<std::size_t>(options.width) * options.height, -1.0);

    struct Prepared {
        double distance = 0.0;
        std::vector<Projected> pts;
        std::vector<V2> uvs;
        const Texture* tex = nullptr;
        double shade = 1.0;
        int r = 0, g = 0, b = 0;
        Blend blend;
        Fit fit;
        const SbcRecord* lm_page = nullptr;
        std::vector<V2> lmuv;
        double lm_base = 0.0;
    };
    std::vector<Prepared> blended;

    auto mesh_bounds = [](const RenderMesh& mesh, V3& out_lo, V3& out_hi) {
        out_lo = {1e30, 1e30, 1e30};
        out_hi = {-1e30, -1e30, -1e30};
        for (const auto& v : mesh.vertices) {
            out_lo = {std::min(out_lo.x, static_cast<double>(v.x)),
                      std::min(out_lo.y, static_cast<double>(v.y)),
                      std::min(out_lo.z, static_cast<double>(v.z))};
            out_hi = {std::max(out_hi.x, static_cast<double>(v.x)),
                      std::max(out_hi.y, static_cast<double>(v.y)),
                      std::max(out_hi.z, static_cast<double>(v.z))};
        }
    };

    for (const auto& mesh : meshes) {
        if (mesh.vertices.empty()) {
            continue;
        }
        if (options.sectors) {
            V3 mlo, mhi;
            mesh_bounds(mesh, mlo, mhi);
            if (!cam.world_aabb_visible(mlo, mhi)) {
                continue;
            }
        }
        auto base_vertex = [&](std::uint16_t index) {
            if (!mesh.placed) {
                return v3(mesh.local[index]);
            }
            const double s = mesh.place_scale != 0.0 ? mesh.place_scale : 1.0;
            return mul(sub(v3(mesh.vertices[index]), v3(mesh.place_origin)), 1.0 / s);
        };

        for (const auto& face : mesh.faces) {
            if (face.indices.size() < 3) {
                continue;
            }
            std::vector<V3> view;
            view.reserve(face.indices.size());
            bool bad = false;
            for (std::uint16_t index : face.indices) {
                if (index >= mesh.vertices.size()) {
                    bad = true;
                    break;
                }
                view.push_back(cam.view(v3(mesh.vertices[index])));
            }
            if (bad || view.size() < 3) {
                continue;
            }
            std::vector<V2> uvs;
            uvs.reserve(view.size());
            const V3 origin = v3(face.origin);
            const V3 uaxis = v3(face.u);
            const V3 vaxis = v3(face.v);
            const double uv_scale = face.uv_scale;
            if (options.clip > 0.0f) {
                // gm/render.py measures the distance from the face plane origin,
                // not from the polygon's world centre (its LevelFace keeps the
                // local .sbw centre); mirrored here for frame parity.
                const V3 d = sub(origin, cam.eye);
                if (dot(d, d) > static_cast<double>(options.clip) * options.clip) {
                    continue;
                }
            }
            for (std::uint16_t index : face.indices) {
                const V3 d = sub(base_vertex(index), origin);
                uvs.push_back({dot(d, uaxis) * uv_scale, dot(d, vaxis) * uv_scale});
            }
            const Fit fit = uv_fit(uvs);

            const SbcRecord* page = nullptr;
            std::vector<V2> lmuv;
            double lm_base = 0.0;
            if (options.lightmaps && lightmaps != nullptr && face.lightmap >= 0 &&
                static_cast<std::size_t>(face.lightmap) < lightmaps->records.size()) {
                page = &lightmaps->records[static_cast<std::size_t>(face.lightmap)];
                lmuv.reserve(view.size());
                for (std::uint16_t index : face.indices) {
                    const V3 d = options.lightmap_world
                                     ? sub(v3(mesh.vertices[index]), origin)
                                     : sub(base_vertex(index), origin);
                    const int u = static_cast<int>((dot(d, uaxis) * 64.0 +
                                                    static_cast<double>(face.uv_u_fp)) /
                                                   2.0) >> 10;
                    const int v = static_cast<int>((dot(d, vaxis) * 64.0 +
                                                    static_cast<double>(face.uv_v_fp)) /
                                                   2.0) >> 10;
                    lmuv.push_back({static_cast<double>(u), static_cast<double>(v)});
                }
                lm_base = static_cast<double>(face.brightness) + options.ambient;
            }

            bool clipped = false;
            for (const auto& v : view) {
                if (v.z < cam.near) {
                    clipped = true;
                    break;
                }
            }
            if (clipped) {
                std::vector<std::vector<V2>> groups;
                groups.push_back(uvs);
                if (page != nullptr) {
                    groups.push_back(lmuv);
                }
                clip_near(view, groups, cam.near);
                if (view.size() < 3) {
                    continue;
                }
                uvs = groups[0];
                if (page != nullptr) {
                    lmuv = groups[1];
                }
            }

            std::vector<Projected> pts;
            pts.reserve(view.size());
            bool ok = true;
            for (const auto& v : view) {
                Projected p;
                if (!cam.project_view(v, p.sx, p.sy, p.invz)) {
                    ok = false;
                    break;
                }
                pts.push_back(p);
            }
            if (!ok) {
                continue;
            }

            const Texture* tex =
                (options.textures && pool != nullptr) ? pool->get(face.texture) : nullptr;
            double shade = shade_face(face, options.lit);
            if (options.brightness && page == nullptr) {
                shade += face_brightness(face);
            }
            int cr = 0, cg = 0, cb = 0;
            face_color(face, cr, cg, cb);
            const Blend blend = face_blend(face, options.materials);

            if (blend.active) {
                Prepared item;
                V3 c{0.0, 0.0, 0.0};
                for (std::uint16_t index : face.indices) {
                    c = add(c, v3(mesh.vertices[index]));
                }
                c = mul(c, 1.0 / static_cast<double>(face.indices.size()));
                const V3 d = sub(c, cam.eye);
                item.distance = dot(d, d);
                item.pts = std::move(pts);
                item.uvs = std::move(uvs);
                item.tex = tex;
                item.shade = shade;
                item.r = cr;
                item.g = cg;
                item.b = cb;
                item.blend = blend;
                item.fit = fit;
                item.lm_page = page;
                item.lmuv = std::move(lmuv);
                item.lm_base = lm_base;
                blended.push_back(std::move(item));
                continue;
            }

            if (options.solid) {
                draw_poly(result.rgb, zbuf, options.width, options.height, pts, uvs, tex, shade,
                          cr, cg, cb, tex != nullptr, fit, blend, page, lmuv, lm_base,
                          options.cull);
            }
            if (options.wire) {
                for (std::size_t k = 0; k < pts.size(); ++k) {
                    draw_line(result.rgb, options.width, options.height, pts[k],
                              pts[(k + 1) % pts.size()]);
                }
            }
        }
    }

    if (options.solid && !blended.empty()) {
        std::stable_sort(blended.begin(), blended.end(),
                         [](const Prepared& a, const Prepared& b) {
                             return a.distance > b.distance;
                         });
        for (const auto& item : blended) {
            draw_poly(result.rgb, zbuf, options.width, options.height, item.pts, item.uvs,
                      item.tex, item.shade, item.r, item.g, item.b, item.tex != nullptr,
                      item.fit, item.blend, item.lm_page, item.lmuv, item.lm_base,
                      options.cull);
        }
    }
    return result;
}

} // namespace gm
