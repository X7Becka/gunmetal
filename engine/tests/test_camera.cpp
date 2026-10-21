// Deterministic first/third-person camera rig (docs/CAMERA_CPP.md).
//
// Synthetic part: the proven basis `0x41e025` (identity, yaw/pitch/roll signs,
// orthonormality, det = +1), the first-person eye/target formulas, the ±π/6 /
// ±20° / ±π/2 clamps, the three triangle-wave shake channels with the exact
// table from docs/SHAKE.md §5.2, the dormant view-pitch decay and the recoil
// velocity, the engine focal scales (1, 5/6 at 320x200; 1, 1 at 640x480), the
// perspective/near/far mapping, third-person distance/occlusion and the
// optional eye smoothing.  Two runs of the same 120-frame script must be
// bit-identical.
//
// Real part: `dm1.sbt`, `level0.sbt` and `level1.sbt` (game install, skipped
// when absent).  The campaign slot-0 spawn is turned into a player state and
// the resulting eye/target/forward are compared with the Python reference
// `gm.scene.first_person(spawn, eye=10.0, distance=100.0)`:
//
//   dm1    spawn (-160,176,64)  eye (-160,176,74)      target (-160,276,74)
//   level0 spawn (24,-208,48)   eye (24,-208,58)       target (24,-108,58)
//   level1 spawn (84,4180,-3064) yaw -1.9198621771937623
//          eye (84,4180,-3054)
//          target (-9.969262078590845, 4145.797985667433, -3054)
//          forward (-0.9396926207859084, -0.3420201433256685, 0)
#include "camera/camera.h"

#include "player/controller.h"
#include "render/collide.h"
#include "res/sbw.h"
#include "res/sbt.h"
#include "res/scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

constexpr double kPi = 3.14159265358979323846;
const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

