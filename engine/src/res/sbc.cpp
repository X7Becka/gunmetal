#include "res/sbc.h"

#include "core/bytes.h"

#include <cstring>

namespace gm {

std::optional<Sbc> parse_sbc(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < 8) {
        error = "blob too short for .sbc";
        return std::nullopt;
    }
    Sbc sbc;
    sbc.magic.assign(reinterpret_cast<const char*>(data.data()), 4);
    if (sbc.magic != "SBC0" && sbc.magic != "SBC1") {
        error = "unknown .sbc magic '" + sbc.magic + "'";
        return std::nullopt;
    }
    const std::uint32_t count = rd32(data.data() + 4);
    if (count > 0x10000) {
        error = "implausible .sbc record count " + std::to_string(count);
        return std::nullopt;
    }

    std::size_t offset = 8;
    sbc.records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 4 > data.size()) {
            error = "truncated .sbc record header at " + std::to_string(i);
            return std::nullopt;
        }
        const std::uint32_t desc = rd32(data.data() + offset);
        offset += 4;
        const std::uint32_t w_log = desc & 0xFFFFu;
        const std::uint32_t h_log = desc >> 16;
        if (w_log > 12 || h_log > 12) {
            error = "implausible .sbc dimensions at record " + std::to_string(i);
            return std::nullopt;
        }
        SbcRecord record;
        record.width = 1 << w_log;
        record.height = 1 << h_log;
        const std::size_t bytes = static_cast<std::size_t>(record.width) *
                                  static_cast<std::size_t>(record.height);
        if (offset + bytes > data.size()) {
            error = "truncated .sbc pixels at record " + std::to_string(i);
            return std::nullopt;
        }
        record.pixels.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                             data.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
        offset += bytes;
        sbc.records.push_back(std::move(record));
    }
    sbc.file_end = offset;
    return sbc;
}

} // namespace gm
