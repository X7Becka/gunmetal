#pragma once

// First/third-person camera rig over the player state of
// `player/controller.h` (docs/CAMERA_CPP.md).
//
// Pure mathematics, no SDL and no global state.  The proven part reproduces
// the engine's first-person camera exactly:
//
//   * basis `0x41e025 = Rz(-yaw) · Rx(-pitch) · Ry(roll)`; the columns are
//     `right` (e1), `forward` (e2) and `up` (e3); `forward = e2`
//     (docs/CAMERA.md §2–§4);
//   * `eye = player.pos + (0, 0, eye_height)`, i.e. the proven
//     `camera_z = spawn_z + player[0x2F50]` formula (§5);
//   * angles: `pitch = player[0x18] + view_pitch + shake_pitch`,
//     `roll = player[0x20] + shake_roll`, `yaw = player[0x28] + shake_yaw`;
//     the body pitch clamps to ±π/6, the body roll to ±20°, view pitch to
//     ±π/2 (§6, docs/SHAKE.md §2–§5);
//   * the three `SET_CAMERA_*_SHAKE` channels are triangle waves with period
//     `4·period_ms`, peak `amplitude` and `angle += amplitude/period · s`
//     (docs/SHAKE.md §5, tools/re/shake.py);
//   * `SOURCEPLAYER_RECOIL(D)` is a horizontal velocity impulse, *not* an
//     angle (docs/SHAKE.md §6);
//   * projection focal scales `+0x90 = zoom`, `+0x94 = zoom·4H/(3W)`
//     (docs/CAMERA.md §7).
//
// Everything is in the engine world frame.  The `.sbw` object-header basis
// `diag(1.875)` (docs/FORMATS.md §12.4) is already applied by res/sbw and
// render/collide, so `PlayerState::pos`, spawns and collision results are the
// same frame; never scale the eye or target by 1.875 again.
//
// Third person, smoothing and the near plane are explicitly hypotheses
// (docs/CAMERA_CPP.md §7): the shipped game only builds the first-person and
// the cutscene frames, and the engine has no proven camera interpolation.
//
// Determinism: `double` only, a fixed order of operations, no clock, no
// randomness, no static mutable state.  With `-ffp-contract=off` (set by
// engine/CMakeLists.txt) two runs over the same inputs are bit-identical.

#include <array>
#include <string>

#include "player/controller.h" // PlayerState
#include "render/collide.h"    // CollisionWorld (optional third-person occlusion)
#include "render/render.h"     // Vec3d

namespace gm {

/// Row-major 4x4 matrix.  `view()` maps world points to the camera frame
/// (x right, y up, z forward), the same axes as `gm.render.Camera.view`.
struct Mat4 {
    std::array<double, 16> m{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                             0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};

    double& at(int row, int col) { return m[static_cast<std::size_t>(row * 4 + col)]; }
    double at(int row, int col) const {
        return m[static_cast<std::size_t>(row * 4 + col)];
    }

    /// Homogeneous product `M · (p, 1)`; the caller divides by `w` itself.
    std::array<double, 4> transform(const Vec3d& p) const;

