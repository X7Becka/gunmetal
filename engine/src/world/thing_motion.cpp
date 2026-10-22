#include "world/thing_motion.h"

#include <algorithm>
#include <cmath>

#include "world/things.h"

namespace gm {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;
/// 0x4a2748 / 0x4a2750: the lean angle the walkers write into `+0x38` while
/// the yaw rate exceeds it (pi/8).
constexpr double kLeanAngle = 0.08726646259971647;
/// 0x4a2758: yaw rate damping, applied after every integration step.
constexpr double kYawRateDamping = 0.75;
/// 0x4a2780: the `1.25 * radius` clearance factor of 0x4746b0 / 0x474a10 /
/// 0x473240.
constexpr double kClearanceFactor = 1.25;
/// 0x40100000: the sideways bias 0x472120 writes into `+0x20c`/`+0x214`.
constexpr double kSideBias = 2.5;
/// 0x3fa99999 9999999a: the impulse damping 0x472120 writes into
/// `+0x218`/`+0x220`.
constexpr double kImpulseDamping = 0.05;
/// 0x40340000: the `|error| >= 20` gate of the vertical seek in 0x473240.
constexpr double kVerticalSeekGate = 20.0;
/// 0x4752d0: `vz += 0.2 * gravity * level_gravity * dt`.
constexpr double kGravityScale = 0.2;

/// 0x470c70 keeps the roll target of these types and zeroes it for the rest
/// (`movement_type in {0,1,5,6,7,8}`).
bool keeps_roll(int type) {
    switch (type) {
        case 0:
        case 1:
        case 5:
        case 6:
        case 7:
        case 8:
            return true;
        default:
            return false;
    }
}

/// `ABS(fmod(v, 2*pi))` of the shared tick part: the yaw is kept in [0, 2*pi).
double wrap_yaw(double value) {
    return std::fabs(std::fmod(value, kTwoPi));
}

/// Signed clamp with the original's "no overshoot" shape: limit `delta` by
/// `error` when both move toward each other.
double clamp_delta(double delta, double error) {
    if (error >= 0.0) {
        return std::min(delta, error);
    }
    return std::max(delta, error);
}

/// The turret's pitch step flips the sign of `damping*dt` for a negative error
/// before clamping (`0x474ea0`, the `local_20 = -local_20` branch), unlike the
/// yaw step above.
double clamp_pitch_delta(double delta, double error) {
    if (error >= 0.0) {
        return std::min(delta, error);
    }
    delta = -delta;
    return std::max(delta, error);
}

/// Shared turn integration of types 2/3/4/13 (`0x474260`, `0x4746b0` and
/// `0x474a10`): accel, clamp, lean into the turn, accumulate, damp.  These
/// three handlers use the plain `turn_max`; only `0x473240` and `0x474ea0`
/// honour the `0.25 * turn_max` variant of `flags_b & 0x00800000`.
void integrate_yaw(ThingMotionState& state, const ThingMotionConfig& config,
                   double dt, int turn) {
    if (turn == 2) {
        state.yaw_rate -= dt * config.turn_accel;
    } else if (turn == 3) {
        state.yaw_rate += dt * config.turn_accel;
    }
    const double limit = config.turn_max;
    if (std::isfinite(limit) && limit > 0.0) {
        state.yaw_rate = std::clamp(state.yaw_rate, -limit, limit);
    }
    // flags_b +0x40000000: lean by +/- pi/8 while turning hard.
    if ((config.flags_b & 0x40000000u) != 0) {
        if (state.yaw_rate > kLeanAngle) {
            state.roll_target = kLeanAngle;
        } else if (state.yaw_rate < -kLeanAngle) {
            state.roll_target = -kLeanAngle;
        }
    }
    state.yaw_target += state.yaw_rate;
    state.yaw_rate *= kYawRateDamping;
    state.last_yaw = state.yaw_target;
}

/// Shared tick preamble (`0x470c70`, instance +0x30/+0x38/+0x40 block).
void prepare_targets(ThingMotionState& state, const ThingMotionConfig& config) {
    state.pitch_target = state.pitch;
    state.roll_target = keeps_roll(config.movement_type) ? state.roll : 0.0;
    state.yaw_target = state.yaw;
}

/// Shared tick postlude: pitch snaps, yaw snaps (wrapped into [0, 2*pi)),
/// roll is smoothed toward its target at `roll_rate`.
void finish_angles(ThingMotionState& state, double dt) {
    if (state.yaw != state.yaw_target) {
        const double wrapped = wrap_yaw(state.yaw_target);
        if (state.last_yaw == state.yaw_target) {
            state.last_yaw = wrapped;
        }
        state.yaw = wrapped;
    }
    state.pitch = state.pitch_target;
    if (state.roll_target <= state.roll) {
        if (state.roll_target < state.roll) {
            state.roll -= dt * state.roll_rate;
            if (state.roll < state.roll_target) {
                state.roll = state.roll_target;
            }
        }
    } else {
        state.roll += dt * state.roll_rate;
        if (state.roll_target < state.roll) {
            state.roll = state.roll_target;
        }
    }
}

std::array<Vec3d, 3> basis_of(const ThingMotionState& state) {
    return thing_basis(state.roll, state.pitch, state.yaw);
}

/// The shared tick part adds the two collision impulses to the tick velocity
/// (+0x208 along the object's `right` in x/y only, +0x210 to z).
void add_impulses(ThingMotionState& state, double dt) {
    if (state.impulse_side != 0.0) {
        const auto basis = basis_of(state);
        state.velocity.x += basis[0].x * state.impulse_side * dt;
        state.velocity.y += basis[0].y * state.impulse_side * dt;
    }
    if (state.impulse_up != 0.0) {
        state.velocity.z += dt * state.impulse_up;
    }
}

/// The dormant thruster of the tick (`+0x1e8`): pushes along the object's
/// `right` in x/y.  No shipped writer fills the accumulator, so in the retail
/// build this never moves anything (docs/THING_FIELDS.md §4a).
void add_thrust(ThingMotionState& state, double dt) {
    if (state.thrust_speed == 0.0) {
        return;
    }
    const auto basis = basis_of(state);
    state.velocity.x += basis[0].x * state.thrust_speed * dt;
    state.velocity.y += basis[0].y * state.thrust_speed * dt;
}

/// `0x472120` + `0x472690`: sweep the assembled velocity through the world,
/// zero the vertical speed on a floor/ceiling contact, and run the impulse
/// bounce.  0x472120 gates the bounce with `flags_b & 0x08000000` (side) and
/// `& 0x10000000` (up); both impulses are written only there.
void apply_velocity(ThingMotionState& state, const ThingMotionConfig& config,
                    const ThingMotionInput& input, ThingMotionStep& out) {
    const double travel = length(state.velocity);
    if (!(travel > 0.0)) {
        return;
    }
    Vec3d desired = state.position + state.velocity;
    bool blocked = false;
    if (input.world != nullptr && config.radius > 0.0 &&
        !input.world->empty()) {
        const auto hit = input.world->sphere_cast(state.position, state.velocity,
                                                  config.radius, travel);
        if (hit.has_value()) {
            desired = hit->centre;
            blocked = true;
        }
    }
    const Vec3d actual = desired - state.position;
    if (std::fabs(actual.z - state.velocity.z) > 1e-9) {
        // `if (target.z != resolved.z) { vertical_speed = 0; ... }`
        state.vertical_speed = 0.0;
        out.grounded = true;
    }
    state.position = desired;
    out.displacement = actual;
    out.blocked = blocked;
    out.moved = true;

    // `0x472120`: forward/side impulse bounce.
    if ((config.flags_b & 0x08000000u) == 0) {
        state.impulse_side = 0.0;
    } else if (state.impulse_side != 0.0) {
        // The original also rolls `rand() < dt*impulse_damping` here; the
        // deterministic port reflects on a real contact only.
        if (blocked) {
            state.impulse_side = -state.impulse_side;
            state.impulse_damping *= 0.5;
            out.bounced = true;
        }
    } else if (blocked) {
        state.impulse_damping = kImpulseDamping;
        state.impulse_side_bias = kSideBias;
        out.bounced = true;
    }
    // The vertical analogue (+0x210/+0x214/+0x220).
    if ((config.flags_b & 0x10000000u) == 0) {
        state.impulse_up = 0.0;
    } else if (state.impulse_up != 0.0) {
        if (blocked) {
            state.impulse_up = -state.impulse_up;
            state.impulse_damping_up *= 0.5;
            out.bounced = true;
        }
    } else if (blocked) {
        state.impulse_damping_up = kImpulseDamping;
        state.impulse_up_bias = kSideBias;
        out.bounced = true;
    }
    state.velocity = {};
}

/// `0x474260` (type 2) and `0x4746b0` (types 3/4): probe the surroundings and
/// walk forward while the clearance allows it.
void step_avoid(ThingMotionState& state, const ThingMotionConfig& config,
                const ThingMotionInput& input, ThingMotionStep& out) {
    const auto basis = basis_of(state);
    const Vec3d forward = basis[1];
    const Vec3d right = basis[0];
    const double radius = config.radius;
    const double dt = input.dt;
    const double step = state.speed * dt;

    const double ahead =
        thing_probe(input.world, state.position, forward, radius);
    int turn = 0;
    bool move = false;
    if (config.movement_type == 3 || config.movement_type == 4) {
        // 0x4746b0: no side probes, a fixed turn direction.  The handler is
        // called with `param_4 = 1` for type 3 and 0 for type 4, and computes
        // `turn = 3 - (param_4 != 0)`, i.e. 2 for type 3 and 3 for type 4.
        if (ahead < kClearanceFactor * radius) {
            turn = config.movement_type == 3 ? 2 : 3;
        }
        move = kClearanceFactor * radius < ahead;
    } else {
        const double left =
            thing_probe(input.world, state.position, -right, radius);
        const double right_free = std::min(
            thing_probe(input.world, state.position, right, radius),
            kThingProbeLength);
        const double two_r = 2.0 * radius;
        if (ahead < two_r || left < two_r || right_free < two_r ||
            ahead < left || ahead < right_free) {
            turn = left < right_free ? 3 : 2;
        }
        move = step + radius < ahead;
    }
    out.turn = turn;
    integrate_yaw(state, config, dt, turn);
    if (move) {
        if (config.movement_type == 2) {
            // 0x474260 moves along all three forward components.
            state.velocity.x += step * forward.x;
            state.velocity.y += step * forward.y;
            state.velocity.z += step * forward.z;
        } else {
            // 0x4746b0 only touches x/y.
            state.velocity.x += step * forward.x;
            state.velocity.y += step * forward.y;
        }
    }
    out.moved = move;
}

/// `0x474a10` (type 13): 1.25r clearance, turn toward the target when free and
/// toward the open side when blocked.
void step_ram(ThingMotionState& state, const ThingMotionConfig& config,
              const ThingMotionInput& input, ThingMotionStep& out) {
    const auto basis = basis_of(state);
    const Vec3d forward = basis[1];
    const Vec3d right = basis[0];
    const double dt = input.dt;
    const double clearance = kClearanceFactor * config.radius;
    const double ahead =
        thing_probe(input.world, state.position, forward, config.radius);

    const Vec3d to_target = input.target.position - state.position;
    const double lateral = dot(to_target, right);

    int turn = 0;
    bool move = true;
    if (clearance <= ahead) {
        if (input.target.valid) {
            turn = lateral > 0.0 ? 3 : 2;
            move = false; // 0x474a10 zeroes the distance after choosing a side
        }
    } else if (std::fabs(state.yaw_rate) <= 0.001) {
        turn = 2;
        if (input.target.valid && lateral < 0.0) {
            turn = 3;
        }
    } else {
        turn = state.yaw_rate >= 0.0 ? 3 : 2;
    }
    out.turn = turn;
    integrate_yaw(state, config, dt, turn);
    if (move && clearance < ahead) {
        const double step = state.speed * dt;
        state.velocity.x += step * forward.x;
        state.velocity.y += step * forward.y;
        out.moved = true;
    }
}

/// `0x473240` (types 14/15/17): seek the target yaw with a bounded rate, or
/// fall back to the three-probe avoid when there is none.
void step_seek_avoid(ThingMotionState& state, const ThingMotionConfig& config,
                     const ThingMotionInput& input, ThingMotionStep& out) {
    const auto basis = basis_of(state);
    const Vec3d forward = basis[1];
    const Vec3d right = basis[0];
    const double dt = input.dt;
    const double radius = config.radius;
    const double ahead =
        thing_probe(input.world, state.position, forward, radius);

    if (input.target.valid) {
        const double error = input.target.yaw_error;
        if (error > 0.0) {
            state.yaw_rate += dt * config.turn_accel;
        } else if (error < 0.0) {
            state.yaw_rate -= dt * config.turn_accel;
        }
        const bool quarter = (config.flags_b & 0x00800000u) != 0;
        const double limit = quarter ? 0.25 * config.turn_max : config.turn_max;
        if (std::isfinite(limit) && limit > 0.0) {
            state.yaw_rate = std::clamp(state.yaw_rate, -limit, limit);
        }
        const double delta = clamp_delta(dt * state.yaw_rate, error);
        state.yaw_target += delta;
        state.yaw_rate *= kYawRateDamping;
        state.last_yaw = state.yaw_target;
        out.turn = delta > 0.0 ? 3 : (delta < 0.0 ? 2 : 0);
        // Vertical seek: only for gravity-free things and only past 20 units.
        // Note the original adds the raw error when the step would overshoot
        // it, so the two branches have different units.
        if (config.gravity == 0.0 &&
            std::fabs(input.target.vertical_error) >= kVerticalSeekGate) {
            const double climb = dt * config.climb_speed;
            if (std::fabs(climb) <= std::fabs(input.target.vertical_error)) {
                state.velocity.z +=
                    input.target.vertical_error <= 0.0 ? -climb : climb;
            } else {
                state.velocity.z += input.target.vertical_error;
            }
        }
    } else {
        const double left =
            thing_probe(input.world, state.position, -right, radius);
        const double right_free = std::min(
            thing_probe(input.world, state.position, right, radius),
            kThingProbeLength);
        const double two_r = 2.0 * radius;
        int turn = 0;
        if (ahead < two_r || left < two_r || right_free < two_r ||
            ahead < left || ahead < right_free) {
            turn = left < right_free ? 3 : 2;
        }
        out.turn = turn;
        integrate_yaw(state, config, dt, turn);
    }

    // Move: `min(speed*dt, clearance - radius)` along the forward axis.
    double travel = state.speed * dt;
    if (ahead < kThingProbeFree) {
        travel = std::min(travel, ahead - radius);
    }
    if (travel > 0.0) {
        state.velocity.x += travel * forward.x;
        state.velocity.y += travel * forward.y;
        state.velocity.z += travel * forward.z;
        out.moved = true;
    }
}

/// `0x474ea0` (type 5): turret aiming.  Yaw only -- `0x473a00` zeroes the
/// pitch error in this build, so the `+0x30` accumulator of the pitch never
/// changes; the `+0xc8` field is reused as the pitch rate.
void step_turret(ThingMotionState& state, const ThingMotionConfig& config,
                 const ThingMotionInput& input, ThingMotionStep& out) {
    const double dt = input.dt;
    double delta = 0.0;
    double pitch_delta = 0.0;
    if (!input.target.valid) {
        state.yaw_rate -= dt * config.turn_accel;
        if (config.turn_max > 0.0) {
            state.yaw_rate =
                std::clamp(state.yaw_rate, -config.turn_max, config.turn_max);
        }
        delta = dt * state.yaw_rate;
    } else {
        const double error = input.target.yaw_error;
        if (error >= 0.0) {
            if (error > 0.0) {
                state.yaw_rate += dt * config.turn_accel;
            }
        } else {
            state.yaw_rate -= dt * config.turn_accel;
        }
        const bool quarter = (config.flags_b & 0x00800000u) != 0;
        const double limit = quarter ? 0.25 * config.turn_max : config.turn_max;
        if (limit > 0.0) {
            state.yaw_rate = std::clamp(state.yaw_rate, -limit, limit);
        }
        delta = clamp_delta(dt * state.yaw_rate, error);
        pitch_delta =
            clamp_pitch_delta(config.damping * dt, input.target.pitch_error);
    }
    state.yaw_rate *= kYawRateDamping;
    state.yaw_target += delta;
    if (config.yaw_limit_lo != 0.0 || config.yaw_limit_hi != 0.0) {
        if (state.yaw_target < config.yaw_limit_lo) {
            state.yaw_target = config.yaw_limit_lo;
            state.yaw_rate = 0.0;
        } else if (state.yaw_target > config.yaw_limit_hi) {
            state.yaw_target = config.yaw_limit_hi;
            state.yaw_rate = 0.0;
        }
    }
    state.pitch_target += pitch_delta;
    if (config.pitch_limit_lo != 0.0 || config.pitch_limit_hi != 0.0) {
        state.pitch_target =
            std::clamp(state.pitch_target, config.pitch_limit_lo,
                       config.pitch_limit_hi);
    }
    state.last_yaw = state.yaw_target;
    out.turn = delta > 0.0 ? 3 : (delta < 0.0 ? 2 : 0);
}

/// `0x476070` (type 16): a stationary gun that aims and fires at the first
/// weapon band that contains the target distance.  The trail-following half of
/// the handler (the 0xf0-point table at `0x819070`) is replaced by the
/// caller-supplied target; the fire gate, cooldown and band search are the
/// original's.
void step_patrol(ThingMotionState& state, const ThingMotionConfig& config,
                 const ThingMotionInput& input, ThingMotionStep& out) {
    const auto basis = basis_of(state);
    const Vec3d forward = basis[1];
    const Vec3d right = basis[0];
    const double dt = input.dt;
    if (!input.target.valid) {
        return;
    }
    const Vec3d to_target = input.target.position - state.position;
    const double ahead = dot(to_target, forward);
    const double lateral = dot(to_target, right);
    const double distance = length(to_target);

    if (ahead > 0.0 && std::fabs(lateral) * 4.0 < ahead &&
        state.attack_cooldown == 0) {
        // +0x3e8 picks the scan direction: non-zero walks the bands backwards.
        const std::size_t count = config.weapons.size();
        for (std::size_t n = 0; n < count; ++n) {
            const std::size_t i = state.weapon_reverse ? count - 1 - n : n;
            const ProximityEntry& weapon = config.weapons[i];
            if (weapon.event == 0) {
                continue;
            }
            if (static_cast<double>(weapon.low) <= distance &&
                distance <= static_cast<double>(weapon.high)) {
                out.fired_weapon = static_cast<int>(i);
                out.fired_event = weapon.event;
                state.attack_cooldown += weapon.cooldown;
                state.weapon_reverse = !state.weapon_reverse;
                // +0x3ec/+0x3f0: rand() % period == 0 adds step to the
                // cooldown.  Without a generator the bonus is skipped.
                if (config.timer_period != 0 && input.rng != nullptr &&
                    input.rng->next() % config.timer_period == 0) {
                    state.attack_cooldown += config.timer_step;
                }
                break;
            }
        }
    }

    const double error = input.target.yaw_error;
    if (error > 0.0) {
        state.yaw_rate += dt * config.turn_accel;
    } else if (error < 0.0) {
        state.yaw_rate -= dt * config.turn_accel;
    }
    if (config.turn_max > 0.0) {
        state.yaw_rate =
            std::clamp(state.yaw_rate, -config.turn_max, config.turn_max);
    }
    const double delta = clamp_delta(dt * state.yaw_rate, error);
    state.yaw_target += delta;
    state.last_yaw = state.yaw_target;
    out.turn = delta > 0.0 ? 3 : (delta < 0.0 ? 2 : 0);
}

/// `0x4752d0` (type 6): the only proven gravity path.  Kept here so the mode
/// table is complete; `game/projectile.h` is the weapon-facing runtime with
/// the same arithmetic plus the proximity table, the `+0x32c hits` counter and
/// the expire event.  This step owns none of those: it only integrates
/// gravity, moves with collision and applies `damping` on contact.
/// `0x4344df`: `max + (mid + min) / 4` over the absolute components.
double drive_distance(double dx, double dy, double dz) {
    double a = std::fabs(dx);
    double b = std::fabs(dy);
    double c = std::fabs(dz);
    if (a < b) {
        std::swap(a, b);
    }
    if (a < c) {
        std::swap(a, c);
    }
    return a + (b + c) * 0.25;
}

/// `0x476830` (type 18): repel the body from every overlapping target.
///
/// For each target the original checks the vertical band
/// `|thing.z - target.z| < thing.box[2] + target.z_band` (0x476874), then the
/// planar octagon `0x4344df(dx, dy, 0) < thing.box[1] + target.range`
/// (0x47689f..0x4768b0).  The step is `min(reach - distance, dt * speed)` and
/// moves the small-axis velocity component away from the target: the dominant
/// planar axis decides which component (0x4768c4..0x476920).  A step above 0.1
/// posts `+0x3f8` once per target when `+0x3f4 > 0` (0x476929..0x47695c).
void step_push(ThingMotionState& state, const ThingMotionConfig& config,
               const ThingMotionInput& input, ThingMotionStep& out) {
    state.velocity = {};
    for (std::size_t i = 0; i < input.push_targets.size(); ++i) {
        const ThingPushTarget& target = input.push_targets[i];
        const double band = config.box[2] + target.z_band;
        if (!(std::fabs(target.position.z - state.position.z) < band)) {
            continue;
        }
        const double dx = state.position.x - target.position.x;
        const double dy = state.position.y - target.position.y;
        const double reach = config.box[1] + target.range;
        const double distance = drive_distance(dx, dy, 0.0);
        if (!(distance < reach)) {
            continue;
        }
        double step = reach - distance;
        const double limit = input.dt * state.speed;
        if (limit < step) {
            step = limit; // dVar2 = min(reach - d, dt * speed), 0x4768b8
        }
        if (!(step > 0.0)) {
            continue;
        }
        if (std::fabs(dx) <= std::fabs(dy)) {
            // Equal or larger y delta: the y component is pushed.
            if (target.position.y - state.position.y >= 0.0) {
                state.velocity.y -= step;
            } else {
                state.velocity.y += step;
            }
        } else if (target.position.x - state.position.x >= 0.0) {
            state.velocity.x -= step;
        } else {
            state.velocity.x += step;
        }
        out.moved = true;
        out.pushed = static_cast<int>(i);
        if (step > 0.1 && config.push_event_gate > 0) {
            out.fired_event = static_cast<std::uint32_t>(config.push_event);
        }
    }
}

void step_projectile(ThingMotionState& state, const ThingMotionConfig& config,
                     const ThingMotionInput& input, ThingMotionStep& out) {
    const double dt = input.dt;
    const auto basis = basis_of(state);
    state.vertical_speed +=
        kGravityScale * config.gravity * config.level_gravity * dt;
    Vec3d desired = state.position + basis[1] * (state.speed * dt);
    desired.z -= state.vertical_speed * dt;

    bool blocked = false;
    if (input.world != nullptr && config.radius > 0.0 &&
        !input.world->empty()) {
        const Vec3d delta = desired - state.position;
        const auto hit = input.world->sphere_cast(state.position, delta,
                                                  config.radius, length(delta));
        if (hit.has_value()) {
            desired = hit->centre;
            blocked = true;
        }
    }
    out.displacement = desired - state.position;
    state.position = desired;
    out.moved = true;
    if (blocked) {
        out.blocked = true;
        state.vertical_speed = 0.0;
        // `speed *= +0xc8`, and the angular rates decay by the same factor.
        state.speed *= config.damping;
        state.pitch_rate *= config.damping;
        state.roll_rate *= config.damping;
        state.yaw_rate *= config.damping;
        out.bounced = true;
    }
    state.velocity = {};
}

} // namespace