bool same_double(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

bool same_vec(const gm::Vec3d& a, const gm::Vec3d& b) {
    return same_double(a.x, b.x) && same_double(a.y, b.y) && same_double(a.z, b.z);
}

bool same_state(const gm::CameraState& a, const gm::CameraState& b) {
    return a.mode == b.mode && same_vec(a.eye, b.eye) && same_vec(a.target, b.target) &&
           same_vec(a.right, b.right) && same_vec(a.forward, b.forward) &&
           same_vec(a.up, b.up) && same_vec(a.player_eye, b.player_eye) &&
           same_double(a.yaw, b.yaw) && same_double(a.pitch, b.pitch) &&
           same_double(a.roll, b.roll) && same_double(a.aim_yaw, b.aim_yaw) &&
           same_double(a.view_pitch, b.view_pitch) &&
           same_double(a.body_pitch, b.body_pitch) &&
           same_double(a.body_roll, b.body_roll) &&
           same_double(a.shake_pitch, b.shake_pitch) &&
           same_double(a.shake_roll, b.shake_roll) &&
           same_double(a.shake_yaw, b.shake_yaw) &&
           same_double(a.eye_height, b.eye_height) &&
           same_double(a.distance, b.distance) && a.occluded == b.occluded &&
           same_double(a.fov, b.fov) && same_double(a.aspect, b.aspect) &&
           same_double(a.near_plane, b.near_plane) &&
           same_double(a.far_plane, b.far_plane);
}

bool nonzero_finite(const gm::Vec3d& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           (v.x != 0.0 || v.y != 0.0 || v.z != 0.0);
}

double det(const gm::CameraBasis& b) {
    return gm::dot(gm::cross(b.right, b.forward), b.up);
}

void check_orthonormal(const gm::CameraBasis& b) {
    CHECK_NEAR(gm::length(b.right), 1.0, 1e-12);
    CHECK_NEAR(gm::length(b.forward), 1.0, 1e-12);
    CHECK_NEAR(gm::length(b.up), 1.0, 1e-12);
    CHECK_NEAR(gm::dot(b.right, b.forward), 0.0, 1e-12);
    CHECK_NEAR(gm::dot(b.right, b.up), 0.0, 1e-12);
    CHECK_NEAR(gm::dot(b.forward, b.up), 0.0, 1e-12);
    CHECK_NEAR(det(b), 1.0, 1e-12);
}

gm::PlayerState player_at(double x, double y, double z, double yaw = 0.0,
                          double eye_height = 10.0) {
    gm::PlayerState player;
    player.pos = {x, y, z};
    player.yaw = yaw;
    player.eye_height = eye_height;
    return player;
}

gm::CameraUpdate update_ok(gm::CameraRig& rig, const gm::PlayerState& player,
                           const gm::LookInput& look, double dt) {
    const gm::CameraUpdate result = rig.update(player, look, dt);
    if (!result.ok) {
        std::fprintf(stderr, "FAIL update: %s\n", result.error.c_str());
        ++g_failures;
    }
    return result;
}

// --- synthetic .sbw --------------------------------------------------------

struct TriSpec {
    gm::Vec3d a;
    gm::Vec3d b;
    gm::Vec3d c;
};

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

gm::CollisionWorld wall_world_at_y(double y) {
    std::vector<TriSpec> specs;
    const double half = 100.0;
    specs.push_back({{-half, y, 0.0}, {-half, y, 100.0}, {half, y, 100.0}});
    specs.push_back({{-half, y, 0.0}, {half, y, 100.0}, {half, y, 0.0}});
    gm::CollisionWorld world;
    std::string error;
    const bool ok = world.build(make_level(specs), &error);
    CHECK(ok);
    CHECK(error.empty());
    return world;
}

// --- basis -----------------------------------------------------------------

void test_basis() {
    const gm::CameraBasis zero = gm::camera_basis(0.0, 0.0, 0.0);
    CHECK_NEAR(zero.right.x, 1.0, 1e-15);
    CHECK_NEAR(zero.right.y, 0.0, 1e-15);
    CHECK_NEAR(zero.right.z, 0.0, 1e-15);
    CHECK_NEAR(zero.forward.x, 0.0, 1e-15);
    CHECK_NEAR(zero.forward.y, 1.0, 1e-15);
    CHECK_NEAR(zero.forward.z, 0.0, 1e-15);
    CHECK_NEAR(zero.up.x, 0.0, 1e-15);
    CHECK_NEAR(zero.up.y, 0.0, 1e-15);
    CHECK_NEAR(zero.up.z, 1.0, 1e-15);

    // yaw = +90° looks along +X; right becomes -Y (docs/CAMERA.md §2).
    const gm::CameraBasis yaw90 = gm::camera_basis(0.0, 0.0, kPi / 2.0);
    CHECK_NEAR(yaw90.forward.x, 1.0, 1e-15);
    CHECK_NEAR(yaw90.forward.y, 0.0, 1e-15);
    CHECK_NEAR(yaw90.forward.z, 0.0, 1e-15);
    CHECK_NEAR(yaw90.right.x, 0.0, 1e-15);
    CHECK_NEAR(yaw90.right.y, -1.0, 1e-15);

    // pitch = +30° looks down: forward.z = -sin(30°), up.z = cos(30°).
    const gm::CameraBasis pitch30 = gm::camera_basis(0.0, kPi / 6.0, 0.0);
    CHECK_NEAR(pitch30.forward.z, -0.5, 1e-15);
    CHECK_NEAR(pitch30.up.z, std::cos(kPi / 6.0), 1e-15);
    CHECK_NEAR(pitch30.up.y, 0.5, 1e-15);

    // roll = +20°, everything else 0.
    const double roll20 = 20.0 * kPi / 180.0;
    const gm::CameraBasis rolled = gm::camera_basis(roll20, 0.0, 0.0);
    CHECK_NEAR(rolled.right.x, std::cos(roll20), 1e-15);
    CHECK_NEAR(rolled.right.z, -std::sin(roll20), 1e-15);
    CHECK_NEAR(rolled.forward.y, 1.0, 1e-15);
    CHECK_NEAR(rolled.up.x, std::sin(roll20), 1e-15);
    CHECK_NEAR(rolled.up.z, std::cos(roll20), 1e-15);

    // Python reference for roll 5°, pitch 15°, yaw 30° (see the test header).
    const gm::CameraBasis golden = gm::camera_basis(5.0 * kPi / 180.0, 15.0 * kPi / 180.0,
                                                    30.0 * kPi / 180.0);
    CHECK_NEAR(golden.right.x, 0.851451132606246, 1e-12);
    CHECK_NEAR(golden.right.y, -0.5176327743474075, 1e-12);
    CHECK_NEAR(golden.right.z, -0.08418598282936919, 1e-12);
    CHECK_NEAR(golden.forward.x, 0.4829629131445341, 1e-12);
    CHECK_NEAR(golden.forward.y, 0.8365163037378079, 1e-12);
    CHECK_NEAR(golden.forward.z, -0.25881904510252074, 1e-12);
    CHECK_NEAR(golden.up.x, 0.20439616755332307, 1e-12);
    CHECK_NEAR(golden.up.y, 0.1797130615794005, 1e-12);
    CHECK_NEAR(golden.up.z, 0.9622501868990583, 1e-12);

    for (int i = 0; i < 200; ++i) {
        const double yaw = -3.0 + 0.031 * static_cast<double>(i);
        const double pitch = -0.5 + 0.0051 * static_cast<double>(i);
        const double roll = -0.34 + 0.0034 * static_cast<double>(i);
        check_orthonormal(gm::camera_basis(roll, pitch, yaw));
    }

    // look_at mirrors gm.render.Camera, including the parallel-up fallback.
    gm::CameraBasis looked;
    CHECK(gm::look_at({1.0, 2.0, 3.0}, {1.0, 12.0, 3.0}, {0.0, 0.0, 1.0}, looked));
    check_orthonormal(looked);
    CHECK_NEAR(looked.forward.y, 1.0, 1e-15);

    CHECK(gm::look_at({0.0, 0.0, 0.0}, {0.0, 0.0, 10.0}, {0.0, 0.0, 1.0}, looked));
    check_orthonormal(looked);
    CHECK_NEAR(looked.up.y, 1.0, 1e-15); // fallback up = +Y
}

// --- first person ----------------------------------------------------------

void test_first_person() {
    gm::CameraRig rig;
    const gm::PlayerState player = player_at(1.0, 2.0, 3.0);
    update_ok(rig, player, {}, 1.0 / 91.0);

    const gm::CameraState& state = rig.state();
    CHECK(state.mode == gm::CameraMode::First);
    CHECK_NEAR(state.eye.x, 1.0, 0.0);
    CHECK_NEAR(state.eye.y, 2.0, 0.0);
    CHECK_NEAR(state.eye.z, 13.0, 0.0); // pos.z + eye_height (player[0x2F50])
    CHECK_NEAR(state.target.x, 1.0, 0.0);
    CHECK_NEAR(state.target.y, 102.0, 0.0); // gm/scene.first_person distance 100
    CHECK_NEAR(state.target.z, 13.0, 0.0);
    CHECK_NEAR(rig.eye().z, 13.0, 0.0);
    CHECK_NEAR(rig.target().y, 102.0, 0.0);
    CHECK_NEAR(rig.up().z, 1.0, 1e-15);
    CHECK_NEAR(rig.forward().y, 1.0, 1e-15);
    CHECK_NEAR(rig.right().x, 1.0, 1e-15);
    CHECK_NEAR(state.eye_height, 10.0, 0.0);
    CHECK_NEAR(state.view_pitch, 0.0, 0.0);
    CHECK_NEAR(state.aim_yaw, 0.0, 0.0);

    const gm::Mat4 view = rig.view();
    const auto origin = view.transform(state.eye);
    CHECK_NEAR(origin[0], 0.0, 1e-12);
    CHECK_NEAR(origin[1], 0.0, 1e-12);
    CHECK_NEAR(origin[2], 0.0, 1e-12);
    CHECK_NEAR(origin[3], 1.0, 0.0);
    const auto ahead = view.transform(state.target);
    CHECK_NEAR(ahead[0], 0.0, 1e-12);
    CHECK_NEAR(ahead[1], 0.0, 1e-12);
    CHECK_NEAR(ahead[2], 100.0, 1e-12);
    const auto right = view.transform({6.0, 2.0, 3.0});
    CHECK_NEAR(right[0], 5.0, 1e-12);
    const auto up = view.transform({1.0, 2.0, 18.0});
    CHECK_NEAR(up[1], 5.0, 1e-12);

    // Eye height is read from the player every frame (LIGHT/MEDIUM/HEAVY...).
    gm::PlayerState tall = player;
    tall.eye_height = 14.0;
    update_ok(rig, tall, {}, 1.0 / 91.0);
    CHECK_NEAR(rig.eye().z, 17.0, 0.0);

    // Body angles enter the basis; pitch > 0 tilts the target down.
    CHECK(rig.set_body_angles(0.1, 0.05));
    update_ok(rig, player, {}, 1.0 / 91.0);
    CHECK_NEAR(rig.state().pitch, 0.1, 1e-15);
    CHECK_NEAR(rig.state().roll, 0.05, 1e-15);
    CHECK_NEAR(rig.target().z, 13.0 - std::sin(0.1) * 100.0, 1e-12);
    check_orthonormal({rig.right(), rig.forward(), rig.up()});
}

// --- yaw / pitch / limits --------------------------------------------------

void test_yaw_pitch_limits() {
    gm::CameraRig rig;
    gm::PlayerState player = player_at(0.0, 0.0, 0.0, 0.5);
    gm::LookInput look;
    look.yaw = 0.5;
    update_ok(rig, player, look, 1.0 / 91.0);
    CHECK_NEAR(rig.state().aim_yaw, 1.0, 1e-15); // player yaw + look delta
    CHECK_NEAR(rig.forward().x, std::sin(1.0), 1e-15);
    CHECK_NEAR(rig.forward().y, std::cos(1.0), 1e-15);

    // View pitch accumulates across frames and is never reset by the build.
    gm::CameraRig pitch_rig;
    look = {};
    look.pitch = 10.0 * kPi / 180.0;
    update_ok(pitch_rig, player_at(0.0, 0.0, 0.0), look, 1.0 / 91.0);
    CHECK_NEAR(pitch_rig.state().view_pitch, 10.0 * kPi / 180.0, 1e-15);
    CHECK_NEAR(pitch_rig.forward().z, -std::sin(10.0 * kPi / 180.0), 1e-15);
    update_ok(pitch_rig, player_at(0.0, 0.0, 0.0), look, 1.0 / 91.0);
    CHECK_NEAR(pitch_rig.state().view_pitch, 20.0 * kPi / 180.0, 1e-15);

    // A huge one-shot pitch is clamped to +π/2, not rejected (docs §6).
    gm::CameraRig huge;
    look = {};
    look.pitch = 1e300;
    update_ok(huge, player_at(0.0, 0.0, 0.0), look, 1.0 / 91.0);
    CHECK_NEAR(huge.state().view_pitch, kPi / 2.0, 0.0);
    CHECK_NEAR(huge.forward().z, -1.0, 1e-15);
    CHECK(std::isfinite(huge.forward().x) && std::isfinite(huge.forward().y));

    // Body pitch ±π/6 and roll ±20° clamps.
    gm::CameraRig clamped;
    CHECK(clamped.set_body_angles(1.0, 1.0));
    CHECK_NEAR(clamped.state().body_pitch, kPi / 6.0, 1e-15);
    CHECK_NEAR(clamped.state().body_roll, 20.0 * kPi / 180.0, 1e-15);
    CHECK(clamped.set_body_angles(-1.0, -1.0));
    CHECK_NEAR(clamped.state().body_pitch, -kPi / 6.0, 1e-15);
    CHECK_NEAR(clamped.state().body_roll, -20.0 * kPi / 180.0, 1e-15);

    // Look roll also clamps at ±20°.
    gm::CameraRig roll_rig;
    look = {};
    look.roll = 10.0;
    update_ok(roll_rig, player_at(0.0, 0.0, 0.0), look, 1.0 / 91.0);
    CHECK_NEAR(roll_rig.state().body_roll, 20.0 * kPi / 180.0, 1e-15);

    // The lock flag (player[0x3358]) zeroes the view pitch.
    gm::CameraRig locked;
    look = {};
    look.pitch = 0.3;
    update_ok(locked, player_at(0.0, 0.0, 0.0), look, 1.0 / 91.0);
    CHECK_NEAR(locked.state().view_pitch, 0.3, 1e-15);
    locked.set_pitch_locked(true);
    update_ok(locked, player_at(0.0, 0.0, 0.0), {}, 1.0 / 91.0);
    CHECK_NEAR(locked.state().view_pitch, 0.0, 0.0);
}

// --- shake -----------------------------------------------------------------

void test_shake() {
    // Exact table of docs/SHAKE.md §5.2: A = 0.4, P = 100 ms.
    gm::ShakeChannel channel;
    const int durations[] = {400, 300, 200, 100, 1};
    const double values[] = {-0.4, 0.0, 0.4, 0.0, -0.396};
    for (int i = 0; i < 5; ++i) {
        channel.set(0.4, 100, durations[i]);
        CHECK_NEAR(channel.value(), values[i], 1e-15);
    }
    channel.set(0.4, 100, 400);
    channel.tick_ms(100);
    CHECK(channel.duration_ms == 300);
    channel.tick_ms(1000);
    CHECK(channel.duration_ms == 0);
    CHECK_NEAR(channel.value(), 0.0, 0.0);
    channel.set(0.4, 0, -5); // period 0 -> 1, duration negative -> 0
    CHECK(channel.period_ms == 1);
    CHECK(channel.duration_ms == 0);
    channel.set(std::numeric_limits<double>::quiet_NaN(), 100, 400);
    CHECK_NEAR(channel.value(), 0.0, 0.0); // non-finite amplitude is ignored

    // The applied angle is sampled before the timer tick (same order as
    // tools/re/shake.py --trace: value, then tick).
    gm::CameraRig rig;
    gm::ShakeImpulse impulse;
    impulse.pitch = 0.4;
    impulse.period_ms = 100;
    impulse.duration_ms = 400;
    CHECK(rig.add_shake(impulse));
    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.0);
    CHECK_NEAR(rig.state().shake_pitch, -0.4, 1e-15);
    CHECK(rig.shake_channel(0).duration_ms == 400);
    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.1);
    CHECK_NEAR(rig.state().shake_pitch, -0.4, 1e-15);
    CHECK(rig.shake_channel(0).duration_ms == 300);
    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.1);
    CHECK_NEAR(rig.state().shake_pitch, 0.0, 0.0);
    CHECK(rig.shake_channel(0).duration_ms == 200);
    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.1);
    CHECK_NEAR(rig.state().shake_pitch, 0.4, 1e-15);
    CHECK(rig.shake_channel(0).duration_ms == 100);
    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.1);
    CHECK_NEAR(rig.state().shake_pitch, 0.0, 0.0);
    CHECK(rig.shake_channel(0).duration_ms == 0);
    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.1);
    CHECK_NEAR(rig.state().shake_pitch, 0.0, 0.0);

    // Roll/yaw channels are added after the player clamps (docs/SHAKE.md §5.3).
    gm::CameraRig all;
    impulse = {};
    impulse.pitch = 0.1;
    impulse.roll = 0.2;
    impulse.yaw = 0.3;
    impulse.period_ms = 100;
    impulse.duration_ms = 400;
    CHECK(all.add_shake(impulse));
    update_ok(all, player_at(0.0, 0.0, 0.0), {}, 0.0);
    CHECK_NEAR(all.state().shake_pitch, -0.1, 1e-15);
    CHECK_NEAR(all.state().shake_roll, -0.2, 1e-15);
    CHECK_NEAR(all.state().shake_yaw, -0.3, 1e-15);
    CHECK_NEAR(all.state().roll, -0.2, 1e-15);
    CHECK_NEAR(all.state().yaw, -0.3, 1e-15);

    // Invalid impulses are rejected without touching the channels.
    gm::CameraRig bad;
    impulse = {};
    impulse.pitch = std::numeric_limits<double>::quiet_NaN();
    CHECK(!bad.add_shake(impulse));
    impulse = {};
    impulse.pitch = 0.1;
    impulse.period_ms = 0;
    CHECK(!bad.add_shake(impulse));
    impulse.period_ms = 100;
    impulse.duration_ms = -1;
    CHECK(!bad.add_shake(impulse));
    CHECK(!bad.add_shake(std::numeric_limits<double>::infinity()));

    gm::CameraRig shorthand;
    CHECK(shorthand.add_shake(0.05));
    CHECK(shorthand.shake_channel(0).period_ms == 200);
    CHECK(shorthand.shake_channel(0).duration_ms == 800);
    shorthand.clear_shake();
    CHECK(!shorthand.shake_channel(0).active());
}

