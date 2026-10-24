// Reusable HUD overlay (M8.6 slice 2): one `.sof` film frame - or a stack of
// them - composed onto an already rendered RGB frame, with clipping statistics.
//
// Synthetic half (no game files): a 128x64 pattern tile (32 transparent
// columns, two colours) pins the stats invariant
// drawn + skipped + clipped == layer_area for scale 1/2, edge clipping, a layer
// entirely outside the target, INT_MIN/INT_MAX origins, the opacity blend
// formula (0.25/0.5/0.75), the transparency-key contrast, determinism, the
// layered composition and every refusal (empty film, bad scale/opacity/
// palette/target/tick/frame).
//
// Shipped half (read-only install): AMMO4.SOF (ammo.fdx line 4, 131 frames
// @ 24 fps, start_frame 1) is the film `main.cpp::overlay_hud` draws, so
// `overlay_film` must reproduce `hud_player::draw` byte for byte at
// (14,125) for several tick/scale/opacity/key combinations.  Two FNV-1a64
// numbers are pinned: 0x2e32d52ce8678d4b is the overlay path with the
// transparent index 0 (what the CLI does) and 0xaaf521e923f31099 is
// hud_player's no-key golden (docs/HUD_PLAYER.md §4).  Numbers and the
// proven/limits split: docs/HUD_OVERLAY.md.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "core/path.h"
#include "render/hud.h"
#include "render/hud_overlay.h"
#include "render/hud_player.h"
#include "render/render.h"
#include "res/bmp.h"
#include "res/sof.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

