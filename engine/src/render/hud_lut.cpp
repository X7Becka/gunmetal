#include "render/hud_lut.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>

namespace gm {
namespace {

bool set_error(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
    return false;
}

int clamp_level(int level) { return std::clamp(level, 0, kHudLutMaxLevel); }

// ------------------------------------------------------------- builder
// (`0x483290`; every constant below is from docs/LUT_BUILDER.md §2)

/// The fade scale: `0x4a2978` is the double `1/11` (`0x3FB745D1745D1746`).
constexpr int kHudLutScale = kHudLutIdentityLevel;

using PaletteSlot = std::array<int, 3>;

bool skip_slot(int index) {
    // 0x483250: the 20 slots outside the palette ramps.
    return index == 173 || index == 174 || index == 175 || index == 191 ||
           (index >= 240 && index <= 255);
}

/// `0x4228e0`: the weighted distance in the 6-bit colour space.
///
/// All three coefficients stay positive for 6-bit slots and for the targets the
/// builder can produce (at most `trunc(63 * 15/11) == 85`, so
/// `0x17f - (85 + 63) == 235 > 0`).  With 8-bit inputs the third coefficient
/// goes negative and this is no longer a distance (2834/4096 against the dump).
std::int64_t metric(int pr, int pg, int pb, int r, int g, int b) {
    const std::int64_t dr = pr - r;
    const std::int64_t dg = pg - g;
    const std::int64_t db = pb - b;
    const std::int64_t rsum = r + pr;
    return (rsum + 0x100) * dr * dr + 512 * dg * dg + (0x17f - rsum) * db * db;
}

/// Nearest slot in `1..254`; strict `<` on the running minimum, so equal
/// distances keep the lower index (`<=` loses 147 of 4096 bytes).
int nearest(const std::array<PaletteSlot, kHudLutEntries>& slots, int r, int g, int b) {
    const PaletteSlot& first = slots[1];
    std::int64_t best = metric(first[0], first[1], first[2], r, g, b);
    int best_index = 1;
    for (int index = 2; index < kHudLutEntries - 1; ++index) {
        const PaletteSlot& slot = slots[static_cast<std::size_t>(index)];
        const std::int64_t distance = metric(slot[0], slot[1], slot[2], r, g, b);
        if (distance < best) {
            best = distance;
            best_index = index;
        }
    }
    return best_index;
}

/// `double(k) * 1/11`: `0x48f9ef` loads `k`, multiplies by `0x4a2978`, and
/// stores the product back as a `double`, which is what the callee receives.
double level_factor(int level) {
    return static_cast<double>(level) * (1.0 / static_cast<double>(kHudLutScale));
}

/// `0x40e0e0` (`__ftol`) with x87 rounding control 11: truncation, not
/// rounding.  Components are non-negative, so this is a plain cast.
int scaled(int component, double factor) {
    return static_cast<int>(std::trunc(static_cast<double>(component) * factor));
}

} // namespace

bool load_hud_lut(const std::filesystem::path& file, HudLut& lut,
                  std::string* error, HudLutStats* stats) {
    lut = HudLut{};
    if (error != nullptr) {
        error->clear();
    }
    const std::string path = file.string();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return set_error(error, "hud_lut: cannot read '" + path + "'");
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return set_error(error, "hud_lut: cannot size '" + path + "'");
    }
    if (static_cast<std::uintmax_t>(size) != static_cast<std::uintmax_t>(kHudLutBytes)) {
        return set_error(error, "hud_lut: '" + path + "' is " + std::to_string(size) +
                                    " bytes, expected " +
                                    std::to_string(kHudLutBytes) + " (16 x 256)");
    }
    in.seekg(0, std::ios::beg);
    std::uint8_t bytes[kHudLutBytes] = {};
    in.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(kHudLutBytes));
    if (!in || static_cast<std::size_t>(in.gcount()) != kHudLutBytes) {
        return set_error(error, "hud_lut: short read from '" + path + "'");
    }

    for (int level = 0; level < kHudLutLevels; ++level) {
        for (int index = 0; index < kHudLutEntries; ++index) {
            lut.levels[static_cast<std::size_t>(level)][static_cast<std::size_t>(index)] =
                bytes[static_cast<std::size_t>(level) *
                          static_cast<std::size_t>(kHudLutEntries) +
                      static_cast<std::size_t>(index)];
        }
    }
    if (stats != nullptr) {
        stats->source = path;
        stats->bytes = kHudLutBytes;
        stats->levels = kHudLutLevels;
        stats->entries = kHudLutEntries;
    }
    return true;
}

bool hud_lut_skip_slot(int index) {
    return index >= 0 && index < kHudLutEntries && skip_slot(index);
}