const std::array<ThingMoveInfo, 19>& thing_move_table() {
    static const std::array<ThingMoveInfo, 19> table{{
        {0, ThingMove::idle, "idle", "0x470c70", "proven",
         "no case: only the shared tick part runs"},
        {1, ThingMove::spin, "spin", "0x470c70 case 1", "proven",
         "angles += angle_rate*dt"},
        {2, ThingMove::avoid, "avoid", "0x474260", "proven",
         "forward/left/right probes at 2r, move if d > speed*dt+r"},
        {3, ThingMove::avoid_left, "avoid-left", "0x4746b0(...,1)", "proven",
         "1.25r probe, fixed turn direction"},
        {4, ThingMove::avoid_right, "avoid-right", "0x4746b0(...,0)", "proven",
         "1.25r probe, fixed turn direction"},
        {5, ThingMove::turret, "turret", "0x474ea0", "proven",
         "yaw only; +0xc8 is the pitch rate; +0x228/+0x238 clamps"},
        {6, ThingMove::projectile, "projectile", "0x4752d0", "proven",
         "gravity/bounce/hits here, weapon-facing twin in game/projectile.h"},
        {7, ThingMove::seek, "seek", "0x475930(...,0)", "external",
         "game/projectile.h homing branch, no line of sight"},
        {8, ThingMove::seek_los, "seek-los", "0x475930(...,1)", "external",
         "game/projectile.h homing branch with the LOS probe"},
        {9, ThingMove::path, "path", "0x475f10 case 9", "stub",
         "world/ai_move.h path_advance; no shipped type uses it"},
        {10, ThingMove::path_loop, "pathloop", "0x475f10 case 10", "stub",
         "world/ai_move.h path_advance; no shipped type uses it"},
        {11, ThingMove::path_random, "pathrandom", "0x475f10 case 11", "stub",
         "calls rand(), not modelled"},
        {12, ThingMove::path_expire, "pathexpire", "0x475f10 case 12", "stub",
         "world/ai_move.h path_advance"},
        {13, ThingMove::ram, "ram", "0x474a10", "proven",
         "1.25r probe, target side choice, x/y step"},
        {14, ThingMove::seek_far, "seek-far", "0x473240", "proven",
         "target yaw rate + no-overshoot clamp + 3 probes"},
        {15, ThingMove::seek_near, "seek-near", "0x473240", "proven",
         "same body, other mode flags (not written by this build)"},
        {16, ThingMove::patrol, "patrol", "0x476070", "model",
         "fire gate/bands/cooldown proven, aim supplied by the caller"},
        {17, ThingMove::seek_17, "seek-17", "0x473240", "proven",
         "same body, third mode flag (not written by this build)"},
        {18, ThingMove::push, "push", "0x476830", "proven",
         "repel from overlapping targets; no shipped type uses it"},
    }};
    return table;
}