// --- decay / recoil --------------------------------------------------------

void test_decay_and_recoil() {
    CHECK_NEAR(gm::decay_multiplier(0.0), 1.0, 0.0);
    CHECK_NEAR(gm::decay_multiplier(0.49), 1.0, 0.0);
    CHECK_NEAR(gm::decay_multiplier(0.5), 1.0, 1e-15);
    CHECK_NEAR(gm::decay_multiplier(1.0), 1.75, 1e-15);

    // Dormant decay branch, formula of 0x443a36..0x443a58:
    // view_pitch -= (t1 - t2) * 0.016 * multiplier / divisor.
    gm::CameraRig rig;
    rig.set_view_pitch_decay(10, 4, 0.5, 1.0);
    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.016);
    CHECK_NEAR(rig.state().view_pitch, -6.0 * 0.016, 1e-15);

    gm::CameraRig zoomed;
    zoomed.set_view_pitch_decay(10, 4, 1.0, 2.0);
    update_ok(zoomed, player_at(0.0, 0.0, 0.0), {}, 0.016);
    CHECK_NEAR(zoomed.state().view_pitch, -(6.0 * 0.016 * 1.75 / 2.0), 1e-15);

    // Recoil golden (tools/re/shake.py --recoil 200 --yaw 30 --view-pitch 10).
    const double yaw30 = 30.0 * kPi / 180.0;
    const double pitch10 = 10.0 * kPi / 180.0;
    const gm::Vec3d velocity = gm::recoil_velocity(yaw30, pitch10, 200.0);
    CHECK_NEAR(velocity.x, -98.48077530122079, 1e-12);
    CHECK_NEAR(velocity.y, -170.57370639048867, 1e-12);
    CHECK_NEAR(velocity.z, 0.0, 0.0);

    // The rig uses aim_yaw (player + look) and view_pitch, not body pitch.
    gm::CameraRig shooter;
    gm::PlayerState player = player_at(0.0, 0.0, 0.0, yaw30);
    gm::LookInput look;
    look.pitch = pitch10;
    update_ok(shooter, player, look, 1.0 / 91.0);
    CHECK(shooter.set_body_angles(0.4, 0.0)); // torso pitch must not matter
    update_ok(shooter, player, {}, 1.0 / 91.0);
    const gm::Vec3d shot = shooter.recoil_velocity(200.0);
    CHECK_NEAR(shot.x, velocity.x, 1e-12);
    CHECK_NEAR(shot.y, velocity.y, 1e-12);

    gm::CameraRig straight;
    update_ok(straight, player_at(0.0, 0.0, 0.0), {}, 0.0);
    const gm::Vec3d back = straight.recoil_velocity(200.0);
    CHECK_NEAR(back.x, 0.0, 0.0);
    CHECK_NEAR(back.y, -200.0, 0.0);
}

