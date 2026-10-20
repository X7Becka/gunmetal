// Deterministic 91 Hz player controller (docs/PLAYER_CONTROLLER.md).
//
// Synthetic worlds are hand-written `SbwLevel`s (identity object transform), so
// the whole pipeline `SbwLevel -> CollisionWorld -> Controller` is exercised:
//   * fall in free space and onto a floor, with the semi-implicit Euler values
//     pinned analytically;
//   * jump take-off and apex;
//   * wall stop and diagonal slide;
//   * step-up onto a 2-unit ledge and step-down off it;
//   * ceiling hit while jumping;
//   * speed clamp, two-run determinism, huge velocity without tunnelling;
//   * NaN/inf/oversized dt rejected without touching the state.
//
// Real part: `level0.sbw` (game install, skipped when absent).  The spawn is
// the proven `SET_PLAYER_START[0]` point (24, -208, 48) from docs/PLAYER.md;
// the controller drops it onto the proven floor (48.546875, docs/COLLIDE.md)
// and then runs 100 ticks of `forward` at yaw +90 deg (+X).  Goldens for that
// run are pinned from the implementation and cross-checked against the wall
// distance from docs/COLLIDE.md (the +X wall is 70.671875 away from the spawn
// eye, so the capsule axis stops at 94.671875 - radius).
#include "player/controller.h"

#include "render/collide.h"
#include "res/sbw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (eps))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s=%.17g != %s=%.17g\n", __FILE__, __LINE__,   \
                         #a, va, #b, vb);                                                    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";
constexpr double kPi = 3.14159265358979323846;

// --- synthetic .sbw construction -------------------------------------------

struct TriSpec {
    gm::Vec3d a;
    gm::Vec3d b;
    gm::Vec3d c;
};

void put_f32(std::uint8_t* p, float value) {
    std::memcpy(p, &value, sizeof(value));
}

/// Identity object transform: origin (0,0,0), basis diag(1,1,1), so local
/// coordinates are already world coordinates.
void write_identity_header(std::array<std::uint8_t, 96>& header) {
    header.fill(0);
    for (int point = 0; point < 4; ++point) {
        for (int component = 0; component < 3; ++component) {
            const float value =
                (point == 0) ? 0.0f : (component == point - 1 ? 1.0f : 0.0f);
            put_f32(header.data() + 8 * (point * 3 + component) + 4, value);
        }
    }
}

gm::SbwLevel make_level(const std::vector<TriSpec>& specs) {
    gm::SbwLevel level;
    level.header.sectors.resize(1);
    for (const TriSpec& spec : specs) {
        gm::SbwObjectRef object;
        write_identity_header(object.header);
        level.header.sectors.at(0).objects.push_back(object);

        gm::SbwGeometry geometry;
        geometry.n_verts = 3;
        geometry.vertices = {gm::Vec3f{static_cast<float>(spec.a.x),
                                       static_cast<float>(spec.a.y),
                                       static_cast<float>(spec.a.z)},
                             gm::Vec3f{static_cast<float>(spec.b.x),
                                       static_cast<float>(spec.b.y),
                                       static_cast<float>(spec.b.z)},
                             gm::Vec3f{static_cast<float>(spec.c.x),
                                       static_cast<float>(spec.c.y),
                                       static_cast<float>(spec.c.z)}};
        gm::SbwPoly poly;
        poly.indices = {0, 1, 2};
        geometry.polys.push_back(poly);
        geometry.n_polys = 1;
        level.geometry.push_back(geometry);
    }
    level.header.object_count = specs.size();
    return level;
}

void add_quad(std::vector<TriSpec>& out, gm::Vec3d p0, gm::Vec3d p1, gm::Vec3d p2,
              gm::Vec3d p3) {
    out.push_back({p0, p1, p2});
    out.push_back({p0, p2, p3});
}

gm::CollisionWorld build_world(const std::vector<TriSpec>& specs) {
    gm::CollisionWorld world;
    std::string error;
    const bool ok = world.build(make_level(specs), &error);
    CHECK(ok);
    CHECK(error.empty());
    return world;
}

std::vector<TriSpec> floor_quad(double z, double half = 200.0) {
    std::vector<TriSpec> out;
    add_quad(out, {-half, -half, z}, {half, -half, z}, {half, half, z}, {-half, half, z});
    return out;
}