template <typename T, typename U>
void check_eq(const T& got, const U& want, const std::string& what) {
    ++checks;
    if (!(got == want)) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

std::string hex64(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%016llx",
                  static_cast<unsigned long long>(value));
    return buffer;
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

/// 256 RGBA quads with distinct, non-grey colours (same table as test_hud_player).
std::vector<std::uint8_t> make_palette() {
    std::vector<std::uint8_t> palette(256 * 4);
    for (int i = 0; i < 256; ++i) {
        palette[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(i);
        palette[static_cast<std::size_t>(i) * 4 + 1] =
            static_cast<std::uint8_t>(255 - i);
        palette[static_cast<std::size_t>(i) * 4 + 2] =
            static_cast<std::uint8_t>((i * 7) & 0xff);
        palette[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return palette;
}

gm::RenderResult make_target(int width, int height, int r, int g, int b) {
    gm::RenderResult target;
    target.width = width;
    target.height = height;
    target.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    for (std::size_t i = 0; i + 2 < target.rgb.size(); i += 3) {
        target.rgb[i + 0] = static_cast<std::uint8_t>(r);
        target.rgb[i + 1] = static_cast<std::uint8_t>(g);
        target.rgb[i + 2] = static_cast<std::uint8_t>(b);
    }
    return target;
}

int changed_pixels(const gm::RenderResult& image, int r, int g, int b) {
    int changed = 0;
    for (std::size_t i = 0; i + 2 < image.rgb.size(); i += 3) {
        if (image.rgb[i] != static_cast<std::uint8_t>(r) ||
            image.rgb[i + 1] != static_cast<std::uint8_t>(g) ||
            image.rgb[i + 2] != static_cast<std::uint8_t>(b)) {
            ++changed;
        }
    }
    return changed;
}

void check_untouched(const gm::RenderResult& before, const gm::RenderResult& after,
                     const std::string& what) {
    check(before.width == after.width && before.height == after.height,
          what + ": target size unchanged");
    check(before.rgb == after.rgb, what + ": target pixels unchanged");
}

std::vector<std::uint8_t> flat_tile(std::uint8_t index) {
    return std::vector<std::uint8_t>(128 * 64, index);
}

/// 32 transparent columns (index 0), then index 7 above y=32 and index 9 below.
/// 2048 of the 8192 pixels are keyed out, so a full 320x200 frame shows
/// drawn=6144 / skipped=2048 / clipped=0 at scale 1.
std::vector<std::uint8_t> pattern_tile() {
    std::vector<std::uint8_t> pixels(128 * 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) {
            pixels[static_cast<std::size_t>(y) * 128 + static_cast<std::size_t>(x)] =
                x < 32 ? 0 : (y < 32 ? 7 : 9);
        }
    }
    return pixels;
}

gm::Sof make_sof(const std::vector<std::vector<std::uint8_t>>& tiles) {
    gm::Sof film;
    for (const auto& pixels : tiles) {
        const auto tile = gm::encode_sof_tile(pixels);
        film.data.insert(film.data.end(), tile.begin(), tile.end());
    }
    return film;
}

gm::Fdx make_fdx(std::uint32_t count, std::uint32_t fps) {
    std::string error;
    auto fdx = gm::parse_fdx(gm::build_fdx({{count, fps, 0}}), error);
    check(fdx.has_value(), "synthetic fdx parses: " + error);
    return fdx ? *fdx : gm::Fdx{};
}

/// One-tile pattern film, `count = 1`, 24 fps, scripted start frame 0.
gm::HudFilm make_pattern_film() {
    gm::HudFilm film;
    film.sof = make_sof({pattern_tile()});
    film.fdx = make_fdx(1, 24);
    film.count = 1;
    film.fps = 24;
    film.start_frame = 0;
    film.loop = true;
    return film;
}

std::uint8_t blend_channel(int dst, int src, double alpha) {
    const double value =
        static_cast<double>(dst) + (static_cast<double>(src) - static_cast<double>(dst)) * alpha;
    return static_cast<std::uint8_t>(std::floor(value + 0.5));
}

void test_stats_and_scale() {
    const auto film = make_pattern_film();
    const auto palette = make_palette();

    // --- scale 1 at (0,0): the whole 128x64 layer fits a 320x200 frame.
    gm::RenderResult image = make_target(320, 200, 11, 22, 33);
    gm::HudOverlayOptions options; // defaults: (0,0), scale 1, opacity 1, key 0, tick 0
    gm::HudOverlayStats stats;
    std::string error = "stale";
    check(gm::overlay_film(image, film, palette, options, &stats, &error),
          "scale 1 overlays (" + error + ")");
    check(error.empty(), "overlay_film clears the error on success");
    check_eq(stats.layer_width, 128, "scale 1 layer width");
    check_eq(stats.layer_height, 64, "scale 1 layer height");
    check_eq(stats.drawn_pixels, 6144, "scale 1 drawn cells");
    check_eq(stats.skipped_transparent, 2048, "scale 1 transparent cells");
    check_eq(stats.clipped_pixels, 0, "scale 1 clipped cells");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels,
             stats.layer_width * stats.layer_height, "scale 1 area invariant");
    check_eq(changed_pixels(image, 11, 22, 33), 6144, "scale 1 changed pixels");
    std::cout << "synthetic: pattern 128x64 at (0,0) scale=1: layer=" << stats.layer_width
              << "x" << stats.layer_height << " drawn=" << stats.drawn_pixels
              << " skipped=" << stats.skipped_transparent
              << " clipped=" << stats.clipped_pixels << " changed="
              << changed_pixels(image, 11, 22, 33) << " area="
              << stats.layer_width * stats.layer_height << "\n";

    const int drawn1 = stats.drawn_pixels;
    const int skipped1 = stats.skipped_transparent;

    // --- scale 2: every source pixel becomes a 2x2 block, so the area and
    //     both counts are exactly 4x (the layer still fits: 256x128 in 320x200).
    gm::RenderResult doubled = make_target(320, 200, 11, 22, 33);
    options.scale = 2;
    check(gm::overlay_film(doubled, film, palette, options, &stats, &error),
          "scale 2 overlays (" + error + ")");
    check_eq(stats.layer_width, 256, "scale 2 layer width");
    check_eq(stats.layer_height, 128, "scale 2 layer height");
    check_eq(stats.drawn_pixels, drawn1 * 4, "scale 2 drawn = 4x scale 1");
    check_eq(stats.skipped_transparent, skipped1 * 4, "scale 2 skipped = 4x scale 1");
    check_eq(stats.clipped_pixels, 0, "scale 2 does not clip");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels,
             stats.layer_width * stats.layer_height, "scale 2 area invariant");
    check_eq(changed_pixels(doubled, 11, 22, 33), 6144 * 4, "scale 2 changed pixels");
    std::cout << "synthetic: pattern at (0,0) scale=2: layer=" << stats.layer_width << "x"
              << stats.layer_height << " drawn=" << stats.drawn_pixels
              << " skipped=" << stats.skipped_transparent
              << " clipped=" << stats.clipped_pixels << " changed="
              << changed_pixels(doubled, 11, 22, 33) << " area="
              << stats.layer_width * stats.layer_height << "\n";
}

void test_clipping() {
    const auto film = make_pattern_film();
    const auto palette = make_palette();

    // --- partially off the left edge and past the bottom: x=-8, y=192.
    //     Visible: columns 8..127 (120 source columns) and rows 0..7.
    gm::RenderResult image = make_target(320, 200, 11, 22, 33);
    const gm::RenderResult before = image;
    gm::HudOverlayOptions options;
    options.x = -8;
    options.y = 192;
    gm::HudOverlayStats stats;
    std::string error;
    check(gm::overlay_film(image, film, palette, options, &stats, &error),
          "clipped overlay succeeds (" + error + ")");
    check_eq(stats.drawn_pixels, 768, "clipped drawn cells (96 columns x 8 rows)");
    check_eq(stats.skipped_transparent, 2048, "clipped transparent cells");
    check_eq(stats.clipped_pixels, 5376, "clipped cells outside the frame");
    check(stats.clipped_pixels > 0, "clipped run reports clipped pixels");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels, 8192,
             "clipped area invariant");

    // Nothing outside the visible rectangle [0,120) x [192,200) may change.
    int strays = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const bool inside = x < 120 && y >= 192;
            if (inside) {
                continue;
            }
            const std::size_t o = (static_cast<std::size_t>(y) * image.width + x) * 3;
            if (image.rgb[o] != before.rgb[o] || image.rgb[o + 1] != before.rgb[o + 1] ||
                image.rgb[o + 2] != before.rgb[o + 2]) {
                ++strays;
            }
        }
    }
    check_eq(strays, 0, "clipped overlay writes nothing outside its visible rectangle");
    std::cout << "synthetic: pattern at (-8,192) scale=1: drawn=" << stats.drawn_pixels
              << " skipped=" << stats.skipped_transparent
              << " clipped=" << stats.clipped_pixels << " strays=" << strays << "\n";

    // --- entirely outside the target is a successful no-op.
    gm::RenderResult outside = make_target(320, 200, 11, 22, 33);
    const gm::RenderResult outside_before = outside;
    options.x = 1000;
    options.y = 1000;
    check(gm::overlay_film(outside, film, palette, options, &stats, &error),
          "fully outside overlay succeeds");
    check_eq(stats.drawn_pixels, 0, "fully outside draws nothing");
    check_eq(stats.skipped_transparent, 2048, "fully outside still classifies the key");
    check_eq(stats.clipped_pixels, 6144, "fully outside clips the rest");
    check_untouched(outside_before, outside, "fully outside");

    // --- extreme origins must not overflow int64 arithmetic or write.
    gm::RenderResult extremes = make_target(320, 200, 11, 22, 33);
    const gm::RenderResult extremes_before = extremes;
    options.x = std::numeric_limits<int>::min();
    options.y = std::numeric_limits<int>::max();
    check(gm::overlay_film(extremes, film, palette, options, &stats, &error),
          "INT_MIN/INT_MAX origins succeed (" + error + ")");
    check_eq(stats.drawn_pixels, 0, "INT_MIN/INT_MAX draws nothing");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels, 8192,
             "INT_MIN/INT_MAX area invariant");
    check_untouched(extremes_before, extremes, "INT_MIN/INT_MAX origins");
}

