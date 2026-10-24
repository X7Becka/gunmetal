#pragma once

// The exact fade tables of the cockpit band (`M8.5` slice 3): render/hud_lut.h.
//
// What this is
// ------------
// The HUD layer `0x491a70` does not blit the `PANEL` picture as it is: every
// pixel index goes through one of 16 tables of 256 bytes at `0x682f90`
// (`0x491f10`..`0x491f2d` passes `&DAT_00682f90 + k*0x100` to the blit
// helper), and the level `k` comes from the player field `+0x81c24c`
// (`k = (field + 0x80) >> 4`, clamped to 15; docs/HUD_AUDIT.md §2.7).  The
// tables are built at start-up by `0x48f970` -> `0x483290(dst, k/11, 1)`, so
// level 11 is the identity and level 0 is black.  The dump
// `~/.cache/gunmetal-re/hud_lut.bin` (4096 bytes, `tools/win/dump_lut.lua`) is
// the only artifact the shipped game does not contain, because the tables are
// derived from the palette at run time.
//
// Table, and the code that builds it
// ----------------------------------
// `build_hud_lut` **builds** the tables from the installed palette with the
// formula of `0x483290` (docs/LUT_BUILDER.md): 6-bit palette slots
// (`component >> 2`), target component `trunc(component * k/11)`, then the
// weighted nearest-slot search of `0x4228e0` over slots `1..254` with a strict
// `<`, skipping the 20 slots of `0x483250` (`{173,174,175,191} ∪ {240..255}`).
// On the real `FRONTEND/GAMEPAL` it reproduces the dump byte for byte
// (4096/4096, FNV `0x6330406254771f3d`), so the dump is a check, not the
// source.  `load_hud_lut` still reads the dump when one is at hand.
//
// `render/upscale.h` (`fade_rgba`) keeps the channel model
// `(v*k + max/2)/max` for RGBA frames; `build_model_lut` builds the same model
// in the index space of these tables, which is what makes the difference
// measurable instead of a claim.
//
// What is proven by code and data (numbers: docs/HUD_LUT.md §2)
// -------------------------------------------------------------
//  * 16 tables are really built; level 11 is a colour identity on all 256
//    entries (254/256 by index: `0 -> 1`, both black, and `65 -> 48`, which
//    has the same RGB as `65`).
//  * level 0 pushes almost everything to index 1 (black) but leaves the 20
//    slots of the skip list alone (173..175, 191, 240..255, plus `1`, which is
//    black anyway).
//  * levels 12..15 are **not** near-identity: 132/73/48/45 colour identities
//    out of 256.  The factor `k/11 > 1` brightens, which the clamped RGBA
//    model cannot express.
//
// Not here (next slice, docs/HUD_LUT.md §5): wiring the tables into
// `fade_rgba`/`ui/pager_render` or the CLI, and a palettised frame path - the
// tables are index -> index, so a caller that wants colours has to apply
// `FRONTEND/GAMEPAL` (res/bmp.h `installed_palette`).
//
// Errors are text (`std::string* error`), following `render/upscale.h`: a
// caller prints ``[error] hud_lut: ...`` and returns 2 instead of drawing a
// silently black or half-loaded table.  `lut` is zeroed on every failure.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace gm {

/// Levels in `0x682f90`: one table per fade step, `k = 0..15`.
inline constexpr int kHudLutLevels = 16;

/// Entries in one table: one per palette index, `0..255`.
inline constexpr int kHudLutEntries = 256;

/// Size of the dump: `16 * 256` bytes, exactly.
inline constexpr std::size_t kHudLutBytes =
    static_cast<std::size_t>(kHudLutLevels) * static_cast<std::size_t>(kHudLutEntries);

/// Highest level the tables cover; the engine clamps `k` to it
/// (docs/HUD_AUDIT.md §2.7: `k = (field + 0x80) >> 4`, clamp 15).
inline constexpr int kHudLutMaxLevel = kHudLutLevels - 1;

