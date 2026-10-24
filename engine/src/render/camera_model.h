#pragma once

// Camera model: camera modes, chase/orbit placement, the engine's focal
// scales and a deterministic shake impulse (docs/CAMERA_MODEL.md).
//
// Provenance is split explicitly; every number either points at a section of
// docs/CAMERA.md / docs/SHAKE.md / docs/CAMERA_CPP.md or is marked MODEL.
//
//   * cockpit (first person) is proven: the camera sits at the player eye and
//     its orientation is `yaw/pitch/roll`; `pitch > 0` looks down, `yaw = 0`
//     looks along +Y (docs/CAMERA.md §2–§5, `0x446590`/`0x41e025`).
//   * engine focal scales are proven: `fx = zoom`, `fy = zoom·4H/(3W)`, i.e.
//     `(1, 5/6)` at 320x200 and `(1, 1)` at 640x480 (docs/CAMERA.md §7).
//   * the `SET_CAMERA_*_SHAKE` waveform is proven: a triangle with period
//     `4·period_ms`, peak `amplitude`, `angle += amplitude/period · s`
//     (docs/SHAKE.md §5.2); `triangle_shake()` reproduces it exactly.
//   * chase/orbit/fixed modes are MODEL: the shipped game builds only the
//     first-person frame and the cutscene (`yaw + π`, `zoom·0.25`); no
//     third-person distance/height is proven (docs/CAMERA_CPP.md §5.1, §7).
//   * `CameraModel`'s shake impulse is MODEL: a seeded xorshift32 sample with
//     a linear amplitude envelope.  The shipped game's shake lives in three
//     script-driven channels, and no shipped `.sbt` calls them
//     (docs/SHAKE.md §7.3), so an amplitude/decay pair cannot be taken from
//     data.
//
// Frame: the engine world frame of `.sbw` (X right, Y forward, Z up).  The
// caller passes the *eye* point as `target`; adding `player[0x2F50]`
// (`PlayerState::eye_height`) is the caller's job, exactly as in the game.
//
// Determinism: `double` only, a fixed order of operations, no clock, no
// `rand()`, no static mutable state.  With `-ffp-contract=off` (set by
// engine/CMakeLists.txt) two runs over the same inputs and the same seed are
// bit-identical.
//
// The namespace is `gm::camera_model`, not `gm`: `camera/camera.h` already
// declares `gm::CameraMode`, and this module must not touch that header.

#include <cstdint>
#include <string>

#include "render/render.h" // Vec3d

