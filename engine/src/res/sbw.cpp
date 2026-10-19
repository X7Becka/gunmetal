#include "res/sbw.h"

#include "core/bytes.h"

#include <cstring>

namespace gm {
namespace {

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
    header.effects.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SbwEffectRef effect;
        if (!read_cstr(data, offset, effect.name) || offset + 12 > data.size()) {
            error = "truncated effect table at entry " + std::to_string(i);
            return std::nullopt;
        }
        effect.kind = rd32(data.data() + offset);
        effect.start = rd32(data.data() + offset + 4);
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

} // namespace gm
