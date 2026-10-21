#pragma once

// Pure movement primitives for the original AI (`0x476a50`/`0x475f10` family).
//
// This is the A2/A3 step of docs/AI_CPP.md: a single waypoint seek with the
// original's octagonal arrival metric, the "back off" step of the attack mover
// (`0x473240`, report §6.2) and a swept-sphere collision clamp.  It
// deliberately does not own state, timers or pathfinding.

#include <cstddef>

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

} // namespace gm