// --- projection ------------------------------------------------------------

void test_projection() {
    gm::CameraRig rig;
    const double near_plane = 0.9;
    const double far_plane = 1000.0;
    const gm::Mat4 projection = rig.projection(kPi / 2.0, 4.0 / 3.0, near_plane, far_plane);
    CHECK_NEAR(projection.at(0, 0), 0.75, 1e-15); // fy/aspect, fy = 1/tan(45°)
    CHECK_NEAR(projection.at(1, 1), 1.0, 1e-15);
    CHECK_NEAR(projection.at(2, 2), (far_plane + near_plane) / (far_plane - near_plane), 1e-15);
    CHECK_NEAR(projection.at(2, 3),
               -2.0 * far_plane * near_plane / (far_plane - near_plane), 1e-15);
    CHECK_NEAR(projection.at(3, 2), 1.0, 0.0);
    CHECK_NEAR(projection.at(3, 3), 0.0, 0.0);

    const auto ndc = projection.project({1.0, 1.0, 10.0});
    CHECK_NEAR(ndc.x, 0.075, 1e-15);
    CHECK_NEAR(ndc.y, 0.1, 1e-15);
    CHECK_NEAR(ndc.z, (projection.at(2, 2) * 10.0 + projection.at(2, 3)) / 10.0, 1e-15);

    // Near/far map to the NDC z range [-1, +1].
    const auto near_ndc = projection.project({0.0, 0.0, near_plane});
    const auto far_ndc = projection.project({0.0, 0.0, far_plane});
    CHECK_NEAR(near_ndc.z, -1.0, 1e-12);
    CHECK_NEAR(far_ndc.z, 1.0, 1e-12);
    const auto origin = projection.project({0.0, 0.0, 0.0});
    CHECK(std::isnan(origin.x) && std::isnan(origin.y) && std::isnan(origin.z));

    // Proven focal scales of docs/CAMERA.md §7.
    const gm::FocalScales vga = gm::engine_focal(320.0, 200.0);
    CHECK_NEAR(vga.x, 1.0, 0.0);
    CHECK_NEAR(vga.y, 5.0 / 6.0, 1e-15);
    const gm::FocalScales square = gm::engine_focal(640.0, 480.0);
    CHECK_NEAR(square.x, 1.0, 0.0);
    CHECK_NEAR(square.y, 1.0, 0.0);
    const gm::FocalScales cutscene = gm::engine_focal(320.0, 200.0, 0.25);
    CHECK_NEAR(cutscene.x, 0.25, 0.0);
    CHECK_NEAR(cutscene.y, 0.25 * 5.0 / 6.0, 1e-15);
    CHECK_NEAR(gm::engine_focal(0.0, 200.0).x, 0.0, 0.0);

    // focal <-> fov round-trip and the 320x200 vertical angle.
    CHECK_NEAR(gm::focal_to_fov(1.0), kPi / 2.0, 1e-15);
    CHECK_NEAR(gm::focal_to_fov(5.0 / 6.0), 1.7521161011963868, 1e-15);
    CHECK_NEAR(gm::fov_to_focal(gm::focal_to_fov(5.0 / 6.0)), 5.0 / 6.0, 1e-15);

    // engine_projection is projection() with fy = 1/tan(vfov/2), aspect = fy/fx.
    const gm::Mat4 engine = rig.engine_projection(320.0, 200.0, 1.0, 0.9, 1000.0);
    CHECK_NEAR(engine.at(0, 0), 1.0, 0.0);
    CHECK_NEAR(engine.at(1, 1), 5.0 / 6.0, 1e-15);
    const double focal_y = 5.0 / 6.0;
    const double fov = gm::focal_to_fov(focal_y);
    const gm::Mat4 equivalent = rig.projection(fov, focal_y / 1.0, 0.9, 1000.0);
    for (int i = 0; i < 16; ++i) {
        CHECK_NEAR(engine.m[static_cast<std::size_t>(i)],
                   equivalent.m[static_cast<std::size_t>(i)], 1e-12);
    }
    const gm::Mat4 engine_vga = rig.engine_projection(640.0, 480.0, 1.0, 0.9, 1000.0);
    CHECK_NEAR(engine_vga.at(0, 0), 1.0, 0.0);
    CHECK_NEAR(engine_vga.at(1, 1), 1.0, 0.0);
    const gm::Mat4 engine_zoom = rig.engine_projection(320.0, 200.0, 0.25, 0.9, 1000.0);
    CHECK_NEAR(engine_zoom.at(0, 0), 0.25, 0.0);
    CHECK_NEAR(engine_zoom.at(1, 1), 0.25 * 5.0 / 6.0, 1e-15);

    // Invalid projection arguments return the identity.
    const gm::Mat4 identity;
    const gm::Mat4 invalid[] = {
        rig.projection(0.0, 1.0, 0.9, 1000.0),
        rig.projection(kPi, 1.0, 0.9, 1000.0),
        rig.projection(kPi / 2.0, 0.0, 0.9, 1000.0),
        rig.projection(kPi / 2.0, std::numeric_limits<double>::quiet_NaN(), 0.9, 1000.0),
        rig.projection(kPi / 2.0, 1.0, 0.0, 1000.0),
        rig.projection(kPi / 2.0, 1.0, 100.0, 100.0),
    };
    for (const gm::Mat4& matrix : invalid) {
        for (int i = 0; i < 16; ++i) {
            CHECK_NEAR(matrix.m[static_cast<std::size_t>(i)],
                       identity.m[static_cast<std::size_t>(i)], 0.0);
        }
    }
    CHECK_NEAR(rig.engine_projection(0.0, 200.0, 1.0, 0.9, 1000.0).at(0, 0), 1.0, 0.0);
}

