#pragma once

// Pure movement primitives for the original AI (`0x476a50`/`0x475f10` family).
//
// This is the A2 step of docs/AI_CPP.md: a single waypoint seek with the
// original's octagonal arrival metric and a swept-sphere collision clamp.  It
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
/// is `speed_per_tick * 91 * dt`; a wall clamps it to the swept-sphere contact.
AiSeekResult ai_seek_step(const Vec3d& from, const Vec3d& target,
                          const CollisionWorld& world, const AiMoveParams& params,
                          double dt);

} // namespace gm