namespace gm::camera_model {

/// Proven vertical FOV equivalent of the 320x200 engine frame:
/// `2·atan(1/(5/6)) = 2·atan(6/5)`.  The engine itself has no FOV angle, only
/// the focal scales (docs/CAMERA.md §7), so this is a derived value.
inline constexpr double kFov320x200 = 1.7521161011963868;

/// Proven vertical FOV of the 640x480 engine frame (`fy = 1`, docs/CAMERA.md
/// §7): `2·atan(1) = π/2`.
inline constexpr double kFov640x480 = 1.5707963267948966;

enum class CameraMode {
    /// Proven first person (docs/CAMERA.md §4–§5): position = eye, angles as
    /// given.  `chase`, `orbit` and `fixed` are MODEL (see the file comment).
    cockpit,
    chase, ///< MODEL: behind the eye on `chase_distance`, up `chase_height`.
    orbit, ///< MODEL: on a sphere around the eye, looking at it.
    fixed, ///< MODEL: tripod at `CameraConfig::fixed_position`.
};

/// Tunables.  Every field carries its provenance; `MODEL` means "chosen by
/// this module, not taken from `gundx.exe`".
struct CameraConfig {
    /// Vertical FOV in radians.  MODEL: 90° is the convenient default of
    /// docs/CAMERA_CPP.md §4.2; the engine has only focal scales.  Used by
    /// `pose()` indirectly through the caller's projection; `focal_adjust()`
    /// uses `kFov320x200`/`kFov640x480` formulas instead.
    double fov = 1.5707963267948966;
    /// MODEL: `+0xe0 = 0.9` exists in the camera struct, its role as the near
    /// plane is not proven (docs/CAMERA.md §7, docs/CAMERA_CPP.md §7).
    double near_plane = 0.9;
    /// MODEL (docs/CAMERA_CPP.md §7).
    double far_plane = 1.0e5;
    /// MODEL: docs/CAMERA_CPP.md §0/§5.1 third-person hypothesis (60/0).
    double chase_distance = 60.0;
    /// MODEL: docs/CAMERA_CPP.md §0/§5.1 third-person hypothesis (60/0).
    double chase_height = 0.0;
    /// MODEL: default peak of `add_shake()` (radians).  No shipped script
    /// calls `SET_CAMERA_*_SHAKE` (docs/SHAKE.md §7.3), so there is no data
    /// value; 0.05 rad = 2.9° is a visible but small kick.
    double shake_amplitude = 0.05;
    /// MODEL: linear envelope decay of `shake()`, radians per second.  Must
    /// be > 0 so that `tick()` always reaches exactly 0 (the proven engine
    /// timer is linear in milliseconds too, docs/SHAKE.md §5.4).
    double shake_decay = 1.0;
    /// Proven waveform parameter (docs/SHAKE.md §5.1): quarter period in ms,
    /// the full triangle lasts `4·shake_period_ms`.  200 ms is the
    /// docs/CAMERA_CPP.md §3.1 default of the reference rig.
    double shake_period_ms = 200.0;
    /// MODEL default duration of `triangle_shake()` (docs/CAMERA_CPP.md §3.1
    /// uses 800 ms); the formula itself is proven.
    double shake_duration_ms = 800.0;
    /// MODEL: `gm/scene.first_person` look-at distance (docs/CAMERA_CPP.md §0);
    /// not used by `pose()`, kept for callers that build a view target.
    double focus_distance = 100.0;
    /// MODEL: tripod position of `CameraMode::fixed`.
    Vec3d fixed_position{};
    /// Seed of the shake xorshift32 generator.  0 is remapped to a non-zero
    /// constant so the sequence cannot stall (MODEL).
    std::uint32_t seed = 0x9E3779B9u;
};

struct CameraPose {
    Vec3d position{};
    double yaw = 0.0;   ///< radians; 0 looks along +Y (proven)
    double pitch = 0.0; ///< radians; > 0 looks down (proven)
    double roll = 0.0;  ///< radians; + roll tilts towards +X (proven basis)
};

/// Engine focal scales (docs/CAMERA.md §7): `x = zoom`, `y = zoom·4H/(3W)`.
/// Non-positive or non-finite arguments yield `(0, 0)`.
struct FocalScales {
    double x = 1.0;
    double y = 1.0;
};
FocalScales engine_focal(double width, double height, double zoom = 1.0) noexcept;

/// `tan(fov/2) = 1/focal` under the engine normalisation of docs/CAMERA.md §7
/// (the step from focal to pixels is not proven, §11.6).  A non-positive or
/// non-finite focal gives 0; a non-positive, non-finite or `>= π` fov gives 0.
double focal_to_fov(double focal) noexcept;
double fov_to_focal(double fov) noexcept;

/// Proven camera axes at `roll = 0` (docs/CAMERA.md §0/§3):
/// `forward = (sin y·cos p, cos y·cos p, −sin p)`, `right = (cos y, −sin y, 0)`,
/// `up = (sin y·sin p, cos y·sin p, cos p)`.
Vec3d forward(double yaw, double pitch) noexcept;
Vec3d right(double yaw) noexcept;
Vec3d up(double yaw, double pitch) noexcept;

/// Proven one `SET_CAMERA_*_SHAKE` sample (docs/SHAKE.md §5.2):
/// `p = max(1, period_ms)`, `r = duration_ms mod 4p`,
/// `s = (r < 2p) ? r − p : 3p − r`, result `amplitude/p · s`.
/// `duration_ms <= 0` gives 0; the peak is reached at `r = 0`/`r = 2p`, the
/// zeroes at `r = p`/`r = 3p`.  Non-finite input also gives 0.
double triangle_shake(double duration_ms, double period_ms, double amplitude) noexcept;

/// Camera model: mode placement, focal adjust and a deterministic shake.
///
/// Errors never throw: every fallible call records a text in `error()` and
/// leaves the previous state untouched (the same contract as
/// `CameraRig::update`, docs/CAMERA_CPP.md §2).  `error()` is cleared by a
/// successful `configure()`, `add_shake()` or `tick()` on a valid input.
class CameraModel {
public:
    CameraModel() = default;
    explicit CameraModel(const CameraConfig& config);

