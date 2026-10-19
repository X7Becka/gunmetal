#include "res/sou.h"

#include "core/bytes.h"

#include <cstring>

namespace gm {

std::optional<SouSample> parse_sou(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < 4) {
        error = "blob too short for .sou";
        return std::nullopt;
    }
    SouSample sample;
    sample.magic.assign(reinterpret_cast<const char*>(data.data()), 4);
    if (sample.magic == "SOU0" || sample.magic == "SOU1") {
        if (data.size() < 16) {
            error = "truncated .sou header";
            return std::nullopt;
        }
        sample.rate = rd32(data.data() + 4);
        sample.flags = rd32(data.data() + 8);
        sample.length = rd32(data.data() + 12);
        if (16 + static_cast<std::size_t>(sample.length) > data.size()) {
            error = "truncated .sou payload";
            return std::nullopt;
        }
        sample.payload.assign(data.begin() + 16,
                              data.begin() + static_cast<std::ptrdiff_t>(16 + sample.length));
        return sample;
    }
    if (sample.magic == "SOU2") {
        if (data.size() < 20) {
            error = "truncated SOU2 header";
            return std::nullopt;
        }
        sample.rate = rd32(data.data() + 4);
        sample.flags = rd32(data.data() + 8);
        sample.length = rd32(data.data() + 12);
        sample.units = rd32(data.data() + 16);
        if (20 + static_cast<std::size_t>(sample.units) * 4 > data.size()) {
            error = "truncated SOU2 payload";
            return std::nullopt;
        }
        sample.payload.assign(data.begin() + 20,
                              data.begin() + static_cast<std::ptrdiff_t>(20 + sample.units * 4));
        return sample;
    }
    error = "unknown .sou magic '" + sample.magic + "'";
    return std::nullopt;
}

} // namespace gm
