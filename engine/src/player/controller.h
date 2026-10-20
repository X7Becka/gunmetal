#pragma once

// Deterministic fixed-tick player controller (a vertical capsule) on top of
// the collision primitives in `render/collide.h`.
//
// Frame: the engine world frame of `.sbw` (X right, Y forward, Z up), the same
// one docs/CAMERA.md and docs/PLAYER.md use.  `PlayerState::pos` is the *feet*
// point (the bottom centre of the capsule); the camera sits at
// `pos.z + eye_height`, which is the proven first-person formula
// `camera_z = spawn_z + player[0x2F50]` (docs/PLAYER.md §4).
//
// Determinism: this file and controller.cpp use `double` only, never read the
// clock or any platform state, and keep a fixed order of operations (sphere 0,
// 1, 2; triangles in build order; sequential slides).  One call to `step()`
// advances exactly `ControllerConfig::dt` seconds, 1/91 s by default (the HUD
// playback tick, docs/HUD_PLAYER.md).  With `-ffp-contract=off` (set by
// engine/CMakeLists.txt) two runs of the same inputs are bit-identical.
//
// The capsule is approximated by three spheres (feet, middle, head) swept with
// `CollisionWorld::sphere_cast`.  That is a union-of-spheres capsule: it is
// exact on floors, walls and ceilings away from the rounded caps, and the
// rounded bottom is what produces the skin gap used for ground detection.
//
// See docs/PLAYER_CONTROLLER.md for the formulas, the proven/hypothesis split
// of the constants and the goldens.

#include <string>

#include "render/collide.h" // Vec3d, CollisionWorld

namespace gm {

/// Kinematic state of one player.  All distances are world units, velocities
/// are world units per second.
struct PlayerState {
    Vec3d pos{};            ///< feet point: bottom centre of the capsule
    Vec3d vel{};            ///< world units / second
    double yaw = 0.0;       ///< camera yaw in radians (docs/CAMERA.md §6)
    double height = 10.0;   ///< current capsule height (crouch aware)
    double eye_height = 10.0; ///< current eye height above `pos`
    bool on_ground = false; ///< contact with a floor from the previous tick
    bool crouching = false; ///< `height < ControllerConfig::height`
};

/// Player intent for one tick.  `forward`/`strafe` are in the player frame and
/// are clamped to [-1, 1]; `+forward` is along `yaw`, `+strafe` is to the right
/// (the camera `right` axis of docs/CAMERA.md).
struct Input {
    double forward = 0.0;
    double strafe = 0.0;
    bool jump = false;
    bool crouch = false;
};

/// Tunables.  Defaults are a LIGHT-chassis-sized capsule; the provenance of
/// every number is in docs/PLAYER_CONTROLLER.md §4.
struct ControllerConfig {
    double radius = 3.0;          ///< capsule radius (world units)
    double height = 10.0;         ///< standing capsule height
    double eye_height = 10.0;     ///< eye above the feet (LIGHT CHASSIS = 10.0)
    double gravity = 700.0;       ///< world units / s^2
    double accel = 10.0;          ///< ground acceleration, 1/s
    double friction = 8.0;        ///< ground friction, 1/s
    double max_speed = 1.70 * 91.0; ///< 154.7: LIGHT chassis op4 * 91 Hz
    double step_height = 3.0;     ///< max step-up and step-down snap
    double dt = 1.0 / 91.0;       ///< fixed tick length (seconds)
    double jump_speed = 200.0;    ///< take-off vertical speed
    double air_control = 0.35;    ///< fraction of `accel` while airborne
    double crouch_scale = 0.7;    ///< crouched height / standing height
    double max_dt = 0.25;         ///< larger `dt` is a caller error
    double contact_skin = 1e-6;   ///< push-out gap kept by depenetration
    int max_slides = 4;           ///< collide-and-slide iterations per substep
    int max_substeps = 32;        ///< hard cap on substeps per tick
};

/// Outcome of one tick.  On `ok == false` `error` explains why and `state` is
/// the unchanged input state; the controller never throws and never mutates
/// the world.
struct StepResult {
    bool ok = true;
    std::string error;
    PlayerState state{};
    bool hit_wall = false;
    bool hit_floor = false;
    bool hit_ceiling = false;
    bool stepped_up = false;
    bool stepped_down = false;
    bool jumped = false;
    int substeps = 0;
    int slides = 0;
};

/// Initial state for `feet` and `yaw`, with the capsule height and eye height
/// taken from `config`.
PlayerState make_state(const Vec3d& feet, double yaw, const ControllerConfig& config = {});

/// Camera point for rendering: `pos + (0, 0, eye_height)`.
Vec3d eye_position(const PlayerState& state);

/// Advance exactly `config.dt` seconds.  Inputs are validated first: NaN/inf
/// anywhere, a non-positive or too large `dt`, a non-positive radius/height or
/// a capsule thinner than its diameter are reported as a clean error.
StepResult step(const PlayerState& state, const Input& input, const CollisionWorld& world,
                const ControllerConfig& config = {});

} // namespace gm
