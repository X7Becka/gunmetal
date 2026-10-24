// `render/camera_model.h` (docs/CAMERA_MODEL.md): camera modes, the proven
// engine focal scales, the `SET_CAMERA_*_SHAKE` triangle wave and the
// deterministic shake impulse.
//
// Cockpit numbers come from docs/CAMERA.md §2–§5 (0x446590/0x41e025), focal
// numbers from §7 (`fx = zoom`, `fy = zoom·4H/(3W)`), the waveform table from
// docs/SHAKE.md §5.2.  Chase/orbit/fixed are MODEL (docs/CAMERA_CPP.md §5.1).
#include "render/camera_model.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

using gm::Vec3d;

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
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);      \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                \
    do {                                                                                     \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (tol))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s = %.17g != %.17g (tol %.3g)\n", __FILE__,   \
                         __LINE__, #a, va, vb, static_cast<double>(tol));                    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_ERR(obj, contains)                                                             \
    do {                                                                                     \
        if ((obj).error().find(contains) == std::string::npos) {                             \
            std::fprintf(stderr, "FAIL %s:%d: error \"%s\" lacks \"%s\"\n", __FILE__,       \
                         __LINE__, (obj).error().c_str(), contains);                         \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

bool same_pose(const gm::camera_model::CameraPose& a, const gm::camera_model::CameraPose& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.yaw == b.yaw && a.pitch == b.pitch &&
           a.roll == b.roll;
}

void test_defaults() {
    using namespace gm::camera_model;
    CameraModel model;
    const CameraConfig& c = model.config();
    std::printf("defaults: fov=%.17g near=%.17g far=%.17g chase=(%.17g, %.17g) "
                "shake=(%.17g, %.17g) seed=0x%08x\n",
                c.fov, c.near_plane, c.far_plane, c.chase_distance, c.chase_height,
                c.shake_amplitude, c.shake_decay, c.seed);
    CHECK(model.ok());
    CHECK_NEAR(c.fov, 1.5707963267948966, 1e-15);   // MODEL 90°, docs/CAMERA_CPP.md §4.2
    CHECK_NEAR(c.near_plane, 0.9, 1e-15);           // MODEL, camera +0xe0
    CHECK_NEAR(c.far_plane, 1.0e5, 1e-9);           // MODEL
    CHECK_NEAR(c.chase_distance, 60.0, 1e-15);      // MODEL, docs/CAMERA_CPP.md §5.1
    CHECK_NEAR(c.chase_height, 0.0, 1e-15);         // MODEL
    CHECK_NEAR(c.shake_amplitude, 0.05, 1e-15);     // MODEL
    CHECK_NEAR(c.shake_decay, 1.0, 1e-15);          // MODEL
    CHECK_NEAR(c.shake_period_ms, 200.0, 1e-15);    // docs/SHAKE.md §5.1 shape
    CHECK(model.shake() == 0.0);
}

void test_cockpit() {
    using namespace gm::camera_model;
    CameraModel model;
    const Vec3d target{24.0, -208.0, 58.0}; // level0 spawn (24,-208,48) + eye 10

    const CameraPose p = model.pose(target, 0.0, 0.0);
    std::printf("cockpit:  pos=(%.17g, %.17g, %.17g) yaw=%.17g pitch=%.17g roll=%.17g\n",
                p.position.x, p.position.y, p.position.z, p.yaw, p.pitch, p.roll);
    CHECK_NEAR(p.position.x, 24.0, 0.0);
    CHECK_NEAR(p.position.y, -208.0, 0.0);
    CHECK_NEAR(p.position.z, 58.0, 0.0);
    CHECK(p.yaw == 0.0 && p.pitch == 0.0 && p.roll == 0.0);

    // yaw = +90° looks along +X, pitch = +0.3 looks down (docs/CAMERA.md §2–§3).
    const double yaw = 1.5707963267948966;
    const double pitch = 0.3;
    const CameraPose q = model.pose(target, yaw, pitch);
    const Vec3d f = forward(yaw, pitch);
    const Vec3d r = right(yaw);
    const Vec3d u = up(yaw, pitch);
    std::printf("cockpit:  forward=(%.17g, %.17g, %.17g)\n", f.x, f.y, f.z);
    std::printf("cockpit:  right  =(%.17g, %.17g, %.17g)\n", r.x, r.y, r.z);
    std::printf("cockpit:  up     =(%.17g, %.17g, %.17g)\n", u.x, u.y, u.z);
    CHECK_NEAR(f.x, 0.955336489125606, 1e-15);      // sin(90°)·cos(0.3)
    CHECK_NEAR(f.y, 5.849748867581718e-17, 1e-15);  // cos(90°)·cos(0.3)
    CHECK_NEAR(f.z, -0.29552020666133955, 1e-15);   // −sin(0.3): looks down
    CHECK_NEAR(r.x, 6.123233995736766e-17, 1e-15);
    CHECK_NEAR(r.y, -1.0, 1e-15);
    CHECK_NEAR(r.z, 0.0, 0.0);
    CHECK_NEAR(u.z, std::cos(pitch), 1e-15);
    CHECK_NEAR(std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z), 1.0, 1e-15);
    CHECK(q.position.x == target.x && q.position.y == target.y && q.position.z == target.z);
    CHECK(q.yaw == yaw && q.pitch == pitch && q.roll == 0.0);

    // Roll is passed through (the caller owns body roll, docs/CAMERA.md §6).
    const CameraPose s = model.pose(target, yaw, pitch, 0.2, CameraMode::cockpit);
    CHECK(s.roll == 0.2);
    CHECK(s.position.z == target.z);
}

