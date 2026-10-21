#pragma once

// Pure movement primitives for the original AI (`0x476a50`/`0x475f10` family).
//
// This is the A2/A3 step of docs/AI_CPP.md: a single waypoint seek with the
// original's octagonal arrival metric, the "back off" step of the attack mover
// (`0x473240`, report §6.2) and a swept-sphere collision clamp.  It
// deliberately does not own state, timers or pathfinding.

#include <cstddef>
#include <cstdint>
#include <span>

#include "render/collide.h"

namespace gm {

struct AiMoveParams {
    double speed_per_tick = 0.0; ///< THING_TYPES+0x10, units per 91 Hz tick
    double radius = 0.0;         ///< collision radius (Thing::radius)
    double arrive_radius = 50.0; ///< +0x70, octagonal distance
};

struct AiSeekResult {
    Vec3d position{}; ///< from + clamped delta
    Vec3d delta{};    ///< actual displacement this step
    bool arrived = false;
    bool blocked = false;
};

/// One seek step. `dt` is seconds (normally 1/91).  The desired displacement
/// is `speed_per_tick * 91 * dt`; a wall clamps it to the swept-sphere contact
/// only when `radius > 0` (radius 0 disables the collision query).
AiSeekResult ai_seek_step(const Vec3d& from, const Vec3d& target,
                          const CollisionWorld& world, const AiMoveParams& params,
                          double dt);

/// One "back off" step, the `retreat` branch of `0x473240`: walk straight away
/// from `threat` by `speed_per_tick * 91 * dt`, with the same swept-sphere
/// clamp as `ai_seek_step`.  The direction is planar (the engineering scaffold
/// has no floor, so the vertical difference never moves the body); a threat
/// directly above/below produces no step.  `arrive_radius` is ignored -- the
/// caller decides when the standoff is wide enough.
AiSeekResult ai_retreat_step(const Vec3d& from, const Vec3d& threat,
                             const CollisionWorld& world,
                             const AiMoveParams& params, double dt);

// --- A4: path family (`0x475f10`), pure and deterministic -----------------
//
// The original's `0x475f10` is a small automaton over a fixed list of
// locations (`THING_TYPES +0x320` count, `+0x324` ids, resolved through the
// location table `0x80d8e0`).  It owns no coordinates, only the current index
// (`inst+0x4ec`) and the "a point is set" flag (`inst+0x4f4`); the walking is
// done by `0x476a50`, which clears `+0x4f4` on arrival.  This primitive keeps
// that split: `path_advance` chooses the next waypoint, the caller moves the
// body and clears `state.waiting` when it gets there.
//
// `pathrandom` (movement type 11) is deliberately *not* modelled: it calls
// `rand()` (`0x480e30`, report §6.8), and determinism is a hard requirement of
// this scaffold.
enum class AiPathMode : std::uint8_t {
    path = 9,    ///< `0x475f10` case 9: walk to the last point, then stand
    loop = 10,   ///< case 10: pathloop, wrap 3 -> 0
    expire = 12, ///< case 12: pathexpire, die after reaching the last point
};

/// `0x475f10` rejects `0x1f < i`, so a shipped path has at most 32 points.
/// Longer spans are truncated to the first 32 waypoints.
inline constexpr int kAiPathMaxPoints = 32;

/// Persistent path state of one agent: the original's `inst+0x4ec` index and
/// the `inst+0x4f4` "point is set" flag, plus the terminal `expired` latch
/// (`0x4703a0` has already been posted; the thing is dead).
struct AiPathState {
    int index = -1;       ///< current/last selected waypoint, -1 = not started
    bool waiting = false; ///< +0x4f4 > 0: point selected, body not there yet
    bool expired = false; ///< pathexpire already reached the end
};

/// Result of one `path_advance` call.  `state` is the input state after the
/// transition, so a caller threads it exactly like the original's two fields.
struct AiPathStep {
    int index = -1;      ///< selected (or still pending) waypoint index
    Vec3d point{};       ///< waypoint value at `index` (valid when index >= 0)
    bool selected = false; ///< a *new* waypoint was chosen by this call
    bool waiting = false;  ///< a point is pending; the body must walk to `point`
    bool arrived = false;  ///< `path`: already standing on the last point
    bool expired = false;  ///< `expire`: last point reached, post `0x4703a0`
    AiPathState state{};   ///< state to pass to the next call
};

/// One step of `0x475f10` over `waypoints`, in order, without randomness and
/// without touching any global.  Semantics (report §6.8):
///
/// * while `state.waiting` is set nothing new is chosen (the walker is still
///   on its way); `selected` is false and the pending point is returned;
/// * otherwise the next index is `index + 1`; past the end `path` stays on the
///   last point (`arrived`), `loop` wraps to 0, `expire` reports `expired`;
/// * `expire` also expires immediately on an empty list (the original's
///   `path_count < 1` branch); `path`/`loop` stay idle there;
/// * an out-of-range index (corrupt save/type) restarts from -1 instead of
///   reading outside the list.
AiPathStep path_advance(const AiPathState& state,
                        std::span<const Vec3d> waypoints, AiPathMode mode);

/// Clear the pending flag when the body reached `point` (the arrival branch of
/// `0x476a50`); the next `path_advance` then selects the following waypoint.
/// A no-op on an already cleared state.
inline void path_mark_arrived(AiPathState& state) { state.waiting = false; }

} // namespace gm
