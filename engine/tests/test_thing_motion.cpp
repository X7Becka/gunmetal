// Thing movement automata (docs/THING_FIELDS.md §4a, docs/AI_CPP.md §7).
//
// Synthetic part: one `ThingMotionState` per proven mode, stepped against a
// hand-built `.sbw` (floor, ceiling and two walls) so the probes, the swept
// move, the floor stick and the impulse bounce are exercised:
//
//   * spin (type 1): angles += angle_rate*dt, pitch/roll targets;
//   * avoid (type 2): three probes at 2r, left/right choice, `speed*dt+r` gate;
//   * avoid-left/right (3/4): 1.25r gate and a fixed turn direction;
//   * turret (5): accel/clamp/no-overshoot, the `+0x228/+0x238` limits and the
//     fact that the mode never moves the body;
//   * ram (13) and seek 14/15/17: 1.25r gate, target yaw rate, vertical seek;
//   * patrol (16): the `p0 <= d <= p1` firing band, the 14-degree cone and the
//     `p2` cooldown;
//   * projectile (6): `vz += 0.2*g*level_g*dt`, `z -= vz*dt`, bounce damping;
//   * the shared tick part: floor stick, impulse bounce (`+0x208/+0x210`) and
//     the dormant `+0x1e8` thruster;
//   * negatives (bad dt, expired, unknown type) and two-run determinism.
//
// Real part: level0.sbt and dm1.sbt from the shipped game.  The movement-type
// histogram, the Grenade/ROCKET flag words (`+0x1a8/1ac/1b0`, docs
// THING_FIELDS §8), and a 91-tick smoke run over every spawned instance
// (`Things::load` + `thing_motion_config`) that must stay finite, in range and
// bit-identical on a second run.  This is the "no regression over the real
// .sbt types" pass; the corpus counts themselves stay in CTest `things_full`.
#include "world/thing_motion.h"

#include "camera/camera.h"
#include "res/sbt.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "world/things.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                         #a, #b, static_cast<long long>(va), static_cast<long long>(vb));     \
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
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kLean = 0.08726646259971647;

// --- synthetic .sbw fixtures (same shape as test_collide.cpp) ---------------

struct TriSpec {
    gm::Vec3d a;
    gm::Vec3d b;
    gm::Vec3d c;
};

void put_f32(std::uint8_t* p, float value) { std::memcpy(p, &value, sizeof(value)); }

/// The 96-byte object header is 12 x f64 (origin + basis), not 12 f32 pairs:
/// see test_sbw_transform and docs/WORLD_FIELDS.md §3.
void put_f64(std::uint8_t* p, double value) { std::memcpy(p, &value, sizeof(value)); }

