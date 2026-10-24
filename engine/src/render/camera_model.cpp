#include "render/camera_model.h"

#include <cmath>
#include <cstdint>

namespace gm::camera_model {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kDefaultSeed = 0x9E3779B9u;

bool finite(double v) { return std::isfinite(v); }

bool finite_vec(const Vec3d& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::string with_value(const std::string& text, double value) {
    return text + " (got " + std::to_string(value) + ")";
}

} // namespace

FocalScales engine_focal(double width, double height, double zoom) noexcept {
    if (!finite(width) || !finite(height) || !finite(zoom) || width <= 0.0 || height <= 0.0 ||
        zoom <= 0.0) {
        return FocalScales{0.0, 0.0};
    }
    return FocalScales{zoom, zoom * 4.0 * height / (3.0 * width)};
}

double focal_to_fov(double focal) noexcept {
    if (!finite(focal) || focal <= 0.0) {
        return 0.0;
    }
    return 2.0 * std::atan(1.0 / focal);
}

double fov_to_focal(double fov) noexcept {
    if (!finite(fov) || fov <= 0.0 || fov >= kPi) {
        return 0.0;
    }
    return 1.0 / std::tan(fov * 0.5);
}

Vec3d forward(double yaw, double pitch) noexcept {
    if (!finite(yaw) || !finite(pitch)) {
        return Vec3d{0.0, 1.0, 0.0};
    }
    const double sy = std::sin(yaw);
    const double cy = std::cos(yaw);
    const double sp = std::sin(pitch);
    const double cp = std::cos(pitch);
    return Vec3d{sy * cp, cy * cp, -sp};
}

Vec3d right(double yaw) noexcept {
    if (!finite(yaw)) {
        return Vec3d{1.0, 0.0, 0.0};
    }
    return Vec3d{std::cos(yaw), -std::sin(yaw), 0.0};
}

Vec3d up(double yaw, double pitch) noexcept {
    if (!finite(yaw) || !finite(pitch)) {
        return Vec3d{0.0, 0.0, 1.0};
    }
    const double sy = std::sin(yaw);
    const double cy = std::cos(yaw);
    const double sp = std::sin(pitch);
    const double cp = std::cos(pitch);
    return Vec3d{sy * sp, cy * sp, cp};
}

double triangle_shake(double duration_ms, double period_ms, double amplitude) noexcept {
    if (!finite(duration_ms) || !finite(period_ms) || !finite(amplitude) || duration_ms <= 0.0 ||
        period_ms <= 0.0) {
        return 0.0;
    }
    const double p = period_ms;
    double r = std::fmod(duration_ms, 4.0 * p);
    if (r < 0.0) {
        r += 4.0 * p;
    }
    const double s = (r < 2.0 * p) ? (r - p) : (3.0 * p - r);
    return (amplitude / p) * s;
}

CameraModel::CameraModel(const CameraConfig& config) {
    (void)configure(config);
}

bool CameraModel::valid_config(const CameraConfig& config) const {
    if (!finite(config.fov) || config.fov <= 0.0 || config.fov >= kPi) {
        error_ = with_value("fov must be finite and inside (0, 180) degrees, rad", config.fov);
        return false;
    }
    if (!finite(config.near_plane) || config.near_plane <= 0.0) {
        error_ = with_value("near_plane must be finite and > 0", config.near_plane);
        return false;
    }
    if (!finite(config.far_plane) || config.far_plane <= config.near_plane) {
        error_ = with_value("far_plane must be finite and > near_plane", config.far_plane);
        return false;
    }
    if (!finite(config.chase_distance) || config.chase_distance < 0.0) {
        error_ = with_value("chase_distance must be finite and >= 0", config.chase_distance);
        return false;
    }
    if (!finite(config.chase_height)) {
        error_ = with_value("chase_height must be finite", config.chase_height);
        return false;
    }
    if (!finite(config.shake_amplitude) || config.shake_amplitude < 0.0) {
        error_ = with_value("shake_amplitude must be finite and >= 0", config.shake_amplitude);
        return false;
    }
    if (!finite(config.shake_decay) || config.shake_decay <= 0.0) {
        error_ = with_value("shake_decay must be finite and > 0", config.shake_decay);
        return false;
    }
    if (!finite(config.shake_period_ms) || config.shake_period_ms <= 0.0) {
        error_ = with_value("shake_period_ms must be finite and > 0", config.shake_period_ms);
        return false;
    }
    if (!finite(config.shake_duration_ms) || config.shake_duration_ms < 0.0) {
        error_ = with_value("shake_duration_ms must be finite and >= 0", config.shake_duration_ms);
        return false;
    }
    if (!finite(config.focus_distance) || config.focus_distance <= 0.0) {
        error_ = with_value("focus_distance must be finite and > 0", config.focus_distance);
        return false;
    }
    if (!finite_vec(config.fixed_position)) {
        error_ = "fixed_position must be finite";
        return false;
    }
    return true;
}

bool CameraModel::configure(const CameraConfig& config) {
    error_.clear();
    if (!valid_config(config)) {
        return false;
    }
    config_ = config;
    clear_shake();
    rng_ = config_.seed == 0u ? kDefaultSeed : config_.seed;
    return true;
}

CameraPose CameraModel::pose(const Vec3d& target, double yaw, double pitch,
                             CameraMode mode) const {
    return pose(target, yaw, pitch, 0.0, mode);
}

CameraPose CameraModel::pose(const Vec3d& target, double yaw, double pitch, double roll,
                             CameraMode mode) const {
    if (!finite_vec(target) || !finite(yaw) || !finite(pitch) || !finite(roll)) {
        error_ = "pose: target, yaw, pitch and roll must be finite";
        return CameraPose{target, yaw, pitch, 0.0};
    }

    Vec3d position = target;
    double out_yaw = yaw;
    double out_pitch = pitch;
    double out_roll = roll;
    const CameraConfig& c = config_;

    switch (mode) {
    case CameraMode::cockpit:
        // Proven: eye = player position, angles as given (docs/CAMERA.md §4–§5).
        break;
    case CameraMode::chase: {
        // MODEL: yaw-only horizontal back-off, so pitch cannot swing the
        // camera down/up; matches docs/CAMERA_CPP.md §5.1 at pitch = 0.
        const Vec3d back = forward(yaw, 0.0);
        position.x = target.x - back.x * c.chase_distance;
        position.y = target.y - back.y * c.chase_distance;
        position.z = target.z + c.chase_height;
        break;
    }
    case CameraMode::orbit: {
        // MODEL: full-forward offset, then look back at the target.  The
        // recovered angles satisfy forward(yaw, pitch) = (target − position)/d.
        const Vec3d f = forward(yaw, pitch);
        position.x = target.x - f.x * c.chase_distance;
        position.y = target.y - f.y * c.chase_distance;
        position.z = target.z - f.z * c.chase_distance;
        const double dx = target.x - position.x;
        const double dy = target.y - position.y;
        const double dz = target.z - position.z;
        out_yaw = std::atan2(dx, dy);
        out_pitch = std::atan2(-dz, std::hypot(dx, dy));
        out_roll = 0.0;
        break;
    }
    case CameraMode::fixed:
        // MODEL: tripod; the target is ignored, the angles are the stored ones.
        position = c.fixed_position;
        break;
    }

    return CameraPose{position, out_yaw + shake_yaw_, out_pitch + shake_pitch_,
                      out_roll + shake_roll_};
}

Vec3d CameraModel::focal_adjust(const Vec3d& camera_space, double width, double height,
                                double zoom) const {
    if (!finite_vec(camera_space)) {
        error_ = "focal_adjust: camera_space must be finite";
        return camera_space;
    }
    if (!finite(width) || width <= 0.0) {
        error_ = with_value("focal_adjust: width must be finite and > 0", width);
        return camera_space;
    }
    if (!finite(height) || height <= 0.0) {
        error_ = with_value("focal_adjust: height must be finite and > 0", height);
        return camera_space;
    }
    if (!finite(zoom) || zoom <= 0.0) {
        error_ = with_value("focal_adjust: zoom must be finite and > 0", zoom);
        return camera_space;
    }
    error_.clear();
    const FocalScales focal = engine_focal(width, height, zoom);
    return Vec3d{camera_space.x * focal.x, camera_space.y * focal.y, camera_space.z};
}

double CameraModel::next_unit() {
    std::uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return static_cast<double>(x) * (2.0 / 4294967295.0) - 1.0;
}

void CameraModel::sample_shake() {
    const double pitch = next_unit();
    const double roll = next_unit();
    const double yaw = next_unit();
    // Fixed channel weights (MODEL: pitch 1.0, roll 0.5, yaw 0.5); the
    // normalisation makes the offset magnitude exactly the envelope.
    const double n2 = pitch * pitch + 0.25 * roll * roll + 0.25 * yaw * yaw;
    const double len = std::sqrt(n2);
    if (!(len > 1e-12)) {
        shake_pitch_ = shake_env_;
        shake_roll_ = 0.0;
        shake_yaw_ = 0.0;
        return;
    }
    shake_pitch_ = shake_env_ * pitch / len;
    shake_roll_ = shake_env_ * 0.5 * roll / len;
    shake_yaw_ = shake_env_ * 0.5 * yaw / len;
}

void CameraModel::clear_shake() {
    shake_env_ = 0.0;
    shake_pitch_ = 0.0;
    shake_roll_ = 0.0;
    shake_yaw_ = 0.0;
    shake_elapsed_ = 0.0;
}

void CameraModel::add_shake(double amplitude) {
    if (!finite(amplitude)) {
        error_ = with_value("add_shake: amplitude must be finite", amplitude);
        return;
    }
    error_.clear();
    const double peak = std::fabs(amplitude);
    if (peak == 0.0) {
        clear_shake();
        return;
    }
    shake_env_ = peak;
    shake_elapsed_ = 0.0;
    rng_ = config_.seed == 0u ? kDefaultSeed : config_.seed;
    sample_shake();
}

bool CameraModel::tick(double dt) {
    if (!finite(dt) || dt <= 0.0) {
        error_ = with_value("tick: dt must be finite and > 0", dt);
        return false;
    }
    error_.clear();
    if (shake_env_ <= 0.0) {
        return true; // no impulse: no-op
    }
    shake_env_ -= config_.shake_decay * dt;
    if (shake_env_ <= 0.0) {
        shake_env_ = 0.0;
        shake_pitch_ = 0.0;
        shake_roll_ = 0.0;
        shake_yaw_ = 0.0;
        shake_elapsed_ = 0.0; // the impulse is over
        return true;
    }
    shake_elapsed_ += dt;
    sample_shake();
    return true;
}

} // namespace gm::camera_model
