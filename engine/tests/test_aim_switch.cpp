// Aimed switches (kind 0x101) through the collision world (docs/DOORS_CPP.md
// §2.3, docs/DOOR_FIELDS.md §2.2).
//
// `0x44fc30` casts a ray from the player's eye (`0x43c51f`, length
// `0x40c38800` = 10 000), and the hit gives the three ints the switch operands
// compare with: sector, object and polygon.  `GameRuntime::aim_switch` is that
// half: raycast through `CollisionWorld`, build `PolygonHit` from the triangle
// record, and let the world's `kind 0x101` switches resolve it.
//
// The rest of this file is synthetic: one quad at x = 100 (sector 0, object 0,
// polygon 0) and switches whose operands are exactly those three ints.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "camera/camera.h"
#include "game/runtime.h"
#include "res/sbt.h"
#include "res/sbw.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        const auto va = (a);                                                   \
        const auto vb = (b);                                                   \
        if (!(va == vb)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n",      \
                         __FILE__, __LINE__, #a, #b,                           \
                         static_cast<long long>(va),                           \
                         static_cast<long long>(vb));                          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void put_i32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void put_f64(std::vector<std::uint8_t>& bytes, std::size_t offset, double value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// The 96-byte object header is 12 x f64 (origin + identity basis).
void write_identity_object(std::uint8_t* header) {
    std::memset(header, 0, 96);
    for (int point = 0; point < 4; ++point) {
        for (int axis = 0; axis < 3; ++axis) {
            const double value = (point == 0) ? 0.0 : (axis == point - 1 ? 1.0 : 0.0);
            std::memcpy(header + 8 * (point * 3 + axis), &value, sizeof(value));
        }
    }
}

struct SwitchSpec {
    int kind = 0x101;
    int target = 0;
    int radius = 100;
    std::array<int, 3> pos{};
};

std::vector<std::uint8_t> make_switch_payload(const std::vector<SwitchSpec>& specs) {
    std::vector<std::uint8_t> payload(4 + specs.size() * 0x20, 0);
    put_u32(payload, 0, static_cast<std::uint32_t>(specs.size()));
    for (std::size_t k = 0; k < specs.size(); ++k) {
        const std::size_t base = 4 + k * 0x20;
        put_i32(payload, base + 0, specs[k].kind);
        put_i32(payload, base + 4, specs[k].target);
        put_i32(payload, base + 8, specs[k].radius);
        for (int axis = 0; axis < 3; ++axis) {
            put_i32(payload, base + 12 + static_cast<std::size_t>(axis) * 4,
                    specs[k].pos[static_cast<std::size_t>(axis)]);
        }
        put_i32(payload, base + 0x18, 0);
        put_i32(payload, base + 0x1C, -1);
    }
    return payload;
}

/// One quad at x = 100, y/z in [-50, 50]: sector 0, object 0, polygon 0.
gm::SbwLevel make_wall() {
    gm::SbwLevel geometry;
    geometry.header.sectors.resize(1);
    gm::SbwObjectRef object;
    write_identity_object(object.header.data());
    geometry.header.sectors[0].objects.push_back(object);
    geometry.header.object_count = 1;

    gm::SbwGeometry wall;
    wall.vertices = {gm::Vec3f{100.0f, -50.0f, -50.0f},
                     gm::Vec3f{100.0f, 50.0f, -50.0f},
                     gm::Vec3f{100.0f, 50.0f, 50.0f},
                     gm::Vec3f{100.0f, -50.0f, 50.0f}};
    wall.n_verts = 4;
    gm::SbwPoly poly;
    poly.indices = {0, 1, 2, 3};
    wall.polys.push_back(poly);
    wall.n_polys = 1;
    geometry.geometry.push_back(std::move(wall));
    return geometry;
}

gm::SbtLevel make_level(const std::vector<SwitchSpec>& switches) {
    gm::SbtLevel level;
    gm::SbtRecord record;
    record.type = 1;
    record.payload = make_switch_payload(switches);
    level.records.push_back(std::move(record));
    return level;
}

/// `GameRuntime` is neither copyable nor movable, so the caller owns it.
void build(gm::GameRuntime& runtime, const gm::SbtLevel& level,
           const gm::SbwLevel& geometry) {
    std::string error;
    if (!runtime.build_world(level, geometry, &error)) {
        std::fprintf(stderr, "aim_switch: build_world: %s\n", error.c_str());
    }
}

void test_aim_hits_the_matching_polygon() {
    // Two kind-0x101 switches: one on the wall's triple (0, 0, 0) and one on a
    // polygon that does not exist.  Only the first can ever fire.
    const std::vector<SwitchSpec> switches = {
        {0x101, 999, 200, {0, 0, 0}},
        {0x101, 998, 200, {0, 0, 1}},
    };
    const gm::SbtLevel level = make_level(switches);
    const gm::SbwLevel geometry = make_wall();
    gm::GameRuntime runtime;
    build(runtime, level, geometry);

    const std::array<gm::Vec3d, 1> eyes = {gm::Vec3d{0.0, 0.0, 0.0}};
    CHECK(runtime.set_player_eyes(eyes));
    CHECK_EQ(runtime.world().switches().size(), std::size_t{2});

    // Wrong way: no hit at all.
    CHECK_EQ(runtime.aim_switch(0, gm::Vec3d{-1.0, 0.0, 0.0}), std::size_t{0});
    CHECK_EQ(runtime.world().stats().aim_presses, std::size_t{0});

    // The ray reaches the wall at t = 100 > max_distance 50: no hit.
    CHECK_EQ(runtime.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}, 50.0), std::size_t{0});

    // Proper hit: exactly the switch whose triple matches.
    CHECK_EQ(runtime.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}), std::size_t{1});
    CHECK_EQ(runtime.world().stats().aim_presses, std::size_t{1});
    CHECK(runtime.world().switches()[0].pressed);
    CHECK(!runtime.world().switches()[1].pressed);

    // 0x44fc30 has no latch: the next frame fires again.
    CHECK_EQ(runtime.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}), std::size_t{1});
    CHECK_EQ(runtime.world().stats().aim_presses, std::size_t{2});

    // A direction over the radius (operand 3 = 200) still fires; 201 does not,
    // because the check is on the ray length, not on the wall distance.
    {
        const std::vector<SwitchSpec> short_radius = {{0x101, 999, 99, {0, 0, 0}}};
        const gm::SbtLevel tight = make_level(short_radius);
        gm::GameRuntime narrow;
        build(narrow, tight, geometry);
        CHECK(narrow.set_player_eyes(eyes));
        CHECK_EQ(narrow.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}), std::size_t{0});
    }

    // No player 5 and a zero direction are both no-ops.
    CHECK_EQ(runtime.aim_switch(5, gm::Vec3d{1.0, 0.0, 0.0}), std::size_t{0});
    CHECK_EQ(runtime.aim_switch(0, gm::Vec3d{0.0, 0.0, 0.0}), std::size_t{0});
    CHECK_EQ(runtime.world().stats().aim_presses, std::size_t{2});
}

