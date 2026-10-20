// Indexed sprite/texture blitter (engine/src/render/sprite.*).
//
// Synthetic cases pin the rules directly: transparency, clip on every edge,
// negative origins, flips, integer scales 1/2/3, bilinear weights, opacity
// 0/128/255 and additive saturation.  The real cases blit shipped `.sbm`
// sprites from PICS/ and sprites/ over a deterministic RGB background and
// compare the whole frame (including row padding) with FNV-1a64 goldens from
// a temporary Python reference built on gm/render.py's sampler and gm's
// archive/palette readers.  Negatives cover malformed targets, palettes,
// sprites, scales and channels.  Goldens and limits: docs/SPRITE.md.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "render/sprite.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "res/sbm.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Entry i = (i, 255-i, 7i), the same table as test_hud.cpp.
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

gm::Sprite make_sprite(int width, int height, const std::vector<std::uint8_t>& pixels,
                       std::uint32_t flags = 1) {
    gm::Sprite sprite;
    sprite.width = width;
    sprite.height = height;
    sprite.pixels = pixels;
    sprite.flags = flags;
    return sprite;
}

/// Deterministic RGB background, so alpha/additive never blend into a flat
/// field.  Padding bytes stay 0.
std::vector<std::uint8_t> make_target(int width, int height, int stride) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(stride) *
                                  static_cast<std::size_t>(height), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t o = static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
                                  static_cast<std::size_t>(x) * 3;
            rgb[o + 0] = static_cast<std::uint8_t>((x * 7 + y * 3 + 11) & 0xff);
            rgb[o + 1] = static_cast<std::uint8_t>((x * 2 + y * 11 + 29) & 0xff);
            rgb[o + 2] = static_cast<std::uint8_t>((x * 13 + y * 5 + 47) & 0xff);
        }
    }
    return rgb;
}

gm::SpriteTarget target_view(std::vector<std::uint8_t>& rgb, int width, int height, int stride) {
    return gm::SpriteTarget{width, height, stride, rgb};
}

std::vector<std::uint8_t> initial_pixel(int x, int y) {
    return {static_cast<std::uint8_t>((x * 7 + y * 3 + 11) & 0xff),
            static_cast<std::uint8_t>((x * 2 + y * 11 + 29) & 0xff),
            static_cast<std::uint8_t>((x * 13 + y * 5 + 47) & 0xff)};
}

std::vector<std::uint8_t> palette_pixel(const std::vector<std::uint8_t>& palette, int index) {
    const std::size_t o = static_cast<std::size_t>(index) * 4;
    return {palette[o], palette[o + 1], palette[o + 2]};
}

const std::uint8_t* at(const std::vector<std::uint8_t>& rgb, int stride, int x, int y) {
    return rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
           static_cast<std::size_t>(x) * 3;
}

void check_pixel(const std::vector<std::uint8_t>& rgb, int stride, int x, int y,
                 const std::vector<std::uint8_t>& want, const std::string& what) {
    const std::uint8_t* p = at(rgb, stride, x, y);
    const bool ok = p[0] == want[0] && p[1] == want[1] && p[2] == want[2];
    if (!ok) {
        check(false, what + ": pixel (" + std::to_string(x) + "," + std::to_string(y) +
                         ") is [" + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," +
                         std::to_string(p[2]) + "], want [" + std::to_string(want[0]) + "," +
                         std::to_string(want[1]) + "," + std::to_string(want[2]) + "]");
    }
}

void check_index(const std::vector<std::uint8_t>& rgb, int stride, int x, int y,
                 const std::vector<std::uint8_t>& palette, int index,
                 const std::string& what) {
    check_pixel(rgb, stride, x, y, palette_pixel(palette, index), what);
}

void check_initial(const std::vector<std::uint8_t>& rgb, int stride, int x, int y,
                   const std::string& what) {
    check_pixel(rgb, stride, x, y, initial_pixel(x, y), what);
}