void test_chase() {
    using namespace gm::camera_model;
    CameraConfig c;
    c.chase_distance = 60.0;
    c.chase_height = 12.0;
    CameraModel model;
    CHECK(model.configure(c));
    const Vec3d target{100.0, 200.0, 30.0};

    const CameraPose p = model.pose(target, 0.0, 0.0, CameraMode::chase);
    std::printf("chase:    target=(%.17g, %.17g, %.17g) yaw=0 -> pos=(%.17g, %.17g, %.17g)\n",
                target.x, target.y, target.z, p.position.x, p.position.y, p.position.z);
    CHECK_NEAR(p.position.x, 100.0, 1e-15);
    CHECK_NEAR(p.position.y, 140.0, 1e-15); // 200 − 60
    CHECK_NEAR(p.position.z, 42.0, 1e-15);  // 30 + 12
    CHECK(p.yaw == 0.0 && p.pitch == 0.0 && p.roll == 0.0);

    // yaw = +90°: behind is −X, so the camera sits at (40, 200, 42).
    const CameraPose q =
        model.pose(target, 1.5707963267948966, 0.0, CameraMode::chase);
    std::printf("chase:    yaw=90° -> pos=(%.17g, %.17g, %.17g)\n", q.position.x, q.position.y,
                q.position.z);
    CHECK_NEAR(q.position.x, 40.0, 1e-15);
    CHECK_NEAR(q.position.y, 200.0, 1e-15);
    CHECK_NEAR(q.position.z, 42.0, 1e-15);

    // MODEL choice: the offset uses yaw only, so pitch cannot swing the camera
    // down/up; only the orientation follows pitch (docs/CAMERA_MODEL.md §2.2).
    const CameraPose r =
        model.pose(target, 1.5707963267948966, 0.5, CameraMode::chase);
    std::printf("chase:    yaw=90° pitch=0.5 -> pos=(%.17g, %.17g, %.17g) pitch=%.17g\n",
                r.position.x, r.position.y, r.position.z, r.pitch);
    CHECK_NEAR(r.position.x, 40.0, 1e-15);
    CHECK_NEAR(r.position.y, 200.0, 1e-15);
    CHECK_NEAR(r.position.z, 42.0, 1e-15);
    CHECK_NEAR(r.pitch, 0.5, 1e-15);

    // Distance 0 degenerates to the eye point.
    c.chase_distance = 0.0;
    c.chase_height = 0.0;
    CHECK(model.configure(c));
    const CameraPose z = model.pose(target, 0.7, -0.2, CameraMode::chase);
    CHECK_NEAR(z.position.x, target.x, 1e-15);
    CHECK_NEAR(z.position.y, target.y, 1e-15);
    CHECK_NEAR(z.position.z, target.z, 1e-15);
}

