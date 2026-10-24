#include "game/chassis_physics.h"

#include <cmath>
#include <utility>

namespace gm {
namespace {

void fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

/// `value` must be finite and either strictly positive or (for the fields where
/// zero is meaningful: damping, acceleration, gravity) non-negative.
bool check_range(double value, const char* name, bool allow_zero, std::string* error) {
    if (!std::isfinite(value)) {
        fail(error, std::string("chassis_physics: ") + name + " is not finite");
        return false;
    }
    if (allow_zero ? (value < 0.0) : (value <= 0.0)) {
        fail(error, std::string("chassis_physics: ") + name +
                        (allow_zero ? " is negative" : " must be positive"));
        return false;
    }
    return true;
}

bool finite_state(const ChassisState& state) {
    return finite(state.position) && finite(state.velocity) &&
           std::isfinite(state.suspension_offset) &&
           std::isfinite(state.suspension_velocity);
}

} // namespace

std::optional<ChassisParams> chassis_params(const ChassisSpec& spec, double retention,
                                            std::string* error) {
    if (!std::isfinite(retention) || retention <= 0.0 || retention >= 1.0) {
        fail(error, "chassis '" + spec.name + "': retention k must be in (0, 1)");
        return std::nullopt;
    }
    if (!std::isfinite(spec.mass) || spec.mass <= 0.0) {
        fail(error, "chassis '" + spec.name + "': mass must be positive");
        return std::nullopt;
    }
    if (!std::isfinite(spec.max_speed) || spec.max_speed <= 0.0) {
        fail(error, "chassis '" + spec.name + "': op4 max speed must be positive");
        return std::nullopt;
    }

    ChassisParams params;
    params.mass = spec.mass;
    // The original's steady state per tick is dv/(1-k) with dv = op4
    // (`0x444476`..`0x4444d5`); convert units/tick to units/second with 91 Hz.
    params.max_speed = spec.max_speed * kChassisTickHz / (1.0 - retention);
    // Continuous equivalent of the proven per-tick filter v <- k*v + dv.
    params.drag = -kChassisTickHz * std::log(retention);
    // Semi-implicit steady state of v' = accel - drag*v is accel/drag, so this
    // makes the plateau exactly `max_speed`.
    params.accel = params.drag * params.max_speed;
    if (!std::isfinite(params.max_speed) || !std::isfinite(params.drag) ||
        !std::isfinite(params.accel)) {
        fail(error, "chassis '" + spec.name + "': derived parameters are not finite");
        return std::nullopt;
    }
    return params;
}

bool ChassisPhysics::configure(const ChassisParams& params, std::string* error) {
    if (!check_range(params.mass, "mass", false, error) ||
        !check_range(params.max_speed, "max_speed", false, error) ||
        !check_range(params.drag, "drag", false, error) ||
        !check_range(params.accel, "accel", true, error) ||
        !check_range(params.suspension_k, "suspension_k", false, error) ||
        !check_range(params.suspension_d, "suspension_d", true, error) ||
        !check_range(params.ride_height, "ride_height", false, error) ||
        !check_range(params.gravity, "gravity", true, error)) {
        return false;
    }
    params_ = params;
    configured_ = true;
    return true;
}

bool ChassisPhysics::step(ChassisState& state, double dt, Vec3d input,
                          std::string* error) const {
    if (!configured_) {
        fail(error, "chassis_physics: step() called before configure()");
        return false;
    }
    if (!std::isfinite(dt)) {
        fail(error, "chassis_physics: dt is not finite");
        return false;
    }
    if (dt <= 0.0) {
        fail(error, "chassis_physics: dt must be positive");
        return false;
    }
    if (dt > kChassisMaxDt) {
        fail(error, "chassis_physics: dt is larger than " +
                        std::to_string(kChassisMaxDt) + " s");
        return false;
    }
    // Explicit spring: the symplectic Euler step of x'' = -k*x is stable only
    // for omega*dt < 2 (the linear damping is integrated implicitly and has no
    // such bound).
    if (dt * std::sqrt(params_.suspension_k) >= 2.0) {
        fail(error, "chassis_physics: dt is too large for the explicit spring "
                    "(dt*sqrt(suspension_k) >= 2)");
        return false;
    }
    if (!finite_state(state)) {
        fail(error, "chassis_physics: state is not finite");
        return false;
    }
    if (!finite(input)) {
        fail(error, "chassis_physics: input is not finite");
        return false;
    }

    // Planar thrust, clamped to unit length; the vertical input is ignored.
    double thrust_x = input.x;
    double thrust_y = input.y;
    const double thrust_length = std::sqrt(thrust_x * thrust_x + thrust_y * thrust_y);
    if (thrust_length > 1.0) {
        thrust_x *= 1.0 / thrust_length;
        thrust_y *= 1.0 / thrust_length;
    }

    // Semi-implicit Euler: the damping is taken at the new velocity, so its
    // steady state is `accel/drag` and the scheme is stable for every dt.
    const double damping = 1.0 + params_.drag * dt;
    Vec3d velocity = state.velocity;
    velocity.x = (velocity.x + thrust_x * params_.accel * dt) / damping;
    velocity.y = (velocity.y + thrust_y * params_.accel * dt) / damping;
    velocity.z -= params_.gravity * dt;
    const Vec3d position = state.position + velocity * dt;

    // Suspension: damping implicit, spring explicit, position uses the new
    // offset velocity (symplectic Euler).
    const double spring = params_.suspension_k * state.suspension_offset;
    const double suspension_velocity =
        (state.suspension_velocity - spring * dt) / (1.0 + params_.suspension_d * dt);
    const double suspension_offset =
        state.suspension_offset + suspension_velocity * dt;

    state.position = position;
    state.velocity = velocity;
    state.suspension_offset = suspension_offset;
    state.suspension_velocity = suspension_velocity;
    return true;
}

double ChassisPhysics::speed(const ChassisState& state) const {
    return std::sqrt(state.velocity.x * state.velocity.x +
                     state.velocity.y * state.velocity.y);
}

double ChassisPhysics::suspension_force(double offset, double offset_velocity) const {
    return params_.mass * (params_.suspension_k * offset +
                           params_.suspension_d * offset_velocity);
}

double ChassisPhysics::body_height(const ChassisState& state) const {
    return params_.ride_height - state.suspension_offset;
}

} // namespace gm