void test_opacity() {
    const auto film = make_pattern_film();
    const auto palette = make_palette();

    gm::RenderResult opaque = make_target(320, 200, 0, 0, 0);
    gm::HudOverlayOptions options;
    gm::HudOverlayStats stats;
    std::string error;
    check(gm::overlay_film(opaque, film, palette, options, &stats, &error),
          "opaque reference overlays (" + error + ")");

    // Drawn area of the pattern at (0,0): x >= 32 for every row.
    const float alphas[3] = {0.25f, 0.5f, 0.75f};
    for (const float alpha : alphas) {
        gm::RenderResult blended = make_target(320, 200, 0, 0, 0);
        options.opacity = alpha;
        check(gm::overlay_film(blended, film, palette, options, &stats, &error),
              "opacity " + std::to_string(alpha) + " overlays (" + error + ")");
        check_eq(stats.drawn_pixels, 6144,
                 "opacity does not change the coverage count");
        check_eq(changed_pixels(blended, 0, 0, 0), 6144,
                 "any opacity keeps every covered pixel distinguishable");

        const double a = static_cast<double>(alpha);
        int mismatches = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 32; x < 128; ++x) {
                const std::size_t o =
                    (static_cast<std::size_t>(y) * 320 + static_cast<std::size_t>(x)) * 3;
                for (int c = 0; c < 3; ++c) {
                    const auto want = blend_channel(0, opaque.rgb[o + c], a);
                    if (blended.rgb[o + c] != want) {
                        ++mismatches;
                    }
                }
            }
        }
        check_eq(mismatches, 0, "opacity " + std::to_string(alpha) +
                                    " matches floor(dst + (src-dst)*a + 0.5)");
    }

    // Numbers for the doc: background 0, opaque filter colour, 0.5 result.
    {
        gm::RenderResult half = make_target(320, 200, 0, 0, 0);
        options.opacity = 0.5f;
        check(gm::overlay_film(half, film, palette, options, &stats, &error),
              "opacity 0.5 sample overlays");
        const int samples[2][2] = {{40, 5}, {60, 50}}; // index 7 and index 9
        for (const auto& point : samples) {
            const std::size_t o =
                (static_cast<std::size_t>(point[1]) * 320 +
                 static_cast<std::size_t>(point[0])) * 3;
            std::cout << "synthetic: opacity=0.5 at (" << point[0] << "," << point[1]
                      << "): bg=(0,0,0) src=(" << static_cast<int>(opaque.rgb[o]) << ","
                      << static_cast<int>(opaque.rgb[o + 1]) << ","
                      << static_cast<int>(opaque.rgb[o + 2]) << ") got=("
                      << static_cast<int>(half.rgb[o]) << ","
                      << static_cast<int>(half.rgb[o + 1]) << ","
                      << static_cast<int>(half.rgb[o + 2]) << ")\n";
            for (int c = 0; c < 3; ++c) {
                const int src = opaque.rgb[o + c];
                const int got = half.rgb[o + c];
                check(got <= src, "opacity 0.5 stays between bg and source");
                if (src > 0) {
                    check(got < src, "opacity 0.5 does not equal the opaque source");
                }
            }
        }
    }

    // opacity 0 is valid: coverage is reported, the buffer is untouched.
    gm::RenderResult zero = make_target(320, 200, 11, 22, 33);
    const gm::RenderResult zero_before = zero;
    options.opacity = 0.0f;
    check(gm::overlay_film(zero, film, palette, options, &stats, &error),
          "opacity 0 is a valid layer");
    check_eq(stats.drawn_pixels, 6144, "opacity 0 still reports coverage");
    check_untouched(zero_before, zero, "opacity 0");
    std::cout << "synthetic: opacity=0: buffer unchanged, coverage=" << stats.drawn_pixels
              << "\n";
}

void test_transparency_key() {
    const auto film = make_pattern_film();
    const auto palette = make_palette();

    gm::RenderResult keyed = make_target(320, 200, 11, 22, 33);
    gm::HudOverlayOptions options;
    gm::HudOverlayStats keyed_stats;
    std::string error;
    check(gm::overlay_film(keyed, film, palette, options, &keyed_stats, &error),
          "keyed overlay succeeds");

    gm::RenderResult opaque = make_target(320, 200, 11, 22, 33);
    options.transparent_zero = false;
    gm::HudOverlayStats opaque_stats;
    check(gm::overlay_film(opaque, film, palette, options, &opaque_stats, &error),
          "no-key overlay succeeds");
    check_eq(opaque_stats.skipped_transparent, 0, "no-key run skips nothing");
    check_eq(opaque_stats.drawn_pixels, 8192, "no-key run covers the whole layer");

    const int keyed_changed = changed_pixels(keyed, 11, 22, 33);
    const int opaque_changed = changed_pixels(opaque, 11, 22, 33);
    check_eq(keyed_changed, 6144, "keyed changed pixels");
    check_eq(opaque_changed, 8192, "no-key changed pixels");
    check(opaque_changed > keyed_changed,
          "transparent_zero=false changes more pixels (" + std::to_string(opaque_changed) +
              " vs " + std::to_string(keyed_changed) + ")");
    std::cout << "synthetic: transparent_zero: true changed=" << keyed_changed
              << " skipped=" << keyed_stats.skipped_transparent
              << " | false changed=" << opaque_changed
              << " skipped=" << opaque_stats.skipped_transparent << "\n";
}

