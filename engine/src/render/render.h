#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "res/bigfile.h"
#include "res/cache.h"
#include "res/sbc.h"
#include "res/sbw.h"
#include "res/sob.h"

namespace gm {

/// Double-precision vector: the reference renderer runs on Python floats, and
/// world coordinates of a level reach ~2000, where float32 rounding is visible
/// in lightmap sampling.
struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// An 8-bit texture resolved to RGB, top-down.
struct Texture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

/// Texture lookup over ``PICS/SBM.IDX`` then ``sprites/SBM.IDX``; the same
/// search order as ``gm.render.TexturePool``.  Blobs go through an
/// :class:`AssetCache`, so repeated faces share one copy.
class TexturePool {
public:
    explicit TexturePool(std::filesystem::path game, AssetCache* cache = nullptr);

    /// ``nullptr`` for "none"/unknown names, like the Python pool.
    const Texture* get(const std::string& name);

    std::size_t loads() const { return loads_; }
    std::size_t misses() const { return misses_; }

private:
    std::unique_ptr<Texture> load(const std::string& name);

    std::filesystem::path game_;
    AssetCache owned_;
    AssetCache* cache_ = nullptr;
    std::vector<std::filesystem::path> archives_;
    std::map<std::string, std::unique_ptr<Texture>> textures_;
    std::vector<std::uint8_t> palette_;
    bool palette_ready_ = false;
    std::size_t loads_ = 0;
    std::size_t misses_ = 0;
};

/// One polygon prepared for the renderer, in the frame of its mesh.
struct RenderFace {
    std::vector<std::uint16_t> indices;
    std::string texture;
    int material = 0;   // index into the material table (0 = opaque)
    std::uint8_t brightness = 0;
    int lightmap = -1;  // 0-based .sbc record, -1 = none
    Vec3f origin;
    Vec3f u;
    Vec3f v;
    Vec3f normal;
    float uv_scale = 1.0f; // 1/65536 for .sob planes
    // Resolved texture, keyed by the pool that produced it; avoids a string map
    // lookup per face per frame in the viewer.
    mutable const void* tex_pool = nullptr;
    mutable const Texture* tex_ptr = nullptr;
    std::int32_t uv_u_fp = 0;
    std::int32_t uv_v_fp = 0;
};

struct RenderMesh {
    Vec3d aabb_lo{1e30, 1e30, 1e30};
    Vec3d aabb_hi{-1e30, -1e30, -1e30};
    std::vector<Vec3d> vertices; // world frame
    std::vector<Vec3d> local;    // file frame
    std::vector<RenderFace> faces;
    // Level objects: ``local = (world - place_origin) / place_scale`` exactly as
    // gm.render.PlacedMesh, so UVs match the reference renderer bit for bit.
    bool placed = false;
    Vec3d place_origin;
    double place_scale = 1.0;
};

/// ``.sbw`` level -> world-space meshes (``origin + basis * local`` exactly as
/// ``gm.render.PlacedMesh``).
std::vector<RenderMesh> build_level_meshes(const SbwLevel& level, const Sbc* lightmaps);

/// ``.sob`` model -> one mesh in its own frame.
std::vector<RenderMesh> build_model_meshes(const Sob& model);

struct RenderOptions {
    int width = 480;
    int height = 480;
    float az = 35.0f;
    float el = 22.0f;
    float zoom = 1.0f;
    float fov = 45.0f;
    bool textures = true;
    bool solid = true;
    bool lit = true;
    bool wire = false;
    bool cull = false;
    bool materials = true;
    bool brightness = true;
    bool lightmaps = true;
    // gm/render.py samples lightmaps with the *world* vertex even for placed
    // objects; the engine compares in the polygon's local frame.  Off by
    // default (engine behaviour); on reproduces the Python viewer exactly.
    bool lightmap_world = false;
    bool sectors = true;
    bool background = true;
    float ambient = 0.0f;
    // Explicit camera (gm/render.py's pos/look): eye and target in world space.
    bool camera_override = false;
    Vec3d eye;
    Vec3d target;
    // Drop faces farther than this from the eye (0 = disabled), as in the viewer.
    float clip = 0.0f;
};

struct RenderResult {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

/// Software renderer: perspective camera, z-buffer, perspective-correct
/// texture mapping, lightmaps and the two-pass material blend of
/// ``gm/render.py``.
RenderResult render_scene(const std::vector<RenderMesh>& meshes,
                          const RenderOptions& options, TexturePool* pool,
                          const Sbc* lightmaps);

} // namespace gm
