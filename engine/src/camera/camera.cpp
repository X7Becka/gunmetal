#include "camera/camera.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace gm {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool finite_vec(const Vec3d& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double clamp_value(double value, double lo, double hi) {
    if (!(value > lo)) { // also catches NaN
        return lo;
    }
    return value > hi ? hi : value;
}

bool fail(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

Mat4 identity() { return Mat4{}; }

bool valid_projection_args(double fov, double aspect, double near_plane, double far_plane) {
    return std::isfinite(fov) && fov > 0.0 && fov < kPi && std::isfinite(aspect) &&
           aspect > 0.0 && std::isfinite(near_plane) && near_plane > 0.0 &&
           std::isfinite(far_plane) && far_plane > near_plane;
}

Mat4 perspective_matrix(double fx, double fy, double near_plane, double far_plane) {
    Mat4 out;
    out.at(0, 0) = fx;
    out.at(1, 1) = fy;
    out.at(2, 2) = (far_plane + near_plane) / (far_plane - near_plane);
    out.at(2, 3) = -2.0 * far_plane * near_plane / (far_plane - near_plane);
    out.at(3, 2) = 1.0; // w' = z, matching the z-forward view frame
    out.at(3, 3) = 0.0;
    return out;
}

bool valid_config(const CameraConfig& config, std::string* error) {
    if (!std::isfinite(config.body_pitch_limit) || config.body_pitch_limit <= 0.0) {
        return fail(error, "config.body_pitch_limit: must be finite and > 0");
    }
    if (!std::isfinite(config.body_roll_limit) || config.body_roll_limit <= 0.0) {
        return fail(error, "config.body_roll_limit: must be finite and > 0");
    }
    if (!std::isfinite(config.view_pitch_limit) || config.view_pitch_limit <= 0.0) {
        return fail(error, "config.view_pitch_limit: must be finite and > 0");
    }
    if (!std::isfinite(config.focus_distance) || config.focus_distance <= 0.0) {
        return fail(error, "config.focus_distance: must be finite and > 0");
    }
    if (!std::isfinite(config.smooth_time) || config.smooth_time < 0.0) {
        return fail(error, "config.smooth_time: must be finite and >= 0");
    }
    if (!std::isfinite(config.third_distance) || config.third_distance < 0.0) {
        return fail(error, "config.third_distance: must be finite and >= 0");
    }
    if (!std::isfinite(config.third_height)) {
        return fail(error, "config.third_height: must be finite");
    }
    if (!std::isfinite(config.third_radius) || config.third_radius < 0.0) {
        return fail(error, "config.third_radius: must be finite and >= 0");
    }
    if (!std::isfinite(config.fov) || config.fov <= 0.0 || config.fov >= kPi) {
        return fail(error, "config.fov: must be in (0, pi)");
    }
    if (!std::isfinite(config.aspect) || config.aspect <= 0.0) {
        return fail(error, "config.aspect: must be finite and > 0");
    }
    if (!std::isfinite(config.near_plane) || config.near_plane <= 0.0) {
        return fail(error, "config.near_plane: must be finite and > 0");
    }
    if (!std::isfinite(config.far_plane) || config.far_plane <= config.near_plane) {
        return fail(error, "config.far_plane: must be finite and > near_plane");
    }
    if (!std::isfinite(config.max_dt) || config.max_dt <= 0.0) {
        return fail(error, "config.max_dt: must be finite and > 0");
    }
    if (!std::isfinite(config.yaw_rate_max) || config.yaw_rate_max < 0.0) {
        return fail(error, "config.yaw_rate_max: must be finite and >= 0");
    }
    if (config.default_shake_period_ms < 1) {
        return fail(error, "config.default_shake_period_ms: must be >= 1");
    }
    if (config.default_shake_duration_ms < 0) {
        return fail(error, "config.default_shake_duration_ms: must be >= 0");
    }
    return true;
}

} // namespace

// --- Mat4 ------------------------------------------------------------------