const ThingMoveInfo& thing_move_info(int movement_type) {
    static const ThingMoveInfo unknown{255, ThingMove::unknown, "unknown",
                                       "-", "stub", "movement_type out of range"};
    if (movement_type < 0 || movement_type >= 19) {
        return unknown;
    }
    return thing_move_table()[static_cast<std::size_t>(movement_type)];
}

std::array<Vec3d, 3> thing_basis(double roll, double pitch, double yaw) {
    const double sr = std::sin(roll);
    const double cr = std::cos(roll);
    const double sp = std::sin(pitch);
    const double cp = std::cos(pitch);
    const double sy = std::sin(yaw);
    const double cy = std::cos(yaw);
    std::array<Vec3d, 3> rows{};
    rows[0] = {cy * cr - sy * sp * sr, -sy * cr - cy * sp * sr, -cp * sr};
    rows[1] = {sy * cp, cy * cp, -sp};
    rows[2] = {sy * sp * cr + cy * sr, cy * sp * cr - sy * sr, cp * cr};
    return rows;
}

double thing_probe(const CollisionWorld* world, const Vec3d& origin,
                   const Vec3d& direction, double radius,
                   double max_distance) {
    if (world == nullptr || world->empty()) {
        return kThingProbeFree;
    }
    const double len = length(direction);
    if (!(len > 0.0) || !std::isfinite(len) || !(max_distance > 0.0)) {
        return kThingProbeFree;
    }
    const auto hit =
        world->raycast(origin, direction * (1.0 / len), max_distance);
    if (!hit.has_value()) {
        return kThingProbeFree;
    }
    const double free_distance = hit->t - radius;
    return free_distance > 0.0 ? free_distance : 0.0;
}