void test_orbit_and_fixed() {
    using namespace gm::camera_model;
    CameraConfig c;
    c.chase_distance = 60.0;
    c.fixed_position = Vec3d{1.0, 2.0, 3.0};
    CameraModel model;
    CHECK(model.configure(c));
    const Vec3d target{0.0, 0.0, 0.0};

    const CameraPose o = model.pose(target, 0.0, 0.2, CameraMode::orbit);
    std::printf("orbit:    pos=(%.17g, %.17g, %.17g) yaw=%.17g pitch=%.17g\n", o.position.x,
                o.position.y, o.position.z, o.yaw, o.pitch);
    CHECK_NEAR(o.position.x, 0.0, 1e-15);
    CHECK_NEAR(o.position.y, -58.803994670474495, 1e-15); // −60·cos(0.2)
    CHECK_NEAR(o.position.z, 11.920159847703673, 1e-15);  // +60·sin(0.2)
    CHECK_NEAR(o.yaw, 0.0, 1e-15);
    CHECK_NEAR(o.pitch, 0.2, 1e-15); // recovered look-at pitch
    CHECK(o.roll == 0.0);
    // The recovered basis points back at the target.
    const Vec3d f = forward(o.yaw, o.pitch);
    CHECK_NEAR(o.position.x + f.x * 60.0, target.x, 1e-12);
    CHECK_NEAR(o.position.y + f.y * 60.0, target.y, 1e-12);
    CHECK_NEAR(o.position.z + f.z * 60.0, target.z, 1e-12);

    const CameraPose fx =
        model.pose(Vec3d{9.0, 9.0, 9.0}, 1.0, -1.0, 0.3, CameraMode::fixed);
    std::printf("fixed:    pos=(%.17g, %.17g, %.17g) yaw=%.17g pitch=%.17g roll=%.17g\n",
                fx.position.x, fx.position.y, fx.position.z, fx.yaw, fx.pitch, fx.roll);
    CHECK_NEAR(fx.position.x, 1.0, 0.0);
    CHECK_NEAR(fx.position.y, 2.0, 0.0);
    CHECK_NEAR(fx.position.z, 3.0, 0.0);
    CHECK(fx.yaw == 1.0 && fx.pitch == -1.0 && fx.roll == 0.3);
}

void test_focal_adjust() {
    using namespace gm::camera_model;
    CameraModel model;
    CHECK_NEAR(kFov320x200, 1.7521161011963868, 1e-15);
    CHECK_NEAR(kFov640x480, 1.5707963267948966, 1e-15);
    CHECK_NEAR(focal_to_fov(1.0), kFov640x480, 1e-15);
    CHECK_NEAR(focal_to_fov(5.0 / 6.0), kFov320x200, 1e-15);
    CHECK_NEAR(fov_to_focal(kFov320x200), 5.0 / 6.0, 1e-15);

    const FocalScales e320 = engine_focal(320.0, 200.0);
    const FocalScales e640 = engine_focal(640.0, 480.0);
    std::printf("focal:    320x200=(%.17g, %.17g) 640x480=(%.17g, %.17g)\n", e320.x, e320.y,
                e640.x, e640.y);
    CHECK_NEAR(e320.x, 1.0, 0.0);
    CHECK_NEAR(e320.y, 0.8333333333333334, 1e-16); // 5/6
    CHECK_NEAR(e640.x, 1.0, 0.0);
    CHECK_NEAR(e640.y, 1.0, 1e-16);

    const Vec3d unit{1.0, 1.0, 1.0};
    const Vec3d a = model.focal_adjust(unit, 320.0, 200.0);
    const Vec3d b = model.focal_adjust(unit, 640.0, 480.0);
    const Vec3d z = model.focal_adjust(unit, 320.0, 200.0, 0.25); // cutscene zoom
    const Vec3d m = model.focal_adjust(Vec3d{2.0, -3.0, 4.0}, 320.0, 200.0);
    std::printf("focal:    unit 320x200=(%.17g, %.17g, %.17g)\n", a.x, a.y, a.z);
    std::printf("focal:    unit 640x480=(%.17g, %.17g, %.17g)\n", b.x, b.y, b.z);
    std::printf("focal:    unit zoom 0.25=(%.17g, %.17g, %.17g)\n", z.x, z.y, z.z);
    std::printf("focal:    (2,-3,4) 320x200=(%.17g, %.17g, %.17g)\n", m.x, m.y, m.z);
    CHECK_NEAR(a.x, 1.0, 0.0);
    CHECK_NEAR(a.y, 0.8333333333333334, 1e-16);
    CHECK_NEAR(a.z, 1.0, 0.0);
    CHECK_NEAR(b.x, 1.0, 0.0);
    CHECK_NEAR(b.y, 1.0, 1e-16);
    CHECK_NEAR(z.x, 0.25, 1e-16);
    CHECK_NEAR(z.y, 0.20833333333333334, 1e-16);
    CHECK_NEAR(m.x, 2.0, 0.0);
    CHECK_NEAR(m.y, -2.5, 1e-16);
    CHECK_NEAR(m.z, 4.0, 0.0);
    CHECK(model.ok());
}