std::uint64_t run_nearest(const std::vector<std::uint8_t>& palette, const gm::Sprite& sprite,
                          int width, int height, int stride, const gm::SpriteBlit& params,
                          std::vector<std::uint8_t>& rgb) {
    std::string error;
    const bool ok = gm::blit(target_view(rgb, width, height, stride), palette, sprite, params,
                             error);
    check(ok, "nearest blit returns true: " + error);
    return fnv1a64(rgb);
}

std::uint64_t run_bilinear(const std::vector<std::uint8_t>& palette, const gm::Sprite& sprite,
                           int width, int height, int stride, const gm::SpriteBlit& params,
                           std::vector<std::uint8_t>& rgb) {
    std::string error;
    const bool ok = gm::blit_scaled_bilinear(target_view(rgb, width, height, stride), palette,
                                             sprite, params, error);
    check(ok, "bilinear blit returns true: " + error);
    return fnv1a64(rgb);
}

void check_golden(std::uint64_t got, std::uint64_t want, const std::string& what) {
    if (got != want) {
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(got));
        check(false, what + ": FNV 0x" + buf + " != expected");
    }
}

const gm::BigEntry* find_entry(const gm::BigFile& file, const std::string& name) {
    for (const gm::BigEntry& entry : file.entries()) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<gm::Sprite> load_real_sprite(const fs::path& game, const std::string& sub,
                                           const std::string& name) {
    std::string error;
    const auto archive = gm::BigFile::open(game / sub / "SBM.IDX", error);
    if (!archive) {
        std::cerr << "skip real sprites: " << error << "\n";
        return std::nullopt;
    }
    const gm::BigEntry* entry = find_entry(*archive, name);
    if (entry == nullptr) {
        std::cerr << "skip real sprites: " << name << " not in " << sub << "\n";
        return std::nullopt;
    }
    const auto blob = archive->read(*entry, error);
    if (!blob) {
        std::cerr << "skip real sprites: " << error << "\n";
        return std::nullopt;
    }
    const auto sbm = gm::parse_sbm(*blob, error);
    if (!sbm || sbm->mips.empty()) {
        std::cerr << "skip real sprites: " << name << ": " << error << "\n";
        return std::nullopt;
    }
    gm::Sprite sprite;
    sprite.width = sbm->width;
    sprite.height = sbm->height;
    sprite.flags = sbm->flags;
    sprite.pixels = sbm->mips[0].pixels;
    return sprite;
}

void synthetic_cases(const std::vector<std::uint8_t>& palette) {
    // 2x2 sprite at (1,1), opaque: exact texel placement.
    {
        const auto sprite = make_sprite(2, 2, {1, 2, 3, 4});
        const int stride = 6 * 3;
        auto rgb = make_target(6, 4, stride);
        gm::SpriteBlit p;
        p.x = 1;
        p.y = 1;
        p.transparent_index = -1;
        run_nearest(palette, sprite, 6, 4, stride, p, rgb);
        check_index(rgb, stride, 1, 1, palette, 1, "2x2 plain top-left");
        check_index(rgb, stride, 2, 1, palette, 2, "2x2 plain top-right");
        check_index(rgb, stride, 1, 2, palette, 3, "2x2 plain bottom-left");
        check_index(rgb, stride, 2, 2, palette, 4, "2x2 plain bottom-right");
        check_initial(rgb, stride, 0, 0, "2x2 plain leaves outside untouched");
    }

    // Both flips swap the corners.
    {
        const auto sprite = make_sprite(2, 2, {1, 2, 3, 4});
        const int stride = 6 * 3;
        auto rgb = make_target(6, 4, stride);
        gm::SpriteBlit p;
        p.x = 1;
        p.y = 1;
        p.flip_x = true;
        p.flip_y = true;
        p.transparent_index = -1;
        run_nearest(palette, sprite, 6, 4, stride, p, rgb);
        check_index(rgb, stride, 1, 1, palette, 4, "2x2 flip both top-left");
        check_index(rgb, stride, 2, 1, palette, 3, "2x2 flip both top-right");
        check_index(rgb, stride, 1, 2, palette, 2, "2x2 flip both bottom-left");
        check_index(rgb, stride, 2, 2, palette, 1, "2x2 flip both bottom-right");
    }

    // scale 2, negative x: the left column of the sprite is clipped.
    {
        const auto sprite = make_sprite(3, 1, {1, 2, 3});
        const int stride = 8 * 3;
        auto rgb = make_target(8, 5, stride);
        gm::SpriteBlit p;
        p.x = -1;
        p.y = 1;
        p.scale = 2;
        p.transparent_index = -1;
        run_nearest(palette, sprite, 8, 5, stride, p, rgb);
        check_index(rgb, stride, 0, 1, palette, 1, "3x1 scale2 clipped x=0");
        check_index(rgb, stride, 1, 1, palette, 2, "3x1 scale2 x=1");
        check_index(rgb, stride, 2, 1, palette, 2, "3x1 scale2 x=2");
        check_index(rgb, stride, 3, 1, palette, 3, "3x1 scale2 x=3");
        check_index(rgb, stride, 4, 1, palette, 3, "3x1 scale2 x=4");
        check_initial(rgb, stride, 5, 1, "3x1 scale2 stops at x=5");
        check_initial(rgb, stride, 0, 0, "3x1 scale2 row above untouched");
        check_initial(rgb, stride, 0, 3, "3x1 scale2 row below untouched");
    }

    // scale 3, clipped on the right and bottom.
    {
        const auto sprite = make_sprite(2, 2, {1, 2, 3, 4});
        const int stride = 8 * 3;
        auto rgb = make_target(8, 6, stride);
        gm::SpriteBlit p;
        p.x = 6;
        p.y = 3;
        p.scale = 3;
        p.transparent_index = -1;
        run_nearest(palette, sprite, 8, 6, stride, p, rgb);
        check_index(rgb, stride, 6, 3, palette, 1, "2x2 scale3 clipped top-left");
        check_index(rgb, stride, 7, 5, palette, 1, "2x2 scale3 clipped stays in texel 1");
        check_initial(rgb, stride, 5, 3, "2x2 scale3 starts at x=6");
    }

    // Transparent index 0: both zero corners stay background.
    {
        const auto sprite = make_sprite(2, 2, {0, 5, 6, 0});
        const int stride = 8 * 3;
        auto rgb = make_target(8, 6, stride);
        gm::SpriteBlit p;
        p.x = 3;
        p.y = 2;
        p.transparent_index = 0;
        run_nearest(palette, sprite, 8, 6, stride, p, rgb);
        check_index(rgb, stride, 4, 2, palette, 5, "key0 top-right drawn");
        check_index(rgb, stride, 3, 3, palette, 6, "key0 bottom-left drawn");
        check_initial(rgb, stride, 3, 2, "key0 top-left transparent");
        check_initial(rgb, stride, 4, 3, "key0 bottom-right transparent");
    }

    // alpha 0 is a valid no-op; the whole frame must stay bit-identical.
    {
        const auto sprite = make_sprite(2, 2, {1, 2, 3, 4});
        const int stride = 8 * 3;
        auto rgb = make_target(8, 6, stride);
        const std::uint64_t before = fnv1a64(rgb);
        gm::SpriteBlit p;
        p.x = 3;
        p.y = 2;
        p.alpha = 0;
        std::string error;
        const bool ok = gm::blit(target_view(rgb, 8, 6, stride), palette, sprite, p, error);
        check(ok && error.empty(), "alpha 0 is a successful no-op");
        check_golden(fnv1a64(rgb), before, "alpha 0 leaves the frame unchanged");
    }

    // additive, alpha 255: clamp per channel at 255.
    {
        const auto sprite = make_sprite(1, 1, {255});
        const int stride = 6 * 3;
        auto rgb = make_target(6, 4, stride);
        gm::SpriteBlit p;
        p.x = 1;
        p.y = 1;
        p.additive = true;
        p.transparent_index = -1;
        run_nearest(palette, sprite, 6, 4, stride, p, rgb);
        // Palette[255] = (255,0,249); dst (1,1) = (21,42,65).
        check_pixel(rgb, stride, 1, 1, {255, 42, 255}, "additive saturates at 255");
    }

    // Bilinear centre of a 2x2 block, weights .5625/.1875/.1875/.0625.
    {
        const auto sprite = make_sprite(2, 2, {1, 2, 3, 4});
        const int stride = 8 * 3;
        auto rgb = make_target(8, 7, stride);
        gm::SpriteBlit p;
        p.x = 1;
        p.y = 1;
        p.scale = 2;
        p.transparent_index = -1;
        run_bilinear(palette, sprite, 8, 7, stride, p, rgb);
        check_pixel(rgb, stride, 2, 2, {1, 253, 12}, "bilinear 2x2 centre weights");
    }

    // Bilinear coverage <= 0.5 drops the pixel (four texels, one opaque).
    {
        const auto sprite = make_sprite(2, 2, {0, 2, 3, 0});
        const int stride = 9 * 3;
        auto rgb = make_target(9, 8, stride);
        gm::SpriteBlit p;
        p.x = 2;
        p.y = 1;
        p.scale = 2;
        p.transparent_index = 0;
        run_bilinear(palette, sprite, 9, 8, stride, p, rgb);
        check_initial(rgb, stride, 3, 2, "bilinear coverage 0.375 is transparent");
        check_pixel(rgb, stride, 4, 2, {1, 253, 9}, "bilinear coverage 0.625 is blended");
    }

    // Synthetic FNV sweep (bilinear alpha+additive included).
    struct Case {
        const char* name;
        int width;
        int height;
        std::vector<std::uint8_t> pixels;
        int target_width;
        int target_height;
        gm::SpriteBlit params;
        bool bilinear;
        std::uint64_t golden;
    };
    const auto cases = std::vector<Case>{
        {"syn_2x2_plain", 2, 2, {1, 2, 3, 4}, 6, 4, {1, 1, 1, false, false, 255, false, -1},
         false, 0xcd06f798ac7c9797ull},
        {"syn_2x2_flip", 2, 2, {1, 2, 3, 4}, 6, 4, {1, 1, 1, true, true, 255, false, -1},
         false, 0x645b91aedb80b747ull},
        {"syn_3x1_scale2_clip", 3, 1, {1, 2, 3}, 8, 5, {-1, 1, 2, false, false, 255, false, -1},
         false, 0xbf6e0ec71d118beeull},
        {"syn_key0", 2, 2, {0, 5, 6, 0}, 8, 6, {3, 2, 1, false, false, 255, false, 0},
         false, 0xad9605eedab6bcb5ull},
        {"syn_alpha128", 2, 2, {1, 2, 3, 4}, 8, 6, {3, 2, 1, false, false, 128, false, -1},
         false, 0x74f137c7393d531aull},
        {"syn_add255", 2, 2, {1, 2, 3, 4}, 8, 6, {3, 2, 1, false, false, 255, true, -1},
         false, 0xf4760823bad8a587ull},
        {"syn_bilinear_2x2", 2, 2, {1, 2, 3, 4}, 8, 7, {1, 1, 2, false, false, 255, false, -1},
         true, 0x91adcab1f73f4869ull},
        {"syn_bilinear_key_flip", 3, 3, {0, 2, 3, 0, 5, 2, 1, 0, 4}, 9, 8,
         {-1, 0, 2, true, false, 255, false, 0}, true, 0x7129feaf17f777d6ull},
        {"syn_bilinear_alpha_add", 2, 2, {1, 2, 3, 4}, 7, 5,
         {1, 0, 3, false, false, 128, true, -1}, true, 0x85c7d50ed324f772ull},
    };
    for (const Case& c : cases) {
        const auto sprite = make_sprite(c.width, c.height, c.pixels);
        const int stride = c.target_width * 3;
        auto rgb = make_target(c.target_width, c.target_height, stride);
        const std::uint64_t got = c.bilinear
                                      ? run_bilinear(palette, sprite, c.target_width,
                                                     c.target_height, stride, c.params, rgb)
                                      : run_nearest(palette, sprite, c.target_width,
                                                    c.target_height, stride, c.params, rgb);
        check_golden(got, c.golden, c.name);
    }
}

void real_cases(const std::vector<std::uint8_t>& palette, const fs::path& game) {
    struct Case {
        const char* name;
        const char* sub;
        const char* sprite;
        int width;
        int height;
        int stride_extra;
        gm::SpriteBlit params;
        bool bilinear;
        std::uint64_t golden;
    };
    const auto cases = std::vector<Case>{
        {"real_aflm1_nearest", "sprites", "AFLM0001", 160, 120, 0,
         {-10, -20, 1, false, false, 255, false, 0}, false, 0xa51c5882558f4e82ull},
        {"real_blam10_scale2_flip", "sprites", "BLAM0010", 200, 160, 0,
         {17, 9, 2, true, false, 255, false, 0}, false, 0x0e3f29d51a3d7c2eull},
        {"real_expl1_scale3_flipy_alpha", "sprites", "EXPL0001", 210, 190, 0,
         {-25, 40, 3, false, true, 128, false, 0}, false, 0xfa29573b5ea595e6ull},
        {"real_baracuda_pics", "PICS", "BARACUDA", 128, 96, 13,
         {-17, 3, 1, false, false, 255, false, 0}, false, 0x19120dfff9a726eaull},
        {"real_aflm1_bilinear", "sprites", "AFLM0001", 160, 140, 0,
         {5, 7, 2, false, true, 255, false, 0}, true, 0x980692a7aeb84e98ull},
        {"real_expl1_additive", "sprites", "EXPL0001", 180, 150, 0,
         {11, 13, 2, false, false, 255, true, 0}, false, 0x7a2fc62d8ea9b0d2ull},
        {"real_blam1_bilinear_alpha", "sprites", "BLAM0001", 256, 192, 0,
         {-4, -6, 3, true, false, 128, false, 0}, true, 0xcfb1a077b4b1113bull},
    };
    for (const Case& c : cases) {
        const auto loaded = load_real_sprite(game, c.sub, c.sprite);
        if (!loaded) {
            check(false, std::string("real sprite unavailable: ") + c.name);
            continue;
        }
        const gm::Sprite& sprite = *loaded;
        check(sprite.pixels.size() == static_cast<std::size_t>(sprite.width) *
                                           static_cast<std::size_t>(sprite.height),
              std::string(c.name) + ": level 0 size matches the header");
        const int stride = c.width * 3 + c.stride_extra;
        auto rgb = make_target(c.width, c.height, stride);
        const std::uint64_t got = c.bilinear
                                      ? run_bilinear(palette, sprite, c.width, c.height, stride,
                                                     c.params, rgb)
                                      : run_nearest(palette, sprite, c.width, c.height, stride,
                                                    c.params, rgb);
        check_golden(got, c.golden, c.name);
    }
}

void negative_cases(const std::vector<std::uint8_t>& palette) {
    const auto sprite = make_sprite(2, 2, {1, 2, 3, 4});
    const int stride = 8 * 3;

    const auto expect_false = [&](auto fn, const std::string& what) {
        auto rgb = make_target(8, 6, stride);
        std::string error;
        const bool ok = fn(rgb, error);
        check(!ok && !error.empty(), what + " is rejected with a message");
    };

    expect_false([&](auto& rgb, std::string& error) {
        return gm::blit(target_view(rgb, 0, 6, 0), palette, sprite, {}, error);
    }, "zero-width target");
    expect_false([&](auto& rgb, std::string& error) {
        return gm::blit(target_view(rgb, 8, 0, stride), palette, sprite, {}, error);
    }, "zero-height target");
    expect_false([&](auto& rgb, std::string& error) {
        auto short_target = std::vector<std::uint8_t>(stride * 6 - 1);
        return gm::blit(target_view(short_target, 8, 6, stride), palette, sprite, {}, error);
    }, "short target buffer");
    expect_false([&](auto& rgb, std::string& error) {
        return gm::blit(target_view(rgb, 8, 6, stride - 1), palette, sprite, {}, error);
    }, "stride below width*3");
    expect_false([&](auto& rgb, std::string& error) {
        const std::vector<std::uint8_t> short_palette(256 * 4 - 1);
        return gm::blit(target_view(rgb, 8, 6, stride), short_palette, sprite, {}, error);
    }, "palette shorter than 256 RGBA entries");

    const auto bad_sprite = [&](const gm::Sprite& bad, const std::string& what) {
        auto rgb = make_target(8, 6, stride);
        std::string error;
        check(!gm::blit(target_view(rgb, 8, 6, stride), palette, bad, {}, error) && !error.empty(),
              what + " is rejected with a message");
    };
    auto zero_width = sprite;
    zero_width.width = 0;
    bad_sprite(zero_width, "zero-width sprite");
    auto zero_height = sprite;
    zero_height.height = 0;
    bad_sprite(zero_height, "zero-height sprite");
    auto short_sprite = sprite;
    short_sprite.pixels.pop_back();
    bad_sprite(short_sprite, "short sprite pixel buffer");

    const auto bad_params = [&](const gm::SpriteBlit& params, const std::string& what) {
        auto rgb = make_target(8, 6, stride);
        std::string error;
        check(!gm::blit(target_view(rgb, 8, 6, stride), palette, sprite, params, error) &&
                  !error.empty(),
              what + " is rejected with a message");
        check(!gm::blit_scaled_bilinear(target_view(rgb, 8, 6, stride), palette, sprite,
                                        params, error) &&
                  !error.empty(),
              std::string("bilinear ") + what + " is rejected with a message");
    };
    auto p = gm::SpriteBlit{};
    p.scale = 0;
    bad_params(p, "scale 0");
    p.scale = -1;
    bad_params(p, "negative scale");
    p.scale = gm::kSpriteMaxScale + 1;
    bad_params(p, "scale above the limit");
    p.scale = std::numeric_limits<int>::max();
    bad_params(p, "huge scale");
    p = gm::SpriteBlit{};
    p.alpha = -1;
    bad_params(p, "negative alpha");
    p.alpha = 256;
    bad_params(p, "alpha above 255");
    p = gm::SpriteBlit{};
    p.transparent_index = -2;
    bad_params(p, "transparent index below -1");
    p.transparent_index = 256;
    bad_params(p, "transparent index above 255");

    // Valid empty rectangles are successful no-ops, not errors.
    {
        auto rgb = make_target(8, 6, stride);
        const std::uint64_t before = fnv1a64(rgb);
        std::string error;
        gm::SpriteBlit off;
        off.x = -100;
        off.y = 10000;
        const bool ok = gm::blit(target_view(rgb, 8, 6, stride), palette, sprite, off, error);
        check(ok && error.empty(), "fully clipped sprite is a successful no-op");
        check_golden(fnv1a64(rgb), before, "fully clipped sprite leaves the frame unchanged");
    }
}

} // namespace

int main() {
    const auto palette = make_palette();
    synthetic_cases(palette);
    negative_cases(palette);

    const fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (fs::exists(game)) {
        std::string error;
        const auto installed = gm::installed_palette(game, error);
        if (!installed) {
            check(false, "installed palette: " + error);
        } else {
            real_cases(*installed, game);
        }
    } else {
        std::cout << "sprite: game assets absent, synthetic cases only\n";
    }

    if (failures != 0) {
        std::cerr << "sprite: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "sprite: all checks passed\n";
    return 0;
}
