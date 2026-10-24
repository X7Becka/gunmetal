// Texture scroll and 16-bit animation of a polygon (`M4.2.2`, slice 2).
//
// The proven formulas (VA and docs) and the models are listed in
// docs/TEXTURE_ANIM.md; this file is only the arithmetic of
// `render/texture_anim.h`.
#include "render/texture_anim.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace gm::texanim {
namespace {

/// s16 -> int, the way the engine reads `+0x5c`/`+0x60` (`float(int *(short*))`).
int sign16(std::uint16_t raw) {
    return static_cast<int>(static_cast<std::int16_t>(raw));
}

bool finite(double value) {
    return std::isfinite(value);
}

double length(Vec3d v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

bool fits_s16(int value) {
    return value >= -32768 && value <= 32767;
}

bool fits_u16(int value) {
    return value >= 0 && value <= 65535;
}

/// One 16-bit half of the `0x4342f0` lerp: float32 arithmetic, `__ftol`
/// truncation toward zero, then the 16-bit store (`mov [poly+off], ax`).
/// `a`/`b` are the unsigned halves, `t` is already validated to `[0, 1]`.
int lerp16(int a, int b, double t) {
    const float fa = static_cast<float>(static_cast<std::uint16_t>(a));
    const float fb = static_cast<float>(static_cast<std::uint16_t>(b));
    const float f = static_cast<float>(t);
    const float r = (fb - fa) * f + fa;
    if (!std::isfinite(static_cast<double>(r))) {
        return static_cast<int>(a);
    }
    const int truncated = static_cast<int>(r); // round toward zero, like __ftol
    return static_cast<std::uint16_t>(truncated);
}

/// Shared validation of the two-key lerp: `t` finite and inside `[0, 1]`,
/// both anim slots fitting their unsigned 16-bit home.
bool anim_inputs_ok(const PolyAnim& a, const PolyAnim& b, double t, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    if (!finite(t)) {
        return fail("anim_step: t is not finite (NaN or Inf)");
    }
    if (t < 0.0 || t > 1.0) {
        return fail("anim_step: t must be a keyframe fraction in [0, 1]");
    }
    if (!fits_u16(a.anim0) || !fits_u16(a.anim1) || !fits_u16(b.anim0) || !fits_u16(b.anim1)) {
        return fail("anim_step: anim0/anim1 must fit the u16 slot (0..65535)");
    }
    return true;
}

} // namespace

PolyAnim PolyAnim::from_raw(std::uint16_t scroll_u, std::uint16_t scroll_v,
                            std::uint16_t anim0, std::uint16_t anim1) {
    PolyAnim anim;
    anim.scroll_u = sign16(scroll_u);
    anim.scroll_v = sign16(scroll_v);
    anim.anim0 = static_cast<int>(anim0);
    anim.anim1 = static_cast<int>(anim1);
    return anim;
}

PolyAnim from_poly(const SbwPoly& poly) {
    return PolyAnim::from_raw(poly.scroll_u, poly.scroll_v, poly.anim0, poly.anim1);
}

bool has_scroll(const PolyAnim& anim) {
    return anim.scroll_u != 0 || anim.scroll_v != 0;
}

bool has_anim(const PolyAnim& anim) {
    return anim.anim0 != 0 || anim.anim1 != 0;
}

Vec3d scroll_offset(const PolyAnim& anim, Vec3d u_axis, Vec3d v_axis, double tick,
                    std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return Vec3d{};
    };

    if (!finite(tick)) {
        return fail("scroll_offset: tick is not finite (NaN or Inf)");
    }
    if (!finite(u_axis.x) || !finite(u_axis.y) || !finite(u_axis.z)) {
        return fail("scroll_offset: u_axis is not finite");
    }
    if (!finite(v_axis.x) || !finite(v_axis.y) || !finite(v_axis.z)) {
        return fail("scroll_offset: v_axis is not finite");
    }
    if (!fits_s16(anim.scroll_u)) {
        return fail("scroll_offset: scroll_u must fit s16 (-32768..32767)");
    }
    if (!fits_s16(anim.scroll_v)) {
        return fail("scroll_offset: scroll_v must fit s16 (-32768..32767)");
    }
    if (length(u_axis) == 0.0) {
        return fail("scroll_offset: u_axis is zero (a scroll weight needs a direction)");
    }
    if (length(v_axis) == 0.0) {
        return fail("scroll_offset: v_axis is zero (a scroll weight needs a direction)");
    }

    // Documented formula: t * (scroll_u * u_axis + scroll_v * v_axis) / 256.0
    // (0x434c11 builds exactly this in float32, with `-param_5` as the scalar).
    const double scale = tick / 256.0;
    const double u_weight = static_cast<double>(anim.scroll_u) * scale;
    const double v_weight = static_cast<double>(anim.scroll_v) * scale;
    const Vec3d offset{u_weight * u_axis.x + v_weight * v_axis.x,
                       u_weight * u_axis.y + v_weight * v_axis.y,
                       u_weight * u_axis.z + v_weight * v_axis.z};
    if (!finite(offset.x) || !finite(offset.y) || !finite(offset.z)) {
        return fail("scroll_offset: tick too large, the offset is not finite");
    }
    return offset;
}

std::pair<int, int> anim_step(const PolyAnim& key_a, const PolyAnim& key_b, double t,
                              std::string* error) {
    if (!anim_inputs_ok(key_a, key_b, t, error)) {
        return {0, 0};
    }
    return {lerp16(key_a.anim0, key_b.anim0, t), lerp16(key_a.anim1, key_b.anim1, t)};
}

std::pair<int, int> anim_step(const PolyAnim& key, double t, std::string* error) {
    if (!anim_inputs_ok(key, key, t, error)) {
        return {0, 0};
    }
    // MODEL (see the header): the two slots are each other's far key.
    return {lerp16(key.anim0, key.anim1, t), lerp16(key.anim1, key.anim0, t)};
}

} // namespace gm::texanim