void test_shake_waveform() {
    using namespace gm::camera_model;
    // docs/SHAKE.md §5.2 table: A = 0.4 rad, P = 100 ms, full period 400 ms.
    struct Row {
        double duration;
        double expected;
    };
    const Row rows[] = {{400.0, -0.4}, {300.0, 0.0}, {200.0, 0.4}, {100.0, 0.0}, {1.0, -0.396}};
    for (const Row& row : rows) {
        const double got = triangle_shake(row.duration, 100.0, 0.4);
        std::printf("wave:     duration=%.1f ms -> %.17g (expected %.17g)\n", row.duration, got,
                    row.expected);
        CHECK_NEAR(got, row.expected, 1e-15);
    }
    CHECK(triangle_shake(0.0, 100.0, 0.4) == 0.0);
    CHECK(triangle_shake(-5.0, 100.0, 0.4) == 0.0);
    CHECK(triangle_shake(150.0, 0.0, 0.4) == 0.0);
    // Peaks are ±amplitude, zeroes at r = p and r = 3p.
    CHECK_NEAR(triangle_shake(800.0, 100.0, 0.4), -0.4, 1e-15);
    CHECK_NEAR(triangle_shake(600.0, 100.0, 0.4), 0.4, 1e-15);
}

void test_shake_impulse() {
    using namespace gm::camera_model;
    CameraModel model;
    const Vec3d target{0.0, 0.0, 0.0};
    const CameraPose clean = model.pose(target, 0.0, 0.0, CameraMode::cockpit);

    model.add_shake(1.0);
    std::printf("shake:    add(1.0) env=%.17g pitch=%.17g roll=%.17g yaw=%.17g\n",
                model.shake(), model.shake_pitch(), model.shake_roll(), model.shake_yaw());
    CHECK(model.shake() == 1.0);
    // xorshift32 golden: seed 0x9E3779B9, first three samples normalised to a
    // unit offset (weighted pitch 1.0, roll 0.5, yaw 0.5).
    CHECK_NEAR(model.shake_pitch(), -0.6982337496373869, 1e-14);
    CHECK_NEAR(model.shake_roll(), 0.7151637082602379, 1e-14);
    CHECK_NEAR(model.shake_yaw(), -0.03178838238697359, 1e-14);

    const CameraPose shaken = model.pose(target, 0.0, 0.0, CameraMode::cockpit);
    std::printf("shake:    pose pitch=%.17g roll=%.17g yaw=%.17g (clean %.17g/%.17g/%.17g)\n",
                shaken.pitch, shaken.roll, shaken.yaw, clean.pitch, clean.roll, clean.yaw);
    CHECK(!same_pose(clean, shaken));
    CHECK_NEAR(shaken.pitch, -0.6982337496373869, 1e-14);
    CHECK_NEAR(shaken.roll, 0.7151637082602379, 1e-14);
    CHECK_NEAR(shaken.yaw, -0.03178838238697359, 1e-14);
    // The eye does not move in cockpit mode.
    CHECK_NEAR(shaken.position.x, target.x, 0.0);
    CHECK_NEAR(shaken.position.y, target.y, 0.0);
    CHECK_NEAR(shaken.position.z, target.z, 0.0);

    // dt = 1/64 is exact in binary, decay = 1 rad/s, peak = 1 rad: the
    // envelope reaches exactly 0 after 64 ticks (63 still positive).
    int ticks = 0;
    for (int i = 0; i < 63; ++i) {
        CHECK(model.tick(1.0 / 64.0));
        ++ticks;
    }
    std::printf("shake:    after 63 x 1/64 s env=%.17g\n", model.shake());
    CHECK(model.shake() > 0.0);
    CHECK(model.tick(1.0 / 64.0)); // tick 64
    ++ticks;
    std::printf("shake:    after %d ticks env=%.17g (exactly 0)\n", ticks, model.shake());
    CHECK(model.shake() == 0.0);
    CHECK(model.shake_pitch() == 0.0 && model.shake_roll() == 0.0 && model.shake_yaw() == 0.0);
    CHECK(same_pose(model.pose(target, 0.0, 0.0, CameraMode::cockpit), clean));

    // No impulse: tick is a no-op and keeps the state bit-identical.
    const double before = model.shake();
    CHECK(model.tick(1.0 / 64.0));
    CHECK(model.shake() == before);
    CHECK(model.shake_elapsed() == 0.0);
    CHECK(model.ok());

    // 91 Hz ticks (the engine tick, docs/PLAYER.md): also reaches exactly 0.
    CameraModel hz;
    hz.add_shake(1.0);
    int n = 0;
    while (hz.shake() > 0.0 && n < 200) {
        CHECK(hz.tick(1.0 / 91.0));
        ++n;
    }
    std::printf("shake:    91 Hz: env=0 after %d ticks (%.17g s)\n", n, n / 91.0);
    CHECK(hz.shake() == 0.0);
    CHECK(n == 91 || n == 92);

    // Zero amplitude clears; negative is a magnitude.
    model.add_shake(0.0);
    CHECK(model.shake() == 0.0);
    model.add_shake(-0.25);
    CHECK(model.shake() == 0.25);
    // The sample is unit length, so |offset| = env.
    const double len = std::sqrt(model.shake_pitch() * model.shake_pitch() +
                                 model.shake_roll() * model.shake_roll() +
                                 model.shake_yaw() * model.shake_yaw());
    CHECK_NEAR(len, 0.25, 1e-15);
}