/// Level whose table is the identity: the engine builds the tables with the
/// factor `k/11` (`0x483290(dst, k/11, 1)`), so `k == 11` neither darkens nor
/// brightens.  It is **not** the highest level.
inline constexpr int kHudLutIdentityLevel = 11;

/// Bytes of a palette this module accepts for colour comparisons: 256 RGBA
/// quads, the layout of `BmpImage::palette` (`res/bmp.h`) and of
/// `gm/palette.py::installed`.
inline constexpr std::size_t kHudLutPaletteBytes =
    static_cast<std::size_t>(kHudLutEntries) * 4u;

/// The 16 fade tables, exactly as dumped from `0x682f90`.
///
/// `levels[k][i]` is the index the engine draws where the source picture has
/// palette index `i` at fade level `k`.  It is a **palette index**, not a
/// colour and not a channel value.
struct HudLut {
    std::array<std::array<std::uint8_t, kHudLutEntries>, kHudLutLevels> levels{};

    bool operator==(const HudLut&) const = default;
};

/// Numbers one load produced.
///
/// `source` is the path as given (not canonicalised: the caller may print it
/// back), `bytes` is the file size that was accepted (always `kHudLutBytes`
/// on success), `levels`/`entries` echo the table shape, so
/// `bytes == levels * entries`.
struct HudLutStats {
    std::string source;
    std::size_t bytes = 0;
    int levels = 0;
    int entries = 0;
};

/// Numbers one level of a loaded table has.
///
/// `identity` counts entries with `levels[k][i] == i` (same palette index).
/// `colour_identity` counts entries whose **RGB triple** equals the source
/// entry's triple in the supplied palette - the stronger statement, because
/// two indices can name the same colour (`0` and `1` are both black).  It is
/// `-1` when no usable palette was given (`palette.size() != kHudLutPaletteBytes`),
/// so "not measured" is never printed as a zero.
///
/// `white` counts entries equal to `255`; `min`/`max` are the smallest and
/// largest entry of the table.  All four are plain counts/values, no model.
struct HudLutLevel {
    int identity = 0;
    int colour_identity = -1;
    int white = 0;
    int min = 0;
    int max = 0;
};

/// Read the 4096-byte dump into `lut`.
///
/// Accepts exactly `kHudLutBytes` bytes: the tables are positional
/// (`levels[k] = bytes[k*256 .. k*256+255]`), so a short file would silently
/// shift every level and a longer one would hide an unexpected second payload.
/// A missing/unreadable file, an empty file and any other size are refused
/// with text naming the path and the size that was found.
///
/// `stats` is filled only on success; `lut` is zeroed on every failure, so a
/// stale table is never drawn as a loaded one.
bool load_hud_lut(const std::filesystem::path& file, HudLut& lut,
                  std::string* error = nullptr, HudLutStats* stats = nullptr);

/// Build the 16 fade tables from the installed palette, exactly as `0x48f970`
/// does at start-up (`docs/LUT_BUILDER.md`):
///
///     for k = 0..15: 0x483290(dst = &0x682f90 + k*256, double(k) * 1/11, 1)
///
/// `palette_rgba` is the 256-quad palette of `kHudLutPaletteBytes` bytes, RGBX
/// as `BmpImage::palette`/`installed_palette` return it (alpha is ignored).  The
/// builder works in the engine's **6-bit** colour space: every component is
/// `component >> 2` (the loader at `0x47f0b4` shifts the BMP palette down by
/// two, and `0x418769` clamps to `0x3f`), and the metric constants only make
/// sense there.  A component of slot `i` becomes `trunc(component * factor)`
/// with `factor = k/11` in `double`; `0x40e0e0` (`__ftol`) sets x87 rounding
/// to truncation, so this is truncation, not rounding.  Its precision control
/// stays at 53 bits, so the product is the `double` one this code computes; an
/// 80-bit extended product would differ in 36 of 3776 slots
/// (docs/HUD_LUT.md §2b).  The colour then goes
/// back to the nearest palette slot by the weighted metric of `0x4228e0`
/// (docs/LOOK16.md §1):
///
///     d(j) = (pj.r + r + 0x100) * (pj.r - r)^2
///          + 512                * (pj.g - g)^2
///          + (0x17f - pj.r - r) * (pj.b - b)^2
///
/// and the search covers slots `1..254` with a strict `<`, so a tie keeps the
/// **lower** index and slot `0` is never produced.  The 20 slots of `0x483250`
/// (`{173,174,175,191} ∪ {240..255}`) are copied unchanged (`dst[i] = i`).
///
/// On the real `FRONTEND/GAMEPAL` this reproduces the dump byte for byte -
/// 4096/4096, FNV-1a64 `0x6330406254771f3d` (docs/HUD_LUT.md §2b).  It is not a
/// second model: `build_model_lut` stays the `(v*k + max/2)/max` approximation.
///
/// The result is a **palette index -> palette index** table like the dump, so
/// colours still come from the palette.  Fails (false + *error*, `out` zeroed)
/// when the span is not exactly `kHudLutPaletteBytes` bytes or has no data,
/// because a short palette would silently shift every slot.
bool build_hud_lut(std::span<const std::uint8_t> palette_rgba, HudLut& out,
                   std::string* error = nullptr);

