#include "res/sbm.h"

#include "core/bytes.h"

#include <algorithm>

namespace gm {

std::optional<Sbm> parse_sbm(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < 12) {
        error = "blob too short for an .sbm header";
        return std::nullopt;
    }
    const std::uint8_t log2w = data[4];
    const std::uint8_t log2h = data[5];
    const std::uint8_t lvlm1 = data[6];
    if (log2w > 12 || log2h > 12) {
        error = "implausible dimensions 2^" + std::to_string(log2w) + " x 2^" +
                std::to_string(log2h);
        return std::nullopt;
    }
    if (lvlm1 > 12) {
        error = "implausible mip count " + std::to_string(lvlm1 + 1);
        return std::nullopt;
    }
    // Level k is `1 << (log2 - k)`; a chain longer than the smaller dimension
    // would shift by a negative amount (UB, and a hard crash in a checked
    // build).  gm/sbm.py rejects the same files through expected_size().
    if (lvlm1 > log2w || lvlm1 > log2h) {
        error = "mip count exceeds texture dimensions";
        return std::nullopt;
    }

    Sbm sbm;
    sbm.width = 1 << log2w;
    sbm.height = 1 << log2h;
    sbm.levels = lvlm1 + 1;
    sbm.flags = rd32(data.data() + 8);

    std::size_t offset = 0;
    for (int k = 0; k < sbm.levels; ++k) {
        SbmLevel level;
        level.width = std::max(1, 1 << (log2w - k));
        level.height = std::max(1, 1 << (log2h - k));
        const std::size_t bytes = static_cast<std::size_t>(level.width) *
                                  static_cast<std::size_t>(level.height);
        if (12 + offset + bytes > data.size()) {
            error = "truncated mip chain at level " + std::to_string(k);
            return std::nullopt;
        }
        level.pixels.assign(data.begin() + static_cast<std::ptrdiff_t>(12 + offset),
                            data.begin() + static_cast<std::ptrdiff_t>(12 + offset + bytes));
        offset += bytes;
        sbm.mips.push_back(std::move(level));
    }
    if (12 + offset < data.size()) {
        sbm.extra.assign(data.begin() + static_cast<std::ptrdiff_t>(12 + offset), data.end());
    }
    return sbm;
}

std::vector<std::uint8_t> sbm_to_rgba(const SbmLevel& level,
                                      std::span<const std::uint8_t> palette) {
    std::vector<std::uint8_t> rgba(level.pixels.size() * 4, 0);
    const std::size_t colors = palette.size() / 4;
    for (std::size_t i = 0; i < level.pixels.size(); ++i) {
        const std::size_t index = level.pixels[i];
        if (index < colors) {
            rgba[i * 4 + 0] = palette[index * 4 + 0];
            rgba[i * 4 + 1] = palette[index * 4 + 1];
            rgba[i * 4 + 2] = palette[index * 4 + 2];
            rgba[i * 4 + 3] = 255;
        }
    }
    return rgba;
}

} // namespace gm