void test_aim_ignores_other_kinds() {
    // 0x102/0x103 are blast triggers, 0x104 an exact triple: a ray must not
    // fire any of them even with the right triple.
    const std::vector<SwitchSpec> switches = {
        {0x102, 100, 200, {0, 0, 0}},
        {0x103, 101, 200, {0, 0, 0}},
        {0x104, 102, 200, {0, 0, 0}},
        {0, 103, 200, {0, 0, 0}},
    };
    const gm::SbtLevel level = make_level(switches);
    gm::GameRuntime runtime;
    const gm::SbwLevel wall = make_wall();
    build(runtime, level, wall);
    const std::array<gm::Vec3d, 1> eyes = {gm::Vec3d{0.0, 0.0, 0.0}};
    CHECK(runtime.set_player_eyes(eyes));
    CHECK_EQ(runtime.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}), std::size_t{0});
    for (const gm::Switch& sw : runtime.world().switches()) {
        CHECK(!sw.pressed);
    }
}

void test_aim_is_deterministic() {
    const std::vector<SwitchSpec> switches = {{0x101, 999, 200, {0, 0, 0}}};
    const gm::SbtLevel level = make_level(switches);
    const gm::SbwLevel geometry = make_wall();
    const std::array<gm::Vec3d, 1> eyes = {gm::Vec3d{0.0, 0.0, 0.0}};

    gm::GameRuntime first;
    gm::GameRuntime second;
    build(first, level, geometry);
    build(second, level, geometry);
    CHECK(first.set_player_eyes(eyes));
    CHECK(second.set_player_eyes(eyes));
    for (int tick = 0; tick < 10; ++tick) {
        CHECK_EQ(first.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}),
                 second.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}));
    }
    CHECK_EQ(first.world().stats().aim_presses, second.world().stats().aim_presses);
    CHECK_EQ(first.world().stats().aim_presses, std::size_t{10});
}

} // namespace