void thing_thrust_step(double& speed, double accel, double max, int& timer,
                       double dt) {
    if (!(accel > 0.0) || !(dt > 0.0)) {
        return;
    }
    if (timer < 0) {
        timer += static_cast<int>(dt);
        speed -= dt * accel;
        if (speed < -max) {
            speed = -max;
        }
        return;
    }
    if (timer < 1) {
        if (speed < 0.0) {
            speed += dt * accel;
            if (speed > 0.0) {
                speed = 0.0;
            }
        }
        if (speed > 0.0) {
            speed -= dt * accel;
            if (speed < 0.0) {
                speed = 0.0;
            }
        }
        return;
    }
    timer -= static_cast<int>(dt);
    if (timer < 0) {
        timer = 0;
    }
    speed += dt * accel;
    if (speed > max) {
        speed = max;
    }
}

ThingMotionStep thing_motion_step(ThingMotionState& state,
                                  const ThingMotionConfig& config,
                                  const ThingMotionInput& input) {
    ThingMotionStep out;
    const ThingMoveInfo& info = thing_move_info(config.movement_type);
    out.supported =
        info.status == "proven" || info.status == "model";
    if (!std::isfinite(input.dt) || !(input.dt > 0.0)) {
        return out;
    }
    if (state.expired) {
        return out;
    }

    prepare_targets(state, config);
    // The tick decrements the attack cooldown every step (`+0x3e4`).
    if (state.attack_cooldown > 0) {
        state.attack_cooldown -= static_cast<int>(input.dt);
        if (state.attack_cooldown < 0) {
            state.attack_cooldown = 0;
        }
    }
    add_impulses(state, input.dt);
    thing_thrust_step(state.thrust_speed, state.thrust_accel, state.thrust_max,
                      state.thrust_timer, input.dt);
    add_thrust(state, input.dt);

    switch (config.movement_type) {
        case 0:
            break;
        case 1:
            state.pitch_target += state.pitch_rate * input.dt;
            state.roll_target += state.roll_rate * input.dt;
            state.yaw_target += state.yaw_rate * input.dt;
            state.last_yaw = state.yaw_target;
            break;
        case 2:
        case 3:
        case 4:
            step_avoid(state, config, input, out);
            break;
        case 5:
            step_turret(state, config, input, out);
            break;
        case 13:
            step_ram(state, config, input, out);
            break;
        case 14:
        case 15:
        case 17:
            step_seek_avoid(state, config, input, out);
            break;
        case 16:
            step_patrol(state, config, input, out);
            break;
        case 6:
            step_projectile(state, config, input, out);
            break;
        case 18:
            step_push(state, config, input, out);
            break;
        default:
            // Types 7..12 live in game/projectile.h and world/ai_move.h.
            break;
    }

    finish_angles(state, input.dt);

    if (input.apply_velocity && config.movement_type != 6) {
        apply_velocity(state, config, input, out);
    } else {
        state.velocity = {};
    }
    return out;
}

