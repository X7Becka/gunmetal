// Thing animation binding: AI state -> clip -> frame (`M6.3.1`, slice 2).
//
// The engine keeps two independent facts that the C++ port never joined:
//
//  * a thing instance carries a **model name** (`SOB_TABLE` index through
//    `THING_TYPES +0x1a4`, `docs/THINGS_CPP.md`), and the model loader
//    `0x42b4e1` resolves that name to `SOBS/<name>.sob` **first** and to
//    `Sobanim/<name>.soo` only when no `.sob` of that name exists
//    (`docs/SOO.md` §2).  A `.soo` is therefore a *clip*: a named list of
//    poses with 91 Hz durations, played by `0x433e00`;
//  * the AI state (`AiState`, `world/ai.h`) is a runtime value with no
//    animation field at all.
//
// This module is the missing join, kept as pure data: no renderer, no
// `Things` mutation, no SDL.  It reads the shipped `Sobanim/` directory and
// evaluates the *proven* timeline (`res/anim.h`: mode 0 holds the last frame,
// mode 1 wraps, durations are engine ticks at 91 Hz) for one thing at one
// tick.
//
// What is **proven** (see docs/THING_ANIM_BIND.md §1 for the evidence):
//
//  * a `Thing::model` name whose `SOBS/<name>.sob` is absent is played as the
//    `.soo` of the same name (`0x42b4e1`, all 40 shipped clips satisfy the
//    precondition - none of them has a same-named `.sob`);
//  * `mode` 0/1 = hold/loop and the frame selection at a tick
//    (`0x433e00`, `docs/SOO.md` §3);
//  * only the **first** pose of a clip carries the animation id
//    (`0x42b7bd`), so the clip belongs to the model, not to a runtime state.
//
// What is a **model** (i.e. not supported by shipped data or by a proven
// engine branch):
//
//  * the state -> clip *variant* mapping.  No shipped `.soo` is named
//    `<model>_idle` / `<model>_walk` / `<model>_atk` / `<model>_dead` (0 of
//    40), and the clip names that do look state-like (`kndeath`, `kneel`,
//    `slump`, `kingyell`) are separate **models** of separate types, not
//    variants of one model.  The suffix is therefore only a *probe*: the base
//    clip of the model is the fallback for every state, and the shipped
//    corpus always takes that fallback;
//  * `state == dead` holds the clip on its last frame even when the file says
//    `mode 1` (a dead body must not loop).  `ThingAnimConfig::hold_on_death`
//    switches that off; with `false` the file's own mode is used verbatim;
//  * `patrol` and `alert` share the "walk" clip (`ThingClip::walk`): the AI
//    scaffold has no separate "walk" and "alert" motion, and the data has no
//    clip for either.
//
// Deliberately not done here: interpolation/blending between clips (the
// engine morphs *within* a clip, `0x433f6d`; there is no cross-clip blend),
// `.soa` playback (`.soa` is the sprite/flag animation of `0x42b81b`, not a
// model clip, and a `.sob` carries no timeline at all), and wiring the result
// into `Things`/`Ai` - that needs the frame tick of `M9`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "res/anim.h"
#include "world/ai.h"
#include "world/things.h"