// --- third person ----------------------------------------------------------

void test_third_person() {
    gm::CameraRig rig;
    rig.set_mode(gm::CameraMode::Third);
    rig.set_third_person(60.0, 5.0, 1.0);
    update_ok(rig, player_at(0.0, 0.0, 10.0), {}, 1.0 / 91.0);
    CHECK(rig.state().mode == gm::CameraMode::Third);
    CHECK_NEAR(rig.state().player_eye.z, 20.0, 0.0);
    CHECK_NEAR(rig.state().eye.x, 0.0, 0.0);
    CHECK_NEAR(rig.state().eye.y, -60.0, 0.0);
    CHECK_NEAR(rig.state().eye.z, 25.0, 0.0);
    CHECK_NEAR(rig.state().distance, 60.0, 0.0);
    CHECK(!rig.state().occluded);
    CHECK_NEAR(rig.target().y, 40.0, 0.0); // eye + forward * focus_distance

    // A wall 50 units behind the player shortens the sweep (radius 1 -> 49).
    gm::CollisionWorld world = wall_world_at_y(-50.0);
    gm::CameraRig occluded;
    occluded.set_mode(gm::CameraMode::Third);
    occluded.set_third_person(60.0, 5.0, 1.0);
    occluded.set_collision_world(&world);
    update_ok(occluded, player_at(0.0, 0.0, 10.0), {}, 1.0 / 91.0);
    CHECK(occluded.state().occluded);
    CHECK_NEAR(occluded.state().distance, 49.0, 1e-12);
    CHECK_NEAR(occluded.eye().y, -49.0, 1e-12);
    CHECK_NEAR(occluded.eye().z, 25.0, 1e-12);

    // Radius 0 disables occlusion; first person ignores the third-person data.
    gm::CameraRig no_collision;
    no_collision.set_mode(gm::CameraMode::Third);
    no_collision.set_third_person(60.0, 5.0, 0.0);
    no_collision.set_collision_world(&world);
    update_ok(no_collision, player_at(0.0, 0.0, 10.0), {}, 1.0 / 91.0);
    CHECK(!no_collision.state().occluded);
    CHECK_NEAR(no_collision.state().distance, 60.0, 0.0);

    gm::CameraRig first;
    first.set_mode(gm::CameraMode::Third);
    first.set_third_person(60.0, 5.0, 1.0);
    update_ok(first, player_at(0.0, 0.0, 10.0), {}, 1.0 / 91.0);
    first.set_mode(gm::CameraMode::First);
    update_ok(first, player_at(0.0, 0.0, 10.0), {}, 1.0 / 91.0);
    CHECK_NEAR(first.eye().z, 20.0, 0.0);
    CHECK_NEAR(first.state().distance, 0.0, 0.0);
    CHECK_NEAR(first.target().y, 100.0, 0.0);
}

