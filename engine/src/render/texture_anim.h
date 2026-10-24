// Texture scroll and 16-bit animation of a world polygon (`M4.2.2`, slice 2).
//
// Two fields next to the UV pair of a `.sbw` polygon were never read by the
// C++ engine: the scroll weights `+0x5c`/`+0x60` and the animated s16 slots
// `+0x5e`/`+0x62`.  The original reads them in two places:
//
//  * `0x434c80` (body at `0x434c11`) turns the weights plus the polygon's own
//    `u_axis`/`v_axis` into a **world-space offset**
//    `-t·(scroll_u·u_axis + scroll_v·v_axis)/256` (docs/FORMATS.md §10.1,
//    docs/WORLD_MATERIALS.md §3.2), where `t` is the integer scalar the caller
//    passes as the last argument (a tick count is the documented reading);
//  * `0x4342f0` (the interpolation loop of the `.soo` player `0x433e00`)
//    lerps the **16-bit halves** `0x5c`, `0x5e`, `0x60`, `0x62`, `0x66`
//    between two key polygons in float32 and stores the truncated result back
//    into the same 16-bit slot.  `0x5c`/`0x60` are read as `short`, the
//    `anim*` halves as `ushort`.
//
// This module is the pure arithmetic of those two facts: no render pass, no UV
// sampling, no scripting commands.  docs/TEXTURE_ANIM.md lists what is proven,
// what is a model and what is deliberately not wired.
//
// Units: everything here is `double` on the caller's side (the project rule for
// the render path); the engine itself computes in `float32` and truncates to
// `int` - see docs/TEXTURE_ANIM.md §2 and §3 for the two places where that
// matters (the float32 lerp and the `int` truncation are reproduced, the world
// offset is kept in `double`).
#pragma once

#include "render/render.h"
#include "res/sbw.h"

#include <cstdint>
#include <string>
#include <utility>

namespace gm::texanim {

/// The four 16-bit fields of one polygon that this slice covers, already
/// converted from the raw on-disk words (`SbwPoly` keeps them as `u16`).
///
/// `scroll_*` are **signed**: the engine reads `+0x5c`/`+0x60` with `float(int
/// *(short*))` (`0x434c11`), so `0xFFFF` is `-1` and not `65535`.
/// `anim0`/`anim1` keep the raw **unsigned** value `0..65535`: `0x4342f0`
/// reads them with `*(ushort*)` (docs/FORMATS.md §10.1 lists `0x5e` as `u16`),
/// and its interpolation is done in that unsigned domain.
struct PolyAnim {
    int scroll_u = 0; ///< `+0x5c`: s16 scroll weight along `u_axis`
    int scroll_v = 0; ///< `+0x60`: s16 scroll weight along `v_axis`
    int anim0 = 0;    ///< `+0x5e`: u16 animated half (interpolated by `0x4342f0`)
    int anim1 = 0;    ///< `+0x62`: u16 animated half (interpolated by `0x4342f0`)