void test_determinism() {
    const auto film = make_pattern_film();
    const auto palette = make_palette();
    gm::HudOverlayOptions options;
    options.x = 7;
    options.y = -3;
    options.scale = 2;
    options.opacity = 0.75f;
    options.tick = 91;

    gm::RenderResult first = make_target(320, 200, 11, 22, 33);
    gm::RenderResult second = first;
    gm::HudOverlayStats a;
    gm::HudOverlayStats b;
    std::string error;
    check(gm::overlay_film(first, film, palette, options, &a, &error), "determinism run 1");
    check(gm::overlay_film(second, film, palette, options, &b, &error), "determinism run 2");
    check(first.rgb == second.rgb, "determinism: identical buffers");
    check_eq(a.drawn_pixels, b.drawn_pixels, "determinism: drawn equal");
    check_eq(a.skipped_transparent, b.skipped_transparent, "determinism: skipped equal");
    check_eq(a.clipped_pixels, b.clipped_pixels, "determinism: clipped equal");
    check_eq(a.layer_width, b.layer_width, "determinism: layer width equal");

    // A null stats/error pair must be allowed.
    gm::RenderResult quiet = make_target(320, 200, 11, 22, 33);
    check(gm::overlay_film(quiet, film, palette, options, nullptr, nullptr),
          "overlay_film accepts null stats and error");
}

void test_glyph_layer_and_compose() {
    const auto film = make_pattern_film();
    const auto palette = make_palette();
    gm::HudOverlayOptions options;
    gm::HudOverlayStats stats;
    std::string error;

    // --- the frame primitive on a decoded frame.
    gm::RenderResult single = make_target(320, 200, 11, 22, 33);
    check(gm::overlay_glyph_layer(single, film.sof, 0, palette, options, &stats, &error),
          "overlay_glyph_layer frame 0 (" + error + ")");
    check_eq(stats.drawn_pixels, 6144, "glyph layer drawn");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels,
             stats.layer_width * stats.layer_height, "glyph layer area invariant");
    gm::RenderResult null_stats = make_target(320, 200, 11, 22, 33);
    check(gm::overlay_glyph_layer(null_stats, film.sof, 0, palette, options, nullptr, &error),
          "overlay_glyph_layer accepts null stats");
    check(null_stats.rgb == single.rgb, "glyph layer with null stats matches");

    // --- a two-layer stack: same pixels as two sequential overlays.
    gm::HudOverlayOptions second_options;
    second_options.x = 128;
    second_options.y = 64;
    second_options.scale = 2;
    second_options.opacity = 0.75f;
    const gm::HudOverlayLayer layers[2] = {{&film, options}, {&film, second_options}};

    gm::RenderResult stacked = make_target(320, 200, 11, 22, 33);
    gm::HudOverlayStats stacked_stats;
    check(gm::compose_hud_overlay(stacked, layers, palette, &stacked_stats, &error),
          "compose_hud_overlay two layers (" + error + ")");
    check_eq(stacked_stats.layer_width, 256, "stack stats: widest layer");
    check_eq(stacked_stats.layer_height, 128, "stack stats: tallest layer");
    check_eq(stacked_stats.drawn_pixels, 6144 + 16384, "stack stats: drawn summed");
    check_eq(stacked_stats.skipped_transparent, 2048 + 8192,
             "stack stats: skipped summed");
    check_eq(stacked_stats.clipped_pixels, 8192, "stack stats: clipped summed");
    check_eq(stacked_stats.drawn_pixels + stacked_stats.skipped_transparent +
                 stacked_stats.clipped_pixels,
             8192 + 32768, "stack stats: area invariant");
    check(stacked_stats.clipped_pixels > 0, "stack stats: second layer is clipped");

    gm::RenderResult sequential = make_target(320, 200, 11, 22, 33);
    check(gm::overlay_film(sequential, film, palette, options, nullptr, &error),
          "sequential layer 0");
    check(gm::overlay_film(sequential, film, palette, second_options, nullptr, &error),
          "sequential layer 1");
    check(stacked.rgb == sequential.rgb,
          "stack composition equals two sequential overlays");

    std::cout << "synthetic: compose 2 layers: drawn=" << stacked_stats.drawn_pixels
              << " skipped=" << stacked_stats.skipped_transparent
              << " clipped=" << stacked_stats.clipped_pixels
              << " layer=" << stacked_stats.layer_width << "x" << stacked_stats.layer_height
              << " parity=sequential\n";

    // --- an invalid later layer must not touch the buffer.
    gm::HudOverlayOptions bad_options = second_options;
    bad_options.scale = 0;
    const gm::HudOverlayLayer bad_layers[2] = {{&film, options}, {&film, bad_options}};
    gm::RenderResult guarded = make_target(320, 200, 11, 22, 33);
    const gm::RenderResult guarded_before = guarded;
    check(!gm::compose_hud_overlay(guarded, bad_layers, palette, &stats, &error),
          "compose rejects a bad later layer");
    check(!error.empty(), "compose reports the bad layer");
    check_untouched(guarded_before, guarded, "compose bad later layer");

    const gm::HudOverlayLayer null_layer[2] = {{&film, options}, {nullptr, {}}};
    gm::RenderResult null_guarded = make_target(320, 200, 11, 22, 33);
    const gm::RenderResult null_before = null_guarded;
    check(!gm::compose_hud_overlay(null_guarded, null_layer, palette, &stats, &error),
          "compose rejects a null film");
    check_untouched(null_before, null_guarded, "compose null film");

    // --- an empty stack is a successful no-op.
    gm::RenderResult empty = make_target(320, 200, 11, 22, 33);
    const gm::RenderResult empty_before = empty;
    check(gm::compose_hud_overlay(empty, std::span<const gm::HudOverlayLayer>{}, palette,
                                  &stats, &error),
          "compose accepts an empty stack");
    check_eq(stats.drawn_pixels, 0, "empty stack draws nothing");
    check_eq(stats.layer_width, 0, "empty stack has no layer");
    check_untouched(empty_before, empty, "empty stack");

    // --- target/palette refusals happen before any layer.
    gm::RenderResult no_pixels = make_target(0, 0, 0, 0, 0);
    check(!gm::compose_hud_overlay(no_pixels, layers, palette, &stats, &error),
          "compose rejects an empty target");
    gm::RenderResult short_buffer = make_target(320, 200, 11, 22, 33);
    short_buffer.rgb.pop_back();
    check(!gm::compose_hud_overlay(short_buffer, layers, palette, &stats, &error),
          "compose rejects a short buffer");
    gm::RenderResult short_palette_target = make_target(320, 200, 11, 22, 33);
    std::vector<std::uint8_t> short_palette(255 * 4, 0);
    check(!gm::compose_hud_overlay(short_palette_target, layers, short_palette, &stats,
                                   &error),
          "compose rejects a 255-entry palette");
}