std::vector<TriSpec> ceiling_quad(double z, double half = 200.0) {
    std::vector<TriSpec> out;
    add_quad(out, {-half, -half, z}, {-half, half, z}, {half, half, z}, {half, -half, z});
    return out;
}

/// Vertical wall at `x`, normal -X, spanning `z0..z1`, `y` in +-half.
std::vector<TriSpec> wall_x(double x, double z0, double z1, double half = 100.0) {
    std::vector<TriSpec> out;
    add_quad(out, {x, -half, z0}, {x, -half, z1}, {x, half, z1}, {x, half, z0});
    return out;
}

/// Vertical wall at `y`, normal -Y, spanning `z0..z1`, `x` in +-half.
std::vector<TriSpec> wall_y(double y, double z0, double z1, double half = 100.0) {
    std::vector<TriSpec> out;
    add_quad(out, {-half, y, z0}, {-half, y, z1}, {half, y, z1}, {half, y, z0});
    return out;
}

/// Floor at z = 0, a ledge top at z = 4 for x in [10, 40] and its vertical
/// face at x = 10.  The ledge is taller than the capsule radius (3), so the
/// rounded bottom alone cannot climb it: only the step-up path can.
std::vector<TriSpec> step_specs() {
    std::vector<TriSpec> out = floor_quad(0.0);
    add_quad(out, {10, -50, 0}, {10, -50, 4}, {10, 50, 4}, {10, 50, 0}); // face
    add_quad(out, {10, -50, 4}, {40, -50, 4}, {40, 50, 4}, {10, 50, 4}); // top
    add_quad(out, {40, -50, 0}, {40, -50, 4}, {40, 50, 4}, {40, 50, 0}); // far face
    return out;
}