std::array<double, 4> Mat4::transform(const Vec3d& p) const {
    return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
            at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
            at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3),
            at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3)};
}

Vec3d Mat4::project(const Vec3d& p) const {
    const std::array<double, 4> h = transform(p);
    if (h[3] == 0.0) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }
    return {h[0] / h[3], h[1] / h[3], h[2] / h[3]};
}

// --- basis -----------------------------------------------------------------

CameraBasis camera_basis(double roll, double pitch, double yaw) {
    const double sA = std::sin(roll);
    const double cA = std::cos(roll);
    const double sB = std::sin(pitch);
    const double cB = std::cos(pitch);
    const double sC = std::sin(yaw);
    const double cC = std::cos(yaw);

    CameraBasis basis;
    basis.right = {cC * cA - sC * sB * sA, -sC * cA - cC * sB * sA, -cB * sA};
    basis.forward = {sC * cB, cC * cB, -sB};
    basis.up = {cC * sA + sC * sB * cA, -sC * sA + cC * sB * cA, cB * cA};
    return basis;
}

bool look_at(const Vec3d& eye, const Vec3d& target, const Vec3d& up_hint,
             CameraBasis& out, std::string* error) {
    const Vec3d delta = target - eye;
    const double len = length(delta);
    if (!finite_vec(delta) || !(len > 1e-12)) {
        return fail(error, "look_at: eye and target must differ and be finite");
    }
    const Vec3d forward = delta * (1.0 / len);

    Vec3d up = finite_vec(up_hint) ? up_hint : Vec3d{0.0, 0.0, 1.0};
    if (std::fabs(dot(forward, up)) > 0.99) {
        up = {0.0, 1.0, 0.0};
    }
    if (std::fabs(dot(forward, up)) > 0.99) {
        up = {1.0, 0.0, 0.0};
    }
    Vec3d right = cross(forward, up);
    double right_len = length(right);
    if (!(right_len > 1e-12)) { // unreachable with the fallbacks above; kept safe
        up = std::fabs(forward.x) < 0.9 ? Vec3d{1.0, 0.0, 0.0} : Vec3d{0.0, 1.0, 0.0};
        right = cross(forward, up);
        right_len = length(right);
        if (!(right_len > 1e-12)) {
            return fail(error, "look_at: degenerate up vector");
        }
    }
    out.forward = forward;
    out.right = right * (1.0 / right_len);
    out.up = cross(out.right, forward);
    return true;
}

double decay_multiplier(double scale) {
    if (!std::isfinite(scale)) {
        return 1.0;
    }
    if (scale < 0.5) {
        return 1.0;
    }
    return 0.25 + 1.5 * scale;
}

Vec3d recoil_velocity(double yaw, double view_pitch, double amount) {
    const double k = std::fabs(std::cos(view_pitch));
    return {-std::sin(yaw) * k * amount, -std::cos(yaw) * k * amount, 0.0};
}

// --- projection helpers ----------------------------------------------------

FocalScales engine_focal(double width, double height, double zoom) {
    if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) ||
        !std::isfinite(height) || !std::isfinite(zoom)) {
        return {0.0, 0.0};
    }
    return {zoom, zoom * (4.0 * height) / (3.0 * width)};
}

double focal_to_fov(double focal) {
    if (!std::isfinite(focal) || !(focal > 0.0)) {
        return 0.0;
    }
    return 2.0 * std::atan(1.0 / focal);
}

double fov_to_focal(double fov) {
    if (!std::isfinite(fov) || !(fov > 0.0) || !(fov < kPi)) {
        return 0.0;
    }
    return 1.0 / std::tan(fov / 2.0);
}

// --- ShakeChannel ----------------------------------------------------------

void ShakeChannel::set(double amplitude_rad, int period, int duration) {
    amplitude = std::isfinite(amplitude_rad) ? amplitude_rad : 0.0;
    period_ms = period <= 0 ? 1 : period;
    duration_ms = duration < 0 ? 0 : duration;
    amplitude_per_period = amplitude / static_cast<double>(period_ms);
}