void test_negatives() {
    const auto film = make_pattern_film();
    const auto palette = make_palette();

    auto expect_fail = [&](gm::RenderResult& image, const gm::HudFilm& bad_film,
                           const gm::HudOverlayOptions& options, const std::string& what) {
        const gm::RenderResult before = image;
        gm::HudOverlayStats stats;
        std::string error;
        const bool ok = gm::overlay_film(image, bad_film, palette, options, &stats, &error);
        check(!ok, what + " is refused");
        check(!error.empty(), what + " explains itself");
        check(stats.drawn_pixels == 0 && stats.layer_width == 0,
              what + " leaves zero stats");
        check_untouched(before, image, what);
    };

    gm::HudOverlayOptions good;

    // Empty film: no tiles, no index record.
    {
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        expect_fail(image, gm::HudFilm{}, good, "empty film");
    }
    // count = 0.
    {
        gm::HudFilm bad = make_pattern_film();
        bad.count = 0;
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        expect_fail(image, bad, good, "film with count 0");
    }
    // count above the tile count.
    {
        gm::HudFilm bad = make_pattern_film();
        bad.count = 2;
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        expect_fail(image, bad, good, "film with count above the tiles");
    }
    // scale out of range.
    for (const int scale : {0, 5, -1, std::numeric_limits<int>::min()}) {
        gm::HudOverlayOptions bad = good;
        bad.scale = scale;
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        expect_fail(image, film, bad, "scale " + std::to_string(scale));
    }
    // opacity outside 0..1 and non-finite values.
    for (const float opacity : {-0.1f, 1.5f, 100.0f, std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        gm::HudOverlayOptions bad = good;
        bad.opacity = opacity;
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        expect_fail(image, film, bad, "opacity " + std::to_string(opacity));
    }
    // Tick overflow.
    {
        gm::HudOverlayOptions bad = good;
        bad.tick = std::numeric_limits<std::uint64_t>::max();
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        expect_fail(image, film, bad, "tick UINT64_MAX");
    }
    // Frame out of range on the frame primitive.
    {
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        const gm::RenderResult before = image;
        gm::HudOverlayStats stats;
        std::string error;
        check(!gm::overlay_glyph_layer(image, film.sof, 7, palette, good, &stats, &error),
              "frame 7 of a 1-tile film is refused");
        check(!error.empty(), "frame refusal explains itself");
        check_untouched(before, image, "frame out of range");
    }
    // Short palette / empty target / short buffer.
    {
        gm::RenderResult image = make_target(320, 200, 11, 22, 33);
        const gm::RenderResult before = image;
        std::vector<std::uint8_t> short_palette(255 * 4, 0);
        std::string error;
        check(!gm::overlay_film(image, film, short_palette, good, nullptr, &error),
              "255-entry palette is refused");
        check_untouched(before, image, "short palette");

        gm::RenderResult empty_target = make_target(0, 0, 0, 0, 0);
        check(!gm::overlay_film(empty_target, film, palette, good, nullptr, &error),
              "empty target is refused");

        gm::RenderResult short_buffer = make_target(320, 200, 11, 22, 33);
        short_buffer.rgb.pop_back();
        const gm::RenderResult short_before = short_buffer;
        check(!gm::overlay_film(short_buffer, film, palette, good, nullptr, &error),
              "short RGB buffer is refused");
        check_untouched(short_before, short_buffer, "short RGB buffer");
    }
}

// ------------------------------------------------------------------ shipped

bool test_shipped(const fs::path& game) {
    std::error_code ec;
    if (!fs::is_directory(game / "Film", ec)) {
        std::cout << "hud_overlay: game not installed, shipped cases skipped\n";
        return false;
    }
    std::string error;
    const auto film = gm::load_hud_film(game, "AMMO4", error);
    check(film.has_value(), "AMMO4 loads (" + error + ")");
    if (!film) {
        return true;
    }
    const auto palette = gm::installed_palette(game, error);
    check(palette.has_value(), "installed palette (" + error + ")");
    if (!palette) {
        return true;
    }
    check(film->count == 131 && film->fps == 24, "AMMO4 is 131 frames @ 24 fps");
    check(film->start_frame == 1 && film->loop, "AMMO4 uses start_frame 1 / loop");

    // The film frame the tick selects (tick 0 -> tile 1).
    const auto current = gm::frame_at(*film, 0, error);
    check(current.has_value(), "frame_at AMMO4 tick 0");
    const std::size_t frame = current ? current->frame : 0;

    // --- 1) 320x200, film at (0,0), scale 1.
    gm::RenderResult image = make_target(320, 200, 7, 9, 11);
    gm::HudOverlayOptions options;
    gm::HudOverlayStats stats;
    check(gm::overlay_film(image, *film, *palette, options, &stats, &error),
          "shipped scale 1 overlay (" + error + ")");
    check_eq(stats.layer_width, 128, "shipped scale 1 layer width");
    check_eq(stats.layer_height, 64, "shipped scale 1 layer height");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels, 8192,
             "shipped scale 1 area invariant");
    check_eq(stats.clipped_pixels, 0, "shipped scale 1 clips nothing");
    const int changed1 = changed_pixels(image, 7, 9, 11);
    check_eq(changed1, stats.drawn_pixels, "shipped scale 1: every drawn cell changed");
    std::cout << "shipped: AMMO4 tick=0 frame=" << frame
              << " at (0,0) scale=1 fill=(7,9,11): layer=" << stats.layer_width << "x"
              << stats.layer_height << " drawn=" << stats.drawn_pixels
              << " skipped=" << stats.skipped_transparent
              << " clipped=" << stats.clipped_pixels << " changed=" << changed1
              << " area=" << stats.layer_width * stats.layer_height << "\n";

    // --- 1b) the same frame without the transparency key: the three index-0
    //         pixels of the shipped film are painted, so more pixels change.
    {
        gm::RenderResult unkeyed = make_target(320, 200, 7, 9, 11);
        gm::HudOverlayOptions unkeyed_options = options;
        unkeyed_options.transparent_zero = false;
        gm::HudOverlayStats unkeyed_stats;
        check(gm::overlay_film(unkeyed, *film, *palette, unkeyed_options, &unkeyed_stats,
                               &error),
              "shipped no-key overlay (" + error + ")");
        check_eq(unkeyed_stats.skipped_transparent, 0, "shipped no-key skips nothing");
        check_eq(unkeyed_stats.drawn_pixels, 8192, "shipped no-key covers the layer");
        const int changed_unkeyed = changed_pixels(unkeyed, 7, 9, 11);
        check_eq(changed_unkeyed, 8192, "shipped no-key changed pixels");
        check(changed_unkeyed > changed1,
              "shipped transparency key changes fewer pixels (" +
                  std::to_string(changed1) + " vs " + std::to_string(changed_unkeyed) + ")");
        std::cout << "shipped: transparent_zero: true changed=" << changed1
                  << " skipped=" << stats.skipped_transparent << " | false changed="
                  << changed_unkeyed << " skipped=" << unkeyed_stats.skipped_transparent
                  << " (AMMO4 frame " << frame << " has "
                  << (8192 - stats.drawn_pixels) << " index-0 pixels)\n";
    }

    // --- 2) scale 2: area and both counts x4 (layer 256x128 fits).
    gm::RenderResult doubled = make_target(320, 200, 7, 9, 11);
    options.scale = 2;
    check(gm::overlay_film(doubled, *film, *palette, options, &stats, &error),
          "shipped scale 2 overlay (" + error + ")");
    check_eq(stats.layer_width, 256, "shipped scale 2 layer width");
    check_eq(stats.layer_height, 128, "shipped scale 2 layer height");
    check_eq(stats.drawn_pixels, 8189 * 4, "shipped scale 2 drawn = 4x");
    check_eq(stats.skipped_transparent, 3 * 4, "shipped scale 2 skipped = 4x");
    check_eq(stats.clipped_pixels, 0, "shipped scale 2 clips nothing");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels, 32768,
             "shipped scale 2 area invariant");
    std::cout << "shipped: AMMO4 tick=0 at (0,0) scale=2: layer=" << stats.layer_width
              << "x" << stats.layer_height << " drawn=" << stats.drawn_pixels
              << " skipped=" << stats.skipped_transparent
              << " clipped=" << stats.clipped_pixels << " changed="
              << changed_pixels(doubled, 7, 9, 11) << " area="
              << stats.layer_width * stats.layer_height << "\n";

    // --- 3) clipping at x=-8, y=192.
    options.scale = 1;
    options.x = -8;
    options.y = 192;
    gm::RenderResult clipped = make_target(320, 200, 7, 9, 11);
    const gm::RenderResult clipped_before = clipped;
    check(gm::overlay_film(clipped, *film, *palette, options, &stats, &error),
          "shipped clipped overlay (" + error + ")");
    check(stats.clipped_pixels > 0, "shipped clipped run reports clipped pixels");
    check_eq(stats.drawn_pixels, 960, "shipped clipped drawn cells");
    check_eq(stats.skipped_transparent, 3, "shipped clipped transparent cells");
    check_eq(stats.clipped_pixels, 7229, "shipped clipped cells");
    check_eq(stats.drawn_pixels + stats.skipped_transparent + stats.clipped_pixels, 8192,
             "shipped clipped area invariant");
    int strays = 0;
    for (int y = 0; y < clipped.height; ++y) {
        for (int x = 0; x < clipped.width; ++x) {
            if (x < 120 && y >= 192) {
                continue;
            }
            const std::size_t o = (static_cast<std::size_t>(y) * 320 + x) * 3;
            if (clipped.rgb[o] != clipped_before.rgb[o] ||
                clipped.rgb[o + 1] != clipped_before.rgb[o + 1] ||
                clipped.rgb[o + 2] != clipped_before.rgb[o + 2]) {
                ++strays;
            }
        }
    }
    check_eq(strays, 0, "shipped clipping writes only inside [0,120)x[192,200)");
    std::cout << "shipped: AMMO4 tick=0 at (-8,192) scale=1: drawn=" << stats.drawn_pixels
              << " skipped=" << stats.skipped_transparent
              << " clipped=" << stats.clipped_pixels << " strays=" << strays << "\n";

    // --- 4) opacity 0.5: between the black background and the opaque pixel.
    options.x = 0;
    options.y = 0;
    gm::RenderResult opaque = make_target(320, 200, 0, 0, 0);
    options.opacity = 1.0f;
    check(gm::overlay_film(opaque, *film, *palette, options, &stats, &error),
          "shipped opaque reference");
    gm::RenderResult half = make_target(320, 200, 0, 0, 0);
    options.opacity = 0.5f;
    check(gm::overlay_film(half, *film, *palette, options, &stats, &error),
          "shipped opacity 0.5 overlay (" + error + ")");
    int mismatches = 0;
    int sampled = 0;
    for (int i = 0; i < 8192; ++i) {
        const int y = i / 128;
        const int x = i % 128;
        const std::size_t o = (static_cast<std::size_t>(y) * 320 + x) * 3;
        const bool touched =
            opaque.rgb[o] != 0 || opaque.rgb[o + 1] != 0 || opaque.rgb[o + 2] != 0;
        if (!touched) {
            continue;
        }
        ++sampled;
        for (int c = 0; c < 3; ++c) {
            if (half.rgb[o + c] != blend_channel(0, opaque.rgb[o + c], 0.5)) {
                ++mismatches;
            }
        }
    }
    check(sampled > 1000, "shipped opacity: enough touched pixels");
    check_eq(mismatches, 0, "shipped opacity 0.5 matches the blend formula");
    {
        // A couple of concrete triples for the doc.
        int printed = 0;
        for (int i = 0; i < 8192 && printed < 2; ++i) {
            const int y = i / 128;
            const int x = i % 128;
            const std::size_t o = (static_cast<std::size_t>(y) * 320 + x) * 3;
            const int sum = opaque.rgb[o] + opaque.rgb[o + 1] + opaque.rgb[o + 2];
            if (sum < 200 || sum > 700) {
                continue;
            }
            std::cout << "shipped: opacity=0.5 at (" << x << "," << y << "): bg=(0,0,0)"
                      << " src=(" << static_cast<int>(opaque.rgb[o]) << ","
                      << static_cast<int>(opaque.rgb[o + 1]) << ","
                      << static_cast<int>(opaque.rgb[o + 2]) << ") got=("
                      << static_cast<int>(half.rgb[o]) << ","
                      << static_cast<int>(half.rgb[o + 1]) << ","
                      << static_cast<int>(half.rgb[o + 2]) << ")\n";
            ++printed;
        }
    }
    std::cout << "shipped: opacity=0.5: touched=" << sampled
              << " formula_mismatches=" << mismatches << "\n";

    // --- 5) parity with hud_player::draw (what main.cpp::overlay_hud calls).
    struct Combo {
        std::uint64_t tick;
        int x;
        int y;
        int scale;
        float opacity;
        bool transparent;
    };
    const Combo combos[4] = {
        {0, 14, 125, 1, 1.0f, true},
        {91, 14, 125, 2, 0.75f, true},
        {182, 0, 0, 1, 0.5f, true},
        {0, 14, 125, 4, 1.0f, false},
    };
    for (const Combo& combo : combos) {
        gm::RenderResult from_draw = make_target(320, 200, 0, 0, 0);
        std::string draw_error;
        check(gm::draw(*film, from_draw, *palette, combo.x, combo.y, combo.transparent,
                       combo.scale, static_cast<double>(combo.opacity), combo.tick,
                       draw_error),
              "draw reference (" + draw_error + ")");
        gm::RenderResult from_overlay = make_target(320, 200, 0, 0, 0);
        gm::HudOverlayOptions combo_options;
        combo_options.x = combo.x;
        combo_options.y = combo.y;
        combo_options.scale = combo.scale;
        combo_options.opacity = combo.opacity;
        combo_options.transparent_zero = combo.transparent;
        combo_options.tick = combo.tick;
        gm::HudOverlayStats combo_stats;
        check(gm::overlay_film(from_overlay, *film, *palette, combo_options, &combo_stats,
                               &error),
              "overlay reference (" + error + ")");
        check(from_overlay.rgb == from_draw.rgb,
              "parity with draw at tick " + std::to_string(combo.tick) + " scale " +
                  std::to_string(combo.scale) + " opacity " +
                  std::to_string(combo.opacity) + " transparent " +
                  std::to_string(combo.transparent));
        std::cout << "shipped: parity tick=" << combo.tick << " at (" << combo.x << ","
                  << combo.y << ") scale=" << combo.scale << " opacity=" << combo.opacity
                  << " key=" << (combo.transparent ? 1 : 0)
                  << " fnv=" << hex64(fnv1a64(from_overlay.rgb))
                  << " drawn=" << combo_stats.drawn_pixels
                  << " clipped=" << combo_stats.clipped_pixels << " == draw\n";
    }

    // The overlay path (transparent index 0, installed palette) at the HUD
    // position, tick 0: pinned number for docs/HUD_OVERLAY.md.
    {
        gm::RenderResult reference = make_target(320, 200, 0, 0, 0);
        gm::HudOverlayOptions hud_options;
        hud_options.x = 14;
        hud_options.y = 125;
        check(gm::overlay_film(reference, *film, *palette, hud_options, &stats, &error),
              "HUD-position overlay");
        check_eq(fnv1a64(reference.rgb), 0x2e32d52ce8678d4bull,
                 "HUD-position FNV (transparent index 0)");
        std::cout << "shipped: AMMO4 (14,125) tick=0 key=1 fnv="
                  << hex64(fnv1a64(reference.rgb)) << " (pinned)\n";
    }

    // hud_player's no-key golden, reproduced through the overlay API with the
    // Kflm0000 palette (docs/HUD_PLAYER.md §4: aaf521e923f31099).
    {
        const auto bmp_path = gm::resolve_ci(game, "Film/Kflm0000.bmp");
        check(bmp_path.has_value(), "Kflm0000.bmp is resolvable");
        const auto bytes = bmp_path ? read_file(*bmp_path) : std::vector<std::uint8_t>{};
        std::string bmp_error;
        const auto bmp = gm::parse_bmp(bytes, bmp_error);
        check(bmp.has_value(), "Kflm0000.bmp parses (" + bmp_error + ")");
        if (bmp) {
            gm::RenderResult reference = make_target(320, 200, 0, 0, 0);
            gm::HudOverlayOptions hud_options;
            hud_options.x = 14;
            hud_options.y = 125;
            hud_options.transparent_zero = false;
            check(gm::overlay_film(reference, *film, bmp->palette, hud_options, &stats,
                                   &error),
                  "no-key overlay with the Kflm0000 palette");
            check_eq(fnv1a64(reference.rgb), 0xaaf521e923f31099ull,
                     "hud_player's no-key golden through overlay_film");
            std::cout << "shipped: AMMO4 (14,125) tick=0 key=0 Kflm0000 fnv="
                      << hex64(fnv1a64(reference.rgb))
                      << " (== hud_player golden aaf521e923f31099)\n";
        }
    }

    // --- 6) determinism on the shipped film.
    {
        gm::HudOverlayOptions det;
        det.x = 14;
        det.y = 125;
        det.tick = 91;
        det.scale = 2;
        det.opacity = 0.75f;
        gm::RenderResult a = make_target(320, 200, 0, 0, 0);
        gm::RenderResult b = make_target(320, 200, 0, 0, 0);
        gm::HudOverlayStats sa;
        gm::HudOverlayStats sb;
        check(gm::overlay_film(a, *film, *palette, det, &sa, &error), "shipped determinism 1");
        check(gm::overlay_film(b, *film, *palette, det, &sb, &error), "shipped determinism 2");
        check(a.rgb == b.rgb, "shipped determinism: identical buffers");
        check_eq(sa.drawn_pixels, sb.drawn_pixels, "shipped determinism: drawn");
        check_eq(sa.clipped_pixels, sb.clipped_pixels, "shipped determinism: clipped");
    }

    // --- 7) stacked composition of two real films equals two overlays.
    {
        const auto weapon = gm::load_hud_film(game, "WEAPON8", error);
        check(weapon.has_value(), "WEAPON8 loads (" + error + ")");
        if (weapon) {
            gm::HudOverlayOptions low;
            low.x = 0;
            low.y = 0;
            gm::HudOverlayOptions high;
            high.x = 128;
            high.y = 64;
            high.transparent_zero = false;
            const gm::HudOverlayLayer layers[2] = {{&*film, low}, {&*weapon, high}};

            gm::RenderResult stacked = make_target(320, 200, 0, 0, 0);
            gm::HudOverlayStats stacked_stats;
            check(gm::compose_hud_overlay(stacked, layers, *palette, &stacked_stats, &error),
                  "shipped compose AMMO4+WEAPON8 (" + error + ")");
            check_eq(stacked_stats.layer_width, 128, "shipped compose layer width");
            check_eq(stacked_stats.layer_height, 64, "shipped compose layer height");
            check_eq(stacked_stats.drawn_pixels + stacked_stats.skipped_transparent +
                         stacked_stats.clipped_pixels,
                     16384, "shipped compose area invariant");

            gm::RenderResult sequential = make_target(320, 200, 0, 0, 0);
            check(gm::overlay_film(sequential, *film, *palette, low, nullptr, &error),
                  "shipped sequential AMMO4");
            check(gm::overlay_film(sequential, *weapon, *palette, high, nullptr, &error),
                  "shipped sequential WEAPON8");
            check(stacked.rgb == sequential.rgb,
                  "shipped compose equals two sequential overlays");
            std::cout << "shipped: compose AMMO4+WEAPON8: drawn="
                      << stacked_stats.drawn_pixels
                      << " skipped=" << stacked_stats.skipped_transparent
                      << " clipped=" << stacked_stats.clipped_pixels
                      << " fnv=" << hex64(fnv1a64(stacked.rgb)) << " parity=sequential\n";
        }
    }
    return true;
}

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }

    test_stats_and_scale();
    test_clipping();
    test_opacity();
    test_transparency_key();
    test_determinism();
    test_glyph_layer_and_compose();
    test_negatives();

    const bool shipped = test_shipped(game);

    if (!shipped) {
        std::cout << "hud_overlay: shipped half skipped (no game install)\n";
    }
    std::cout << "hud_overlay: " << checks << " checks, " << failures << " failed\n";
    if (failures == 0) {
        std::cout << "hud_overlay: OK\n";
        return 0;
    }
    std::cerr << "hud_overlay: " << failures << " failure(s)\n";
    return 1;
}