void test_determinism() {
    using namespace gm::camera_model;
    struct Trail {
        CameraPose mid{};
        CameraPose end{};
    };
    auto run = [](std::uint32_t seed) {
        CameraConfig c;
        c.seed = seed;
        CameraModel model;
        CHECK(model.configure(c));
        CameraPose last{};
        for (int i = 0; i < 200; ++i) {
            if (i % 50 == 0) {
                model.add_shake(0.3);
            }
            model.tick(1.0 / 91.0);
            const double yaw = 0.01 * static_cast<double>(i);
            const double pitch = 0.2 * std::sin(0.05 * static_cast<double>(i));
            const CameraMode mode = (i % 2 == 0) ? CameraMode::cockpit : CameraMode::chase;
            last = model.pose(Vec3d{10.0, 20.0, 30.0}, yaw, pitch, 0.1, mode);
        }
        // A final live impulse makes the end pose depend on the seed too (the
        // impulses inside the loop have already decayed by tick 200).
        model.add_shake(0.4);
        return Trail{last, model.pose(Vec3d{10.0, 20.0, 30.0}, 0.5, 0.0, 0.1,
                                      CameraMode::chase)};
    };

    const Trail a = run(0x9E3779B9u);
    const Trail b = run(0x9E3779B9u);
    std::printf("determ:   same seed -> pos=(%.17g, %.17g, %.17g) yaw=%.17g pitch=%.17g "
                "roll=%.17g\n",
                a.end.position.x, a.end.position.y, a.end.position.z, a.end.yaw, a.end.pitch,
                a.end.roll);
    CHECK(same_pose(a.mid, b.mid)); // bit-for-bit, not just near
    CHECK(same_pose(a.end, b.end));

    const Trail c = run(12345u);
    std::printf("determ:   other seed -> yaw=%.17g pitch=%.17g roll=%.17g\n", c.end.yaw,
                c.end.pitch, c.end.roll);
    CHECK(!same_pose(a.end, c.end));

    CameraModel s1;
    CameraModel s2;
    s1.add_shake(1.0);
    s2.add_shake(1.0);
    CHECK(s1.shake_pitch() == s2.shake_pitch());
    CHECK(s1.shake_roll() == s2.shake_roll());
    CHECK(s1.shake_yaw() == s2.shake_yaw());
}

