#include "res/sof.h"

#include <cctype>
#include <cstdio>

namespace gm {
namespace {

bool is_space(std::uint8_t c) { return c == ' ' || c == '\t'; }

/// ``[0-9]+ , [0-9]+`` with optional surrounding blanks (gm/sof.py _FDX_RE).
bool parse_pair(const std::uint8_t* p, std::size_t n, std::uint32_t& a,
                std::uint32_t& b) {
    std::size_t i = 0;
    while (i < n && is_space(p[i])) {
        ++i;
    }
    const std::size_t a_start = i;
    while (i < n && std::isdigit(p[i]) != 0) {
        ++i;
    }
    if (i == a_start) {
        return false;
    }
    std::uint64_t va = 0;
    for (std::size_t k = a_start; k < i; ++k) {
        va = va * 10 + (p[k] - '0');
        if (va > 0xFFFFFFFFull) {
            return false;
        }
    }
    while (i < n && is_space(p[i])) {
        ++i;
    }
    if (i >= n || p[i] != ',') {
        return false;
    }
    ++i;
    while (i < n && is_space(p[i])) {
        ++i;
    }
    const std::size_t b_start = i;
    while (i < n && std::isdigit(p[i]) != 0) {
        ++i;
    }
    if (i == b_start) {
        return false;
    }
    std::uint64_t vb = 0;
    for (std::size_t k = b_start; k < i; ++k) {
        vb = vb * 10 + (p[k] - '0');
        if (vb > 0xFFFFFFFFull) {
            return false;
        }
    }
    while (i < n && is_space(p[i])) {
        ++i;
    }
    if (i != n) {
        return false;
    }
    a = static_cast<std::uint32_t>(va);
    b = static_cast<std::uint32_t>(vb);
    return true;
}

} // namespace

std::optional<std::span<const std::uint8_t>> Sof::tile(std::size_t index) const {
    if (index >= tiles()) {
        return std::nullopt;
    }
    return std::span<const std::uint8_t>(data.data() + index * kSofTileBytes,
                                         kSofTileBytes);
}

std::vector<std::uint8_t> decode_sof_tile(std::span<const std::uint8_t> tile) {
    std::vector<std::uint8_t> out(kSofTileBytes, 0);
    if (tile.size() < kSofTileBytes) {
        return out;
    }
    // src[x*64 + y] -> dst[y*128 + x]
    for (int x = 0; x < kSofTileWidth; ++x) {
        for (int y = 0; y < kSofTileHeight; ++y) {
            out[static_cast<std::size_t>(y) * kSofTileWidth + x] =
                tile[static_cast<std::size_t>(x) * kSofTileSourceWidth + y];
        }
    }
    return out;
}

std::vector<std::uint8_t> encode_sof_tile(std::span<const std::uint8_t> pixels) {
    std::vector<std::uint8_t> out(kSofTileBytes, 0);
    if (pixels.size() < kSofTileBytes) {
        return out;
    }
    for (int y = 0; y < kSofTileHeight; ++y) {
        for (int x = 0; x < kSofTileWidth; ++x) {
            out[static_cast<std::size_t>(x) * kSofTileSourceWidth + y] =
                pixels[static_cast<std::size_t>(y) * kSofTileWidth + x];
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> Sof::frame(std::size_t index) const {
    const auto bytes = tile(index);
    if (!bytes) {
        return std::nullopt;
    }
    return decode_sof_tile(*bytes);
}

std::optional<Sof> parse_sof(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() / kSofTileBytes > kSofMaxTiles) {
        error = "sof: too many tiles (" + std::to_string(data.size() / kSofTileBytes) +
                ", limit " + std::to_string(kSofMaxTiles) + ")";
        return std::nullopt;
    }
    Sof film;
    film.data.assign(data.begin(), data.end());
    return film;
}

std::optional<Fdx> parse_fdx(std::span<const std::uint8_t> data, std::string& error) {
    Fdx fdx;
    fdx.data.assign(data.begin(), data.end());

    std::size_t start = 0;
    std::size_t lineno = 0;
    auto handle = [&](std::size_t end, std::size_t body_len) -> bool {
        // *end* points at '\n' or data.size(); body is [start, start+body_len).
        if (lineno >= kFdxMaxLines) {
            error = "fdx: more than " + std::to_string(kFdxMaxLines) + " lines";
            return false;
        }
        const std::uint8_t* body = data.data() + start;
        bool blank = true;
        for (std::size_t i = 0; i < body_len; ++i) {
            if (!is_space(body[i])) {
                blank = false;
                break;
            }
        }
        if (!blank) {
            std::uint32_t count = 0;
            std::uint32_t fps = 0;
            if (!parse_pair(body, body_len, count, fps)) {
                error = "fdx:" + std::to_string(lineno + 1) +
                        ": expected 'count,fps'";
                return false;
            }
            if (count > kFdxMaxCount) {
                error = "fdx:" + std::to_string(lineno + 1) + ": count " +
                        std::to_string(count) + " exceeds " +
                        std::to_string(kFdxMaxCount);
                return false;
            }
            if (fps == 0) {
                error = "fdx:" + std::to_string(lineno + 1) +
                        ": invalid FPS 0 for film";
                return false;
            }
            if (fps > kFdxMaxFps) {
                error = "fdx:" + std::to_string(lineno + 1) + ": fps " +
                        std::to_string(fps) + " exceeds " +
                        std::to_string(kFdxMaxFps);
                return false;
            }
            fdx.entries.push_back(FdxEntry{count, fps, lineno});
        }
        ++lineno;
        (void)end;
        return true;
    };

    for (std::size_t i = 0; i < data.size(); ++i) {
        if (data[i] != '\n') {
            continue;
        }
        std::size_t body_len = i - start;
        if (body_len > 0 && data[i - 1] == '\r') {
            --body_len;
        }
        if (!handle(i, body_len)) {
            return std::nullopt;
        }
        start = i + 1;
    }
    if (start < data.size()) {
        if (!handle(data.size(), data.size() - start)) {
            return std::nullopt;
        }
    }
    return fdx;
}

std::vector<std::uint8_t> build_fdx(const std::vector<FdxEntry>& entries) {
    std::vector<std::uint8_t> out;
    for (const auto& entry : entries) {
        char buf[64];
        const int n = std::snprintf(buf, sizeof(buf), "%u, %u\r\n", entry.count,
                                    entry.fps);
        if (n > 0) {
            out.insert(out.end(), buf, buf + n);
        }
    }
    return out;
}

} // namespace gm
