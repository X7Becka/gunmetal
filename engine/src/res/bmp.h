#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct BmpImage {
    int width = 0;
    int height = 0;
    int bpp = 0;
    bool top_down = false;
    std::vector<std::uint8_t> palette; // RGBA quads
    std::vector<std::uint8_t> pixels;  // top-down palette indices
};

/// Parse an 8-bit palettised BMP (the only variant the game ships).
std::optional<BmpImage> parse_bmp(std::span<const std::uint8_t> data, std::string& error);

/// The shared 256-colour palette the engine loads from ``FRONTEND\GAMEPAL.BMP``
/// (``0x44af88`` → ``0x47efe0`` → ``0x83d320``), or — for installs without the
/// FRONTEND archive — the first loose BMP carrier that exists (its unused slots
/// are magenta, see ``gm/palette.py``).
std::optional<std::vector<std::uint8_t>> installed_palette(const std::filesystem::path& game,
                                                           std::string& error);

} // namespace gm