void write_identity_header(std::array<std::uint8_t, 96>& header) {
    header.fill(0);
    for (int point = 0; point < 4; ++point) {
        for (int component = 0; component < 3; ++component) {
            const double value =
                (point == 0) ? 0.0 : (component == point - 1 ? 1.0 : 0.0);
            put_f64(header.data() + 8 * (point * 3 + component), value);
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
        geometry.vertices = {gm::Vec3f{static_cast<float>(spec.a.x), static_cast<float>(spec.a.y),
                                       static_cast<float>(spec.a.z)},
                             gm::Vec3f{static_cast<float>(spec.b.x), static_cast<float>(spec.b.y),
                                       static_cast<float>(spec.b.z)},
                             gm::Vec3f{static_cast<float>(spec.c.x), static_cast<float>(spec.c.y),
                                       static_cast<float>(spec.c.z)}};
        gm::SbwPoly poly;
        poly.indices = {0, 1, 2};
        geometry.polys.push_back(poly);
        geometry.n_polys = 1;
        level.geometry.push_back(geometry);
    }
    level.header.object_count = static_cast<std::uint32_t>(specs.size());
    return level;
}

void add_quad(std::vector<TriSpec>& out, gm::Vec3d p0, gm::Vec3d p1, gm::Vec3d p2,
              gm::Vec3d p3) {
    out.push_back({p0, p1, p2});
    out.push_back({p0, p2, p3});
}

std::vector<TriSpec> floor_specs(double z = 0.0) {
    std::vector<TriSpec> out;
    const double s = 400.0;
    add_quad(out, {-s, -s, z}, {s, -s, z}, {s, s, z}, {-s, s, z});
    return out;
}

/// A vertical wall plane `x = value` (or `y = value` when `axis_y`), spanning
/// +/-400 in the other horizontal axis and 0..400 in z.
std::vector<TriSpec> wall_specs(double value, bool axis_y = false) {
    std::vector<TriSpec> out;
    const double s = 400.0;
    if (axis_y) {
        add_quad(out, {-s, value, 0.0}, {s, value, 0.0}, {s, value, 400.0}, {-s, value, 400.0});
    } else {
        add_quad(out, {value, -s, 0.0}, {value, s, 0.0}, {value, s, 400.0}, {value, -s, 400.0});
    }
    return out;
}

std::vector<TriSpec> ceiling_specs(double z) {
    std::vector<TriSpec> out;
    const double s = 400.0;
    add_quad(out, {-s, -s, z}, {-s, s, z}, {s, s, z}, {s, -s, z});
    return out;
}

gm::CollisionWorld build_world(const std::vector<TriSpec>& specs) {
    const gm::SbwLevel level = make_level(specs);
    gm::CollisionWorld world;
    std::string error;
    if (!world.build(level, &error)) {
        std::fprintf(stderr, "thing_motion: world build: %s\n", error.c_str());
    }
    return world;
}

// --- helpers ---------------------------------------------------------------

gm::ThingMotionConfig config_for(int movement_type, double speed = 0.0,
                                 double radius = 0.0) {
    gm::ThingMotionConfig config;
    config.movement_type = movement_type;
    config.speed = speed;
    config.radius = radius;
    return config;
}

/// One `thing_motion_step` with the tick input spelled out.
gm::ThingMotionStep step_once(gm::ThingMotionState& state,
                              const gm::ThingMotionConfig& config,
                              const gm::CollisionWorld* world,
                              const gm::ThingMotionTarget& target = {},
                              double dt = 1.0) {
    gm::ThingMotionInput input;
    input.dt = dt;
    input.world = world;
    input.target = target;
    return gm::thing_motion_step(state, config, input);
}

gm::ThingMotionTarget target_at(const gm::ThingMotionState& state, gm::Vec3d position,
                                bool valid = true) {
    gm::ThingMotionTarget target;
    target.position = position;
    target.valid = valid;
    const gm::Vec3d delta = position - state.position;
    // 0x473a00/-0x41e2b8 azimuth: forward is (sin(yaw), cos(yaw)).
    const double azimuth = std::atan2(delta.x, delta.y);
    double error = azimuth - state.yaw_target;
    while (error > kTwoPi * 0.5) {
        error -= kTwoPi;
    }
    while (error < -kTwoPi * 0.5) {
        error += kTwoPi;
    }
    target.yaw_error = error;
    target.pitch_error = 0.0;
    return target;
}

// --- tests -----------------------------------------------------------------

void test_mode_table() {
    const auto& table = gm::thing_move_table();
    CHECK_EQ(table.size(), 19u);
    for (int type = 0; type < 19; ++type) {
        const gm::ThingMoveInfo& info = gm::thing_move_info(type);
        CHECK_EQ(info.type, type);
        CHECK_EQ(static_cast<int>(info.move), type);
        CHECK(!info.name.empty());
        CHECK(!info.handler.empty());
        CHECK(info.status == "proven" || info.status == "external" ||
              info.status == "stub" || info.status == "model");
    }
    // The five handlers this task pins down.
    CHECK(gm::thing_move_info(2).handler == "0x474260");
    CHECK(gm::thing_move_info(13).handler == "0x474a10");
    CHECK(gm::thing_move_info(5).handler == "0x474ea0");
    CHECK(gm::thing_move_info(7).handler == "0x475930(...,0)");
    CHECK(gm::thing_move_info(8).handler == "0x475930(...,1)");
    CHECK(gm::thing_move_info(16).handler == "0x476070");
    CHECK_EQ(gm::thing_move_info(-1).type, 255); // out of range
    CHECK_EQ(gm::thing_move_info(19).type, 255);
}

void test_basis_agrees_with_camera() {
    for (double pitch : {0.0, 0.3, -0.7}) {
        for (double roll : {0.0, 0.2, -1.1}) {
            for (double yaw : {0.0, 1.0, 3.0, 6.0}) {
                const auto rows = gm::thing_basis(roll, pitch, yaw);
                const gm::CameraBasis camera = gm::camera_basis(roll, pitch, yaw);
                CHECK_NEAR(rows[0].x, camera.right.x, 1e-15);
                CHECK_NEAR(rows[0].y, camera.right.y, 1e-15);
                CHECK_NEAR(rows[0].z, camera.right.z, 1e-15);
                CHECK_NEAR(rows[1].x, camera.forward.x, 1e-15);
                CHECK_NEAR(rows[1].y, camera.forward.y, 1e-15);
                CHECK_NEAR(rows[1].z, camera.forward.z, 1e-15);
                CHECK_NEAR(rows[2].x, camera.up.x, 1e-15);
                CHECK_NEAR(rows[2].z, camera.up.z, 1e-15);
            }
        }
    }
    const auto identity = gm::thing_basis(0.0, 0.0, 0.0);
    CHECK_NEAR(identity[0].x, 1.0, 0.0);
    CHECK_NEAR(identity[1].y, 1.0, 0.0);
    CHECK_NEAR(identity[2].z, 1.0, 0.0);
    // yaw = +pi/2 turns the forward axis to +X (the engine's yaw convention).
    const auto quarter = gm::thing_basis(0.0, 0.0, kTwoPi / 4.0);
    CHECK_NEAR(quarter[1].x, 1.0, 1e-15);
    CHECK_NEAR(quarter[1].y, 0.0, 1e-15);
}

void test_spawn_copy() {
    gm::ThingMotionConfig config = config_for(1, 3.0, 2.0);
    config.angle_rate = {0.1, 0.2, 0.3};
    const gm::ThingMotionState state = gm::thing_motion_spawn(config);
    CHECK_NEAR(state.speed, 3.0, 0.0);
    CHECK_NEAR(state.pitch_rate, 0.1, 0.0);
    CHECK_NEAR(state.roll_rate, 0.2, 0.0);
    CHECK_NEAR(state.yaw_rate, 0.3, 0.0);
    CHECK_NEAR(state.impulse_damping, 0.05, 0.0); // 0x3fa99999 9999999a
}

void test_spin() {
    gm::ThingMotionConfig config = config_for(1, 0.0, 1.0);
    config.angle_rate = {0.1, 0.2, 0.3};
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    gm::ThingMotionStep out;
    for (int i = 0; i < 10; ++i) {
        out = step_once(state, config, nullptr);
        CHECK(out.supported);
    }
    // case 1: pitch/roll/yaw targets advance by rate*dt, then snap.
    CHECK_NEAR(state.pitch, 1.0, 1e-12);
    CHECK_NEAR(state.roll, 2.0, 1e-12);
    CHECK_NEAR(state.yaw, 3.0, 1e-12);
    CHECK_NEAR(state.position.x, 0.0, 0.0); // spin never moves the body
    CHECK(!out.moved);

    // The yaw target is wrapped with ABS(fmod(x, 2*pi)) (0x470c70), so a spin
    // that passes -pi comes back as a positive angle.
    gm::ThingMotionConfig negative = config_for(1);
    negative.angle_rate = {0.0, 0.0, -2.0};
    gm::ThingMotionState turn = gm::thing_motion_spawn(negative);
    step_once(turn, negative, nullptr);
    CHECK_NEAR(turn.yaw_target, -2.0, 1e-15);
    CHECK_NEAR(turn.yaw, 2.0, 1e-15);
}

void test_avoid_open() {
    gm::ThingMotionConfig config = config_for(2, 4.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 0.5;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    gm::ThingMotionStep out = step_once(state, config, nullptr);
    CHECK(out.supported);
    CHECK(out.moved);
    CHECK_EQ(out.turn, 0);
    CHECK_NEAR(out.displacement.x, 0.0, 1e-12);
    CHECK_NEAR(out.displacement.y, 4.0, 1e-12);
    CHECK_NEAR(state.position.y, 4.0, 1e-12);
    CHECK_NEAR(state.yaw_rate, 0.0, 0.0); // no obstacle, no turn

    // yaw = 90 degrees walks along +X.
    gm::ThingMotionState turned = gm::thing_motion_spawn(config);
    turned.yaw = kTwoPi / 4.0;
    turned.yaw_target = turned.yaw;
    out = step_once(turned, config, nullptr);
    CHECK_NEAR(out.displacement.x, 4.0, 1e-12);
    CHECK_NEAR(out.displacement.y, 0.0, 1e-12);
}

void test_avoid_wall_turns_left() {
    // Wall at y = 50, body at the origin with radius 10: the forward probe
    // reports 50-10 = 40 free units, so `ahead < left` fires and the two side
    // probes decide the direction (left is free, right is capped at 300).
    const gm::CollisionWorld world = build_world(wall_specs(50.0, true));
    gm::ThingMotionConfig config = config_for(2, 4.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 0.5;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    const gm::ThingMotionStep out = step_once(state, config, &world);
    CHECK_EQ(out.turn, 2);
    CHECK_NEAR(state.yaw_rate, -0.075, 1e-15); // -0.1 then *0.75
    // 0x470c70 wraps the target with ABS(fmod(...)): -0.1 becomes +0.1.
    CHECK_NEAR(state.yaw, 0.1, 1e-12);
    CHECK(out.moved); // 4+10 < 40
    CHECK_NEAR(out.displacement.y, 4.0, 1e-12);
    CHECK_NEAR(state.position.y, 4.0, 1e-12);
}

void test_avoid_wall_turns_right() {
    // Wall ahead at y = 50 and a nearer wall on the left at x = -30: the left
    // probe (20) is smaller than the right one (300), so the turn is 3.
    std::vector<TriSpec> specs = wall_specs(50.0, true);
    const auto left = wall_specs(-30.0, false);
    specs.insert(specs.end(), left.begin(), left.end());
    const gm::CollisionWorld world = build_world(specs);
    gm::ThingMotionConfig config = config_for(2, 4.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 0.5;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    const gm::ThingMotionStep out = step_once(state, config, &world);
    CHECK_EQ(out.turn, 3);
    CHECK_NEAR(state.yaw_rate, 0.075, 1e-15);
    CHECK_NEAR(state.yaw, 0.1, 1e-12);
}

void test_avoid_blocked() {
    // A wall 12 units ahead with radius 10: 4+10 > 12 -> the `speed*dt+radius`
    // gate keeps the body still, but the turn still happens.
    const gm::CollisionWorld world = build_world(wall_specs(12.0, true));
    gm::ThingMotionConfig config = config_for(2, 4.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 0.5;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    const gm::ThingMotionStep out = step_once(state, config, &world);
    CHECK(!out.moved);
    CHECK(!out.displacement.y);
    CHECK_EQ(out.turn, 2);
    CHECK_NEAR(state.position.y, 0.0, 0.0);
}

void test_avoid_lean() {
    const gm::CollisionWorld world = build_world(wall_specs(50.0, true));
    gm::ThingMotionConfig config = config_for(2, 4.0, 10.0);
    config.turn_accel = 0.2;
    config.turn_max = 1.0;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    step_once(state, config, &world);
    // Without flags_b +0x40000000 there is no lean, and roll stays zeroed.
    CHECK_NEAR(state.roll_target, 0.0, 0.0);
    CHECK_NEAR(state.yaw_rate, -0.15, 1e-15); // -0.2 then *0.75

    config.flags_b = 0x40000000u;
    gm::ThingMotionState leaning = gm::thing_motion_spawn(config);
    step_once(leaning, config, &world);
    CHECK_NEAR(leaning.roll_target, -kLean, 1e-15);
    // type 2 zeroes the roll target in the tick preamble, so the roll itself
    // only moves when roll_rate is non-zero.
    CHECK_NEAR(leaning.roll, 0.0, 0.0);

    // type 1 keeps the roll and lets the target through.
    gm::ThingMotionConfig spin = config_for(1, 0.0, 1.0);
    spin.flags_b = 0x40000000u;
    gm::ThingMotionState spinner = gm::thing_motion_spawn(spin);
    spinner.roll_rate = 0.5;
    spin.angle_rate = {0.0, 0.0, 0.0};
    step_once(spinner, spin, nullptr);
    CHECK_NEAR(spinner.roll, 0.5, 1e-15); // keeps_roll passes the rate through
}

void test_avoid34() {
    // 0x4746b0: no side probes, a fixed turn direction per type.  The wall at
    // y = 20 leaves 20 - 10 = 10 free units, below 1.25*radius = 12.5.
    const gm::CollisionWorld world = build_world(wall_specs(20.0, true));
    for (int type : {3, 4}) {
        gm::ThingMotionConfig config = config_for(type, 4.0, 10.0);
        config.turn_accel = 0.1;
        config.turn_max = 0.5;
        gm::ThingMotionState state = gm::thing_motion_spawn(config);
        const gm::ThingMotionStep out = step_once(state, config, &world);
        CHECK(out.supported);
        CHECK_EQ(out.turn, type == 3 ? 2 : 3); // 3 - (param_4 != 0)
        CHECK(!out.moved); // 1.25*radius = 12.5 > 10
    }
    // 40 free units: 1.25*radius < 40 -> walk straight ahead.
    const gm::CollisionWorld open = build_world(wall_specs(50.0, true));
    {
        const gm::ThingMotionConfig config = config_for(3, 4.0, 10.0);
        gm::ThingMotionState state = gm::thing_motion_spawn(config);
        const gm::ThingMotionStep out = step_once(state, config, &open);
        CHECK_EQ(out.turn, 0);
        CHECK(out.moved);
        CHECK_NEAR(out.displacement.y, 4.0, 1e-12);
    }
    // Blocked at 12 units: 1.25*10 = 12.5 > 12 -> no step, but the turn runs.
    const gm::CollisionWorld close = build_world(wall_specs(12.0, true));
    gm::ThingMotionConfig config = config_for(3, 4.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 0.5;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    const gm::ThingMotionStep out = step_once(state, config, &close);
    CHECK(!out.moved);
    CHECK_EQ(out.turn, 2); // type 3 turns with the negative sign
}

void test_turret() {
    gm::ThingMotionConfig config = config_for(5, 0.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 0.5;
    config.damping = 0.8;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    gm::ThingMotionTarget target;
    target.valid = true;
    for (int i = 0; i < 60; ++i) {
        target = target_at(state, {std::sin(1.0), std::cos(1.0), 0.0});
        step_once(state, config, nullptr, target);
    }
    // The yaw converges on the target without overshooting (the 0x474ea0
    // `local_18 <= dVar2` clamp) and never moves the body.
    CHECK_NEAR(state.yaw, 1.0, 1e-6);
    CHECK_NEAR(state.position.x, 0.0, 0.0);
    CHECK(!step_once(state, config, nullptr, target).moved);

    // The rate is clamped by turn_max, and 0.25*turn_max when flags_b has
    // 0x00800000.
    gm::ThingMotionConfig clamped = config_for(5, 0.0, 10.0);
    clamped.turn_accel = 5.0;
    clamped.turn_max = 2.0;
    clamped.flags_b = 0x00800000u;
    gm::ThingMotionState fast = gm::thing_motion_spawn(clamped);
    gm::ThingMotionTarget far_target;
    far_target.valid = true;
    far_target.yaw_error = 3.0;
    step_once(fast, clamped, nullptr, far_target);
    CHECK_NEAR(fast.yaw_rate, 0.5 * 0.75, 1e-15); // 0.25*2, then damped
    CHECK_NEAR(fast.yaw_target, 0.5, 1e-15);

    // Angle limits (+0x228/+0x230 pitch, +0x238/+0x240 yaw) clamp the target
    // and zero the rate.
    gm::ThingMotionConfig limited = config_for(5, 0.0, 10.0);
    limited.turn_accel = 0.1;
    limited.turn_max = 1.0;
    limited.yaw_limit_lo = -0.2;
    limited.yaw_limit_hi = 0.2;
    gm::ThingMotionState held = gm::thing_motion_spawn(limited);
    gm::ThingMotionTarget right;
    right.valid = true;
    right.yaw_error = 1.0;
    for (int i = 0; i < 20; ++i) {
        step_once(held, limited, nullptr, right);
    }
    CHECK_NEAR(held.yaw_target, 0.2, 1e-15);
    CHECK_NEAR(held.yaw_rate, 0.0, 0.0);
}

void test_ram() {
    // 0x474a10: 1.25r clearance, x/y step only.
    const gm::CollisionWorld world = build_world(wall_specs(50.0, true));
    gm::ThingMotionConfig config = config_for(13, 4.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 0.5;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    gm::ThingMotionStep out = step_once(state, config, &world);
    CHECK(out.supported);
    CHECK(out.moved);
    CHECK_NEAR(out.displacement.y, 4.0, 1e-12);
    CHECK_EQ(out.turn, 0); // 12.5 <= 40, no target -> straight

    // A target to the right makes it turn and hold position (0x474a10 zeroes
    // the ray distance after the side choice).
    gm::ThingMotionState with_target = gm::thing_motion_spawn(config);
    const gm::ThingMotionTarget right = target_at(with_target, {40.0, 30.0, 0.0});
    out = step_once(with_target, config, &world, right);
    CHECK_EQ(out.turn, 3);
    CHECK(!out.moved);

    // Blocked (12 < 12.5) and not turning yet: pick the left side.
    const gm::CollisionWorld close = build_world(wall_specs(12.0, true));
    gm::ThingMotionState near = gm::thing_motion_spawn(config);
    out = step_once(near, config, &close);
    CHECK(!out.moved);
    CHECK_EQ(out.turn, 2);
    CHECK_NEAR(near.yaw_rate, -0.075, 1e-15);
}

void test_seek_avoid() {
    gm::ThingMotionConfig config = config_for(14, 4.0, 10.0);
    config.turn_accel = 0.1;
    config.turn_max = 1.0;
    config.climb_speed = 3.0;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    gm::ThingMotionStep out = step_once(state, config, nullptr,
                                        target_at(state, {30.0, 30.0, 0.0}));
    CHECK(out.supported);
    CHECK_EQ(out.turn, 3); // the target is to the right
    CHECK_NEAR(state.yaw_rate, 0.1 * 0.75, 1e-15);
    CHECK_NEAR(state.yaw_target, 0.1, 1e-15);
    CHECK(out.moved); // free world -> speed*dt along the forward axis

    // No target: the three-probe avoid body runs instead.
    gm::ThingMotionState free_state = gm::thing_motion_spawn(config);
    out = step_once(free_state, config, nullptr);
    CHECK_EQ(out.turn, 0);
    CHECK(out.moved);

    // Vertical seek: gravity-free and |error| >= 20 (0x473240).
    gm::ThingMotionConfig climbing = config_for(14, 0.0, 10.0);
    climbing.climb_speed = 3.0;
    gm::ThingMotionState riser = gm::thing_motion_spawn(climbing);
    gm::ThingMotionTarget above;
    above.valid = true;
    above.vertical_error = 30.0; // bigger than the 3-unit climb step
    out = step_once(riser, climbing, nullptr, above);
    CHECK_NEAR(out.displacement.z, 3.0, 1e-12);
    // A step larger than the error adds the raw error (the original's units
    // mismatch, 0x473240 keeps it).
    gm::ThingMotionState short_hop = gm::thing_motion_spawn(climbing);
    gm::ThingMotionTarget near_error;
    near_error.valid = true;
    near_error.vertical_error = -2.0; // below the gate -> nothing
    out = step_once(short_hop, climbing, nullptr, near_error);
    CHECK_NEAR(out.displacement.z, 0.0, 1e-12);
    // Below 20 nothing happens, and gravity-free only.
    gm::ThingMotionState flat = gm::thing_motion_spawn(climbing);
    gm::ThingMotionTarget shallow;
    shallow.valid = true;
    shallow.vertical_error = 10.0;
    out = step_once(flat, climbing, nullptr, shallow);
    CHECK_NEAR(out.displacement.z, 0.0, 1e-12);
    gm::ThingMotionConfig heavy = climbing;
    heavy.gravity = 1.0;
    gm::ThingMotionState falling = gm::thing_motion_spawn(heavy);
    out = step_once(falling, heavy, nullptr, above);
    CHECK_NEAR(out.displacement.z, 0.0, 1e-12);
}

void test_patrol_fire_gate() {
    gm::ThingMotionConfig config = config_for(16, 0.0, 10.0);
    config.turn_accel = 0.05;
    config.turn_max = 0.4;
    config.weapons.resize(2);
    config.weapons[0] = gm::ProximityEntry{0, 0, 0, 0, {}}; // inactive slot
    config.weapons[1].event = 1001;
    config.weapons[1].low = 10;
    config.weapons[1].high = 100;
    config.weapons[1].cooldown = 7;

    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    const gm::ThingMotionTarget in_band = target_at(state, {0.0, 50.0, 0.0});
    gm::ThingMotionStep out = step_once(state, config, nullptr, in_band);
    CHECK(out.supported);
    CHECK_EQ(out.fired_weapon, 1);
    CHECK_EQ(out.fired_event, 1001u);
    CHECK_EQ(state.attack_cooldown, 7);
    CHECK_NEAR(state.position.y, 0.0, 0.0); // the patrol mode never moves

    // Cooldown: 6 ticks left -> the next shot is refused.
    out = step_once(state, config, nullptr, in_band);
    CHECK_EQ(out.fired_weapon, -1);
    CHECK_EQ(state.attack_cooldown, 6);

    // Out of band: nothing fires even with the cooldown clear.
    gm::ThingMotionState far_state = gm::thing_motion_spawn(config);
    const gm::ThingMotionTarget too_far = target_at(far_state, {0.0, 200.0, 0.0});
    out = step_once(far_state, config, nullptr, too_far);
    CHECK_EQ(out.fired_weapon, -1);

    // The 14-degree cone: |lateral|*4 < forward.
    gm::ThingMotionState cone = gm::thing_motion_spawn(config);
    const gm::ThingMotionTarget wide = target_at(cone, {50.0, 50.0, 0.0});
    out = step_once(cone, config, nullptr, wide);
    CHECK_EQ(out.fired_weapon, -1);
    const gm::ThingMotionTarget edge = target_at(cone, {12.0, 50.0, 0.0});
    out = step_once(cone, config, nullptr, edge);
    CHECK_EQ(out.fired_weapon, 1); // 12*4 = 48 < 50

    // Behind the gun: never fires.
    gm::ThingMotionState back = gm::thing_motion_spawn(config);
    const gm::ThingMotionTarget behind = target_at(back, {0.0, -50.0, 0.0});
    out = step_once(back, config, nullptr, behind);
    CHECK_EQ(out.fired_weapon, -1);
}

void test_projectile() {
    gm::ThingMotionConfig config = config_for(6, 2.0, 1.0);
    config.gravity = 1.0;
    config.damping = 0.5;
    config.level_gravity = 9.8;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    gm::ThingMotionStep out = step_once(state, config, nullptr);
    CHECK(out.supported);
    CHECK_NEAR(state.vertical_speed, 0.2 * 9.8, 1e-12);
    CHECK_NEAR(out.displacement.y, 2.0, 1e-12);
    CHECK_NEAR(out.displacement.z, -(0.2 * 9.8), 1e-12);
    out = step_once(state, config, nullptr);
    CHECK_NEAR(state.vertical_speed, 2.0 * 0.2 * 9.8, 1e-12);

    // Bounce: the floor stops the fall, `speed *= damping`.
    const gm::CollisionWorld world = build_world(floor_specs(0.0));
    gm::ThingMotionState falling = gm::thing_motion_spawn(config);
    falling.position = {0.0, 0.0, 5.0};
    for (int i = 0; i < 40; ++i) {
        out = step_once(falling, config, &world);
        if (out.blocked) {
            break;
        }
    }
    CHECK(out.blocked);
    CHECK(out.bounced);
    CHECK_NEAR(falling.vertical_speed, 0.0, 0.0);
    CHECK_NEAR(falling.speed, 1.0, 1e-12); // 2.0 * 0.5
}

void test_floor_and_ceiling() {
    std::vector<TriSpec> specs = floor_specs(0.0);
    const auto ceiling = ceiling_specs(100.0);
    specs.insert(specs.end(), ceiling.begin(), ceiling.end());
    const gm::CollisionWorld world = build_world(specs);

    // A downward impulse is clamped by the floor: vertical speed is zeroed and
    // the step reports the contact (0x472120's `param_3[2] != param_4[2]`).
    gm::ThingMotionConfig config = config_for(0, 0.0, 1.0);
    config.flags_b = 0x10000000u;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    state.position = {0.0, 0.0, 2.0};
    state.vertical_speed = -9.0;
    state.impulse_up = -5.0;
    gm::ThingMotionStep out = step_once(state, config, &world);
    CHECK(out.blocked);
    CHECK(out.grounded);
    CHECK_NEAR(state.vertical_speed, 0.0, 0.0);
    CHECK_NEAR(state.position.z, 1.0, 1e-9); // radius 1 -> resting on z = 0
    CHECK_NEAR(out.displacement.z, -1.0, 1e-9);

    // The vertical impulse is reflected (0x472120, flags_b +0x10000000).
    CHECK_NEAR(state.impulse_up, 5.0, 1e-12);
    CHECK_NEAR(state.impulse_damping_up, 0.025, 1e-15);

    // Upward into the ceiling: same rule, the sky is the limit.
    gm::ThingMotionState up = gm::thing_motion_spawn(config);
    up.position = {0.0, 0.0, 98.0};
    up.impulse_up = 5.0;
    out = step_once(up, config, &world);
    CHECK(out.blocked);
    CHECK(out.grounded);
    CHECK_NEAR(up.position.z, 99.0, 1e-9);
}

void test_impulse_bounce() {
    const gm::CollisionWorld world = build_world(wall_specs(2.0, false)); // x = 2
    gm::ThingMotionConfig config = config_for(0, 0.0, 1.0);
    config.flags_b = 0x08000000u;
    gm::ThingMotionState state = gm::thing_motion_spawn(config);
    state.impulse_side = 3.0; // along `right` = +X at yaw 0
    gm::ThingMotionStep out = step_once(state, config, &world);
    CHECK(out.supported);
    CHECK(out.blocked);
    CHECK(out.bounced);
    CHECK_NEAR(out.displacement.x, 1.0, 1e-9);
    CHECK_NEAR(state.impulse_side, -3.0, 1e-12);
    CHECK_NEAR(state.impulse_damping, 0.025, 1e-15);

    // Without the flag the impulse is cleared before it can move anything.
    gm::ThingMotionConfig plain = config_for(0, 0.0, 1.0);
    gm::ThingMotionState cleared = gm::thing_motion_spawn(plain);
    cleared.impulse_side = 3.0;
    out = step_once(cleared, plain, nullptr);
    CHECK_NEAR(out.displacement.x, 3.0, 1e-12); // the tick still adds it
    CHECK_NEAR(cleared.impulse_side, 0.0, 0.0); // then 0x472120 clears it

    // A blocked step with no impulse arms the 2.5 side bias.
    gm::ThingMotionConfig armed = config_for(0, 0.0, 1.0);
    armed.flags_b = 0x08000000u;
    gm::ThingMotionState bias = gm::thing_motion_spawn(armed);
    bias.position = {1.0, 0.0, 0.0};
    bias.impulse_side = 3.0;
    out = step_once(bias, armed, &world);
    CHECK(out.blocked);
    // The impulse was already live, so it is reflected, not re-armed.
    CHECK_NEAR(bias.impulse_side, -3.0, 1e-12);
}

void test_thruster() {
    double speed = 0.0;
    int timer = 3;
    gm::thing_thrust_step(speed, 2.0, 5.0, timer, 1.0);
    CHECK_NEAR(speed, 2.0, 0.0);
    CHECK_EQ(timer, 2);
    gm::thing_thrust_step(speed, 2.0, 5.0, timer, 1.0);
    CHECK_NEAR(speed, 4.0, 0.0);
    gm::thing_thrust_step(speed, 2.0, 5.0, timer, 1.0);
    CHECK_NEAR(speed, 5.0, 0.0); // clamped by +0x1e0
    CHECK_EQ(timer, 0);
    gm::thing_thrust_step(speed, 2.0, 5.0, timer, 1.0);
    CHECK_NEAR(speed, 3.0, 0.0); // decays to 0 at `accel` per tick
    gm::thing_thrust_step(speed, 2.0, 5.0, timer, 1.0);
    CHECK_NEAR(speed, 1.0, 0.0);
    gm::thing_thrust_step(speed, 2.0, 5.0, timer, 1.0);
    CHECK_NEAR(speed, 0.0, 0.0);

    // Negative timer: reverse thrust, clamped by -max, then decays.
    double reverse = 0.0;
    int negative = -3;
    for (int i = 0; i < 3; ++i) {
        gm::thing_thrust_step(reverse, 2.0, 5.0, negative, 1.0);
    }
    CHECK_NEAR(reverse, -5.0, 0.0);
    CHECK_EQ(negative, 0);
    gm::thing_thrust_step(reverse, 2.0, 5.0, negative, 1.0);
    gm::thing_thrust_step(reverse, 2.0, 5.0, negative, 1.0);
    CHECK_NEAR(reverse, -1.0, 0.0);

    // A zero-accel command is a no-op (the retail build never fills the
    // accumulator, so this is the shape that actually runs: nothing).
    double still = 0.0;
    int still_timer = 4;
    gm::thing_thrust_step(still, 0.0, 1.0, still_timer, 1.0);
    CHECK_NEAR(still, 0.0, 0.0);
    CHECK_EQ(still_timer, 4);
}

void test_negatives() {
    gm::ThingMotionConfig config = config_for(2, 4.0, 10.0);
    gm::ThingMotionState state = gm::thing_motion_spawn(config);

    // Non-positive / non-finite dt does nothing.
    for (double dt : {0.0, -1.0, std::nan(""), -std::numeric_limits<double>::infinity()}) {
        const gm::ThingMotionStep out = step_once(state, config, nullptr, {}, dt);
        CHECK(!out.moved);
        CHECK_NEAR(state.position.y, 0.0, 0.0);
    }
    // Expired instances are frozen.
    state.expired = true;
    CHECK(!step_once(state, config, nullptr).moved);
    state.expired = false;

    // An out-of-range movement type is reported as unsupported and does not
    // move, but the shared angle part still runs.
    gm::ThingMotionConfig unknown = config_for(200, 4.0, 10.0);
    gm::ThingMotionState odd = gm::thing_motion_spawn(unknown);
    const gm::ThingMotionStep out = step_once(odd, unknown, nullptr);
    CHECK(!out.supported);
    CHECK(!out.moved);
    CHECK_NEAR(odd.position.y, 0.0, 0.0);

    // A zero-radius body still steps (no sphere cast, no floor query).
    gm::ThingMotionConfig point = config_for(2, 4.0, 0.0);
    gm::ThingMotionState point_state = gm::thing_motion_spawn(point);
    CHECK(step_once(point_state, point, nullptr).moved);
}

void test_determinism() {
    const gm::CollisionWorld world = build_world(wall_specs(50.0, true));
    gm::ThingMotionConfig config = config_for(2, 6.0, 10.0);
    config.turn_accel = 0.25;
    config.turn_max = 1.5;
    config.flags_b = 0x40000000u;
    auto run = [&](gm::ThingMotionState& state) {
        for (int i = 0; i < 200; ++i) {
            step_once(state, config, &world);
        }
    };
    gm::ThingMotionState a = gm::thing_motion_spawn(config);
    gm::ThingMotionState b = gm::thing_motion_spawn(config);
    a.roll_rate = 0.5;
    b.roll_rate = 0.5;
    run(a);
    run(b);
    CHECK_EQ(std::memcmp(&a.position, &b.position, sizeof(gm::Vec3d)), 0);
    CHECK_EQ(std::memcmp(&a.yaw, &b.yaw, sizeof(double)), 0);
    CHECK_EQ(std::memcmp(&a.yaw_rate, &b.yaw_rate, sizeof(double)), 0);
    CHECK_EQ(std::memcmp(&a.roll, &b.roll, sizeof(double)), 0);
    CHECK(std::isfinite(a.position.x) && std::isfinite(a.position.y) &&
          std::isfinite(a.position.z));
    CHECK(a.yaw >= 0.0 && a.yaw < kTwoPi);
}

// --- real data -------------------------------------------------------------

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

gm::SbtLevel load_real(const fs::path& path) {
    const std::vector<std::uint8_t> bytes = read_file(path);
    std::string error;
    const auto level = gm::parse_sbt(bytes, error);
    if (!level) {
        std::fprintf(stderr, "thing_motion: parse_sbt(%s): %s\n", path.c_str(),
                     error.c_str());
        return {};
    }
    return *level;
}

int type_count(const gm::SbtLevel& level) {
    for (const gm::SbtRecord& record : level.records) {
        if (record.type == 4 && record.payload.size() >= 4) {
            std::uint32_t count = 0;
            std::memcpy(&count, record.payload.data(), 4);
            return static_cast<int>(count);
        }
    }
    return 0;
}

void test_real_level(const fs::path& path, const std::string& name,
                     int expected_types) {
    const gm::SbtLevel level = load_real(path);
    if (type_count(level) == 0) {
        std::fprintf(stderr, "thing_motion: %s: no THING_TYPES\n", name.c_str());
        ++g_failures;
        return;
    }
    CHECK_EQ(type_count(level), expected_types);

    // Movement-type histogram of the *type table* (docs/THING_FIELDS.md §8).
    std::array<int, 19> histogram{};
    int seen = 0;
    for (int type = 0; type < expected_types; ++type) {
        const auto events = gm::thing_type_events(level, type);
        CHECK(events.has_value());
        if (!events) {
            continue;
        }
        ++seen;
        CHECK(events->movement_type >= 0 && events->movement_type <= 18);
        ++histogram[static_cast<std::size_t>(events->movement_type)];
        // The 40 proved types all carry flags_a bit 0 (slot used).
        CHECK((events->flags_a & 1u) != 0);
    }
    CHECK_EQ(seen, expected_types);
    if (name == "level0") {
        CHECK_EQ(histogram[0], 10);
        CHECK_EQ(histogram[1], 1);
        CHECK_EQ(histogram[5], 1);
        CHECK_EQ(histogram[6], 38);
        CHECK_EQ(histogram[8], 1);
        CHECK_EQ(histogram[10], 1);
        CHECK_EQ(histogram[15], 4);
        CHECK_EQ(histogram[18], 1);
        // Grenade / ROCKET flag words of the type table (docs §8).
        const auto grenade = gm::thing_type_events(level, 28);
        CHECK(grenade.has_value());
        if (grenade) {
            CHECK_EQ(grenade->flags_a, 0x00000005u);
            CHECK_EQ(grenade->flags_b, 0x00000838u);
            CHECK_EQ(grenade->flags_c, 0x00506000u);
            CHECK_NEAR(grenade->speed, 2.0, 0.0);
            CHECK_EQ(grenade->movement_type, 6);
        }
        const auto rocket = gm::thing_type_events(level, 31);
        CHECK(rocket.has_value());
        if (rocket) {
            CHECK_EQ(rocket->flags_a, 0x00000085u);
            CHECK_EQ(rocket->flags_b, 0x0000080cu);
            CHECK_EQ(rocket->flags_c, 0x00306000u);
            CHECK_NEAR(rocket->speed, 18.0, 0.0);
        }
    }

    // Every spawned instance of the level steps 91 ticks in an empty world:
    // finite, deterministic, in range.
    const gm::VmState vm = gm::vm_state_from_level(level);
    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    const gm::Things things = gm::Things::load(level, vm, options);
    CHECK(things.ok());
    CHECK(!things.things().empty());
    std::array<int, 19> instance_histogram{};
    for (const gm::Thing& thing : things.things()) {
        const gm::ThingMotionConfig config = gm::thing_motion_config(thing);
        gm::ThingMotionState state = gm::thing_motion_spawn(config);
        if (thing.position.has_value()) {
            state.position = {static_cast<double>((*thing.position)[0]),
                              static_cast<double>((*thing.position)[1]),
                              static_cast<double>((*thing.position)[2])};
        }
        gm::ThingMotionState again = state;
        for (int i = 0; i < 91; ++i) {
            step_once(state, config, nullptr);
            step_once(again, config, nullptr);
        }
        CHECK_EQ(std::memcmp(&state.position, &again.position, sizeof(gm::Vec3d)), 0);
        CHECK(std::isfinite(state.position.x) && std::isfinite(state.position.y) &&
              std::isfinite(state.position.z));
        CHECK(std::isfinite(state.yaw) && state.yaw >= 0.0 && state.yaw < kTwoPi);
        CHECK(std::isfinite(state.yaw_rate));
        if (config.movement_type >= 0 && config.movement_type <= 18) {
            ++instance_histogram[static_cast<std::size_t>(config.movement_type)];
        }
    }
    std::printf("thing_motion: %s: %zu types, %zu instances, movement {", name.c_str(),
                static_cast<std::size_t>(expected_types), things.things().size());
    bool first = true;
    for (int type = 0; type < 19; ++type) {
        if (instance_histogram[static_cast<std::size_t>(type)] == 0) {
            continue;
        }
        std::printf("%s%d:%d", first ? "" : ", ", type,
                    instance_histogram[static_cast<std::size_t>(type)]);
        first = false;
    }
    std::printf("}\n");
}

} // namespace

int main() {
    test_mode_table();
    test_basis_agrees_with_camera();
    test_spawn_copy();
    test_spin();
    test_avoid_open();
    test_avoid_wall_turns_left();
    test_avoid_wall_turns_right();
    test_avoid_blocked();
    test_avoid_lean();
    test_avoid34();
    test_turret();
    test_ram();
    test_seek_avoid();
    test_patrol_fire_gate();
    test_projectile();
    test_floor_and_ceiling();
    test_impulse_bounce();
    test_thruster();
    test_negatives();
    test_determinism();

    if (fs::exists(kGame / "level0.sbt")) {
        test_real_level(kGame / "level0.sbt", "level0", 57);
        test_real_level(kGame / "dm1.sbt", "dm1", 59);
    } else {
        std::printf("thing_motion: %s not found, real-level checks skipped\n",
                    kGame.c_str());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "thing_motion: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("thing_motion: ok\n");
    return 0;
}