// --- smoothing -------------------------------------------------------------

void test_smoothing() {
    gm::CameraConfig config;
    config.smooth_time = 0.1;
    gm::CameraRig rig(config);

    update_ok(rig, player_at(0.0, 0.0, 0.0), {}, 0.05);
    CHECK_NEAR(rig.eye().y, 0.0, 0.0); // first frame snaps to the player eye
    CHECK_NEAR(rig.eye().z, 10.0, 0.0);

    const double alpha = 1.0 - std::exp(-0.05 / 0.1);
    update_ok(rig, player_at(0.0, 10.0, 0.0), {}, 0.05);
    CHECK_NEAR(rig.eye().y, 10.0 * alpha, 1e-15); // 3.934693402873666
    CHECK_NEAR(rig.eye().z, 10.0, 1e-15);

    const double second = 10.0 * alpha + (10.0 - 10.0 * alpha) * alpha;
    update_ok(rig, player_at(0.0, 10.0, 0.0), {}, 0.05);
    CHECK_NEAR(rig.eye().y, second, 1e-15);
    CHECK_NEAR(rig.state().player_eye.y, second, 0.0);

    for (int i = 0; i < 400; ++i) {
        update_ok(rig, player_at(0.0, 10.0, 0.0), {}, 0.05);
    }
    CHECK_NEAR(rig.eye().y, 10.0, 1e-9);

    // snap_smoothing teleports the eye; smooth_time = 0 is the direct path.
    rig.snap_smoothing();
    update_ok(rig, player_at(5.0, 5.0, 5.0), {}, 0.05);
    CHECK_NEAR(rig.eye().x, 5.0, 0.0);
    CHECK_NEAR(rig.eye().y, 5.0, 0.0);

    gm::CameraConfig direct;
    direct.smooth_time = 0.0;
    gm::CameraRig no_smoothing(direct);
    update_ok(no_smoothing, player_at(0.0, 0.0, 0.0), {}, 0.05);
    update_ok(no_smoothing, player_at(0.0, 10.0, 0.0), {}, 0.05);
    CHECK_NEAR(no_smoothing.eye().y, 10.0, 0.0);
    CHECK_NEAR(no_smoothing.state().player_eye.y, 10.0, 0.0);
}