void ShakeChannel::clear() {
    amplitude = 0.0;
    amplitude_per_period = 0.0;
    period_ms = 1;
    duration_ms = 0;
}

void ShakeChannel::tick_ms(int dt_ms) {
    if (dt_ms <= 0) {
        return;
    }
    duration_ms = dt_ms >= duration_ms ? 0 : duration_ms - dt_ms;
}

double ShakeChannel::value() const {
    if (duration_ms <= 0) {
        return 0.0;
    }
    const long long p = std::max(1, period_ms);
    const long long four_p = 4 * p;
    const long long r = static_cast<long long>(duration_ms) % four_p;
    const long long s = r < 2 * p ? r - p : 3 * p - r;
    return amplitude_per_period * static_cast<double>(s);
}

// --- CameraRig -------------------------------------------------------------

CameraRig::CameraRig(const CameraConfig& config) : config_(config) { reset(); }

void CameraRig::set_config(const CameraConfig& config) { config_ = config; }

void CameraRig::reset() {
    yaw_offset_ = 0.0;
    body_pitch_ = 0.0;
    body_roll_ = 0.0;
    view_pitch_ = 0.0;
    pitch_pending_ = 0.0;
    yaw_pending_ = 0.0;
    yaw_rate_ = 0.0;
    decay_scale_ = 0.5;
    decay_divisor_ = 1.0;
    timer_a_ = 0;
    timer_b_ = 0;
    pitch_locked_ = false;
    for (ShakeChannel& channel : shake_) {
        channel.clear();
    }
    smoothing_ready_ = false;
    smoothed_eye_ = {};
    state_ = compute_state(Vec3d{}, PlayerState{});
    state_.mode = mode_;
}

void CameraRig::set_mode(CameraMode mode) {
    mode_ = mode;
    state_.mode = mode;
}

void CameraRig::set_third_person(double distance, double height, double radius) {
    config_.third_distance = distance;
    config_.third_height = height;
    config_.third_radius = radius;
}

void CameraRig::set_collision_world(const CollisionWorld* world) { world_ = world; }

bool CameraRig::set_body_angles(double pitch, double roll) {
    if (!std::isfinite(pitch) || !std::isfinite(roll)) {
        return false;
    }
    body_pitch_ = clamp_value(pitch, -config_.body_pitch_limit, config_.body_pitch_limit);
    body_roll_ = clamp_value(roll, -config_.body_roll_limit, config_.body_roll_limit);
    state_.body_pitch = body_pitch_;
    state_.body_roll = body_roll_;
    return true;
}

bool CameraRig::add_shake(const ShakeImpulse& impulse) {
    if (!std::isfinite(impulse.pitch) || !std::isfinite(impulse.roll) ||
        !std::isfinite(impulse.yaw) || impulse.period_ms < 1 || impulse.duration_ms < 0) {
        return false;
    }
    if (impulse.pitch != 0.0) {
        shake_[0].set(impulse.pitch, impulse.period_ms, impulse.duration_ms);
    }
    if (impulse.roll != 0.0) {
        shake_[1].set(impulse.roll, impulse.period_ms, impulse.duration_ms);
    }
    if (impulse.yaw != 0.0) {
        shake_[2].set(impulse.yaw, impulse.period_ms, impulse.duration_ms);
    }
    return true;
}

bool CameraRig::add_shake(double pitch_amplitude) {
    ShakeImpulse impulse;
    impulse.pitch = pitch_amplitude;
    impulse.period_ms = config_.default_shake_period_ms;
    impulse.duration_ms = config_.default_shake_duration_ms;
    return add_shake(impulse);
}

void CameraRig::clear_shake() {
    for (ShakeChannel& channel : shake_) {
        channel.clear();
    }
}

bool CameraRig::add_look_impulse(double pitch, double yaw) {
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
        return false;
    }
    pitch_pending_ += pitch;
    yaw_pending_ += yaw;
    return true;
}