    /// `transform` followed by the perspective divide.  A zero `w` yields a
    /// vector of NaN (there is no point on screen), never an exception.
    Vec3d project(const Vec3d& p) const;
};

/// Orthonormal camera basis in the engine world frame (X right, Y forward,
/// Z up): `right = e1`, `forward = e2`, `up = e3` of docs/CAMERA.md §3.
struct CameraBasis {
    Vec3d right{1.0, 0.0, 0.0};
    Vec3d forward{0.0, 1.0, 0.0};
    Vec3d up{0.0, 0.0, 1.0};
};

/// Proven basis `0x41e025` (`R = Rz(-yaw)·Rx(-pitch)·Ry(roll)`), with the
/// expanded formulas of docs/CAMERA.md §3.  `pitch > 0` looks down;
/// `yaw = +π/2` looks along +X.  Non-finite arguments produce a non-finite
/// basis; callers validate first (see `CameraRig::update`).
CameraBasis camera_basis(double roll, double pitch, double yaw);

/// Camera basis that looks from *eye* at *target*, with *up_hint* as the
/// world-up preference.  Mirrors `gm.render.Camera`: when the view direction
/// is parallel to *up_hint* (`|dot| > 0.99`) the fallback up becomes +Y, and
/// if that is parallel too, +X.  Returns false for a non-finite or zero
/// direction (and leaves *out* untouched); a degenerate *up_hint* is not an
/// error, it takes the fallback.
bool look_at(const Vec3d& eye, const Vec3d& target, const Vec3d& up_hint,
             CameraBasis& out, std::string* error = nullptr);

/// `0x443f6d..0x443f94`: 1.0 below 0.5, otherwise `0.25 + 1.5·scale`.
double decay_multiplier(double scale);

/// `SOURCEPLAYER_RECOIL(D)` (0x459f4d..0x459fc7): the horizontal velocity
/// impulse `(-sin(yaw)·|cos(view_pitch)|·D, -cos(yaw)·|cos(view_pitch)|·D, 0)`.
/// Only the view pitch is used, exactly as in the engine; the result is meant
/// to be added to the player velocity, not to a camera angle.
Vec3d recoil_velocity(double yaw, double view_pitch, double amount);

/// Focal scales of the engine camera (docs/CAMERA.md §7):
/// `x = zoom`, `y = zoom·4H/(3W)`.  At 320x200 this is `(1, 5/6)`, at
/// 640x480 `(1, 1)`.  Non-positive *width*/*height* yields `(0, 0)`.
struct FocalScales {
    double x = 1.0;
    double y = 1.0;
};
FocalScales engine_focal(double width, double height, double zoom = 1.0);

/// Angle conversion under the engine's normalisation (`tan(fov/2) = 1/focal`,
/// docs/CAMERA.md §7).  *focal* must be positive; 0 is returned otherwise.
double focal_to_fov(double focal);
double fov_to_focal(double fov);

/// One `SET_CAMERA_*_SHAKE` channel (docs/SHAKE.md §5, `ShakeChannel` in
/// tools/re/shake.py).  `period_ms` is the *quarter* period, `duration_ms` is
/// the remaining time and `amplitude` is the peak angle in radians.
struct ShakeChannel {
    double amplitude = 0.0;          ///< peak angle (radians)
    double amplitude_per_period = 0.0; ///< engine's pre-divided value
    int period_ms = 1;               ///< >= 1, quarter period
    int duration_ms = 0;             ///< remaining time, >= 0

    /// VM handler `0x45b411/0x45b462/0x45b4b3`: non-positive period becomes 1,
    /// negative duration becomes 0, amplitude is divided by the period.
    void set(double amplitude_rad, int period, int duration);
    void clear();
    /// `0x457271..0x4572b3`: `duration -= dt_ms`, floored at 0.
    void tick_ms(int dt_ms);
    /// `0x4467ef..0x4468b1`: 0 when the duration ran out, else
    /// `amplitude/period · s` with `s = (duration mod 4p) - p` for the first
    /// half and `3p - (duration mod 4p)` for the second.
    double value() const;