bool build_hud_lut(std::span<const std::uint8_t> palette_rgba, HudLut& out,
                   std::string* error) {
    out = HudLut{};
    if (error != nullptr) {
        error->clear();
    }
    if (palette_rgba.data() == nullptr || palette_rgba.empty()) {
        return set_error(error, "hud_lut: build needs the 1024-byte palette, got none");
    }
    if (palette_rgba.size() != kHudLutPaletteBytes) {
        return set_error(error,
                         "hud_lut: palette is " + std::to_string(palette_rgba.size()) +
                             " bytes, expected " + std::to_string(kHudLutPaletteBytes) +
                             " (256 RGBA quads)");
    }

    // Six bits per component, exactly like the buffer at `0x83d320`: the loader
    // shifts the BMP palette right by two (`0x47f0b4`) and consumers clamp to
    // `0x3f`.  The metric constants are only meaningful in this range.
    std::array<PaletteSlot, kHudLutEntries> slots{};
    for (int index = 0; index < kHudLutEntries; ++index) {
        const std::size_t at = static_cast<std::size_t>(index) * 4u;
        slots[static_cast<std::size_t>(index)] = PaletteSlot{
            static_cast<int>(palette_rgba[at] >> 2),
            static_cast<int>(palette_rgba[at + 1] >> 2),
            static_cast<int>(palette_rgba[at + 2] >> 2)};
    }

    for (int level = 0; level < kHudLutLevels; ++level) {
        const double factor = level_factor(level);
        auto& table = out.levels[static_cast<std::size_t>(level)];
        for (int index = 0; index < kHudLutEntries; ++index) {
            if (skip_slot(index)) {
                table[static_cast<std::size_t>(index)] =
                    static_cast<std::uint8_t>(index); // 0x4832b4: dst[i] = i
                continue;
            }
            const PaletteSlot& slot = slots[static_cast<std::size_t>(index)];
            const int r = scaled(slot[0], factor);
            const int g = scaled(slot[1], factor);
            const int b = scaled(slot[2], factor);
            table[static_cast<std::size_t>(index)] =
                static_cast<std::uint8_t>(nearest(slots, r, g, b));
        }
    }
    return true;
}

std::uint8_t map(const HudLut& lut, int level, std::uint8_t index) {
    return lut.levels[static_cast<std::size_t>(clamp_level(level))][index];
}

HudLutLevel level_report(const HudLut& lut, int level,
                         std::span<const std::uint8_t> palette) {
    const auto& table = lut.levels[static_cast<std::size_t>(clamp_level(level))];
    // Two indices may name the same colour (`0` and `1` are both black in
    // GAMEPAL), so "colour identity" is the stronger of the two counts and is
    // only meaningful with a whole palette of RGBA quads.
    const bool have_palette = palette.size() == kHudLutPaletteBytes;
    HudLutLevel report;
    report.min = 255;
    report.max = 0;
    int colour = 0;
    for (int index = 0; index < kHudLutEntries; ++index) {
        const std::uint8_t entry = table[static_cast<std::size_t>(index)];
        if (entry == static_cast<std::uint8_t>(index)) {
            ++report.identity;
        }
        if (entry == 255) {
            ++report.white;
        }
        report.min = std::min(report.min, static_cast<int>(entry));
        report.max = std::max(report.max, static_cast<int>(entry));
        if (have_palette) {
            const std::size_t from = static_cast<std::size_t>(index) * 4u;
            const std::size_t to = static_cast<std::size_t>(entry) * 4u;
            if (palette[from] == palette[to] && palette[from + 1] == palette[to + 1] &&
                palette[from + 2] == palette[to + 2]) {
                ++colour;
            }
        }
    }
    report.colour_identity = have_palette ? colour : -1;
    return report;
}

std::string describe(const HudLut& lut, std::span<const std::uint8_t> palette) {
    std::string text;
    for (int level = 0; level < kHudLutLevels; ++level) {
        const HudLutLevel report = level_report(lut, level, palette);
        char colour[32] = {};
        if (report.colour_identity < 0) {
            std::snprintf(colour, sizeof(colour), "n/a");
        } else {
            std::snprintf(colour, sizeof(colour), "%3d/%d", report.colour_identity,
                          kHudLutEntries);
        }
        char line[128] = {};
        std::snprintf(line, sizeof(line),
                      "level %2d: identity %3d/%d colour-identity %s white %d min %d max %d",
                      level, report.identity, kHudLutEntries, colour, report.white,
                      report.min, report.max);
        text += line;
        if (level + 1 != kHudLutLevels) {
            text += '\n';
        }
    }
    return text;
}

bool build_model_lut(HudLut& lut, int max_level, std::string* error) {
    lut = HudLut{};
    if (error != nullptr) {
        error->clear();
    }
    if (max_level < 1 || max_level > kHudLutMaxLevel) {
        return set_error(error, "hud_lut: model max_level " + std::to_string(max_level) +
                                    " outside 1.." + std::to_string(kHudLutMaxLevel));
    }
    const std::int64_t scale = max_level;
    const std::int64_t half = scale / 2;
    for (int level = 0; level < kHudLutLevels; ++level) {
        // Levels above the scale keep its identity, exactly like `fade_lut`
        // in render/upscale.h: the engine build is `0x483290(dst, k/11, 1)`,
        // so `k > 11` is not a second, brighter table in the model.
        const std::int64_t factor = std::min(level, max_level);
        for (int value = 0; value < kHudLutEntries; ++value) {
            lut.levels[static_cast<std::size_t>(level)][static_cast<std::size_t>(value)] =
                static_cast<std::uint8_t>(
                    (static_cast<std::int64_t>(value) * factor + half) / scale);
        }
    }
    return true;
}

} // namespace gm
