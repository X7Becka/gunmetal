#include "res/bmp.h"

#include "core/bytes.h"
#include "core/path.h"
#include "res/bigfile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace gm {
namespace {

/// Fallback carriers: loose BMPs that repeat the engine's table, except that
/// the slots they never use hold magenta placeholders (``FLAG/naUSA.BMP``
/// differs in 21 entries, including the whole light and red ramps).
constexpr const char* kPaletteCarriers[] = {
    "FLAG/naUSA.BMP",
    "FLAG/aanataka.bmp",
    "PICS/oaktabl1.bmp",
    "PICS/mine-1c.bmp",
};

/// ``frontend\gamepal.bmp`` inside the FRONTEND big-file: the engine loads it
/// at ``0x44af88`` through ``0x47efe0`` into ``0x83d320`` and publishes the
/// table at ``0x83d2c4``; the art code reads it as ``[reg+0x83d320]``.  The
/// error string is ``"Unable to load game palette (FRONTEND\GAMEPAL.BMP)"``.
std::optional<std::vector<std::uint8_t>> engine_palette(const std::filesystem::path& game,
                                                        std::string& error) {
    const auto index = resolve_ci(game, "FRONTEND/BMP.IDX");
    if (!index) {
        return std::nullopt;
    }
    std::string open_error;
    auto archive = BigFile::open(*index, open_error);
    if (!archive) {
        return std::nullopt;
    }
    for (const auto& entry : archive->entries()) {
        std::string name = entry.name;
        for (char& ch : name) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (name != "GAMEPAL") {
            continue;
        }
        std::string read_error;
        const auto bytes = archive->read(entry, read_error);
        if (!bytes) {
            error = read_error;
            return std::nullopt;
        }
        return parse_bmp(*bytes, error)->palette;
    }
    return std::nullopt;
}

} // namespace

std::optional<BmpImage> parse_bmp(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') {
        error = "not a BMP";
        return std::nullopt;
    }
    const std::uint32_t off = rd32(data.data() + 10);
    const std::uint32_t hdr = rd32(data.data() + 14);
    if (hdr < 40) {
        error = "unsupported BMP header size " + std::to_string(hdr);
        return std::nullopt;
    }
    const std::int32_t width = static_cast<std::int32_t>(rd32(data.data() + 18));
    const std::int32_t raw_height = static_cast<std::int32_t>(rd32(data.data() + 22));
    const std::uint16_t bpp = rd16(data.data() + 28);
    const std::uint32_t ncol = rd32(data.data() + 46);
    if (bpp != 8) {
        error = "unsupported bpp " + std::to_string(bpp);
        return std::nullopt;
    }
    if (width <= 0 || raw_height == 0) {
        error = "bad dimensions";
        return std::nullopt;
    }
    const bool top_down = raw_height < 0;
    const int height = top_down ? -raw_height : raw_height;

    BmpImage image;
    image.width = width;
    image.height = height;
    image.bpp = bpp;
    image.top_down = top_down;

    const std::size_t colors = ncol ? ncol : 256;
    const std::size_t pal_off = 14 + hdr;
    if (pal_off + colors * 4 > data.size()) {
        error = "truncated palette";
        return std::nullopt;
    }
    image.palette.reserve(colors * 4);
    for (std::size_t i = 0; i < colors; ++i) {
        const std::uint8_t* q = data.data() + pal_off + i * 4; // B,G,R,X
        image.palette.push_back(q[2]);
        image.palette.push_back(q[1]);
        image.palette.push_back(q[0]);
        image.palette.push_back(q[3]);
    }

    const std::size_t stride = (static_cast<std::size_t>(width) + 3) & ~std::size_t{3};
    if (off + stride * static_cast<std::size_t>(height) > data.size()) {
        error = "truncated pixel data";
        return std::nullopt;
    }
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const int src_row = top_down ? y : height - 1 - y;
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * width,
                    data.data() + off + static_cast<std::size_t>(src_row) * stride,
                    static_cast<std::size_t>(width));
    }
    return image;
}

std::optional<std::vector<std::uint8_t>> installed_palette(const std::filesystem::path& game,
                                                           std::string& error) {
    // The engine's own table first (FRONTEND\GAMEPAL.BMP); the loose BMPs below
    // are a fallback and their unused slots are magenta.
    if (const auto from_archive = engine_palette(game, error)) {
        return *from_archive;
    }
    error.clear();
    for (const char* rel : kPaletteCarriers) {
        const auto resolved = resolve_ci(game, rel);
        if (!resolved) {
            continue;
        }
        std::ifstream in(*resolved, std::ios::binary);
        if (!in) {
            continue;
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        std::string parse_error;
        if (const auto bmp = parse_bmp(bytes, parse_error)) {
            return bmp->palette;
        }
        error = parse_error;
    }
    if (error.empty()) {
        error = "no palette carrier BMP found under " + game.string();
    }
    return std::nullopt;
}

} // namespace gm
