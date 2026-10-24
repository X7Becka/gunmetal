#pragma once

// Depth fog of `gundx.exe` 1.36 (M4.1.6, slice 2): the software frame path.
//
// The engine's fog is a *palette index -> palette index* table lookup, applied
// per drawn pixel by the rasteriser pass at `0x4409b8`:
//
//     fbuf[i] = lighttable[level*0x100 + fbuf[i]]        0x441b2d..0x441b5c
//
//     z     = |(64 / a0) * (e2 . (p_world - eye))|       0x440c98..0x440ed1
//     level = clamp(63 - trunc(z), 4, 63)                 0x441921..0x44197d
//
// `e2` is the camera's view-axis row (`ctx+0x48`, docs/CAMERA.md §4), `a0` is
// `ctx+0xa0 = 0.8 * W * k(level)` (256 at 320x200 with `k = 1`, so `64/a0 =
// 0.25`), and `trunc` is the proven `fistp` with RC=11 (`0x40e0e0`: truncation
// toward zero, not rounding).  `0x4804b0` is `fabs`, not `sqrt`: there is no
// `fsqrt` anywhere in the rasteriser.  All of this is proven by
// `tools/re/fog.py --self-test` (21 instruction anchors) and written up in
// docs/FOG.md.
//
// `lighttable` (`ds:0x567104`, 256x256 bytes, built at start-up by `0x4201fe`)
// is a lerp of the 6-bit palette colour toward white with weight `row/64`:
//
//     row < 0x40    c' = min(63, (pal6[c][k]*(0x40-row) + row*0x40) >> 6)
//     0x40..0x7f    white (63,63,63)
//     row >= 0x80   identity: table[row][col] = col
//
// The only rows the fog pass can reach are 4..63 (level is clamped), so row 63
// is the last "white" row and row 0 is the untouched palette.
//
// Two steps of the engine are *not* reproduced and are documented as a model:
//
//   * the builder ends with an 18-bit `r<<12|g<<6|b` -> palette index step
//     through the run-time dither tables `0x567128`/`0x56712c` (BSS).  This
//     module substitutes "exact colour, else nearest palette entry", exactly
//     like `tools/re/fog.py`;
//   * `a0` is an input: `k(level)` (`0x81bf98 + 0x190*index`) is written at run
//     time and has not been decoded, so `FogParams::a0` stays a caller value.
//
// The 3dfx path of the same function (`grFogMode(2)`, `grFogColorValue
// (0xffffff)`, `guFogGenerateExp(table, 0.9f)`, `0x440a8c`..`0x440ab0`) is
// *not* modelled: it is Glide state, not a formula this engine executes.
//
// API shapes: the plain functions are the engine formula and never fail - they
// return `std::numeric_limits<double>::quiet_NaN()` (`fog_z`) or leave the
// buffer untouched (`fog_frame`) for inputs the engine cannot see.  The
// overloads taking `std::string* error` are the checked variants and refuse
// with a text (a0 <= 0, non-finite coordinates, level outside 0..255, palette
// byte > 63).  Nothing here allocates, reads a clock or keeps state, so two
// runs over the same inputs are bit-identical.

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "render/render.h" // Vec3d

namespace gm {

/// `ctx+0xa0`: the depth scale `0.8 * W * k(level)`.  The default is the
/// 320x200, `k = 1` value proven in docs/FOG.md §2 (so `64/a0 = 0.25` and one
/// fog level is 4 world units).
struct FogParams {
    double a0 = 256.0;
};

/// Level range the rasteriser clamps to (`0x441963`/`0x441976`).
inline constexpr int kFogLevelMin = 4;
inline constexpr int kFogLevelMax = 63;

/// Rows of `lighttable`: 256 rows x 256 columns of bytes.
inline constexpr int kFogRows = 256;
inline constexpr int kFogColumns = 256;

/// First row of the "white" band (`0x4202f4`) and of the byte-identity band
/// (`0x4204e2`).
inline constexpr int kFogRowWhite = 0x40;
inline constexpr int kFogRowIdentity = 0x80;

/// `|(64/a0) * (e2 . (p - eye))|` - the camera-space forward distance of *p*
/// scaled by `64/a0` (`0x440c98`..`0x440ed1`).
///
/// Returns NaN when `a0` is not a positive finite number, when any coordinate
/// is not finite, or when the product overflows; the checked overload below
/// names the offending input.
double fog_z(const FogParams& params, Vec3d eye, Vec3d e2, Vec3d p);

/// Checked `fog_z`: on success stores `z` and clears *error; on refusal leaves
/// `z` untouched and fills *error with a text.
bool fog_z(const FogParams& params, Vec3d eye, Vec3d e2, Vec3d p, double& z,
           std::string* error);

/// `clamp(63 - trunc(z), 4, 63)` (`0x441921`..`0x44197d`).
///
/// The argument must be finite; a non-finite `z` returns `kFogLevelMin`
/// (the "no fog" end of the range) so a caller that ignores errors can never
/// index outside the table.  Use the checked overload to get a text instead.
int fog_level(double z);

/// Checked `fog_level`: refuses NaN/inf with a text.
bool fog_level(double z, int& level, std::string* error);

/// The 256x256 `lighttable` (`ds:0x567104`), row-major: `data[level*256+col]`.
struct FogTable {
    std::array<std::uint8_t, 256 * 256> data{};
};

/// Build the whole table from a 6-bit palette (values 0..63, the layout of the
/// engine table at `0x83d320`; the BMP loader shifts the file palette by two).
///
/// Rows 0..0x3f use the `0x4201fe` lerp, rows 0x40..0x7f are white, rows
/// 0x80..0xff are the byte identity.  The quantisation back to a palette index
/// is "first exact colour, else nearest by squared 6-bit distance, ties to the
/// lowest index" - the model of the run-time dither tables (docs/FOG_CPP.md).
///
/// Returns false and fills *error when `pal6` is null or holds a byte > 63;
/// the table is left untouched on refusal.
bool build_fog_table(const std::uint8_t pal6[256][3], FogTable& table,
                     std::string* error);

/// `table[level*256 + index]` (`0x441b29`..`0x441b5c`).
///
/// Levels outside 0..255 return *index* unchanged (a no-op, never an
/// out-of-bounds read); the checked overload refuses with a text.
std::uint8_t fog_pixel(const FogTable& table, int level, std::uint8_t index);

/// Checked `fog_pixel`: refuses a level outside 0..255 with a text.
bool fog_pixel(const FogTable& table, int level, std::uint8_t index,
               std::uint8_t& out, std::string* error);

/// Apply one fog row to a whole 8-bit frame in place, exactly as the pass does:
/// `framebuffer[i] = table[level*256 + framebuffer[i]]`.
///
/// An invalid level leaves the frame untouched; the checked overload refuses.
void fog_frame(const FogTable& table, int level,
               std::span<std::uint8_t> framebuffer);

/// Checked `fog_frame`: refuses a level outside 0..255; the span is untouched.
bool fog_frame(const FogTable& table, int level,
               std::span<std::uint8_t> framebuffer, std::string* error);

} // namespace gm
