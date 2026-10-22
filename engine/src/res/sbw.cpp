#include "res/sbw.h"

#include "core/bytes.h"

#include <cstring>

namespace gm {
namespace {

float rd_f32(const std::uint8_t* p) {
    float value = 0.0f;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

Vec3f rd_vec3(const std::uint8_t* p) {
    return Vec3f{rd_f32(p), rd_f32(p + 4), rd_f32(p + 8)};
}

bool read_u32(std::span<const std::uint8_t> data, std::size_t& offset, std::uint32_t& out) {
    if (offset + 4 > data.size()) {
        return false;
    }
    out = rd32(data.data() + offset);
    offset += 4;
    return true;
}

bool read_cstr(std::span<const std::uint8_t> data, std::size_t& offset, std::string& out) {
    std::size_t end = offset;
    while (end < data.size() && data[end] != 0) {
        ++end;
    }
    if (end >= data.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data.data() + offset), end - offset);
    offset = end + 1;
    return true;
}

} // namespace

std::optional<SbwHeader> parse_sbw_header(std::span<const std::uint8_t> data, std::string& error) {
    SbwHeader header;
    std::size_t offset = 0;

    std::uint32_t count = 0;
    if (!read_u32(data, offset, count)) {
        error = "truncated .sbw (texture count)";
        return std::nullopt;
    }
    // Smallest possible entry: one NUL name byte + flag + value.  Bound the
    // counter by the remaining input *before* reserving: a 4-byte blob with
    // count 0xFFFFFFFF must not ask the allocator for gigabytes.
    if (static_cast<std::size_t>(count) > (data.size() - offset) / 7) {
        error = "implausible texture count " + std::to_string(count);
        return std::nullopt;
    }
    header.textures.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SbwTexRef tex;
        if (!read_cstr(data, offset, tex.name) || offset + 6 > data.size()) {
            error = "truncated texture table at entry " + std::to_string(i);
            return std::nullopt;
        }
        tex.flag = rd16(data.data() + offset);
        tex.value = rd32(data.data() + offset + 2);
        offset += 6;
        header.textures.push_back(std::move(tex));
    }

    if (!read_u32(data, offset, count)) {
        error = "truncated .sbw (effect count)";
        return std::nullopt;
    }
    // Smallest entry: one NUL name byte + kind + start + zero ids.
    if (static_cast<std::size_t>(count) > (data.size() - offset) / 13) {
        error = "implausible effect count " + std::to_string(count);
        return std::nullopt;
    }
    header.effects.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SbwEffectRef effect;
        if (!read_cstr(data, offset, effect.name) || offset + 12 > data.size()) {
            error = "truncated effect table at entry " + std::to_string(i);
            return std::nullopt;
        }
        effect.mode = rd32(data.data() + offset);
        effect.period = rd32(data.data() + offset + 4);
        const std::uint32_t n = rd32(data.data() + offset + 8);
        offset += 12;
        if (offset + static_cast<std::size_t>(n) * 2 > data.size()) {
            error = "truncated effect ids at entry " + std::to_string(i);
            return std::nullopt;
        }
        effect.ids.resize(n);
        for (std::uint32_t k = 0; k < n; ++k) {
            effect.ids[k] = rd16(data.data() + offset + k * 2);
        }
        offset += static_cast<std::size_t>(n) * 2;
        header.effects.push_back(std::move(effect));
    }

    if (!read_u32(data, offset, count)) {
        error = "truncated .sbw (sector count)";
        return std::nullopt;
    }
    // Smallest sector: object count + kind + sub, no objects.
    if (static_cast<std::size_t>(count) > (data.size() - offset) / 7) {
        error = "implausible sector count " + std::to_string(count);
        return std::nullopt;
    }
    header.sectors.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 7 > data.size()) {
            error = "truncated sector table at sector " + std::to_string(i);
            return std::nullopt;
        }
        SbwSectorRef sector;
        const std::uint32_t objects = rd32(data.data() + offset);
        sector.kind = rd16(data.data() + offset + 4);
        sector.sub = data[offset + 6];
        offset += 7;
        if (offset + static_cast<std::size_t>(objects) * 100 > data.size()) {
            error = "truncated objects in sector " + std::to_string(i);
            return std::nullopt;
        }
        sector.objects.resize(objects);
        for (std::uint32_t k = 0; k < objects; ++k) {
            sector.objects[k].id = rd32(data.data() + offset);
            std::memcpy(sector.objects[k].header.data(), data.data() + offset + 4,
                        sector.objects[k].header.size());
            offset += 4 + 96;
        }
        header.object_count += objects;
        header.sectors.push_back(std::move(sector));
    }

    header.header_end = offset;
    return header;
}


