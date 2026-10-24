// Effect frame resolution (`M4.2.6`, slice 2): `-k` -> `.sbw` effect table ->
// frame texture -> archive entry.
//
// A negative polygon texture index is not "no texture": `-k` selects entry
// `k-1` of the effect table of the *same* `.sbw` (loader `0x42aa63`,
// docs/EFFECTS.md §2.1), and the frame drawn is that entry's frame at
// `now - poly[+0x68]` (`0x41de7d`, docs/SOA.md §4).  This module is the
// read-only bridge between those two facts: it resolves an index to its series,
// frame index and frame texture, and checks that the texture really is in the
// BIGFILE archive the texture registry's `kind` selects (docs/EFFECTS.md §2.3).
//
// The per-index -> record half lives in `game/effect_anim.h`
// (`effect_for_texture`, `effect_frame_index`); this module adds the level view
// (catalog), the texture/archive check and the drawing-order answer of
// docs/EFFECTS.md §4.2.  Nothing here is wired into the frame pass yet -
// docs/FX_FRAME.md lists what is still missing.
#pragma once

#include "res/sbw.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gm::fx {

/// One texture of the level registry (`.sbw` table 1, `0x76ed40` at runtime).
struct FxTextureRef {
    std::string name; ///< `+0x07` `char name[9]`
    int kind = 0;     ///< `+0x05`: 0 = `pics\`, 1 = `sprites\`
};

/// Level-scoped view of the effect path: the two `.sbw` tables plus every
/// negative index a polygon actually selects, in polygon order.
///
/// The record fields match `SbwEffectRef` on purpose - the shipped `.sbw` holds
/// the already converted runtime record (`period` in 91 Hz ticks) and its
/// `.soa` twin matches it on 262/262 (level, effect) pairs (docs/EFFECTS.md
/// §2.2), so a second record type would only invite drift.
struct SoaCatalog {
    std::string level;                  ///< informational, e.g. "level0"
    std::vector<SbwEffectRef> effects;  ///< table 2: `-k` -> `effects[k-1]`
    std::vector<FxTextureRef> textures; ///< table 1: frame ids are 1-based
    std::vector<int> negative_indices;  ///< one entry per effect polygon
};

/// Names of the shipped image archives the effect frames can come from.
/// `kind 0` -> `PICS/SBM.IDX`, `kind 1` -> `sprites/SBM.IDX`
/// (docs/EFFECTS.md §2.3); the names are compared case-insensitively because
/// the archives store uppercase names while `.sbw` keeps the authored case.
struct SbmCatalog {
    std::vector<std::string> pics;
    std::vector<std::string> sprites;

    /// `kind` 0 checks `pics`, 1 checks `sprites`; any other kind is false.
    bool contains(int kind, const std::string& name) const;
    /// "PICS" / "sprites" / "" (undocumented kind) - for error messages.
    const char* archive_name(int kind) const;
    /// True when both archives are empty (nothing to verify a frame against).
    bool empty() const { return pics.empty() && sprites.empty(); }
};

/// Build the level view from a parsed `.sbw`: copies tables 1-2 and walks the
/// polygons with the setup's own rule -
///
///     index = (handle < 1) ? i16(poly+0x00) : i16(poly+0x78)
///
/// (`0x9fde8e`..`0x9fdefc`), skipping `n_idx <= 2` polygons before the index is
/// read (`0x9fde93`).  Every negative result is appended to
/// `negative_indices`, duplicates included, so the caller can count polygons.
SoaCatalog make_soa_catalog(const SbwLevel& world, std::string level = {});

/// Key of the drawing order of docs/EFFECTS.md §4.2.  The three fields are the
/// ones a hypothetical effect-specific pass would want to sort by; §4.2 proves
/// they are *not* consulted for the effect branch.
struct FxFrameKey {
    int poly_texture = 0;
    std::uint16_t material = 0;
    std::uint16_t lightmap = 0;
};

/// Resolved effect frame.
struct FxFrameRef {
    int series_index = 0;      ///< `-negative_index - 1` (`effects[k-1]`)
    std::string series_name;   ///< record `+0x4c`
    int frame = 0;             ///< 0-based index inside the series at `tick`
    int frame_count = 0;       ///< record `+0x08` = `|ids|` (1..32)
    bool animated = false;     ///< `mode == 1 && frame_count > 1` (index moves)
};

/// Resolve `-k` against one level.
///
/// * `now_tick` is `[0x9e82d8] - poly[+0x68]` in 91 Hz ticks; shipped polygons
///   have `poly[+0x68] == 0`, so the caller usually passes the render tick.
/// * Returns the series and the frame active at `now_tick`, after checking the
///   frame id against the registry and the frame texture against the archive.
/// * `*error` is cleared on success and filled with a self-contained sentence
///   on every refusal (index >= 0, empty catalogs, past the table, empty or
///   broken record, `period == 0` cycle, frame id outside the registry,
///   undocumented `kind`, texture missing from its archive).
std::optional<FxFrameRef> resolve_fx_frame(int negative_index, const SoaCatalog& soa,
                                           const SbmCatalog& sbm, std::string* error = nullptr,
                                           std::int64_t now_tick = 0);

/// Every distinct negative index of the level, resolved at `now_tick`.
///
/// The result is sorted by `series_index` (`-1` first) and de-duplicated, so
/// two calls on the same catalog give the same sequence.  Indices that do not
/// resolve are left out; their reasons are appended to `*errors` (cleared
/// first), which keeps the function total on a damaged world.
std::vector<FxFrameRef> frames_for_level(const SoaCatalog& soa, const SbmCatalog& sbm,
                                         std::int64_t now_tick = 0,
                                         std::vector<std::string>* errors = nullptr);

/// Rank of the drawing pass of an effect polygon: always `kFxSinglePass`.
constexpr int kFxSinglePass = 0;

/// docs/EFFECTS.md §4.2 (proven): there is no separate effect pass, queue or
/// depth sort - the drawer runs inside the ordinary polygon loop
/// (`0x41ac99`..`0x41add0`), in polygon order, and the frame branch only
/// rewrites the current image id (`0x9fdf1f`).  The setup does not read
/// `material`/`lightmap` for the effect branch either (§1.4), so every key has
/// the same rank.  The parameter exists to make an accidental "effects after
/// opaque geometry" second pass impossible to introduce silently.
int draw_order_rank(FxFrameKey key);

} // namespace gm::fx