namespace gm::thinganim {

/// Largest clip file read; the shipped `.soo` are 30..600 bytes
/// (`docs/SOO.md` §4), so this only bounds a hostile file.
inline constexpr std::size_t kMaxClipBytes = 1u << 20;

/// The clip a thing plays, as far as the *model* can tell.  `none` exists for
/// the "no clip" answer and for the invalid-state refusal; it is never a
/// successful binding.
enum class ThingClip : std::uint8_t { none, idle, walk, attack, death };

/// "none", "idle", "walk", "attack", "death".
const char* clip_name(ThingClip clip);

/// The probe suffix a state-specific clip would carry: `_idle`, `_walk`,
/// `_atk`, `_dead`; empty for `none`.  These files do not exist in the
/// shipped corpus (0 of 40) - the base clip is the fallback.  Exposed so the
/// convention is testable and documented instead of hidden.
std::string_view clip_suffix(ThingClip clip);

/// `AiState` -> clip, **model** mapping (docs/THING_ANIM_BIND.md §2):
///
///     idle   -> idle      patrol -> walk     alert -> walk
///     attack -> attack    dead   -> death
///
/// The `optional` is the refusal channel for a value that is not one of the
/// five named states (for example `static_cast<AiState>(200)`), which
/// `bind_thing_anim` rejects instead of inventing a clip.
std::optional<ThingClip> clip_for_state(AiState state);

/// One resolved instant: the clip that was chosen and the pose inside it.
///
/// `clip` is the *requested* state clip; `clip_name` is the **actual** clip
/// that was loaded - the stem as spelled in `Sobanim/`, e.g. `Cultist`.  They
/// differ when the state-specific variant does not exist and the base clip is
/// used (always, on the shipped data).  `frame` indexes the frames the engine
/// would load (`min(declared, frames)`, `0x42b5f1` + `0x42b637`), and `tick`
/// is the *normalised* engine phase (`SooPose::phase`): for mode 1 it is
/// `tick % total`, for mode 0 it is clamped to `total-1`.  `looping` is
/// `mode == 1` after `hold_on_death` has been applied.
struct ThingAnimBinding {
    ThingClip clip = ThingClip::none;
    std::string clip_name;
    int frame = 0;
    int tick = 0;
    bool looping = false;
};

/// Where the clips come from and the one behavioural model knob.
struct ThingAnimConfig {
    /// Installation root (the directory that holds `Sobanim/`).  Empty means
    /// `resolve_anim_root()`: `$GUNMETAL_DIR`, then the read-only mirror
    /// `~/.cache/gunmetal-re/game`, then `/mnt/d/games/Gunmetal 3dfx`.
    std::filesystem::path root;
    /// A dead thing holds its clip's last frame (see the header comment).
    /// `false` uses the file's own `mode` for every state.
    bool hold_on_death = true;
};

/// The installation the default configuration uses; an empty path when none
/// of the candidates has a `Sobanim/` directory.  Never writes anything.
std::filesystem::path resolve_anim_root(const ThingAnimConfig& config = {});

/// Drop the parsed-clip and directory caches (tests switch roots with it).
void clear_clip_cache();

/// Resolve "thing + AI state + tick" to a clip and a frame.
///
/// The clip name comes from `Thing::model`; an empty model falls back to
/// `Thing::type_name` (the same `THING_TYPES` record, so the fallback is a
/// name, not a guess).  The lookup probes `<name><clip_suffix>` first and the
/// base `<name>` second, ASCII-case-insensitively (the shipped files mix
/// `Cultist.soo`/`DISRUPT.SOO`), inside `<root>/Sobanim/`.
///
/// The frame is computed by `soo_pose_at_tick()` - the proven `0x433e00`
/// timeline - and therefore inherits its arithmetic exactly: 91 Hz ticks,
/// mode 0 hold, mode 1 wrap, linear `alpha` inside the frame.
///
/// Refusals (`nullopt`, error text starts with `thing_anim:`, `*error` is
/// cleared on success; `error == nullptr` is allowed):
///
///  * `tick < 0` - the engine clamps to 0 (`0x433e84`), this module refuses,
///    because a negative tick always means a caller bug;
///  * an `AiState` outside the five named values;
///  * a thing with neither a model nor a type name;
///  * a model name containing a path separator or `..` (path traversal);
///  * no `Sobanim/` directory (`root` unset and no candidate installation);
///  * no clip file for the model (a static `.sob` model, or an unknown name);
///  * the clip file itself is unreadable, larger than `kMaxClipBytes`, or not
///    a `.soo`;
///  * the clip has no frames, a non-positive total duration, or a `mode`
///    other than 0/1 (the messages come from `parse_soo`/`soo_pose_at_tick`,
///    prefixed here).
std::optional<ThingAnimBinding> bind_thing_anim(const Thing& thing, AiState state, int tick,
                                                std::string* error = nullptr);

std::optional<ThingAnimBinding> bind_thing_anim(const Thing& thing, AiState state, int tick,
                                                const ThingAnimConfig& config,
                                                std::string* error = nullptr);

/// One pass over a thing list: `bound + unbound == things.size()` and
/// `animated + still == bound`.
///
/// `animated` means the pose is still moving at `tick`: the clip loops, or
/// (mode 0) the phase has not reached the last tick yet - and the clip has
/// more than one frame with a total duration above one tick.  `still` is a
/// bound thing frozen on its final frame.  Both are exactly the
/// `hold`/`loop` distinction of `0x433e8b`/`0x433eb4` applied to one tick.
struct ThingAnimStats {
    int bound = 0;
    int unbound = 0;
    int animated = 0;
    int still = 0;
};

/// Bind every thing against the parallel state list (`states[i]` belongs to
/// `things[i]`).  Things without a state, or with a refused binding, count as
/// `unbound`; the first refusal text is copied into `first_error` when it is
/// non-null.  A negative `tick` refuses every pair.  Deterministic: the order
/// is the input order, nothing is random or time-dependent.
ThingAnimStats collect(std::span<const Thing> things, std::span<const AiState> states, int tick,
                       std::string* first_error = nullptr);

ThingAnimStats collect(std::span<const Thing> things, std::span<const AiState> states, int tick,
                       const ThingAnimConfig& config, std::string* first_error = nullptr);

} // namespace gm::thinganim
