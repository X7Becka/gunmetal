#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "res/sbw.h" // Vec3f

namespace gm {

struct SobFace {
    std::array<std::string, 8> textures;
    int flags = 0;
    std::vector<int> indices;
    Vec3f origin;
    Vec3f u;
    Vec3f v;
    Vec3f normal;
    Vec3f spare;

    const std::string& texture() const { return textures[0]; }
};

/// Gunmetal ``.sob`` model: a flat token stream (no line semantics).
/// Grammar and the optional ``-1`` sentinel: gm/sob.py.
struct Sob {
    std::vector<Vec3f> vertices;
    std::vector<SobFace> faces;
};

std::optional<Sob> parse_sob(std::span<const std::uint8_t> data, std::string& error);

/// Linear morph between two equal-topology poses (engine 0x433e00).  Vertices
/// and the per-face planes are interpolated; textures/indices come from *a*.
std::optional<Sob> morph_sob(const Sob& a, const Sob& b, double alpha,
                             std::string& error);

} // namespace gm