    /// Validate and install *config*; resets the shake generator.  Returns
    /// false with `error()` set (and the old config kept) when `fov` is
    /// outside `(0, 180°)`, `near_plane <= 0`, `far_plane <= near_plane`,
    /// `chase_distance < 0`, `shake_decay <= 0`, a non-finite number, etc.
    bool configure(const CameraConfig& config);

    const CameraConfig& config() const { return config_; }

    /// Pose for one mode.  `target` is the *eye* point (the caller adds
    /// `eye_height`), angles are radians, `pitch > 0` looks down.
    ///
    ///   * cockpit — `position = target`, angles as given (proven);
    ///   * chase   — `target − (sin yaw, cos yaw, 0)·chase_distance +
    ///     (0, 0, chase_height)`, angles as given (MODEL);
    ///   * orbit   — `target − forward(yaw, pitch)·chase_distance`, angles
    ///     recomputed to look at `target` (MODEL);
    ///   * fixed   — `config().fixed_position`, angles as given, `target`
    ///     ignored (MODEL).
    ///
    /// The current shake offset is added to the three angles of every mode.
    /// Non-finite arguments set `error()` and return `{target, yaw, pitch, 0}`
    /// without shake.
    CameraPose pose(const Vec3d& target, double yaw, double pitch,
                    CameraMode mode = CameraMode::cockpit) const;
    CameraPose pose(const Vec3d& target, double yaw, double pitch, double roll,
                    CameraMode mode) const;

    /// Apply the proven engine focal scales to a camera-space point whose
    /// components are `(e1·p, e3·p, e2·p)` (screen X, screen Y, depth;
    /// docs/CAMERA.md §7): returns `(fx·x, fy·y, z)`.  At 320x200 the unit
    /// vector `(1, 1, 1)` becomes `(1, 5/6, 1)`; at 640x480 it stays
    /// `(1, 1, 1)`.  Non-positive/non-finite width, height or zoom sets
    /// `error()` and returns the input unchanged.
    Vec3d focal_adjust(const Vec3d& camera_space, double width, double height,
                       double zoom = 1.0) const;

    /// Start a shake impulse with the given peak magnitude (radians); the
    /// sign is ignored.  The generator is reset to `config().seed`, so the
    /// impulse is reproducible.  A zero amplitude clears the shake.  NaN/Inf
    /// sets `error()` and changes nothing.
    void add_shake(double amplitude);
    /// `add_shake(config().shake_amplitude)`.
    void add_shake() { add_shake(config_.shake_amplitude); }

    /// Advance the shake envelope by `dt` seconds and draw the next sample.
    /// `shake() -= shake_decay·dt`, clamped at exactly 0; a zero envelope
    /// makes the call a no-op (state and error unchanged).  `dt <= 0`,
    /// NaN or Inf sets `error()` and changes nothing.
    bool tick(double dt);

    /// Magnitude of the current angular shake offset (radians).  Equals the
    /// envelope because the sample is normalised to unit length; 0 after the
    /// impulse decayed.
    double shake() const { return shake_env_; }
    /// Current envelope (peak), radians; `shake()` is its magnitude.
    double shake_envelope() const { return shake_env_; }
    double shake_pitch() const { return shake_pitch_; }
    double shake_roll() const { return shake_roll_; }
    double shake_yaw() const { return shake_yaw_; }
    /// Ticks elapsed since `add_shake()`, in seconds (MODEL bookkeeping).
    double shake_elapsed() const { return shake_elapsed_; }

    void clear_shake();
    void clear_error() { error_.clear(); }
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

private:
    bool valid_config(const CameraConfig& config) const;
    double next_unit(); ///< xorshift32 sample in [-1, 1]
    void sample_shake();

    CameraConfig config_{};
    mutable std::string error_;
    std::uint32_t rng_ = 0x9E3779B9u;
    double shake_env_ = 0.0;
    double shake_pitch_ = 0.0;
    double shake_roll_ = 0.0;
    double shake_yaw_ = 0.0;
    double shake_elapsed_ = 0.0;
};

} // namespace gm::camera_model