void test_negatives() {
    using namespace gm::camera_model;
    CameraModel model;
    const CameraConfig good = model.config();

    CameraConfig bad = good;
    bad.fov = 0.0;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "fov");
    bad.fov = 3.2; // > π = 180°
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "fov");
    bad.fov = std::nan("");
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "fov");
    CHECK_NEAR(model.config().fov, good.fov, 0.0); // old config kept

    bad = good;
    bad.near_plane = 0.0;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "near_plane");
    bad = good;
    bad.far_plane = good.near_plane; // near >= far
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "far_plane");
    bad = good;
    bad.near_plane = 2.0;
    bad.far_plane = 1.0;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "far_plane");

    bad = good;
    bad.chase_distance = -1.0;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "chase_distance");
    bad = good;
    bad.shake_decay = 0.0;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "shake_decay");
    bad = good;
    bad.shake_period_ms = 0.0;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "shake_period_ms");
    bad = good;
    bad.shake_period_ms = std::nan("");
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "shake_period_ms");
    bad = good;
    bad.shake_amplitude = -0.1;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "shake_amplitude");
    bad = good;
    bad.focus_distance = 0.0;
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "focus_distance");
    bad = good;
    bad.fixed_position = Vec3d{0.0, std::nan(""), 0.0};
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "fixed_position");
    bad = good;
    bad.fov = std::nan("");
    CHECK(!model.configure(bad));
    CHECK_ERR(model, "fov");

    // tick: dt <= 0 / NaN / Inf is a clean error and changes nothing.
    CHECK(model.configure(good));
    model.add_shake(1.0);
    const double env = model.shake();
    CHECK(!model.tick(0.0));
    CHECK_ERR(model, "dt");
    CHECK(!model.tick(-0.5));
    CHECK_ERR(model, "dt");
    CHECK(!model.tick(std::nan("")));
    CHECK_ERR(model, "dt");
    CHECK(!model.tick(std::numeric_limits<double>::infinity()));
    CHECK_ERR(model, "dt");
    CHECK(model.shake() == env);
    CHECK(model.tick(1.0 / 64.0)); // valid input clears the error
    CHECK(model.ok());

    // add_shake: NaN/Inf is rejected, the running impulse is untouched.
    const double kept = model.shake();
    model.add_shake(std::nan(""));
    CHECK_ERR(model, "amplitude");
    CHECK(model.shake() == kept);
    model.add_shake(std::numeric_limits<double>::infinity());
    CHECK_ERR(model, "amplitude");
    CHECK(model.shake() == kept);

    // focal_adjust: every argument is validated; the input comes back intact.
    const Vec3d v{1.0, 2.0, 3.0};
    Vec3d out = model.focal_adjust(v, 0.0, 200.0);
    CHECK_ERR(model, "width");
    CHECK(out.x == v.x && out.y == v.y && out.z == v.z);
    out = model.focal_adjust(v, 320.0, -1.0);
    CHECK_ERR(model, "height");
    out = model.focal_adjust(v, 320.0, 200.0, 0.0);
    CHECK_ERR(model, "zoom");
    out = model.focal_adjust(Vec3d{std::nan(""), 0.0, 0.0}, 320.0, 200.0);
    CHECK_ERR(model, "camera_space");

    // pose: non-finite input is an error, not a NaN pose.
    const CameraPose p = model.pose(Vec3d{std::nan(""), 0.0, 0.0}, 0.0, 0.0);
    CHECK_ERR(model, "pose");
    CHECK(std::isnan(p.position.x));
    const CameraPose q = model.pose(Vec3d{0.0, 0.0, 0.0}, std::nan(""), 0.0);
    CHECK_ERR(model, "pose");
    CHECK(q.pitch == 0.0 && q.roll == 0.0);

    // Invalid constructor config leaves the defaults installed but reports.
    CameraConfig broken;
    broken.shake_decay = -1.0;
    CameraModel bad_model(broken);
    CHECK(!bad_model.ok());
    CHECK_ERR(bad_model, "shake_decay");
    CHECK_NEAR(bad_model.config().shake_decay, 1.0, 0.0);
}

} // namespace

int main() {
    test_defaults();
    test_cockpit();
    test_chase();
    test_orbit_and_fixed();
    test_focal_adjust();
    test_shake_waveform();
    test_shake_impulse();
    test_determinism();
    test_negatives();
    if (g_failures != 0) {
        std::fprintf(stderr, "camera_model: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("camera_model: ok\n");
    return 0;
}
