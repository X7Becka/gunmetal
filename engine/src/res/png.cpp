#include "res/png.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace gm {
namespace {

std::uint32_t crc32(std::span<const std::uint8_t> data) {
    static std::array<std::uint32_t, 256> table{};
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t b : data) {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(std::span<const std::uint8_t> data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_chunk(std::vector<std::uint8_t>& out, const char type[4],
               std::span<const std::uint8_t> body) {
    put_u32(out, static_cast<std::uint32_t>(body.size()));
    const std::size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    const std::uint32_t crc =
        crc32(std::span<const std::uint8_t>(out.data() + crc_start, out.size() - crc_start));
    put_u32(out, crc);
}

} // namespace

std::vector<std::uint8_t> encode_png_rgba(int width, int height,
                                          std::span<const std::uint8_t> rgba) {
    std::vector<std::uint8_t> out;
    if (width <= 0 || height <= 0 ||
        rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        return out;
    }

    static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), kSignature, kSignature + 8);

    // IHDR
    std::vector<std::uint8_t> ihdr;
    put_u32(ihdr, static_cast<std::uint32_t>(width));
    put_u32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(0); // interlace
    put_chunk(out, "IHDR", ihdr);

    // Raw scanlines with filter byte 0.
    std::vector<std::uint8_t> raw;
    raw.reserve((static_cast<std::size_t>(width) * 4 + 1) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const std::uint8_t* row = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 4);
    }

    // zlib wrapper: header + stored deflate blocks + adler32.
    std::vector<std::uint8_t> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    std::size_t pos = 0;
    do {
        const std::size_t chunk = std::min<std::size_t>(65535, raw.size() - pos);
        const bool final = pos + chunk == raw.size();
        zlib.push_back(final ? 0x01 : 0x00);
        zlib.push_back(static_cast<std::uint8_t>(chunk & 0xFF));
        zlib.push_back(static_cast<std::uint8_t>((chunk >> 8) & 0xFF));
        const std::uint16_t nlen = static_cast<std::uint16_t>(~chunk);
        zlib.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
        zlib.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + chunk));
        pos += chunk;
    } while (pos < raw.size());

    put_u32(zlib, adler32(raw));
    put_chunk(out, "IDAT", zlib);
    put_chunk(out, "IEND", {});
    return out;
}

} // namespace gm