void CameraRig::set_view_pitch_decay(int timer_a, int timer_b, double scale, double divisor) {
    if (!std::isfinite(scale) || !std::isfinite(divisor) || divisor == 0.0) {
        return;
    }
    timer_a_ = std::max(0, timer_a);
    timer_b_ = std::max(0, timer_b);
    decay_scale_ = scale;
    decay_divisor_ = divisor;
}

Vec3d CameraRig::recoil_velocity(double amount) const {
    return gm::recoil_velocity(state_.aim_yaw, state_.view_pitch, amount);
}

const ShakeChannel& CameraRig::shake_channel(int index) const {
    return shake_[static_cast<std::size_t>(std::clamp(index, 0, 2))];
}

CameraState CameraRig::compute_state(const Vec3d& player_eye, const PlayerState& player) const {
    CameraState state;
    state.mode = mode_;
    state.eye_height = player.eye_height;
    state.player_eye = player_eye;
    state.body_pitch = body_pitch_;
    state.body_roll = body_roll_;
    state.view_pitch = view_pitch_;
    state.aim_yaw = player.yaw + yaw_offset_;
    state.shake_pitch = shake_[0].value();
    state.shake_roll = shake_[1].value();
    state.shake_yaw = shake_[2].value();
    state.pitch = body_pitch_ + view_pitch_ + state.shake_pitch;
    state.roll = body_roll_ + state.shake_roll;
    state.yaw = player.yaw + yaw_offset_ + state.shake_yaw;

    const CameraBasis basis = camera_basis(state.roll, state.pitch, state.yaw);
    state.right = basis.right;
    state.forward = basis.forward;
    state.up = basis.up;

    state.eye = player_eye;
    state.distance = 0.0;
    state.occluded = false;
    if (mode_ == CameraMode::Third) {
        double distance = config_.third_distance > 0.0 ? config_.third_distance : 0.0;
        const Vec3d back = -basis.forward;
        if (world_ != nullptr && config_.third_radius > 0.0 && distance > 0.0) {
            const auto hit = world_->sphere_cast(player_eye, back, config_.third_radius, distance);
            if (hit) {
                distance = std::max(0.0, hit->t);
                state.occluded = true;
            }
        }
        state.distance = distance;
        state.eye = player_eye + back * distance + basis.up * config_.third_height;
    }

    state.target = state.eye + basis.forward * config_.focus_distance;
    state.fov = config_.fov;
    state.aspect = config_.aspect;
    state.near_plane = config_.near_plane;
    state.far_plane = config_.far_plane;
    return state;
}

