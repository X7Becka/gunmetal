// Depth fog (M4.1.6, slice 2): the software `lighttable` of docs/FOG.md.
//
// Everything here follows `tools/re/fog.py`, which in turn pins every constant
// to an instruction of `gundx.exe` 1.36.  The code is a clean-room port of the
// *behaviour* (formula, row bands, clamp), not of the decompiled listing.

#include "render/fog.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace gm {
namespace {

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

bool set_error(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
    return false;
}

bool finite(Vec3d v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Quantise a 6-bit colour back to a palette index the way `tools/re/fog.py`
/// substitutes the engine's run-time dither tables: the first palette entry
/// with exactly this colour, otherwise the nearest by squared distance with
/// ties going to the lowest index.
std::uint8_t nearest_index(const std::uint8_t pal6[256][3], int r, int g,
                           int b) {
    for (int i = 0; i < kFogColumns; ++i) {
        if (pal6[i][0] == r && pal6[i][1] == g && pal6[i][2] == b) {
            return static_cast<std::uint8_t>(i);
        }
    }
    int best = 0;
    int best_distance = -1;
    for (int i = 0; i < kFogColumns; ++i) {
        const int dr = r - static_cast<int>(pal6[i][0]);
        const int dg = g - static_cast<int>(pal6[i][1]);
        const int db = b - static_cast<int>(pal6[i][2]);
        const int distance = dr * dr + dg * dg + db * db;
        if (best_distance < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return static_cast<std::uint8_t>(best);
}

/// One channel of the `0x4201fe` lerp, `row < 0x40`.
int lerp_channel(int channel6, int row) {
    return std::min(0x3F, (channel6 * (0x40 - row) + row * 0x40) >> 6);
}

} // namespace

double fog_z(const FogParams& params, Vec3d eye, Vec3d e2, Vec3d p) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(params.a0) || !(params.a0 > 0.0)) {
        return nan;
    }
    if (!finite(eye) || !finite(e2) || !finite(p)) {
        return nan;
    }
    const double depth = e2.x * (p.x - eye.x) + e2.y * (p.y - eye.y) +
                         e2.z * (p.z - eye.z);
    const double z = std::fabs((64.0 / params.a0) * depth);
    return std::isfinite(z) ? z : nan;
}

bool fog_z(const FogParams& params, Vec3d eye, Vec3d e2, Vec3d p, double& z,
           std::string* error) {
    if (!std::isfinite(params.a0)) {
        return set_error(error, "a0 is not finite");
    }
    if (!(params.a0 > 0.0)) {
        return set_error(error, "a0 must be positive (ctx+0xa0 = 0.8*W*k), got " +
                                    std::to_string(params.a0));
    }
    if (!finite(eye)) {
        return set_error(error, "eye has a non-finite coordinate");
    }
    if (!finite(e2)) {
        return set_error(error, "e2 (the view axis) has a non-finite coordinate");
    }
    if (!finite(p)) {
        return set_error(error, "p has a non-finite coordinate");
    }
    const double depth = e2.x * (p.x - eye.x) + e2.y * (p.y - eye.y) +
                         e2.z * (p.z - eye.z);
    const double value = std::fabs((64.0 / params.a0) * depth);
    if (!std::isfinite(value)) {
        return set_error(error, "z is not finite (coordinate product overflowed)");
    }
    z = value;
    clear_error(error);
    return true;
}

int fog_level(double z) {
    if (!std::isfinite(z)) {
        return kFogLevelMin;
    }
    // The engine clamps in this order: max(...,4) then min(...,0x3f).  Both
    // bounds are applied in `double` first so that a huge |z| never reaches the
    // int conversion.
    const double level = 63.0 - std::trunc(z);
    if (level <= static_cast<double>(kFogLevelMin)) {
        return kFogLevelMin;
    }
    if (level >= static_cast<double>(kFogLevelMax)) {
        return kFogLevelMax;
    }
    return static_cast<int>(level);
}

bool fog_level(double z, int& level, std::string* error) {
    if (!std::isfinite(z)) {
        return set_error(error, "z is not finite (fog_z refused or overflowed)");
    }
    level = fog_level(z);
    clear_error(error);
    return true;
}

bool build_fog_table(const std::uint8_t pal6[256][3], FogTable& table,
                     std::string* error) {
    if (pal6 == nullptr) {
        return set_error(error, "pal6 is null (expected 256 6-bit RGB triples)");
    }
    for (int i = 0; i < kFogColumns; ++i) {
        for (int channel = 0; channel < 3; ++channel) {
            if (pal6[i][channel] > 0x3F) {
                return set_error(error,
                                 "pal6[" + std::to_string(i) + "][" +
                                     std::to_string(channel) + "] = " +
                                     std::to_string(pal6[i][channel]) +
                                     " is outside 0..63 (the engine palette "
                                     "is 6-bit, docs/FOG.md §4)");
            }
        }
    }

    for (int row = 0; row < kFogRows; ++row) {
        std::uint8_t* line = table.data.data() +
                             static_cast<std::size_t>(row) * kFogColumns;
        if (row >= kFogRowIdentity) {
            // `0x4204e2`: the material LUTs above the fog band are the byte
            // identity; the fog pass never asks for these rows.
            for (int col = 0; col < kFogColumns; ++col) {
                line[col] = static_cast<std::uint8_t>(col);
            }
            continue;
        }
        if (row >= kFogRowWhite) {
            const std::uint8_t white =
                nearest_index(pal6, 0x3F, 0x3F, 0x3F);
            for (int col = 0; col < kFogColumns; ++col) {
                line[col] = white;
            }
            continue;
        }
        for (int col = 0; col < kFogColumns; ++col) {
            const int r = lerp_channel(pal6[col][0], row);
            const int g = lerp_channel(pal6[col][1], row);
            const int b = lerp_channel(pal6[col][2], row);
            line[col] = nearest_index(pal6, r, g, b);
        }
    }
    clear_error(error);
    return true;
}

std::uint8_t fog_pixel(const FogTable& table, int level, std::uint8_t index) {
    if (level < 0 || level >= kFogRows) {
        return index;
    }
    return table.data[static_cast<std::size_t>(level) * kFogColumns + index];
}

bool fog_pixel(const FogTable& table, int level, std::uint8_t index,
               std::uint8_t& out, std::string* error) {
    if (level < 0 || level >= kFogRows) {
        return set_error(error, "level " + std::to_string(level) +
                                    " is outside 0..255 (lighttable row)");
    }
    out = table.data[static_cast<std::size_t>(level) * kFogColumns + index];
    clear_error(error);
    return true;
}

void fog_frame(const FogTable& table, int level,
               std::span<std::uint8_t> framebuffer) {
    if (level < 0 || level >= kFogRows) {
        return;
    }
    for (std::uint8_t& pixel : framebuffer) {
        pixel = fog_pixel(table, level, pixel);
    }
}

bool fog_frame(const FogTable& table, int level,
               std::span<std::uint8_t> framebuffer, std::string* error) {
    if (level < 0 || level >= kFogRows) {
        return set_error(error, "level " + std::to_string(level) +
                                    " is outside 0..255 (lighttable row)");
    }
    for (std::uint8_t& pixel : framebuffer) {
        pixel = fog_pixel(table, level, pixel);
    }
    clear_error(error);
    return true;
}

} // namespace gm