    bool active() const { return duration_ms > 0; }
};

/// One call of `SET_CAMERA_{PITCH,ROLL,YAW}_SHAKE`: peak amplitudes in
/// radians plus the shared quarter period and duration in milliseconds.
/// Channels with a zero amplitude are left untouched by `add_shake`.
struct ShakeImpulse {
    double pitch = 0.0;
    double roll = 0.0;
    double yaw = 0.0;
    int period_ms = 200;
    int duration_ms = 800;
};

enum class CameraMode {
    First,
    Third,
};

/// Per-frame look intent.
///   * `yaw`   — turn delta (radians); positive turns towards +X, the proven
///     `player[0x28] += pending` path.  Not clamped (the yaw-rate limiter is
///     dormant in this build, docs/SHAKE.md §4).
///   * `pitch` — *view*-pitch delta (`player[0x2E60]`), positive looks down,
///     clamped to ±π/2 after accumulation.
///   * `roll`  — body roll delta (`player[0x20]`), clamped to ±20°.
struct LookInput {
    double yaw = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
};

/// Tunables.  Every default that is not proven is marked so in
/// docs/CAMERA_CPP.md §7; the first-person path with the defaults reproduces
/// `gm/scene.first_person` bit-for-bit up to libm rounding.
struct CameraConfig {
    double body_pitch_limit = 0.5235987755982988;  ///< π/6, proven (0x81be18)
    double body_roll_limit = 0.3490658503988659;   ///< 20°, proven (0x81be20)
    double view_pitch_limit = 1.5707963267948966;  ///< π/2, proven (0x81bea8/b0)
    double focus_distance = 100.0; ///< look-at distance of gm/scene.first_person
    double smooth_time = 0.0;      ///< eye smoothing time constant, seconds;
                                   ///< 0 disables it (the proven direct path)
    double third_distance = 60.0;  ///< hypothesis, world units
    double third_height = 0.0;     ///< hypothesis, up offset of the eye
    double third_radius = 1.0;     ///< occlusion sphere radius; 0 disables
    double fov = 1.5707963267948966; ///< vertical FOV, radians (hypothesis 90°)
    double aspect = 4.0 / 3.0;       ///< NDC aspect (hypothesis)
    double near_plane = 0.9;         ///< hypothesis (camera +0xe0 = 0.9)
    double far_plane = 100000.0;     ///< hypothesis
    double max_dt = 0.25;            ///< larger dt is a caller error
    double yaw_rate_max = 0.0;       ///< rad/ms; 0 = no limit (dormant field)
    int default_shake_period_ms = 200;
    int default_shake_duration_ms = 800;
};

/// Snapshot of one camera update.  `yaw/pitch/roll` are the angles that built
/// the basis; `aim_yaw`/`view_pitch` are the player-level values used by
/// recoil; `player_eye` is the first-person point before smoothing/third-person
/// offset; `distance`/`occluded` are only meaningful in third-person mode.
struct CameraState {
    CameraMode mode = CameraMode::First;
    Vec3d eye{};
    Vec3d target{0.0, 100.0, 0.0};
    Vec3d right{1.0, 0.0, 0.0};
    Vec3d forward{0.0, 1.0, 0.0};
    Vec3d up{0.0, 0.0, 1.0};
    Vec3d player_eye{};
    double yaw = 0.0;        ///< camera yaw + shake yaw
    double pitch = 0.0;      ///< body pitch + view pitch + shake pitch
    double roll = 0.0;       ///< body roll + shake roll
    double aim_yaw = 0.0;    ///< player yaw + look yaw (no shake)
    double view_pitch = 0.0; ///< p+0x2E60 after the clamp
    double body_pitch = 0.0;
    double body_roll = 0.0;
    double shake_pitch = 0.0;
    double shake_roll = 0.0;
    double shake_yaw = 0.0;
    double eye_height = 10.0;
    double distance = 0.0;
    bool occluded = false;
    double fov = 1.5707963267948966;
    double aspect = 4.0 / 3.0;
    double near_plane = 0.9;
    double far_plane = 100000.0;
};

/// Outcome of one `update`.  On `ok == false` `error` explains why and the rig
/// is unchanged (the same contract as `StepResult` in player/controller.h).
struct CameraUpdate {
    bool ok = true;
    std::string error;
};

/// Deterministic camera rig: first-person state of the engine plus an explicit
/// third-person/smoothing extension.
class CameraRig {
public:
    CameraRig() = default;
    explicit CameraRig(const CameraConfig& config);

    const CameraConfig& config() const { return config_; }
    void set_config(const CameraConfig& config);

    /// Advance one frame.  `player.pos` is the feet point and
    /// `player.eye_height` the proven `player[0x2F50]`; `dt` is in seconds.
    CameraUpdate update(const PlayerState& player, const LookInput& look, double dt);

    CameraMode mode() const { return state_.mode; }
    void set_mode(CameraMode mode);

    /// Third-person hypothesis: eye shifts `distance` back along `-forward`
    /// and `height` up along the camera up axis.  With a collision world set,
    /// a sphere of *radius* is swept backwards from the first-person eye and
    /// the distance is shortened to the first contact (no wall clipping);
    /// `height` is applied after the sweep.
    void set_third_person(double distance, double height = 0.0, double radius = 1.0);
    /// Optional world for third-person occlusion; nullptr disables it.  The
    /// world must outlive the rig (or be cleared with nullptr first).
    void set_collision_world(const CollisionWorld* world);

    /// Set the torso angles (`player[0x18]`/`player[0x20]`), each clamped to
    /// the proven limits.  Non-finite values are ignored (returns false).
    bool set_body_angles(double pitch, double roll);

