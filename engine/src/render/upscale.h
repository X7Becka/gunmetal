#pragma once

// Integer upscale and palette fade for the 320x200 front-end frames (`M8.5`
// slice 2).
//
// Why
// ---
// The pager (`ui/pager_render.h`) and the menus (`ui/menu_render.h`) produce a
// 320x200 frame, `0xRRGGBBAA`, exactly the mode-13h front-end buffer
// `0x6d8fc0` (docs/UI.md §6.3).  The shell shows that buffer at 1:1 or with
// the letterbox blit of `render/scale.h`, which resamples RGB24 to an arbitrary
// window and can filter.  Neither is the presentation this module is for: a
// *pixel-exact* integer zoom that repeats every source pixel as a
// ``scale x scale`` block, so a 320x200 frame becomes 640x400 / 960x600 /
// 2560x1600 with no interpolation, no rounding and no colour change at all.
// The idea is the same one `render/hud.h` uses for HUD layers
// (`HudLayer::scale`, docs/HUD_CPP.md §2): nearest neighbour, every source
// texel becomes a square block, pixels are copied, not blended.
//
// The fade
// --------
// `fade_rgba` applies the front-end fade curve to a finished frame.  The curve
// is the engine's: 16 tables of 256 bytes at `0x682f90`, built at start-up by
// `0x483290(dst, k/11, 1)` (docs/HUD_AUDIT.md §2.7), factor `level/max_level`,
// identity at `level == max_level`, black at `level == 0`.  Applying it to a
// 320x200 RGBA canvas is a *model*: the engine's tables map a palette *index*
// to a palette *index*, and the requantisation back into the palette is not
// resolved, so the only exact statements are the two ends of the curve
// (docs/UPSCALE.md §3, measured against the `hud_lut.bin` dump).
//
// Performance
// -----------
// The fade is built as 256 table entries per call and then applied by three
// lookups per pixel, not by recomputing `(v*level + max/2)/max` per channel:
// a 320x200 frame needs 192 000 lookups instead of 192 000 multiplications and
// divisions, and the numbers the test prints are in docs/UPSCALE.md §4.
//
// Errors are text (`std::string* error`) so a caller can print
// ``[error] upscale: ...`` and return 2 instead of showing a silently empty or
// partly filled buffer.  `out` is emptied on every failure, so a stale frame is
// never presented as a successful upscale.
//
// What is deliberately *not* here (docs/UPSCALE.md §5): bilinear or any other
// filtering (that is `render/scale.h`, which takes RGB24 and a window size),
// aspect/letterbox handling, an 8 bpp palettised path (this module only knows
// `0xRRGGBBAA`), and the window/CLI wiring (`--png-scale`) - the module is the
// arithmetic, the caller owns the presentation.

#include <cstdint>
#include <string>
#include <vector>

namespace gm {

/// Largest integer zoom accepted by `upscale_nearest`.  A policy limit (the
/// 320x200 canvas at 8 is 2560x1600, i.e. 4 096 000 pixels) and a guard against
/// typo-sized requests, not an arithmetic limit: the size check is 64-bit.
inline constexpr int kUpscaleMaxScale = 8;

/// Entries in the fade table: one per 8-bit channel value, `0..255`.
inline constexpr int kFadeLutSize = 256;

/// Numbers one upscale produced.
///
/// `pixels_in`/`pixels_out` are the source and destination pixel counts, so
/// `pixels_out == pixels_in * scale * scale` always holds on success; `scale`
/// echoes the zoom that was applied.
struct UpscaleStats {
    int scale = 1;
    std::size_t pixels_in = 0;
    std::size_t pixels_out = 0;
};

/// Integer nearest-neighbour zoom of a packed `0xRRGGBBAA` frame.
///
/// Writes `w * scale` by `h * scale` pixels into `out`, each source pixel
/// repeated as a `scale x scale` block:
///
///     out[(y*scale + dy) * (w*scale) + x*scale + dx] == src[y*w + x]
///
/// for every `x, y` and every `0 <= dx, dy < scale`.  No channel is touched, so
/// alpha travels with the colour unchanged.
///
/// Fails (returns false, fills *error*, clears *out*) when `src` is null,
/// `w`/`h` is not positive, `scale` is outside `1..kUpscaleMaxScale`, or
/// `w * h * scale * scale` does not fit in `std::size_t`.  `stats` and `error`
/// may be null; `stats` is filled only on success.
bool upscale_nearest(const std::uint32_t* src, int w, int h, int scale,
                     std::vector<std::uint32_t>& out, std::string* error = nullptr,
                     UpscaleStats* stats = nullptr);

/// Build the 256-entry channel table of one fade level.
///
/// `lut[v] = (v * level + max_level/2) / max_level` in integer arithmetic, i.e.
/// the documented curve rounded to nearest (half up).  `level` is clamped to
/// `0..max_level`: 0 is black, `max_level` (and anything above) is the
/// identity.  With `max_level == 11` this is exactly the pager's
/// `fade_channel` (`ui/pager_render.cpp`, docs/PAGER_RENDER.md §2.5), so the two
/// paths cannot drift.
///
/// Fails (false + *error*) when `max_level < 1`; that is the only refused
/// input, because a level outside the range has a defined meaning (clamp) and
/// the engine's tables cover `0..15` with `11` as the identity.
bool fade_lut(int level, int max_level, std::uint8_t (&lut)[kFadeLutSize],
              std::string* error = nullptr);

/// Apply the fade curve of `fade_lut` to a packed `0xRRGGBBAA` buffer.
///
/// RGB are mapped through the table, the alpha byte is preserved **exactly**
/// (the engine fades indexed colour, where there is no alpha to fade).  The
/// table is built once per call, not once per pixel.
///
/// Properties the test pins: `level == 0` leaves `0xRRGGBB00|alpha` (black, alpha
/// kept), `level >= max_level` returns the buffer byte for byte, no channel ever
/// grows (``lut[v+1] >= lut[v]`` and ``lut[v] <= v``), and the result is
/// deterministic.  An empty buffer is a successful no-op.
///
/// `level` is clamped to `0..max_level`; `max_level < 1` is refused with text,
/// because the curve has no meaning without a scale.
bool fade_rgba(std::vector<std::uint32_t>& rgba, int level, int max_level,
               std::string* error = nullptr);

} // namespace gm