bool same_double(double a, double b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool same_state(const gm::PlayerState& a, const gm::PlayerState& b) {
    // Bitwise, so two runs must agree on -0/NaN too, not just on `==`.
    return same_double(a.pos.x, b.pos.x) && same_double(a.pos.y, b.pos.y) &&
           same_double(a.pos.z, b.pos.z) && same_double(a.vel.x, b.vel.x) &&
           same_double(a.vel.y, b.vel.y) && same_double(a.vel.z, b.vel.z) &&
           same_double(a.yaw, b.yaw) && same_double(a.height, b.height) &&
           same_double(a.eye_height, b.eye_height) && a.on_ground == b.on_ground &&
           a.crouching == b.crouching;
}

bool state_matches(const gm::PlayerState& a, const gm::PlayerState& b) {
    return same_state(a, b);
}

// --- synthetic tests -------------------------------------------------------

void test_fall() {
    gm::CollisionWorld empty;
    gm::ControllerConfig config;
    CHECK(config.dt == 1.0 / 91.0); // fixed 91 Hz tick, no clock involved
    gm::PlayerState state = gm::make_state({0.0, 0.0, 1000.0}, 0.0, config);
    const gm::Input idle;

    const int ticks = 10;
    for (int i = 0; i < ticks; ++i) {
        const gm::StepResult result = gm::step(state, idle, empty, config);
        CHECK(result.ok);
        CHECK(result.error.empty());
        state = result.state;
    }
    const double g = config.gravity;
    const double dt = config.dt;
    const double expected_vel = -g * dt * static_cast<double>(ticks);
    const double expected_z =
        1000.0 - g * dt * dt * 0.5 * static_cast<double>(ticks) *
                     static_cast<double>(ticks + 1);
    CHECK_NEAR(state.vel.z, expected_vel, 1e-9);
    CHECK_NEAR(state.pos.z, expected_z, 1e-9);
    CHECK_NEAR(state.pos.x, 0.0, 0.0);
    CHECK_NEAR(state.pos.y, 0.0, 0.0);
    CHECK(!state.on_ground);
    CHECK(!state.crouching);
}

void test_land_and_jump() {
    const gm::CollisionWorld world = build_world(floor_quad(0.0));
    gm::ControllerConfig config;
    const gm::Input idle;
    gm::PlayerState state = gm::make_state({0.0, 0.0, 5.0}, 0.0, config);

    for (int i = 0; i < 30; ++i) {
        const gm::StepResult result = gm::step(state, idle, world, config);
        CHECK(result.ok);
        state = result.state;
    }
    // Resting height is the floor plus the 1e-6 depenetration skin.
    CHECK_NEAR(state.pos.z, config.contact_skin, 1e-5);
    CHECK_NEAR(state.vel.z, 0.0, 1e-12);
    CHECK(state.on_ground);

    // Take-off: v = jump_speed - g*dt, z += v*dt (semi-implicit Euler).
    gm::Input jump;
    jump.jump = true;
    const gm::StepResult take_off = gm::step(state, jump, world, config);
    CHECK(take_off.ok);
    CHECK(take_off.jumped);
    const double expected_vel = config.jump_speed - config.gravity * config.dt;
    CHECK_NEAR(take_off.state.vel.z, expected_vel, 1e-9);
    CHECK_NEAR(take_off.state.pos.z, state.pos.z + expected_vel * config.dt, 1e-9);
    CHECK(!take_off.state.on_ground);

    // Apex over the next ticks, then landing again.
    state = take_off.state;
    double apex = state.pos.z;
    bool landed = false;
    for (int i = 0; i < 90; ++i) {
        const gm::StepResult result = gm::step(state, idle, world, config);
        CHECK(result.ok);
        state = result.state;
        apex = std::max(apex, state.pos.z);
        if (state.on_ground && i > 2) {
            landed = true;
            break;
        }
    }
    CHECK(landed);
    CHECK_NEAR(state.pos.z, config.contact_skin, 1e-5);
    const double expected_apex = config.jump_speed * config.jump_speed /
                                 (2.0 * config.gravity);
    CHECK_NEAR(apex, expected_apex, expected_apex * 0.1);
}

void test_wall_stop_and_slide() {
    std::vector<TriSpec> specs = floor_quad(0.0);
    const std::vector<TriSpec> wall = wall_x(20.0, -10.0, 30.0);
    specs.insert(specs.end(), wall.begin(), wall.end());
    const gm::CollisionWorld world = build_world(specs);
    gm::ControllerConfig config;

    // Head-on: the capsule axis stops `radius` from the wall plane.
    {
        gm::PlayerState state = gm::make_state({-10.0, 0.0, 0.0}, kPi / 2.0, config);
        gm::Input input;
        input.forward = 1.0;
        for (int i = 0; i < 60; ++i) {
            const gm::StepResult result = gm::step(state, input, world, config);
            CHECK(result.ok);
            state = result.state;
        }
        CHECK_NEAR(state.pos.x, 20.0 - config.radius - config.contact_skin, 1e-5);
        CHECK_NEAR(state.pos.y, 0.0, 1e-12);
        CHECK_NEAR(state.pos.z, config.contact_skin, 1e-5);
        CHECK_NEAR(state.vel.x, 0.0, 1e-12);
        CHECK(state.on_ground);
    }

    // Diagonal: the +Y component survives, the +X component is removed.
    {
        gm::PlayerState state = gm::make_state({-10.0, -10.0, 0.0}, kPi / 4.0, config);
        gm::Input input;
        input.forward = 1.0;
        const double y0 = state.pos.y;
        for (int i = 0; i < 60; ++i) {
            const gm::StepResult result = gm::step(state, input, world, config);
            CHECK(result.ok);
            state = result.state;
        }
        CHECK_NEAR(state.pos.x, 20.0 - config.radius - config.contact_skin, 1e-5);
        CHECK(state.pos.y > y0 + 5.0);
        CHECK(state.vel.y > 1.0);
        CHECK(state.on_ground);
    }
}

void test_corner() {
    std::vector<TriSpec> specs = floor_quad(0.0);
    const std::vector<TriSpec> wx = wall_x(20.0, -10.0, 30.0);
    const std::vector<TriSpec> wy = wall_y(10.0, -10.0, 30.0);
    specs.insert(specs.end(), wx.begin(), wx.end());
    specs.insert(specs.end(), wy.begin(), wy.end());
    const gm::CollisionWorld world = build_world(specs);
    gm::ControllerConfig config;

    gm::PlayerState state = gm::make_state({0.0, 0.0, 0.0}, kPi / 4.0, config);
    gm::Input into;
    into.forward = 1.0;
    for (int i = 0; i < 60; ++i) {
        const gm::StepResult result = gm::step(state, into, world, config);
        CHECK(result.ok);
        state = result.state;
    }
    CHECK(finite(state.pos));
    CHECK(finite(state.vel));
    CHECK(state.on_ground);
    CHECK_NEAR(state.pos.x, 20.0 - config.radius - config.contact_skin, 1e-4);
    CHECK_NEAR(state.pos.y, 10.0 - config.radius - config.contact_skin, 1e-4);
    const gm::Vec3d stuck = state.pos;

    // The corner must not trap the capsule: reverse and walk away.
    gm::Input out;
    out.forward = -1.0;
    for (int i = 0; i < 20; ++i) {
        const gm::StepResult result = gm::step(state, out, world, config);
        CHECK(result.ok);
        state = result.state;
    }
    CHECK(std::sqrt((state.pos.x - stuck.x) * (state.pos.x - stuck.x) +
                    (state.pos.y - stuck.y) * (state.pos.y - stuck.y)) > 5.0);
}

void test_step_up() {
    const gm::CollisionWorld world = build_world(step_specs());
    gm::ControllerConfig config;
    config.step_height = 5.0; // > ledge height (4) and > radius (3)

    gm::PlayerState state = gm::make_state({0.0, 0.0, 0.0}, kPi / 2.0, config);
    gm::Input input;
    input.forward = 1.0;
    bool stepped_up = false;
    for (int i = 0; i < 20; ++i) {
        const gm::StepResult result = gm::step(state, input, world, config);
        CHECK(result.ok);
        stepped_up = stepped_up || result.stepped_up;
        state = result.state;
    }
    CHECK(stepped_up);
    CHECK(state.pos.x > 10.0);
    CHECK_NEAR(state.pos.z, 4.0 + config.contact_skin, 1e-5);
    CHECK(state.on_ground);

    // Without step-up the same run must stop at the 4-unit face (control).
    gm::ControllerConfig no_step = config;
    no_step.step_height = 0.0;
    gm::PlayerState blocked = gm::make_state({0.0, 0.0, 0.0}, kPi / 2.0, no_step);
    for (int i = 0; i < 20; ++i) {
        const gm::StepResult result = gm::step(blocked, input, world, no_step);
        CHECK(result.ok);
        blocked = result.state;
    }
    CHECK(blocked.pos.x < 10.0);
    CHECK_NEAR(blocked.pos.x, 10.0 - no_step.radius, 1e-3);
    CHECK_NEAR(blocked.pos.z, no_step.contact_skin, 1e-3);
}

void test_step_down() {
    const gm::CollisionWorld world = build_world(step_specs());
    gm::ControllerConfig config;
    config.step_height = 5.0;
    gm::PlayerState state = gm::make_state({20.0, 0.0, 4.0}, kPi / 2.0, config);
    gm::Input input;
    input.forward = 1.0;
    bool stepped_down = false;
    for (int i = 0; i < 25; ++i) {
        const gm::StepResult result = gm::step(state, input, world, config);
        CHECK(result.ok);
        stepped_down = stepped_down || result.stepped_down;
        state = result.state;
    }
    CHECK(stepped_down);
    CHECK(state.pos.x > 40.0);
    CHECK_NEAR(state.pos.z, config.contact_skin, 1e-4);
    CHECK(state.on_ground);
}

void test_ceiling() {
    std::vector<TriSpec> specs = floor_quad(0.0);
    const std::vector<TriSpec> ceiling = ceiling_quad(20.0);
    specs.insert(specs.end(), ceiling.begin(), ceiling.end());
    const gm::CollisionWorld world = build_world(specs);
    gm::ControllerConfig config;
    gm::PlayerState state = gm::make_state({0.0, 0.0, 0.0}, 0.0, config);
    gm::Input jump;
    jump.jump = true;
    double max_z = state.pos.z;
    bool hit_ceiling = false;
    for (int i = 0; i < 60; ++i) {
        const gm::StepResult result = gm::step(state, i == 0 ? jump : gm::Input{}, world, config);
        CHECK(result.ok);
        hit_ceiling = hit_ceiling || result.hit_ceiling;
        state = result.state;
        max_z = std::max(max_z, state.pos.z);
    }
    // Head at z = 20, so the feet stop at 20 - height.
    CHECK(hit_ceiling);
    CHECK(max_z <= 20.0 - config.height + 1e-4);
    CHECK(max_z >= 20.0 - config.height - 0.05);
    CHECK_NEAR(state.pos.z, config.contact_skin, 1e-4);
    CHECK(state.on_ground);
}

void test_crouch_ceiling() {
    std::vector<TriSpec> specs = floor_quad(0.0);
    const std::vector<TriSpec> ceiling = ceiling_quad(8.0);
    specs.insert(specs.end(), ceiling.begin(), ceiling.end());
    const gm::CollisionWorld world = build_world(specs);
    gm::ControllerConfig config;
    gm::PlayerState crouched = gm::make_state({0.0, 0.0, 0.0}, 0.0, config);
    crouched.height = config.height * config.crouch_scale;
    crouched.eye_height = config.eye_height * config.crouch_scale;
    crouched.crouching = true;

    gm::Input still_crouched;
    still_crouched.crouch = true;
    const gm::StepResult held = gm::step(crouched, still_crouched, world, config);
    CHECK(held.ok);
    CHECK(held.state.crouching);
    CHECK_NEAR(held.state.height, crouched.height, 1e-12);

    // Releasing crouch under the 8-unit ceiling must not stand the capsule up.
    const gm::StepResult blocked = gm::step(held.state, gm::Input{}, world, config);
    CHECK(blocked.ok);
    CHECK(blocked.state.crouching);
    CHECK_NEAR(blocked.state.height, crouched.height, 1e-12);
    CHECK_NEAR(blocked.state.eye_height, crouched.eye_height, 1e-12);
    CHECK(blocked.state.on_ground);
}

void test_speed_limit() {
    const gm::CollisionWorld world = build_world(floor_quad(0.0));
    gm::ControllerConfig config;
    gm::PlayerState state = gm::make_state({0.0, 0.0, 0.0}, 0.0, config);
    gm::Input input;
    input.forward = 1.0;
    for (int i = 0; i < 60; ++i) {
        const gm::StepResult result = gm::step(state, input, world, config);
        CHECK(result.ok);
        state = result.state;
    }
    const double speed = std::sqrt(state.vel.x * state.vel.x + state.vel.y * state.vel.y);
    CHECK_NEAR(speed, config.max_speed, 1e-9);
    CHECK(state.vel.y > 0.0);
    CHECK(state.on_ground);

    // Input magnitudes above one are clamped, not amplified.
    gm::Input strong;
    strong.forward = 5.0;
    const gm::StepResult clamped = gm::step(state, strong, world, config);
    CHECK(clamped.ok);
    const double clamped_speed = std::sqrt(clamped.state.vel.x * clamped.state.vel.x +
                                           clamped.state.vel.y * clamped.state.vel.y);
    CHECK_NEAR(clamped_speed, config.max_speed, 1e-9);
}

void test_speed_unit_is_per_tick() {
    gm::ControllerConfig config;
    CHECK(config.dt == 1.0 / 91.0);
    CHECK_NEAR(config.max_speed, 1.70 * 91.0, 1e-12);
    CHECK_NEAR(config.max_speed * config.dt, 1.70, 1e-12);

    // 91 ticks = one second (0x44db80: 0x5b ticks -> +1 s).  With no friction
    // and instant acceleration the path is op4 * 91 = 154.7 world units.
    const gm::CollisionWorld world = build_world(floor_quad(0.0));
    gm::PlayerState state = gm::make_state({0.0, 0.0, 0.0}, 0.0, config);
    gm::Input input;
    input.forward = 1.0;
    config.accel = 1e9;
    config.friction = 0.0;
    const double y0 = state.pos.y;
    for (int i = 0; i < 91; ++i) {
        const gm::StepResult result = gm::step(state, input, world, config);
        CHECK(result.ok);
        state = result.state;
    }
    CHECK_NEAR(state.pos.y - y0, 154.7, 1.8);
}

void test_no_tunnelling() {
    std::vector<TriSpec> specs = floor_quad(0.0);
    const std::vector<TriSpec> wall = wall_x(0.0, -10.0, 10.0);
    specs.insert(specs.end(), wall.begin(), wall.end());
    const gm::CollisionWorld world = build_world(specs);
    gm::ControllerConfig config;
    config.max_speed = 20000.0; // keep the clamp out of this test
    gm::PlayerState state = gm::make_state({-100.0, 0.0, 0.0}, 0.0, config);
    state.vel = {10000.0, 0.0, 0.0}; // one tick covers 110 units (dt = 1/91)
    const gm::StepResult result = gm::step(state, gm::Input{}, world, config);
    CHECK(result.ok);
    CHECK(result.hit_wall);
    CHECK(result.substeps > 1);
    CHECK(result.state.pos.x <= -config.radius + 1e-3);
}

void test_determinism() {
    const gm::CollisionWorld world = build_world(step_specs());
    gm::ControllerConfig config;

    auto run = [&](std::vector<gm::PlayerState>& trace) {
        gm::PlayerState state = gm::make_state({0.0, 0.0, 0.0}, kPi / 4.0, config);
        for (int tick = 0; tick < 120; ++tick) {
            gm::Input input;
            input.forward = (tick < 80) ? 1.0 : -0.25;
            input.strafe = (tick > 40 && tick < 60) ? 0.5 : 0.0;
            input.jump = (tick == 12 || tick == 55);
            input.crouch = (tick > 70 && tick < 95);
            const gm::StepResult result = gm::step(state, input, world, config);
            CHECK(result.ok);
            state = result.state;
            trace.push_back(state);
        }
    };

    std::vector<gm::PlayerState> first;
    std::vector<gm::PlayerState> second;
    run(first);
    run(second);
    CHECK(first.size() == second.size());
    CHECK(first.size() == 120);
    bool identical = true;
    for (std::size_t i = 0; i < first.size(); ++i) {
        identical = identical && same_state(first[i], second[i]);
    }
    CHECK(identical);

    // Crouch actually changes the capsule and the eye, and standing up under
    // open sky restores the full height.
    gm::PlayerState crouched = gm::make_state({0.0, 0.0, 0.0}, 0.0, config);
    gm::Input crouch;
    crouch.crouch = true;
    const gm::StepResult down = gm::step(crouched, crouch, world, config);
    CHECK(down.ok);
    CHECK(down.state.crouching);
    CHECK_NEAR(down.state.height, config.height * config.crouch_scale, 1e-12);
    CHECK(down.state.eye_height < config.eye_height);
    const gm::StepResult up = gm::step(down.state, gm::Input{}, world, config);
    CHECK(up.ok);
    CHECK(!up.state.crouching);
    CHECK_NEAR(up.state.height, config.height, 1e-12);
}

void test_negatives() {
    const gm::CollisionWorld world = build_world(floor_quad(0.0));
    gm::ControllerConfig config;
    gm::PlayerState state = gm::make_state({0.0, 0.0, 0.0}, 0.0, config);
    const gm::Input input;

    auto expect_error = [&](const gm::ControllerConfig& cfg, const gm::PlayerState& st,
                            const gm::Input& in) {
        const gm::StepResult result = gm::step(st, in, world, cfg);
        CHECK(!result.ok);
        CHECK(!result.error.empty());
        CHECK(state_matches(result.state, st));
        CHECK(result.substeps == 0);
        CHECK(result.slides == 0);
    };

    gm::ControllerConfig cfg = config;
    cfg.dt = 0.0;
    expect_error(cfg, state, input);
    cfg.dt = -1.0 / 91.0;
    expect_error(cfg, state, input);
    cfg.dt = 1.0;
    expect_error(cfg, state, input);
    cfg.dt = std::numeric_limits<double>::infinity();
    expect_error(cfg, state, input);
    cfg.dt = std::numeric_limits<double>::quiet_NaN();
    expect_error(cfg, state, input);

    cfg = config;
    cfg.radius = 0.0;
    expect_error(cfg, state, input);
    cfg.radius = -3.0;
    expect_error(cfg, state, input);
    cfg = config;
    cfg.height = 0.0;
    expect_error(cfg, state, input);
    cfg.height = 5.0; // < 2 * radius
    expect_error(cfg, state, input);
    cfg = config;
    cfg.gravity = std::numeric_limits<double>::quiet_NaN();
    expect_error(cfg, state, input);
    cfg = config;
    cfg.max_speed = std::numeric_limits<double>::infinity();
    expect_error(cfg, state, input);
    cfg = config;
    cfg.max_slides = 0;
    expect_error(cfg, state, input);
    cfg = config;
    cfg.max_substeps = 0;
    expect_error(cfg, state, input);
    cfg = config;
    cfg.contact_skin = 0.0;
    expect_error(cfg, state, input);

    gm::PlayerState bad_state = state;
    bad_state.pos.z = std::numeric_limits<double>::quiet_NaN();
    expect_error(config, bad_state, input);
    bad_state = state;
    bad_state.vel.x = std::numeric_limits<double>::infinity();
    expect_error(config, bad_state, input);
    bad_state = state;
    bad_state.yaw = std::numeric_limits<double>::quiet_NaN();
    expect_error(config, bad_state, input);
    bad_state = state;
    bad_state.height = 0.0;
    expect_error(config, bad_state, input);

    gm::Input bad_input;
    bad_input.forward = std::numeric_limits<double>::quiet_NaN();
    expect_error(config, state, bad_input);
    bad_input = gm::Input{};
    bad_input.strafe = std::numeric_limits<double>::infinity();
    expect_error(config, state, bad_input);

    // A finite but very large dt is rejected as well (no silent long tick).
    gm::ControllerConfig wide = config;
    wide.max_dt = 100.0;
    wide.dt = 10.0;
    const gm::StepResult huge = gm::step(state, input, world, wide);
    CHECK(!huge.ok);
    CHECK(huge.error.find("contact_skin") == std::string::npos);
}

// --- level0 ----------------------------------------------------------------

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    std::vector<std::uint8_t> data;
    std::uint8_t buffer[65536];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        data.insert(data.end(), buffer, buffer + n);
    }
    std::fclose(file);
    return data;
}

