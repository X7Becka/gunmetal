// HUD layer: blit a 128x64 .sof frame into an RGB frame.
//
// Synthetic frames pin the composition rules (exact RGB, clipping, transparent
// index, integer scale 1..4, opacity rounding).  The shipped-film case paints
// AMMO4.SOF frame 0 at the engine's HUD position (x=14, row=125 of a 320x200
// surface, docs/FILM.md §1.3) with the Film/Kflm0000.bmp palette; the FNV-1a64
// values come from gm/sof.py + gm/bmp.py (temporary reference script, not
// committed).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "render/hud.h"
#include "render/render.h"
#include "res/bmp.h"
#include "res/sof.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Entry i = (i, 255-i, 7i).  A distinct table makes every index checkable.
std::vector<std::uint8_t> make_palette() {
    std::vector<std::uint8_t> palette(256 * 4);
    for (int i = 0; i < 256; ++i) {
        palette[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(i);
        palette[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(255 - i);
        palette[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>((i * 7) & 0xff);
        palette[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return palette;
}

gm::Sof make_film(const std::vector<std::uint8_t>& pixels) {
    gm::Sof film;
    film.data = gm::encode_sof_tile(pixels);
    return film;
}

/// 128x64 frame with a 2x2 block of indices 1..4 at the top-left corner and
/// index 0 everywhere else.
gm::Sof make_block_film() {
    std::vector<std::uint8_t> pixels(128 * 64, 0);
    pixels[0] = 1;
    pixels[1] = 2;
    pixels[128] = 3;
    pixels[129] = 4;
    return make_film(pixels);
}

gm::RenderResult make_target(int width, int height, std::uint8_t fill) {
    gm::RenderResult target;
    target.width = width;
    target.height = height;
    target.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3,
                      fill);
    return target;
}

/// Layer with index 0 keyed out, so only the 2x2 block of the synthetic film
/// is drawn and everything else must stay untouched.
gm::HudLayer block_layer(int x, int y, int scale = 1) {
    gm::HudLayer layer;
    layer.x = x;
    layer.y = y;
    layer.scale = scale;
    layer.transparent = true;
    layer.transparent_index = 0;
    return layer;
}

std::vector<std::uint8_t> expected_color(const std::vector<std::uint8_t>& palette, int index) {
    const std::uint8_t* c = palette.data() + static_cast<std::size_t>(index) * 4;
    return {c[0], c[1], c[2]};
}

bool pixel_is(const gm::RenderResult& target, int x, int y,
              const std::vector<std::uint8_t>& rgb) {
    const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(target.width) +
                           static_cast<std::size_t>(x)) *
                          3;
    return target.rgb[o] == rgb[0] && target.rgb[o + 1] == rgb[1] && target.rgb[o + 2] == rgb[2];
}

void check_pixel(const gm::RenderResult& target, int x, int y,
                 const std::vector<std::uint8_t>& palette, int index, const std::string& what) {
    check(pixel_is(target, x, y, expected_color(palette, index)),
          what + ": pixel (" + std::to_string(x) + "," + std::to_string(y) + ") is index " +
              std::to_string(index));
}

void check_fill(const gm::RenderResult& target, std::uint8_t fill, const std::string& what) {
    check(std::all_of(target.rgb.begin(), target.rgb.end(),
                      [fill](std::uint8_t b) { return b == fill; }),
          what + ": buffer untouched");
}

std::size_t count_changed(const gm::RenderResult& a, const gm::RenderResult& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        n += a.rgb[i] != b.rgb[i] ? 1 : 0;
    }
    return n;
}

} // namespace

int main() {
    const auto palette = make_palette();
    const gm::Sof film = make_block_film();

    // --- exact RGB: the 2x2 block lands at (1,1)..(2,2), nothing else moves.
    {
        const auto before = make_target(8, 8, 9);
        auto target = before;
        const auto layer = block_layer(1, 1);
        std::string error = "stale";
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "exact: blit succeeds (" + error + ")");
        check(error.empty(), "exact: error cleared");
        check_pixel(target, 1, 1, palette, 1, "exact");
        check_pixel(target, 2, 1, palette, 2, "exact");
        check_pixel(target, 1, 2, palette, 3, "exact");
        check_pixel(target, 2, 2, palette, 4, "exact");
        check(count_changed(before, target) == 4 * 3,
              "exact: exactly four RGB pixels written");
        check(pixel_is(target, 3, 1, {9, 9, 9}), "exact: right neighbour untouched");
        check(pixel_is(target, 0, 0, {9, 9, 9}), "exact: top-left untouched");
    }

    // --- opaque coverage: without keying the whole 128x64 frame is copied.
    {
        auto target = make_target(130, 66, 9);
        gm::HudLayer layer;
        layer.x = 1;
        layer.y = 1;
        std::string error;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "opaque: blit succeeds (" + error + ")");
        check_pixel(target, 3, 3, palette, 0, "opaque: background index copied");
        check_pixel(target, 1, 1, palette, 1, "opaque");
        check_pixel(target, 2, 2, palette, 4, "opaque");
        check_pixel(target, 128, 64, palette, 0, "opaque: last frame pixel");
        check(pixel_is(target, 0, 0, {9, 9, 9}), "opaque: origin outside the layer");
        check(pixel_is(target, 129, 65, {9, 9, 9}), "opaque: outside corner untouched");
        check(count_changed(make_target(130, 66, 9), target) == 128 * 64 * 3,
              "opaque: 128x64 RGB pixels written");
    }

    // --- negative origin: source (1,1) is the only visible pixel.
    {
        auto target = make_target(8, 8, 9);
        const auto layer = block_layer(-1, -1);
        std::string error;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "negative: blit succeeds (" + error + ")");
        check_pixel(target, 0, 0, palette, 4, "negative");
        check(pixel_is(target, 1, 0, {9, 9, 9}), "negative: source (0,1) clipped");
        check(pixel_is(target, 0, 1, {9, 9, 9}), "negative: source (1,0) clipped");
    }

    // --- clipping at the right/bottom edge: only source (0,0) survives.
    {
        auto target = make_target(8, 8, 9);
        const auto layer = block_layer(7, 7);
        std::string error;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "clip: blit succeeds (" + error + ")");
        check_pixel(target, 7, 7, palette, 1, "clip");
        check(pixel_is(target, 6, 7, {9, 9, 9}), "clip: left neighbour untouched");
        check(pixel_is(target, 7, 6, {9, 9, 9}), "clip: top neighbour untouched");
    }

    // --- fully outside, including int overflow extremes: successful no-op.
    for (const int extreme : {std::numeric_limits<int>::max(), std::numeric_limits<int>::min()}) {
        for (const int scale : {1, 4}) {
            auto target = make_target(8, 8, 9);
            gm::HudLayer layer = block_layer(extreme, extreme == std::numeric_limits<int>::max()
                                                          ? std::numeric_limits<int>::min()
                                                          : std::numeric_limits<int>::max(),
                                             scale);
            layer.transparent = false;
            std::string error;
            check(gm::blit_indexed(target, film, 0, palette, layer, error),
                  "overflow: blit succeeds (" + error + ")");
            check_fill(target, 9, "overflow x=" + std::to_string(extreme) +
                                      " scale=" + std::to_string(scale));
        }
    }
    {
        // Near the int limit, scale 4 still clips cleanly.
        auto target = make_target(8, 8, 9);
        gm::HudLayer layer;
        layer.x = std::numeric_limits<int>::max() - 3;
        layer.y = -2;
        layer.scale = 4;
        std::string error;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "overflow: near-max x succeeds (" + error + ")");
        check_fill(target, 9, "overflow: near-max x");
    }

    // --- transparent index: 0 is the film's background.
    {
        auto target = make_target(8, 8, 9);
        auto layer = block_layer(1, 1);
        std::string error;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "transparent 0: blit succeeds (" + error + ")");
        check_pixel(target, 1, 1, palette, 1, "transparent 0");
        check(pixel_is(target, 0, 0, {9, 9, 9}), "transparent 0: background skipped");
        check(pixel_is(target, 6, 6, {9, 9, 9}), "transparent 0: rest of frame skipped");

        // Index 2 is a foreground pixel: it must now be skipped too.
        auto target2 = make_target(8, 8, 9);
        layer.transparent_index = 2;
        error.clear();
        check(gm::blit_indexed(target2, film, 0, palette, layer, error),
              "transparent 2: blit succeeds (" + error + ")");
        check_pixel(target2, 1, 1, palette, 1, "transparent 2");
        check(pixel_is(target2, 2, 1, {9, 9, 9}), "transparent 2: index 2 skipped");
        check_pixel(target2, 1, 2, palette, 3, "transparent 2");
        check_pixel(target2, 2, 2, palette, 4, "transparent 2");
    }

    // --- integer nearest-neighbour scale 2: every source pixel becomes 2x2.
    {
        auto target = make_target(8, 8, 9);
        const auto layer = block_layer(1, 1, 2);
        std::string error;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "scale 2: blit succeeds (" + error + ")");
        check_pixel(target, 1, 1, palette, 1, "scale 2");
        check_pixel(target, 2, 2, palette, 1, "scale 2");
        check_pixel(target, 3, 1, palette, 2, "scale 2");
        check_pixel(target, 1, 3, palette, 3, "scale 2");
        check_pixel(target, 4, 4, palette, 4, "scale 2");
        check(pixel_is(target, 5, 1, {9, 9, 9}), "scale 2: right of block untouched");
        check(pixel_is(target, 1, 5, {9, 9, 9}), "scale 2: below block untouched");
        check(count_changed(make_target(8, 8, 9), target) == 4 * 4 * 3,
              "scale 2: sixteen RGB pixels written");
    }

    // --- scale 4 clipped by the target: block spans (1,1)..(7,7).
    {
        auto target = make_target(8, 8, 9);
        const auto layer = block_layer(1, 1, 4);
        std::string error;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "scale 4: blit succeeds (" + error + ")");
        check_pixel(target, 1, 1, palette, 1, "scale 4");
        check_pixel(target, 4, 4, palette, 1, "scale 4");
        check_pixel(target, 5, 1, palette, 2, "scale 4");
        check_pixel(target, 7, 2, palette, 2, "scale 4");
        check_pixel(target, 1, 5, palette, 3, "scale 4");
        check_pixel(target, 5, 5, palette, 4, "scale 4");
        check_pixel(target, 7, 7, palette, 4, "scale 4");
    }

    // --- opacity: 0 = no-op, 1 = exact, 0.5 = round-half-up per channel.
    {
        auto target = make_target(8, 8, 100);
        const auto before = target;
        auto layer = block_layer(0, 0);
        std::string error;
        layer.opacity = 0.0;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "opacity 0: blit succeeds (" + error + ")");
        check(count_changed(before, target) == 0, "opacity 0: buffer unchanged");

        layer.opacity = 1.0;
        check(gm::blit_indexed(target, film, 0, palette, layer, error),
              "opacity 1: blit succeeds (" + error + ")");
        check_pixel(target, 0, 0, palette, 1, "opacity 1");
        check_pixel(target, 1, 1, palette, 4, "opacity 1");
        check(pixel_is(target, 2, 2, {100, 100, 100}), "opacity 1: background kept");

        layer.opacity = 0.5;
        auto half = make_target(8, 8, 100);
        check(gm::blit_indexed(half, film, 0, palette, layer, error),
              "opacity 0.5: blit succeeds (" + error + ")");
        // index 1 -> (1,254,7): 100 + (1-100)*0.5 = 50.5 -> 51,
        // 100 + 154*0.5 = 177, 100 + (7-100)*0.5 = 53.5 -> 54.
        check(pixel_is(half, 0, 0, {51, 177, 54}), "opacity 0.5: rounds half up");
        // index 4 -> (4,251,28): 52, 175.5 -> 176, 64.
        check(pixel_is(half, 1, 1, {52, 176, 64}), "opacity 0.5: second pixel");
        check(pixel_is(half, 2, 2, {100, 100, 100}), "opacity 0.5: outside untouched");

        // Out-of-range finite values are clamped, not rejected.
        layer.opacity = 2.0;
        check(gm::blit_indexed(half, film, 0, palette, layer, error),
              "opacity 2 clamps: blit succeeds (" + error + ")");
        check_pixel(half, 0, 0, palette, 1, "opacity 2 clamps to 1");
        layer.opacity = -1.0;
        auto zero = make_target(8, 8, 100);
        check(gm::blit_indexed(zero, film, 0, palette, layer, error),
              "opacity -1 clamps: blit succeeds (" + error + ")");
        check_fill(zero, 100, "opacity -1 clamps to 0");

        layer.opacity = std::numeric_limits<double>::quiet_NaN();
        check(!gm::blit_indexed(zero, film, 0, palette, layer, error),
              "opacity NaN rejected");
        check(!error.empty(), "opacity NaN sets an error");
    }

    // --- invalid frame index: rejected, buffer untouched.
    {
        auto target = make_target(8, 8, 9);
        const auto before = target;
        const auto layer = block_layer(0, 0);
        std::string error;
        check(!gm::blit_indexed(target, film, 1, palette, layer, error),
              "frame 1 of a one-tile film rejected");
        check(!error.empty(), "invalid frame sets an error");
        check(count_changed(before, target) == 0, "invalid frame leaves the buffer");
        check(!gm::blit_indexed(target, film, std::numeric_limits<std::size_t>::max(), palette,
                               layer, error),
              "huge frame index rejected");
    }

    // --- palette shorter than 256 entries: rejected.
    {
        auto target = make_target(8, 8, 9);
        const auto layer = block_layer(0, 0);
        std::string error;
        std::vector<std::uint8_t> short_palette(palette.begin(), palette.begin() + 255 * 4);
        check(!gm::blit_indexed(target, film, 0, short_palette, layer, error),
              "palette with 255 entries rejected");
        check(!error.empty(), "short palette sets an error");
        std::vector<std::uint8_t> tiny(4, 0);
        check(!gm::blit_indexed(target, film, 0, tiny, layer, error),
              "palette with 1 entry rejected");
    }

    // --- scale outside 1..4 and malformed targets: rejected.
    {
        auto target = make_target(8, 8, 9);
        auto layer = block_layer(0, 0);
        std::string error;
        layer.scale = 0;
        check(!gm::blit_indexed(target, film, 0, palette, layer, error), "scale 0 rejected");
        layer.scale = 5;
        check(!gm::blit_indexed(target, film, 0, palette, layer, error), "scale 5 rejected");
        layer.scale = 1;

        gm::RenderResult empty;
        check(!gm::blit_indexed(empty, film, 0, palette, layer, error),
              "empty target rejected");
        auto wrong = make_target(2, 2, 0);
        wrong.rgb.resize(3);
        check(!gm::blit_indexed(wrong, film, 0, palette, layer, error),
              "short RGB target rejected");
    }

    // --- shipped film: AMMO4.SOF frame 0 at (14,125) of a 320x200 surface.
    {
        fs::path game = "/mnt/d/games/Gunmetal 3dfx";
        if (const char* env = std::getenv("GUNMETAL_DIR")) {
            game = env;
        }
        std::error_code ec;
        const fs::path film_path = game / "Film" / "AMMO4.SOF";
        const fs::path palette_path = game / "Film" / "Kflm0000.bmp";
        if (!fs::is_regular_file(film_path, ec) || !fs::is_regular_file(palette_path, ec)) {
            std::cout << "hud: game not installed, shipped-film case skipped\n";
        } else {
            std::string error;
            const auto film_bytes = read_file(film_path);
            const auto shipped = gm::parse_sof(film_bytes, error);
            check(shipped.has_value(), "AMMO4.SOF parses: " + error);
            const auto bmp_bytes = read_file(palette_path);
            const auto bmp = gm::parse_bmp(bmp_bytes, error);
            check(bmp.has_value(), "Kflm0000.bmp parses: " + error);
            if (shipped && bmp) {
                check(bmp->palette.size() == 1024, "Kflm0000.bmp carries 256 colours");
                const auto frame0 = shipped->frame(0);
                check(frame0.has_value() && frame0->size() == 128 * 64,
                      "AMMO4.SOF frame 0 is 128x64");

                gm::HudLayer layer;
                layer.x = 14;
                layer.y = 125;
                auto target = make_target(320, 200, 0);
                error = "stale";
                check(gm::blit_indexed(target, *shipped, 0, bmp->palette, layer, error),
                      "shipped: blit succeeds (" + error + ")");
                check(error.empty(), "shipped: error cleared");
                const std::uint64_t hash = fnv1a64(target.rgb);
                check(hash == 0xe86048ae84209fcaull,
                      "shipped: FNV-1a64 on black 320x200 matches Python (got " +
                          std::to_string(hash) + ")");

                // Same layer over a non-black surface: pins the untouched rest.
                auto background = make_target(320, 200, 0);
                for (std::size_t i = 0; i < background.rgb.size(); i += 3) {
                    background.rgb[i] = 7;
                    background.rgb[i + 1] = 9;
                    background.rgb[i + 2] = 11;
                }
                check(gm::blit_indexed(background, *shipped, 0, bmp->palette, layer, error),
                      "shipped: blit over background succeeds (" + error + ")");
                const std::uint64_t bg_hash = fnv1a64(background.rgb);
                check(bg_hash == 0x5a5caa743a6b4f0aull,
                      "shipped: FNV-1a64 over 7/9/11 background matches Python (got " +
                          std::to_string(bg_hash) + ")");

                // Engine HUD geometry sanity: the overlay must not touch row 124.
                bool row124_clean = true;
                for (int x = 0; x < 320; ++x) {
                    row124_clean = row124_clean && pixel_is(target, x, 124, {0, 0, 0});
                }
                check(row124_clean, "shipped: row 124 stays black");
            }
        }
    }

    if (failures == 0) {
        std::cout << "hud: synthetic + AMMO4.SOF frame 0 FNV e86048ae84209fca, ok\n";
    }
    return failures == 0 ? 0 : 1;
}