// `0x44fc30` takes the aimed ray from the player basis built by `0x41e025`:
// the second output (`local_50`) is the direction handed to `0x43c51f`, i.e.
// `camera_basis().forward`.  `aim_switch_from_camera()` is that path.
void test_aim_from_camera() {
    constexpr double kPi = 3.14159265358979323846;
    const std::vector<SwitchSpec> switches = {{0x101, 999, 200, {0, 0, 0}}};
    const gm::SbtLevel level = make_level(switches);
    const gm::SbwLevel geometry = make_wall();
    gm::GameRuntime runtime;
    build(runtime, level, geometry);
    const std::array<gm::Vec3d, 1> eyes = {gm::Vec3d{0.0, 0.0, 0.0}};
    CHECK(runtime.set_player_eyes(eyes));

    // yaw = +pi/2 looks along +X, straight at the wall at x = 100.
    CHECK_EQ(runtime.aim_switch_from_camera(0, 0.0, 0.0, kPi / 2.0),
             std::size_t{1});
    // yaw = 0 looks along +Y and yaw = -pi/2 along -X: both miss.
    CHECK_EQ(runtime.aim_switch_from_camera(0, 0.0, 0.0, 0.0), std::size_t{0});
    CHECK_EQ(runtime.aim_switch_from_camera(0, 0.0, 0.0, -kPi / 2.0),
             std::size_t{0});

    // Pitch > 0 looks down: from the eye the ray misses the quad.
    CHECK_EQ(runtime.aim_switch_from_camera(0, 0.0, kPi / 2.0, kPi / 2.0),
             std::size_t{0});
    // Roll rotates the right/up rows, not `forward`.
    CHECK_EQ(runtime.aim_switch_from_camera(0, 0.7, 0.0, kPi / 2.0),
             std::size_t{1});

    // The basis overload takes the same forward row.
    CHECK_EQ(runtime.aim_switch_from_basis(0, gm::camera_basis(0.0, 0.0, kPi / 2.0)),
             std::size_t{1});

    // max_distance still applies (the wall is at 100).
    CHECK_EQ(runtime.aim_switch_from_camera(0, 0.0, 0.0, kPi / 2.0, 50.0),
             std::size_t{0});

    // Non-finite angles, an unknown player and a zero basis fire nothing.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(runtime.aim_switch_from_camera(0, nan, 0.0, 0.0), std::size_t{0});
    CHECK_EQ(runtime.aim_switch_from_camera(3, 0.0, 0.0, kPi / 2.0), std::size_t{0});
    gm::CameraBasis zero;
    zero.forward = gm::Vec3d{0.0, 0.0, 0.0};
    CHECK_EQ(runtime.aim_switch_from_basis(0, zero), std::size_t{0});

    // The plain `aim_switch` remains the manual entry point.
    CHECK_EQ(runtime.aim_switch(0, gm::Vec3d{1.0, 0.0, 0.0}), std::size_t{1});
}

int main() {
    test_aim_hits_the_matching_polygon();
    test_aim_ignores_other_kinds();
    test_aim_is_deterministic();
    test_aim_from_camera();
    if (g_failures != 0) {
        std::fprintf(stderr, "aim_switch: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("aim_switch: ok\n");
    return 0;
}