/// `true` for the 20 slots `0x483250` leaves alone: `{173,174,175,191}` and
/// `240..255` (docs/LUT_BUILDER.md §3).  Exposed for tests and callers that
/// want to say *why* an entry did not fade.
bool hud_lut_skip_slot(int index);

/// The table entry for `index` at fade `level`: `levels[clamp(level, 0, 15)][index]`.
///
/// `level` is clamped to `0..15` (**not** to `0..11`): the engine's own clamp
/// is 15 (docs/HUD_AUDIT.md §2.7), and levels 12..15 are real tables that
/// brighten, not aliases of 11.  `index` is a `std::uint8_t`, so it cannot be
/// out of range by construction.
std::uint8_t map(const HudLut& lut, int level, std::uint8_t index);

/// Per-level numbers of `level_report` (see `HudLutLevel` above).
///
/// `level` is clamped to `0..15` like `map`.  `palette` is the 256-quad RGBA
/// table of `FRONTEND/GAMEPAL` (res/bmp.h `installed_palette`); pass it to get
/// `colour_identity`, leave it empty and the field is `-1`.  Alpha is ignored:
/// the engine's palette has no alpha channel that the fade touches.
HudLutLevel level_report(const HudLut& lut, int level,
                         std::span<const std::uint8_t> palette = {});

/// 16 lines, one per level, of the numbers `level_report` returns:
///
///     level  0: identity  21/256 colour-identity  22/256 white 1 min 1 max 255
///
/// Exactly 16 lines, so a caller can print the block as it is; the numbers in
/// docs/HUD_LUT.md §2 are that output, not a re-count.  Without a palette the
/// field reads `colour-identity n/a`.
std::string describe(const HudLut& lut, std::span<const std::uint8_t> palette = {});

/// Fill `lut` with the current channel **model** for comparison:
///
///     levels[k][v] = (v * min(k, max_level) + max_level/2) / max_level
///
/// i.e. the same integer curve as `fade_lut` in `render/upscale.h`
/// (`(v*k + max/2)/max`, half up) applied in the index space of these tables.
/// With `max_level == 11` this is exactly the formula that builds the engine's
/// tables (`0x483290(dst, k/11, 1)`); with `max_level == 11` it is also the
/// pager's `fade_channel`.
///
/// It is a model, not a second copy of the dump: the test compares the two and
/// prints how many of the 256 entries agree per level, which is how the
/// difference of docs/UPSCALE.md §3.2 is kept measurable.
///
/// Fails (false + *error*, `lut` zeroed) when `max_level` is outside
/// `1..kHudLutMaxLevel` (the struct has only 16 levels, so a larger scale has
/// no identity table and is not a meaningful model here).
bool build_model_lut(HudLut& lut, int max_level, std::string* error = nullptr);

} // namespace gm
