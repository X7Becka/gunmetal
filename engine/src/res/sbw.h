#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SbwTexRef {
    std::string name;
    std::uint16_t flag = 0;
    std::uint32_t value = 0;
};

/// One entry of the ``.sbw`` effect table (table 2) — the animation records a
/// polygon selects with a **negative** texture index (``-k`` → entry ``k-1``,
/// docs/EFFECTS.md).
struct SbwEffectRef {
    std::string name;              ///< `+0x4c` (16 bytes)
    std::uint32_t mode = 0;        ///< `+0x00`: 0 one-shot, 1 cycle, 100/101/102 special
    std::uint32_t period = 0;      ///< `+0x04`: cycle length in engine ticks (91 Hz)
    std::vector<std::uint16_t> ids; ///< `+0x0c`: frame texture ids, 1-based
};

struct SbwObjectRef {
    std::uint32_t id = 0;
    std::array<std::uint8_t, 96> header{};
};

struct SbwSectorRef {
    std::uint16_t kind = 0;
    std::uint8_t sub = 0;
    std::vector<SbwObjectRef> objects;
};

/// Tables 1-3 of a ``.sbw`` level (textures, effects, sectors/objects).
/// Geometry (stage 2) follows ``header_end``; see docs/FORMATS.md §3.
struct SbwHeader {
    std::vector<SbwTexRef> textures;
    std::vector<SbwEffectRef> effects;
    std::vector<SbwSectorRef> sectors;
    std::size_t header_end = 0;
    std::size_t object_count = 0;
};

std::optional<SbwHeader> parse_sbw_header(std::span<const std::uint8_t> data, std::string& error);

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct SbwPoly {
    std::uint16_t texture = 0;
    std::uint16_t lightmap = 0;
    std::uint16_t material = 0;
    std::uint16_t overlay = 0;
    std::uint16_t f7a = 0;
    std::uint8_t brightness = 0;
    std::vector<std::uint16_t> indices;
    std::uint16_t scroll_u = 0;
    std::uint16_t scroll_v = 0;
    std::uint16_t u_scale = 0;
    std::uint16_t v_scale = 0;
    std::uint16_t uv_u = 0;
    std::uint16_t uv_v = 0;
    std::uint32_t uv_u_fp = 0;
    std::uint32_t uv_v_fp = 0;
    Vec3f centre;
    Vec3f normal;
    Vec3f u_axis;
    Vec3f v_axis;
    Vec3f corner_a;
    Vec3f corner_b;
    std::uint16_t f74 = 0;
    std::uint16_t f76 = 0;
    std::uint16_t handle = 0;
    std::uint32_t reserved = 0;
    std::uint32_t flags = 0;
    std::uint16_t anim0 = 0;
    std::uint16_t anim1 = 0;
    std::uint16_t anim2 = 0;
    std::array<std::uint8_t, 112> raw{};
};

struct SbwGeometry {
    std::uint16_t n_verts = 0;
    std::uint16_t n_polys = 0;
    std::vector<Vec3f> vertices;
    float radius = 0.0f;
    std::uint8_t f75 = 0;
    std::uint8_t f74 = 0;
    std::uint32_t f7c = 0;
    std::uint32_t f78 = 0;
    Vec3f bounds;
    std::array<std::uint8_t, 24> reserved{};
    std::array<std::uint8_t, 50> tail{};
    std::vector<SbwPoly> polys;
};

/// Full level: header tables + stage-2 geometry (sector-major order).
struct SbwLevel {
    SbwHeader header;
    std::vector<SbwGeometry> geometry;
    std::size_t file_end = 0;
    std::size_t total_vertices = 0;
    std::size_t total_polygons = 0;
};

std::optional<SbwLevel> parse_sbw(std::span<const std::uint8_t> data, std::string& error);

} // namespace gm
