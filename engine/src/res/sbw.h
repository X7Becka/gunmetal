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

struct SbwEffectRef {
    std::string name;
    std::uint32_t kind = 0;
    std::uint32_t start = 0;
    std::vector<std::uint16_t> ids;
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

} // namespace gm