ThingMotionState thing_motion_spawn(const ThingMotionConfig& config) {
    ThingMotionState state;
    state.speed = config.speed;
    state.pitch_rate = config.angle_rate[0];
    state.roll_rate = config.angle_rate[1];
    state.yaw_rate = config.angle_rate[2];
    state.yaw = wrap_yaw(state.yaw);
    state.pitch_target = state.pitch;
    state.roll_target = state.roll;
    state.yaw_target = state.yaw;
    state.last_yaw = state.yaw;
    return state;
}

ThingMotionConfig thing_motion_config(const Thing& thing) {
    ThingMotionConfig config;
    config.movement_type = thing.movement_type;
    config.speed = thing.speed;
    config.turn_accel = thing.turn_accel;
    config.turn_max = thing.turn_max;
    config.radius = thing.radius;
    config.box = thing.box;
    config.gravity = thing.gravity;
    config.damping = thing.damping;
    config.arrive_radius = thing.arrive_radius;
    config.search_radius = thing.search_radius;
    config.climb_speed = thing.climb_speed;
    config.angle_rate = thing.angle_rate;
    config.flags_a = thing.flags_a;
    config.flags_b = thing.flags_b;
    config.flags_c = thing.flags_c;
    config.expire_ticks = thing.expire_ticks;
    config.weapons = thing.proximity;
    config.timer_period = thing.timer_period;
    config.timer_step = thing.timer_step;
    return config;
}

} // namespace gm