// --- determinism -----------------------------------------------------------

void test_determinism() {
    auto run = [](std::vector<gm::CameraState>& frames) {
        gm::CameraConfig config;
        config.smooth_time = 0.08;
        gm::CameraRig rig(config);
        gm::PlayerState player = player_at(0.0, 0.0, 0.0, 0.25);
        gm::ShakeImpulse impulse;
        impulse.pitch = 0.05;
        impulse.roll = 0.03;
        impulse.period_ms = 120;
        impulse.duration_ms = 480;
        for (int i = 0; i < 120; ++i) {
            gm::LookInput look;
            look.yaw = 0.01 * std::sin(0.1 * static_cast<double>(i));
            look.pitch = 0.005 * std::cos(0.13 * static_cast<double>(i));
            look.roll = 0.002 * std::sin(0.07 * static_cast<double>(i));
            player.pos.x = 0.5 * static_cast<double>(i);
            player.pos.y = 10.0 * std::sin(0.02 * static_cast<double>(i));
            player.yaw = 0.25 + 0.001 * static_cast<double>(i);
            if (i % 30 == 0) {
                CHECK(rig.add_shake(impulse));
            }
            if (i % 17 == 0) {
                CHECK(rig.set_body_angles(0.01 * static_cast<double>(i % 20), 0.0));
            }
            if (i == 60) {
                rig.set_mode(gm::CameraMode::Third);
                rig.set_third_person(40.0, 2.0, 0.0);
            }
            update_ok(rig, player, look, 1.0 / 91.0);
            frames.push_back(rig.state());
            CHECK(nonzero_finite(rig.forward()));
            CHECK(nonzero_finite(rig.up()));
            CHECK(nonzero_finite(rig.right()));
            CHECK(std::isfinite(rig.eye().x) && std::isfinite(rig.eye().y) &&
                  std::isfinite(rig.eye().z));
        }
    };

    std::vector<gm::CameraState> first;
    std::vector<gm::CameraState> second;
    run(first);
    run(second);
    CHECK(first.size() == second.size());
    for (std::size_t i = 0; i < first.size() && i < second.size(); ++i) {
        CHECK(same_state(first[i], second[i]));
    }
}

// --- negatives -------------------------------------------------------------

