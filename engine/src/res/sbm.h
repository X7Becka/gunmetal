#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SbmLevel {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // palette indices
};

/// Gunmetal ``.sbm`` 8-bit palettised texture (mip chain, level 0 first).
/// Layout and the byte-exact mip-size rule: docs/FORMATS.md §2.
struct Sbm {
    int width = 0;
    int height = 0;
    int levels = 0;
    std::uint32_t flags = 0;
    std::vector<SbmLevel> mips;
    std::vector<std::uint8_t> extra; // bytes after the mip chain
};

std::optional<Sbm> parse_sbm(std::span<const std::uint8_t> data, std::string& error);

/// Palette indices -> RGBA8, row-major top-down.
std::vector<std::uint8_t> sbm_to_rgba(const SbmLevel& level,
                                      std::span<const std::uint8_t> palette);

} // namespace gm