CameraUpdate CameraRig::update(const PlayerState& player, const LookInput& look, double dt) {
    std::string error;
    if (!valid_config(config_, &error)) {
        return {false, error};
    }
    if (!finite_vec(player.pos)) {
        return {false, "player.pos: NaN/Inf not allowed"};
    }
    if (!std::isfinite(player.yaw)) {
        return {false, "player.yaw: NaN/Inf not allowed"};
    }
    if (!std::isfinite(player.eye_height) || player.eye_height < 0.0) {
        return {false, "player.eye_height: must be finite and >= 0"};
    }
    if (!std::isfinite(look.yaw) || !std::isfinite(look.pitch) || !std::isfinite(look.roll)) {
        return {false, "look: NaN/Inf not allowed"};
    }
    if (!std::isfinite(dt) || dt < 0.0) {
        return {false, "dt: must be finite and >= 0"};
    }
    if (dt > config_.max_dt) {
        return {false, "dt: larger than CameraConfig::max_dt"};
    }

    const double dt_ms = dt * 1000.0;

    // Yaw: the queue accumulates into `player[0x2F40]`, then applies it in the
    // same frame (0x4439af..0x443a03).  The rate limit is dormant in the shipped
    // build because `player[0x2D98]` is never written (docs/SHAKE.md §4).
    yaw_pending_ += look.yaw;
    if (yaw_pending_ != 0.0) {
        double step = yaw_pending_;
        const double limit = config_.yaw_rate_max * dt_ms;
        if (limit > 0.0) {
            step = clamp_value(step, -limit, limit);
        }
        yaw_offset_ += step;
        yaw_pending_ -= step;
        yaw_rate_ = 0.0;
    } else {
        yaw_offset_ += yaw_rate_ * dt_ms;
    }

    // View pitch: pending impulse first, else the (dormant) timer decay, then
    // the lock flag and the ±π/2 clamp (0x443a03..0x443ac8).
    pitch_pending_ += look.pitch;
    if (pitch_pending_ != 0.0) {
        view_pitch_ += pitch_pending_;
        pitch_pending_ = 0.0;
    } else if (timer_a_ != timer_b_) {
        view_pitch_ -= static_cast<double>(timer_a_ - timer_b_) * 0.016 *
                       decay_multiplier(decay_scale_) / decay_divisor_;
    }
    if (pitch_locked_) {
        view_pitch_ = 0.0;
    }
    view_pitch_ = clamp_value(view_pitch_, -config_.view_pitch_limit, config_.view_pitch_limit);

    // Body roll (player[0x20]) is clamped to ±20°; body pitch only changes via
    // set_body_angles, which applies the ±π/6 clamp.
    body_roll_ += look.roll;
    body_roll_ = clamp_value(body_roll_, -config_.body_roll_limit, config_.body_roll_limit);

    Vec3d player_eye{player.pos.x, player.pos.y, player.pos.z + player.eye_height};
    if (config_.smooth_time > 0.0) {
        if (!smoothing_ready_) {
            smoothed_eye_ = player_eye;
            smoothing_ready_ = true;
        } else {
            const double alpha = 1.0 - std::exp(-dt / config_.smooth_time);
            smoothed_eye_ = smoothed_eye_ + (player_eye - smoothed_eye_) * alpha;
        }
        player_eye = smoothed_eye_;
    } else {
        smoothed_eye_ = player_eye;
        smoothing_ready_ = false;
    }

    state_ = compute_state(player_eye, player);
    state_.mode = mode_;

    const int tick_ms = static_cast<int>(std::min(dt_ms, 2147483647.0));
    for (ShakeChannel& channel : shake_) {
        channel.tick_ms(tick_ms);
    }
    return {true, {}};
}

Mat4 CameraRig::view() const {
    Mat4 out;
    out.at(0, 0) = state_.right.x;
    out.at(0, 1) = state_.right.y;
    out.at(0, 2) = state_.right.z;
    out.at(0, 3) = -dot(state_.right, state_.eye);
    out.at(1, 0) = state_.up.x;
    out.at(1, 1) = state_.up.y;
    out.at(1, 2) = state_.up.z;
    out.at(1, 3) = -dot(state_.up, state_.eye);
    out.at(2, 0) = state_.forward.x;
    out.at(2, 1) = state_.forward.y;
    out.at(2, 2) = state_.forward.z;
    out.at(2, 3) = -dot(state_.forward, state_.eye);
    return out;
}

Mat4 CameraRig::projection(double fov, double aspect, double near_plane,
                           double far_plane) const {
    if (!valid_projection_args(fov, aspect, near_plane, far_plane)) {
        return identity();
    }
    const double fy = 1.0 / std::tan(fov / 2.0);
    const double fx = fy / aspect;
    return perspective_matrix(fx, fy, near_plane, far_plane);
}

Mat4 CameraRig::projection() const {
    return projection(config_.fov, config_.aspect, config_.near_plane, config_.far_plane);
}

Mat4 CameraRig::engine_projection(double width, double height, double zoom, double near_plane,
                                  double far_plane) const {
    if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(zoom) || !(zoom > 0.0) || !valid_projection_args(kPi / 2.0, 1.0,
                                                                         near_plane, far_plane)) {
        return identity();
    }
    const FocalScales focal = engine_focal(width, height, zoom);
    return perspective_matrix(focal.x, focal.y, near_plane, far_plane);
}

} // namespace gm
