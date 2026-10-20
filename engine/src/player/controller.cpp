// Deterministic player controller: see player/controller.h and
// docs/PLAYER_CONTROLLER.md.
//
// Pipeline of one tick (fixed order, no globals):
//   1. validate config/state/input (errors leave the state untouched);
//   2. crouch height (stand up only with headroom);
//   3. ground probe -> `grounded`;
//   4. wish direction from yaw, ground friction, acceleration, speed clamp;
//   5. jump, then gravity;
//   6. `substeps` collide-and-slide moves (three-sphere capsule, step-up on
//      the first wall contact while grounded);
//   7. step-down snap when the player was grounded and is not rising;
//   8. depenetration against the triangle soup, final ground probe.

#include "player/controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gm {
namespace {

constexpr double kFloorNormalZ = 0.8;
constexpr double kCeilingNormalZ = -0.8;
constexpr double kNormalTolerance = 1e-12;
constexpr double kGroundProbe = 0.05;
constexpr double kStopSpeed = 1.0;
constexpr int kDepenetrationIterations = 8;
constexpr double kHardMaxDt = 0.25;

bool is_floor(Vec3d normal) { return normal.z >= kFloorNormalZ - kNormalTolerance; }
bool is_ceiling(Vec3d normal) { return normal.z <= kCeilingNormalZ + kNormalTolerance; }

Surface classify(Vec3d normal) {
    if (is_floor(normal)) {
        return Surface::floor;
    }
    if (is_ceiling(normal)) {
        return Surface::ceiling;
    }
    return Surface::wall;
}

bool bad(std::string& error, const char* text) {
    error = text;
    return false;
}

bool valid_config(const ControllerConfig& c, std::string& error) {
    if (!std::isfinite(c.radius) || !(c.radius > 0.0)) {
        return bad(error, "config.radius must be finite and > 0");
    }
    if (!std::isfinite(c.height) || !(c.height > 0.0)) {
        return bad(error, "config.height must be finite and > 0");
    }
    if (c.height < 2.0 * c.radius) {
        return bad(error, "config.height must be >= 2 * config.radius");
    }
    if (!std::isfinite(c.eye_height) || c.eye_height < 0.0) {
        return bad(error, "config.eye_height must be finite and >= 0");
    }
    if (!std::isfinite(c.gravity) || c.gravity < 0.0) {
        return bad(error, "config.gravity must be finite and >= 0");
    }
    if (!std::isfinite(c.accel) || c.accel < 0.0) {
        return bad(error, "config.accel must be finite and >= 0");
    }
    if (!std::isfinite(c.friction) || c.friction < 0.0) {
        return bad(error, "config.friction must be finite and >= 0");
    }
    if (!std::isfinite(c.max_speed) || c.max_speed < 0.0) {
        return bad(error, "config.max_speed must be finite and >= 0");
    }
    if (!std::isfinite(c.step_height) || c.step_height < 0.0) {
        return bad(error, "config.step_height must be finite and >= 0");
    }
    if (!std::isfinite(c.jump_speed) || c.jump_speed < 0.0) {
        return bad(error, "config.jump_speed must be finite and >= 0");
    }
    if (!std::isfinite(c.air_control) || c.air_control < 0.0) {
        return bad(error, "config.air_control must be finite and >= 0");
    }
    if (!std::isfinite(c.crouch_scale) || !(c.crouch_scale > 0.0) ||
        c.crouch_scale > 1.0) {
        return bad(error, "config.crouch_scale must be in (0, 1]");
    }
    if (!std::isfinite(c.dt) || !(c.dt > 0.0)) {
        return bad(error, "config.dt must be finite and > 0");
    }
    if (!std::isfinite(c.max_dt) || !(c.max_dt > 0.0)) {
        return bad(error, "config.max_dt must be finite and > 0");
    }
    if (c.dt > c.max_dt) {
        return bad(error, "config.dt exceeds config.max_dt");
    }
    if (c.dt > kHardMaxDt) {
        return bad(error, "config.dt exceeds the 0.25 s hard limit");
    }
    if (!std::isfinite(c.contact_skin) || !(c.contact_skin > 0.0)) {
        return bad(error, "config.contact_skin must be finite and > 0");
    }
    if (c.max_slides < 1 || c.max_slides > 16) {
        return bad(error, "config.max_slides must be in 1..16");
    }
    if (c.max_substeps < 1 || c.max_substeps > 256) {
        return bad(error, "config.max_substeps must be in 1..256");
    }
    return true;
}

bool valid_state(const PlayerState& s, const ControllerConfig& c, std::string& error) {
    if (!finite(s.pos) || !finite(s.vel)) {
        return bad(error, "state.pos/state.vel must be finite");
    }
    if (!std::isfinite(s.yaw)) {
        return bad(error, "state.yaw must be finite");
    }
    if (!std::isfinite(s.height) || s.height < 2.0 * c.radius) {
        return bad(error, "state.height must be finite and >= 2 * config.radius");
    }
    if (!std::isfinite(s.eye_height) || s.eye_height < 0.0) {
        return bad(error, "state.eye_height must be finite and >= 0");
    }
    return true;
}

bool valid_input(const Input& in, std::string& error) {
    if (!std::isfinite(in.forward) || !std::isfinite(in.strafe)) {
        return bad(error, "input.forward/input.strafe must be finite");
    }
    return true;
}

std::array<double, 3> sphere_offsets(double height, double radius) {
    // Feet, middle, head.  At height == 2 * radius the three spheres coincide
    // and the capsule degenerates to one sphere; the order stays fixed.
    return {radius, height * 0.5, height - radius};
}

struct SweepHit {
    bool hit = false;
    double t = 0.0;
    Vec3d normal{};
    Vec3d point{};
    Surface surface = Surface::wall;
    int sphere = -1;
};

SweepHit sweep_capsule(const CollisionWorld& world, Vec3d pos, Vec3d delta, double height,
                       const ControllerConfig& config) {
    SweepHit best;
    const double distance = length(delta);
    if (!(distance > 0.0) || !std::isfinite(distance)) {
        return best;
    }
    const std::array<double, 3> offsets = sphere_offsets(height, config.radius);
    for (int sphere = 0; sphere < 3; ++sphere) {
        const Vec3d centre = pos + Vec3d{0.0, 0.0, offsets[static_cast<std::size_t>(sphere)]};
        const auto hit = world.sphere_cast(centre, delta, config.radius, distance);
        if (!hit) {
            continue;
        }
        if (!best.hit || hit->t < best.t) {
            best.hit = true;
            best.t = hit->t;
            best.normal = hit->normal;
            best.point = hit->point;
            best.surface = classify(hit->normal);
            best.sphere = sphere;
        }
    }
    return best;
}

bool ground_contact(const CollisionWorld& world, Vec3d pos,
                    const ControllerConfig& config) {
    CollideFilter filter;
    filter.walls = false;
    filter.ceilings = false;
    filter.floors = true;
    filter.min_normal_z = kFloorNormalZ;
    const Vec3d centre = pos + Vec3d{0.0, 0.0, config.radius};
    const auto hit = world.sphere_cast(centre, Vec3d{0.0, 0.0, -1.0}, config.radius,
                                       kGroundProbe, filter);
    return hit.has_value();
}

Vec3d wish_direction(double yaw, const Input& input) {
    const double forward = std::clamp(input.forward, -1.0, 1.0);
    const double strafe = std::clamp(input.strafe, -1.0, 1.0);
    const double sy = std::sin(yaw);
    const double cy = std::cos(yaw);
    // docs/CAMERA.md §6, roll = 0: forward = (sin yaw, cos yaw, 0),
    // right = (cos yaw, -sin yaw, 0).
    Vec3d wish{sy * forward + cy * strafe, cy * forward - sy * strafe, 0.0};
    const double len = length(wish);
    if (len > 1.0) {
        wish = wish * (1.0 / len);
    }
    return wish;
}

void clamp_horizontal_speed(PlayerState& s, double max_speed) {
    const double speed = std::sqrt(s.vel.x * s.vel.x + s.vel.y * s.vel.y);
    if (speed > max_speed && speed > 0.0) {
        const double factor = max_speed / speed;
        s.vel.x *= factor;
        s.vel.y *= factor;
    }
}

void update_crouch(PlayerState& s, bool crouch, const CollisionWorld& world,
                   const ControllerConfig& config) {
    const double target = crouch ? config.height * config.crouch_scale : config.height;
    if (target < s.height) {
        s.height = target;
    } else if (target > s.height) {
        const double diff = target - s.height;
        const SweepHit up = sweep_capsule(world, s.pos,
                                          Vec3d{0.0, 0.0, diff + config.contact_skin},
                                          s.height, config);
        if (!up.hit) {
            s.height = target;
        }
    }
    s.crouching = s.height < config.height - 1e-9;
    const double ratio = config.height > 0.0 ? s.height / config.height : 1.0;
    s.eye_height = config.eye_height * ratio;
}

// Push the capsule out of anything it overlaps.  The deepest contact is
// resolved first, then the loop recomputes: bounded, deterministic, and it
// leaves `contact_skin` between the capsule and the surface so the next
// downward sweep reports a contact instead of an initial overlap.
void depenetrate(const CollisionWorld& world, PlayerState& s, const ControllerConfig& config) {
    std::vector<std::uint32_t> triangles;
    const std::array<double, 3> offsets = sphere_offsets(s.height, config.radius);
    for (int iteration = 0; iteration < kDepenetrationIterations; ++iteration) {
        Aabb box;
        const Vec3d lo = s.pos + Vec3d{-config.radius, -config.radius, -config.contact_skin};
        const Vec3d hi = s.pos + Vec3d{config.radius, config.radius,
                                       s.height + config.contact_skin};
        box.expand(lo);
        box.expand(hi);
        world.overlap_aabb(box, triangles);

        bool found = false;
        double best_push = 0.0;
        Vec3d best_normal{};
        for (int sphere = 0; sphere < 3; ++sphere) {
            const Vec3d centre =
                s.pos + Vec3d{0.0, 0.0, offsets[static_cast<std::size_t>(sphere)]};
            for (const std::uint32_t index : triangles) {
                if (index >= world.triangles().size()) {
                    continue; // defensive: impossible for a world we own
                }
                const Triangle& triangle = world.triangles()[index];
                const Vec3d closest = closest_point_on_triangle(centre, triangle);
                const Vec3d delta = centre - closest;
                const double distance = length(delta);
                Vec3d normal;
                double push = 0.0;
                if (distance > 1e-12) {
                    normal = delta * (1.0 / distance);
                    push = (config.radius + config.contact_skin) - distance;
                } else {
                    // Centre exactly on the triangle: use the face normal,
                    // oriented towards the centre's side.
                    const double side = dot(triangle.normal, centre - triangle.a);
                    normal = side >= 0.0 ? triangle.normal : -triangle.normal;
                    push = config.radius + config.contact_skin;
                }
                if (!(push > 0.0)) {
                    continue;
                }
                if (!found || push > best_push) {
                    found = true;
                    best_push = push;
                    best_normal = normal;
                }
            }
        }
        if (!found) {
            break;
        }
        s.pos = s.pos + best_normal * best_push;
    }
}

bool try_step_up(const CollisionWorld& world, PlayerState& s, Vec3d wish, Vec3d& remaining,
                 const ControllerConfig& config) {
    if (!(config.step_height > 0.0)) {
        return false;
    }
    // The wall may have consumed the whole substep, so use the *intended*
    // horizontal direction (the wish vector) rather than the leftover motion.
    Vec3d horizontal{wish.x, wish.y, 0.0};
    const double horizontal_length = length(horizontal);
    if (!(horizontal_length > 1e-9)) {
        horizontal = Vec3d{remaining.x, remaining.y, 0.0};
    }
    const double horizontal_move = length(horizontal);
    if (!(horizontal_move > 1e-9)) {
        return false;
    }
    // Headroom: the whole capsule must fit one step higher.
    const SweepHit headroom =
        sweep_capsule(world, s.pos,
                      Vec3d{0.0, 0.0, config.step_height + config.contact_skin}, s.height,
                      config);
    if (headroom.hit) {
        return false;
    }
    // Raise, then move forward far enough that the feet are *over* the ledge
    // (a radius, not just this tick's momentum).  This is what keeps the
    // rounded bottom from hooking on the top edge.  A real wall still stops
    // the sweep, in which case the step is rejected below.
    const Vec3d direction = horizontal * (1.0 / horizontal_move);
    const double reach = std::max(horizontal_move, config.radius + 0.1);
    Vec3d raised = s.pos + Vec3d{0.0, 0.0, config.step_height};
    const SweepHit lateral = sweep_capsule(world, raised, direction * reach, s.height, config);
    if (lateral.hit) {
        return false; // something solid is still in the way at step height
    }
    raised = raised + direction * reach;

    // Land on the floor directly below the feet, not on the ledge's edge.
    const auto floor_z = world.floor_at(raised.x, raised.y,
                                        raised.z + config.contact_skin,
                                        config.step_height + config.contact_skin);
    if (!floor_z) {
        return false;
    }
    if (!(*floor_z > s.pos.z + 1e-3)) {
        return false; // no actual rise: this was a wall, not a step
    }
    if (*floor_z > s.pos.z + config.step_height + 1e-9) {
        return false;
    }
    s.pos = Vec3d{raised.x, raised.y, *floor_z};
    s.vel = Vec3d{s.vel.x, s.vel.y, 0.0};
    s.on_ground = true;
    remaining = Vec3d{0.0, 0.0, remaining.z};
    return true;
}

void integrate(PlayerState& s, const CollisionWorld& world, const ControllerConfig& config,
               bool grounded, bool jumped, const Vec3d& wish, StepResult& out) {
    const double speed = length(s.vel);
    double max_step = config.radius * 0.5;
    if (!(max_step > 1e-6)) {
        max_step = 1e-6;
    }
    int substeps = 1;
    const double tick_distance = speed * config.dt;
    if (tick_distance > max_step) {
        const double count = std::ceil(tick_distance / max_step);
        if (count > static_cast<double>(config.max_substeps)) {
            substeps = config.max_substeps;
        } else if (count >= 1.0) {
            substeps = static_cast<int>(count);
        }
    }
    if (substeps < 1) {
        substeps = 1;
    }
    out.substeps = substeps;
    const double h = config.dt / static_cast<double>(substeps);
    bool step_attempted = false;

    for (int sub = 0; sub < substeps; ++sub) {
        Vec3d remaining = s.vel * h;
        for (int slide = 0; slide < config.max_slides; ++slide) {
            const double length_remaining = length(remaining);
            if (!(length_remaining > 1e-12)) {
                remaining = Vec3d{};
                break;
            }
            const SweepHit hit = sweep_capsule(world, s.pos, remaining, s.height, config);
            if (!hit.hit) {
                s.pos = s.pos + remaining;
                remaining = Vec3d{};
                break;
            }
            // Stop one skin short of the contact so the next sweep starts
            // clear of the surface instead of reporting an initial overlap.
            const double travel = hit.t > config.contact_skin ? hit.t - config.contact_skin
                                                              : 0.0;
            if (travel > 0.0) {
                s.pos = s.pos + remaining * (travel / length_remaining);
            }
            remaining = remaining * ((length_remaining - travel) / length_remaining);
            ++out.slides;
            switch (hit.surface) {
            case Surface::floor:
                out.hit_floor = true;
                s.on_ground = true;
                break;
            case Surface::ceiling:
                out.hit_ceiling = true;
                break;
            case Surface::wall:
                out.hit_wall = true;
                break;
            }

            const double into = dot(remaining, hit.normal);
            if (into < 0.0) {
                remaining = remaining - hit.normal * into;
            }
            const double velocity_into = dot(s.vel, hit.normal);
            if (velocity_into < 0.0) {
                s.vel = s.vel - hit.normal * velocity_into;
            }

            if (hit.surface == Surface::wall && grounded && !jumped && !step_attempted &&
                dot(wish, hit.normal) < 0.0) {
                step_attempted = true; // at most one step-up attempt per tick
                if (try_step_up(world, s, wish, remaining, config)) {
                    out.stepped_up = true;
                }
            }

        }
    }
}

void apply_step_down(PlayerState& s, const CollisionWorld& world, const ControllerConfig& config,
                     bool grounded, bool jumped, StepResult& out) {
    if (!grounded || jumped || s.vel.z > 0.0 || !(config.step_height > 0.0)) {
        return;
    }
    // A ray straight down the capsule axis, not a sphere sweep: at a ledge the
    // sweep would hook on the rounded edge instead of finding the floor below.
    const double reach = config.step_height + 2.0 * config.contact_skin;
    const auto floor_z = world.floor_at(s.pos.x, s.pos.y, s.pos.z + config.contact_skin,
                                        reach);
    if (!floor_z) {
        return;
    }
    const double drop = s.pos.z - *floor_z;
    if (!(drop >= -1e-3) || drop > config.step_height + 1e-3) {
        return;
    }
    if (drop > 1e-3) {
        s.pos.z = *floor_z;
        out.stepped_down = true;
    }
    s.vel.z = 0.0;
    s.on_ground = true;
}

} // namespace

