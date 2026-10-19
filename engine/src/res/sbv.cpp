#include "res/sbv.h"

#include <cstring>

namespace gm {
namespace {

constexpr std::uint32_t kMaxWidth = 320;
constexpr char kMagic[4] = {'S', 'B', 'V', '0'};

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

std::optional<Sbv> parse_sbv(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < 16 || std::memcmp(data.data(), kMagic, 4) != 0) {
        error = "not an SBV0 file";
        return std::nullopt;
    }
    Sbv clip;
    clip.width = read_u32(data.data() + 4);
    clip.height = read_u32(data.data() + 8);
    clip.frame_count = read_u32(data.data() + 12);
    if (clip.width > kMaxWidth) {
        error = "implausible width " + std::to_string(clip.width);
        return std::nullopt;
    }
    const std::size_t need = 16 + 8 * static_cast<std::size_t>(clip.frame_count);
    if (data.size() < need) {
        error = "frame table truncated: want " + std::to_string(need) + ", have " +
                std::to_string(data.size());
        return std::nullopt;
    }
    clip.data_start = need;
    clip.table.resize(clip.frame_count);
    for (std::uint32_t i = 0; i < clip.frame_count; ++i) {
        const std::uint8_t* entry = data.data() + 16 + 8 * static_cast<std::size_t>(i);
        clip.table[i].timestamp_ms = read_u32(entry);
        clip.table[i].end_offset = read_u32(entry + 4);
    }
    clip.file_size = data.size();
    return clip;
}

bool sbv_offsets_valid(const Sbv& clip) {
    if (clip.table.empty()) {
        return clip.data_start == clip.file_size;
    }
    return clip.data_start + clip.table.back().end_offset == clip.file_size;
}

} // namespace gm