    /// Apply a triangle-wave shake (three `SET_CAMERA_*_SHAKE` channels).
    /// Returns false without changing anything when the impulse is not finite,
    /// the period is < 1 or the duration is < 0.
    bool add_shake(const ShakeImpulse& impulse);
    /// Shorthand: pitch channel only, with `config().default_shake_*`.
    bool add_shake(double pitch_amplitude);
    void clear_shake();

    /// One-shot look impulse: `pitch` goes to the pending view-pitch
    /// accumulator (`player[0x2F38]`), `yaw` applies on the next update.
    /// Returns false for non-finite values.
    bool add_look_impulse(double pitch, double yaw = 0.0);

    /// Drive the (dormant) view-pitch decay branch of `0x443a36..0x443a58` /
    /// `0x443f6d..0x44406b`.  The shipped build never writes
    /// `player[0x3360/0x3364/0x2F48/0x2FE0]`, so the defaults keep it off;
    /// the setters exist so the model can be checked against tools/re/shake.py
    /// and for future network/demo snapshots.  Non-finite values are ignored.
    void set_view_pitch_decay(int timer_a, int timer_b, double scale = 0.5,
                              double divisor = 1.0);
    void set_pitch_locked(bool locked) { pitch_locked_ = locked; }

    /// `SOURCEPLAYER_RECOIL(D)` under the current aim angles.  The caller adds
    /// the result to the player velocity; the rig itself is not modified.
    Vec3d recoil_velocity(double amount) const;

    const CameraState& state() const { return state_; }
    const Vec3d& eye() const { return state_.eye; }
    const Vec3d& target() const { return state_.target; }
    const Vec3d& up() const { return state_.up; }
    const Vec3d& forward() const { return state_.forward; }
    const Vec3d& right() const { return state_.right; }

    /// World -> view matrix (x right, y up, z forward), row-major:
    /// rows are `right`, `up`, `forward`, each with `-dot(axis, eye)`.
    Mat4 view() const;

    /// Standard perspective for the frame of `view()`: z forward, `w' = z`.
    /// `fov` is the vertical field of view in radians.  Returns the identity
    /// for invalid arguments (fov outside (0, π), aspect <= 0, near <= 0,
    /// far <= near, or any non-finite value).
    Mat4 projection(double fov, double aspect, double near_plane, double far_plane) const;
    Mat4 projection() const;
    /// The engine's exact focal scales: `fx = zoom`, `fy = zoom·4H/(3W)`
    /// (docs/CAMERA.md §7).  This is `projection(fov, aspect, ...)` with
    /// `fy = 1/tan(fov/2)` and `aspect = fy/fx`.
    Mat4 engine_projection(double width, double height, double zoom, double near_plane,
                           double far_plane) const;

    /// Drop all accumulated look state, shake and smoothing (back to the
    /// constructor state).  The configuration and mode are kept.
    void reset();

    /// Make the next smoothed eye exactly the player eye (teleport/respawn).
    void snap_smoothing() { smoothing_ready_ = false; }

    const ShakeChannel& shake_channel(int index) const;

private:
    CameraState compute_state(const Vec3d& player_eye, const PlayerState& player) const;

    CameraConfig config_{};
    CameraMode mode_ = CameraMode::First;

    double yaw_offset_ = 0.0;   ///< accumulated player[0x28] look delta
    double body_pitch_ = 0.0;   ///< player[0x18]
    double body_roll_ = 0.0;    ///< player[0x20]
    double view_pitch_ = 0.0;   ///< player[0x2E60]
    double pitch_pending_ = 0.0; ///< player[0x2F38]
    double yaw_pending_ = 0.0;   ///< player[0x2F40]
    double yaw_rate_ = 0.0;      ///< player[0x2DA0]
    double decay_scale_ = 0.5;   ///< player[0x2F48]
    double decay_divisor_ = 1.0; ///< player[0x2FE0]
    int timer_a_ = 0;            ///< player[0x3360], never written by the build
    int timer_b_ = 0;            ///< player[0x3364], never written by the build
    bool pitch_locked_ = false;  ///< player[0x3358]
    std::array<ShakeChannel, 3> shake_{}; ///< pitch, roll, yaw

    const CollisionWorld* world_ = nullptr;

    bool smoothing_ready_ = false;
    Vec3d smoothed_eye_{};

    CameraState state_{};
};

} // namespace gm