PlayerState make_state(const Vec3d& feet, double yaw, const ControllerConfig& config) {
    PlayerState state;
    state.pos = feet;
    state.vel = Vec3d{};
    state.yaw = yaw;
    state.height = config.height;
    state.eye_height = config.eye_height;
    state.on_ground = false;
    state.crouching = false;
    return state;
}

Vec3d eye_position(const PlayerState& state) {
    return state.pos + Vec3d{0.0, 0.0, state.eye_height};
}

StepResult step(const PlayerState& state, const Input& input, const CollisionWorld& world,
                const ControllerConfig& config) {
    StepResult result;
    result.state = state;

    std::string error;
    if (!valid_config(config, error) || !valid_state(state, config, error) ||
        !valid_input(input, error)) {
        result.ok = false;
        result.error = error;
        return result;
    }

    PlayerState s = state;
    update_crouch(s, input.crouch, world, config);
    // Clear any overlap we were handed (below the floor at spawn, touching a
    // surface exactly) so the first sweep of the tick starts with a skin gap.
    depenetrate(world, s, config);

    bool grounded = ground_contact(world, s.pos, config);
    if (!grounded && state.on_ground && s.vel.z <= 0.0) {
        // Walked just past a ledge: keep the support for one tick so the
        // step-down snap below can find the lower floor.
        grounded = true;
    }
    s.on_ground = grounded;

    const Vec3d wish = wish_direction(s.yaw, input);
    const double wish_length = length(wish);

    // Ground friction.
    double speed = std::sqrt(s.vel.x * s.vel.x + s.vel.y * s.vel.y);
    if (grounded && speed > 0.0) {
        const double control = speed > kStopSpeed ? speed : kStopSpeed;
        double next = speed - control * config.friction * config.dt;
        if (next < 0.0) {
            next = 0.0;
        }
        const double factor = next / speed;
        s.vel.x *= factor;
        s.vel.y *= factor;
        speed = next;
    }

    // Acceleration towards `wish * max_speed`, with a hard speed clamp.
    if (wish_length > 1e-12) {
        const Vec3d direction = wish * (1.0 / wish_length);
        const double wish_speed = wish_length * config.max_speed;
        const double current = s.vel.x * direction.x + s.vel.y * direction.y;
        const double rate = grounded ? config.accel : config.accel * config.air_control;
        double add = rate * wish_speed * config.dt;
        const double room = wish_speed - current;
        if (add > 0.0 && room > 0.0) {
            if (add > room) {
                add = room;
            }
            s.vel.x += direction.x * add;
            s.vel.y += direction.y * add;
        }
    }
    clamp_horizontal_speed(s, config.max_speed);

    bool jumped = false;
    if (input.jump && grounded) {
        s.vel.z = config.jump_speed;
        jumped = true;
        result.jumped = true;
        s.on_ground = false;
    }
    s.vel.z -= config.gravity * config.dt;

    integrate(s, world, config, grounded, jumped, wish, result);
    apply_step_down(s, world, config, grounded, jumped, result);
    depenetrate(world, s, config);
    s.on_ground = ground_contact(world, s.pos, config);

    if (!finite(s.pos) || !finite(s.vel) || !std::isfinite(s.height)) {
        // Overflow from an absurd (but finite) input velocity: report it like
        // any other bad input instead of leaking inf into the caller.
        result.ok = false;
        result.error = "state became non-finite during integration";
        result.state = state;
        return result;
    }

    result.state = s;
    return result;
}

} // namespace gm