std::optional<SbwLevel> parse_sbw(std::span<const std::uint8_t> data, std::string& error) {
    SbwLevel level;
    auto header = parse_sbw_header(data, error);
    if (!header) {
        return std::nullopt;
    }
    level.header = std::move(*header);
    std::size_t offset = level.header.header_end;

    for (std::size_t s = 0; s < level.header.sectors.size(); ++s) {
        const auto& sector = level.header.sectors[s];
        for (std::size_t o = 0; o < sector.objects.size(); ++o) {
            if (offset + 4 > data.size()) {
                error = "truncated object header in sector " + std::to_string(s);
                return std::nullopt;
            }
            SbwGeometry geometry;
            geometry.n_verts = rd16(data.data() + offset);
            geometry.n_polys = rd16(data.data() + offset + 2);
            offset += 4;
            if (offset + static_cast<std::size_t>(geometry.n_verts) * 12 > data.size()) {
                error = "truncated vertices in sector " + std::to_string(s) + " object " +
                        std::to_string(o);
                return std::nullopt;
            }
            geometry.vertices.reserve(geometry.n_verts);
            for (std::uint16_t v = 0; v < geometry.n_verts; ++v) {
                geometry.vertices.push_back(rd_vec3(data.data() + offset));
                offset += 12;
            }
            if (offset + 50 > data.size()) {
                error = "truncated object tail in sector " + std::to_string(s);
                return std::nullopt;
            }
            std::memcpy(geometry.tail.data(), data.data() + offset, geometry.tail.size());
            geometry.radius = rd_f32(data.data() + offset);
            geometry.f75 = data[offset + 4];
            geometry.f74 = data[offset + 5];
            geometry.f7c = rd32(data.data() + offset + 6);
            geometry.f78 = rd32(data.data() + offset + 10);
            geometry.bounds = rd_vec3(data.data() + offset + 14);
            std::memcpy(geometry.reserved.data(), data.data() + offset + 26,
                        geometry.reserved.size());
            offset += 50;

            // Smallest polygon: 12-byte header + 0 indices + 112-byte tail.
            // Bound the counter before reserve so a 16-bit count cannot
            // transiently allocate ~20 MiB from a few input bytes.
            if (static_cast<std::size_t>(geometry.n_polys) > (data.size() - offset) / 124) {
                error = "implausible polygon count in sector " + std::to_string(s) +
                        " object " + std::to_string(o);
                return std::nullopt;
            }
            geometry.polys.reserve(geometry.n_polys);
            for (std::uint16_t k = 0; k < geometry.n_polys; ++k) {
                if (offset + 12 > data.size()) {
                    error = "truncated polygon in sector " + std::to_string(s);
                    return std::nullopt;
                }
                SbwPoly poly;
                poly.texture = rd16(data.data() + offset);
                poly.lightmap = rd16(data.data() + offset + 2);
                poly.material = rd16(data.data() + offset + 4);
                poly.overlay = rd16(data.data() + offset + 6);
                poly.f7a = rd16(data.data() + offset + 8);
                poly.brightness = data[offset + 10];
                const std::uint8_t n_idx = data[offset + 11];
                offset += 12;
                if (offset + static_cast<std::size_t>(n_idx) * 2 + 112 > data.size()) {
                    error = "truncated polygon tail in sector " + std::to_string(s);
                    return std::nullopt;
                }
                poly.indices.resize(n_idx);
                for (std::uint8_t i = 0; i < n_idx; ++i) {
                    poly.indices[i] = rd16(data.data() + offset + i * 2);
                }
                offset += static_cast<std::size_t>(n_idx) * 2;
                const std::uint8_t* tail = data.data() + offset;
                poly.scroll_u = rd16(tail + 0);
                poly.scroll_v = rd16(tail + 2);
                poly.u_scale = rd16(tail + 4);
                poly.v_scale = rd16(tail + 6);
                poly.uv_u = rd16(tail + 8);
                poly.uv_v = rd16(tail + 10);
                poly.uv_u_fp = rd32(tail + 12);
                poly.uv_v_fp = rd32(tail + 16);
                poly.centre = rd_vec3(tail + 20);
                poly.normal = rd_vec3(tail + 32);
                poly.u_axis = rd_vec3(tail + 44);
                poly.v_axis = rd_vec3(tail + 56);
                poly.corner_a = rd_vec3(tail + 68);
                poly.corner_b = rd_vec3(tail + 80);
                poly.f74 = rd16(tail + 92);
                poly.f76 = rd16(tail + 94);
                poly.handle = rd16(tail + 96);
                poly.reserved = rd32(tail + 98);
                poly.flags = rd32(tail + 102);
                poly.anim0 = rd16(tail + 106);
                poly.anim1 = rd16(tail + 108);
                poly.anim2 = rd16(tail + 110);
                std::memcpy(poly.raw.data(), tail, poly.raw.size());
                offset += 112;
                geometry.polys.push_back(std::move(poly));
            }
            level.total_vertices += geometry.n_verts;
            level.total_polygons += geometry.n_polys;
            level.geometry.push_back(std::move(geometry));
        }
    }

    level.file_end = offset;
    return level;
}

} // namespace gm