void test_level0() {
    const fs::path path = kGame / "level0.sbw";
    if (!fs::exists(path)) {
        std::printf("player_controller: level0.sbw not installed, real-level tests skipped\n");
        return;
    }
    const std::vector<std::uint8_t> raw = read_file(path);
    CHECK(!raw.empty());
    std::string error;
    const auto level = gm::parse_sbw(raw, error);
    CHECK(level.has_value());
    if (!level) {
        std::fprintf(stderr, "player_controller: parse_sbw: %s\n", error.c_str());
        return;
    }
    gm::CollisionWorld world;
    error.clear();
    CHECK(world.build(*level, &error));
    CHECK(error.empty());

    gm::ControllerConfig config;
    // Proven spawn point SET_PLAYER_START[0] = (24, -208, 48), yaw 0
    // (docs/PLAYER.md §8); the capsule starts 0.546875 below the floor.
    gm::PlayerState state = gm::make_state({24.0, -208.0, 48.0}, 0.0, config);
    const gm::StepResult settle = gm::step(state, gm::Input{}, world, config);
    CHECK(settle.ok);
    CHECK(settle.state.on_ground);
    CHECK_NEAR(settle.state.pos.z, 48.546875 + config.contact_skin, 1e-4);
    CHECK_NEAR(settle.state.pos.x, 24.0, 1e-9);
    CHECK_NEAR(settle.state.pos.y, -208.0, 1e-9);

    // 100 ticks straight into the +X wall, yaw = +90 degrees.
    state = settle.state;
    state.yaw = kPi / 2.0;
    gm::Input input;
    input.forward = 1.0;
    bool wall = false;
    for (int i = 0; i < 100; ++i) {
        const gm::StepResult result = gm::step(state, input, world, config);
        CHECK(result.ok);
        wall = wall || result.hit_wall;
        state = result.state;
    }
    // Golden after settle + 100 ticks of forward at yaw +90 deg, 91 Hz
    // (implementation-pinned, full precision):
    //   pos = (91.671874088928945, -207.99999999999997, 48.546875999999997)
    //   on_ground = 1
    // x is 94.671875 (the +X wall from docs/COLLIDE.md) - radius - skin.
    std::printf("player_controller: level0 spawn -> (%.17g, %.17g, %.17g) on_ground=%d\n",
                state.pos.x, state.pos.y, state.pos.z, state.on_ground ? 1 : 0);
    CHECK(wall);
    CHECK(state.on_ground);
    CHECK_NEAR(state.pos.x, 91.671874088928945, 1e-6);
    CHECK_NEAR(state.pos.y, -208.0, 1e-6);
    CHECK_NEAR(state.pos.z, 48.546875 + config.contact_skin, 1e-6);
    CHECK_NEAR(gm::eye_position(state).z, 58.546876, 1e-6);
}

} // namespace

int main() {
    test_fall();
    test_land_and_jump();
    test_wall_stop_and_slide();
    test_corner();
    test_step_up();
    test_step_down();
    test_crouch_ceiling();
    test_ceiling();
    test_speed_limit();
    test_speed_unit_is_per_tick();
    test_no_tunnelling();
    test_determinism();
    test_negatives();
    test_level0();
    if (g_failures != 0) {
        std::fprintf(stderr, "player_controller: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("player_controller: ok\n");
    return 0;
}