    /// Convert the four raw little-endian disk words of a polygon.
    /// `scroll_*` are sign-extended, `anim*` are kept as `0..65535`.
    static PolyAnim from_raw(std::uint16_t scroll_u, std::uint16_t scroll_v,
                             std::uint16_t anim0, std::uint16_t anim1);
};

/// `PolyAnim` of a parsed polygon (`SbwPoly::scroll_u/scroll_v/anim0/anim1`).
PolyAnim from_poly(const SbwPoly& poly);

/// `scroll_u != 0 || scroll_v != 0` (as signed values: `0xFFFF` is not zero).
bool has_scroll(const PolyAnim& anim);

/// `anim0 != 0 || anim1 != 0`.  Note that the shipped `.sbw` files have a
/// non-zero `anim*` on **all** 235 069 polygons (the exporter left `f32` bits
/// there, docs/FORMATS.md §10.1), so this predicate is true almost everywhere
/// and is not a useful filter - it is the faithful "the slot is not empty".
bool has_anim(const PolyAnim& anim);

/// World-space offset of a scrolled polygon at `tick`:
///
///     offset = tick * (scroll_u * u_axis + scroll_v * v_axis) / 256.0
///
/// Proven as an expression by `0x434c11`/`0x434c80` (docs/FORMATS.md §10.1,
/// docs/WORLD_MATERIALS.md §3.2), with two caveats documented in
/// docs/TEXTURE_ANIM.md §2:
///
///  * the engine receives the scalar as an **integer** and **negates** it
///    (`* (float)-param_5`), i.e. its offset is this formula with `tick =
///    -param_5`; callers that want the engine's sign pass a negative tick;
///  * the engine additionally divides each axis term by `|axis|^2` (so a
///    non-unit axis is normalised).  This function keeps the documented
///    formula unchanged, i.e. it assumes unit axes - the loader rejects
///    non-unit ones at `0x434463` (`Invalid poly U vector`), although 22 % of
///    the shipped axes are not unit (docs/SBW_POLY.md §3.2), in which case the
///    engine offset is shorter by `1/|axis|` and the direction is the same.
///
/// Refusals (`error` set, zero vector returned, never throws):
/// `tick` is NaN/Inf; an axis component is NaN/Inf; `u_axis` or `v_axis` is
/// the zero vector; `scroll_u`/`scroll_v` does not fit s16.  `tick == 0`
/// gives exactly `(0, 0, 0)`.  The arithmetic is `double`, so `scroll = 32767`
/// does not overflow and is **not** saturated (the engine's `float32` does not
/// overflow either: the largest shipped weight is 15224).
Vec3d scroll_offset(const PolyAnim& anim, Vec3d u_axis, Vec3d v_axis, double tick,
                    std::string* error = nullptr);

/// The proven `0x4342f0` interpolation of the two animated halves between two
/// key polygons, `t` in `[0, 1]` being the keyframe fraction the player
/// computes as `(now - start_of_frame) / duration` (`0x433e00`):
///
///     out = trunc( (float)(u16(key_b) - u16(key_a)) * (float)t + (float)u16(key_a) )
///
/// computed in `float32` and stored back through a 16-bit slot (`trunc` is
/// `__ftol`, round toward zero).  Returns `{anim0, anim1}`: two independent
/// halves, not one value.
///
/// Refusals: `t` is NaN/Inf or outside `[0, 1]`; an `anim*` value does not fit
/// `u16`.  On refusal `{0, 0}` is returned and `*error` is set.
std::pair<int, int> anim_step(const PolyAnim& key_a, const PolyAnim& key_b, double t,
                              std::string* error = nullptr);

/// One-polygon convenience form used by the slice's tests: the polygon's own
/// `anim0` and `anim1` slots are taken as the two ends and each half is
/// stepped with the `0x4342f0` arithmetic, in opposite directions:
///
///     half0(t) = trunc( (anim1 - anim0) * t + anim0 )   // the 0x5e half
///     half1(t) = trunc( (anim0 - anim1) * t + anim1 )   // the 0x62 half
///
/// so `t = 0` reproduces the file's own pair `{anim0, anim1}` and `t = 1`
/// swaps it; `t = 0.5` gives the same midpoint in both slots.
///
/// **MODEL.**  The engine has no such per-polygon interpolation: `0x4342f0`
/// always lerps the *same* half between two key polygons of an `.soo` clip
/// (the pair of key records is chosen by the player `0x433e00`), and in the
/// shipped data `0x5e`/`0x62` are unrelated `f32` bit patterns, not two ends
/// of one animation (docs/FORMATS.md §10.1).  Use the two-key form above for
/// engine-faithful morphing; this overload is a shape the slice asked for.
/// Same refusals as the two-key form.
std::pair<int, int> anim_step(const PolyAnim& key, double t, std::string* error = nullptr);

} // namespace gm::texanim