void test_negatives() {
    gm::CameraRig rig;
    gm::PlayerState player = player_at(1.0, 2.0, 3.0, 0.4);
    gm::LookInput look;
    look.yaw = 0.01;
    look.pitch = 0.02;
    update_ok(rig, player, look, 1.0 / 91.0);
    const gm::CameraState baseline = rig.state();

    auto expect_error = [&](const gm::PlayerState& p, const gm::LookInput& l, double dt) {
        const gm::CameraUpdate result = rig.update(p, l, dt);
        CHECK(!result.ok);
        CHECK(!result.error.empty());
        CHECK(same_state(rig.state(), baseline));
    };

    gm::PlayerState bad = player;
    bad.pos.x = std::numeric_limits<double>::quiet_NaN();
    expect_error(bad, look, 1.0 / 91.0);
    bad = player;
    bad.pos.z = std::numeric_limits<double>::infinity();
    expect_error(bad, look, 1.0 / 91.0);
    bad = player;
    bad.yaw = std::numeric_limits<double>::quiet_NaN();
    expect_error(bad, look, 1.0 / 91.0);
    bad = player;
    bad.eye_height = -1.0;
    expect_error(bad, look, 1.0 / 91.0);
    bad = player;
    bad.eye_height = std::numeric_limits<double>::quiet_NaN();
    expect_error(bad, look, 1.0 / 91.0);

    gm::LookInput bad_look = look;
    bad_look.yaw = std::numeric_limits<double>::quiet_NaN();
    expect_error(player, bad_look, 1.0 / 91.0);
    bad_look = look;
    bad_look.pitch = std::numeric_limits<double>::infinity();
    expect_error(player, bad_look, 1.0 / 91.0);
    bad_look = look;
    bad_look.roll = std::numeric_limits<double>::quiet_NaN();
    expect_error(player, bad_look, 1.0 / 91.0);

    expect_error(player, look, -1e-3);
    expect_error(player, look, 1.0); // > max_dt
    expect_error(player, look, std::numeric_limits<double>::quiet_NaN());
    expect_error(player, look, std::numeric_limits<double>::infinity());

    // Invalid configuration is a clean error too; the rig state is unchanged.
    const gm::CameraConfig good = rig.config();
    gm::CameraConfig config = good;
    config.fov = 0.0;
    rig.set_config(config);
    expect_error(player, look, 1.0 / 91.0);
    config = good;
    config.aspect = std::numeric_limits<double>::quiet_NaN();
    rig.set_config(config);
    expect_error(player, look, 1.0 / 91.0);
    config = good;
    config.near_plane = 0.0;
    rig.set_config(config);
    expect_error(player, look, 1.0 / 91.0);
    config = good;
    config.far_plane = config.near_plane;
    rig.set_config(config);
    expect_error(player, look, 1.0 / 91.0);
    config = good;
    config.smooth_time = -1.0;
    rig.set_config(config);
    expect_error(player, look, 1.0 / 91.0);
    config = good;
    config.third_distance = -1.0;
    rig.set_config(config);
    expect_error(player, look, 1.0 / 91.0);
    config = good;
    config.max_dt = 0.0;
    rig.set_config(config);
    expect_error(player, look, 1.0 / 91.0);
    rig.set_config(good);

    // add_look_impulse / set_body_angles reject non-finite values.
    CHECK(!rig.add_look_impulse(std::numeric_limits<double>::quiet_NaN(), 0.0));
    CHECK(!rig.add_look_impulse(0.0, std::numeric_limits<double>::infinity()));
    CHECK(!rig.set_body_angles(std::numeric_limits<double>::quiet_NaN(), 0.0));
    CHECK(!rig.set_body_angles(0.0, std::numeric_limits<double>::infinity()));

    // A huge finite yaw stays finite and orthonormal.
    gm::CameraRig huge_yaw;
    gm::LookInput yaw_look;
    yaw_look.yaw = 1e300;
    update_ok(huge_yaw, player_at(0.0, 0.0, 0.0), yaw_look, 1.0 / 91.0);
    CHECK(std::isfinite(huge_yaw.state().yaw));
    check_orthonormal({huge_yaw.right(), huge_yaw.forward(), huge_yaw.up()});

    // look_at negatives and degenerate up.
    gm::CameraBasis basis;
    std::string error;
    CHECK(!gm::look_at({1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}, {0.0, 0.0, 1.0}, basis, &error));
    CHECK(!error.empty());
    error.clear();
    CHECK(!gm::look_at({0.0, 0.0, 0.0}, {std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0},
                       {0.0, 0.0, 1.0}, basis, &error));
    CHECK(!error.empty());
    CHECK(gm::look_at({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, basis));
    check_orthonormal(basis);
    CHECK(gm::look_at({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}, basis));
    check_orthonormal(basis);
}

// --- real spawns -----------------------------------------------------------

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

struct SpawnGolden {
    const char* level;
    gm::Vec3d eye;
    gm::Vec3d target;
    gm::Vec3d forward;
};

void test_real_spawns() {
    if (!fs::exists(kGame / "level0.sbt")) {
        std::printf("camera_rig: game not installed, real spawn tests skipped\n");
        return;
    }
    const SpawnGolden goldens[] = {
        {"dm1", {-160.0, 176.0, 74.0}, {-160.0, 276.0, 74.0}, {0.0, 1.0, 0.0}},
        {"level0", {24.0, -208.0, 58.0}, {24.0, -108.0, 58.0}, {0.0, 1.0, 0.0}},
        {"level1",
         {84.0, 4180.0, -3054.0},
         {-9.969262078590845, 4145.797985667433, -3054.0},
         {-0.9396926207859084, -0.3420201433256685, 0.0}},
    };

    for (const SpawnGolden& golden : goldens) {
        const fs::path path = kGame / (std::string(golden.level) + ".sbt");
        const std::vector<std::uint8_t> raw = read_file(path);
        CHECK(!raw.empty());
        if (raw.empty()) {
            continue;
        }
        std::string error;
        const auto level = gm::parse_sbt(raw, error);
        CHECK(level.has_value());
        if (!level) {
            std::fprintf(stderr, "camera_rig: parse_sbt(%s): %s\n", golden.level,
                         error.c_str());
            continue;
        }
        error.clear();
        const auto scene = gm::extract_scene(*level, error, /*with_things=*/false);
        CHECK(scene.has_value());
        if (!scene) {
            std::fprintf(stderr, "camera_rig: extract_scene(%s): %s\n", golden.level,
                         error.c_str());
            continue;
        }
        const gm::SceneSpawn* start = nullptr;
        for (const gm::SceneSpawn& spawn : scene->spawns) {
            if (!spawn.deathmatch && spawn.slot == 0) {
                start = &spawn;
                break;
            }
        }
        CHECK(start != nullptr);
        if (start == nullptr) {
            continue;
        }

        gm::ControllerConfig controller_config;
        const gm::PlayerState player =
            gm::make_state({static_cast<double>(start->x), static_cast<double>(start->y),
                            static_cast<double>(start->z)},
                           start->angles[2], controller_config);
        CHECK_NEAR(player.eye_height, 10.0, 0.0); // LIGHT chassis, docs/CAMERA.md §5

        gm::CameraRig rig;
        CHECK(rig.set_body_angles(start->angles[0], start->angles[1]));
        update_ok(rig, player, {}, 1.0 / 91.0);

        std::printf("camera_rig: %-7s spawn (%g, %g, %g) yaw=%.17g\n", golden.level,
                    static_cast<double>(start->x), static_cast<double>(start->y),
                    static_cast<double>(start->z), start->angles[2]);
        std::printf("           eye    (%.17g, %.17g, %.17g)\n", rig.eye().x, rig.eye().y,
                    rig.eye().z);
        std::printf("           target (%.17g, %.17g, %.17g)\n", rig.target().x,
                    rig.target().y, rig.target().z);

        CHECK_NEAR(rig.eye().x, golden.eye.x, 1e-12);
        CHECK_NEAR(rig.eye().y, golden.eye.y, 1e-12);
        CHECK_NEAR(rig.eye().z, golden.eye.z, 1e-12);
        CHECK_NEAR(rig.target().x, golden.target.x, 1e-9);
        CHECK_NEAR(rig.target().y, golden.target.y, 1e-9);
        CHECK_NEAR(rig.target().z, golden.target.z, 1e-9);
        CHECK_NEAR(rig.forward().x, golden.forward.x, 1e-12);
        CHECK_NEAR(rig.forward().y, golden.forward.y, 1e-12);
        CHECK_NEAR(rig.forward().z, golden.forward.z, 1e-12);
        CHECK_NEAR(gm::length(rig.forward()), 1.0, 1e-12);
    }
}

} // namespace

int main() {
    test_basis();
    test_first_person();
    test_yaw_pitch_limits();
    test_shake();
    test_decay_and_recoil();
    test_projection();
    test_third_person();
    test_smoothing();
    test_determinism();
    test_negatives();
    test_real_spawns();
    if (g_failures != 0) {
        std::fprintf(stderr, "camera_rig: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("camera_rig: ok\n");
    return 0;
}
