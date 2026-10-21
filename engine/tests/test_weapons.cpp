// C++ weapon runtime (docs/WEAPONS_CPP.md).
//
// Synthetic part: hand-built `Weapon` tables and `CollisionWorld`s pin the
// fixed-tick rules — single vs automatic cooldowns, fire-rate quantisation,
// dry fire, magazine reload, slot mask/switch, hitscan hit/miss (including a
// target behind the shooter), deterministic spread and the recoil formula.
//
// Real part: `saved0.sbt` / `saved1.sbt` are parsed through `parse_save` +
// `GameState::from_save`, the tables come from the shipped `level0.sbt`
// (19 weapons / 15 ammo types), and the first shot of the selected weapon
// pins ammo, cooldown, range, time and the placeholder damage.  The saved
// ammo/mask/current values are checked as-is.
//
// Negatives: slot outside 0..11, mask bit clear, table entry missing,
// magazine 0 with a capacity, projectile speed 0, NaN direction, negative and
// NaN damage, and a weapon fire period of 0 (no cooldown).
#include "game/projectile.h"
#include "game/weapons.h"

#include "world/things.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game/events.h"
#include "game/state.h"
#include "player/controller.h"
#include "render/collide.h"
#include "res/save.h"
#include "res/sbt.h"
#include "res/sbtvm.h"
#include "res/sbw.h"

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
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
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

// --- synthetic collision world ---------------------------------------------

struct TriSpec {
    gm::Vec3d a;
    gm::Vec3d b;
    gm::Vec3d c;
};

void put_f32(std::uint8_t* p, float value) {
    std::memcpy(p, &value, sizeof(value));
}

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
        geometry.vertices = {
            gm::Vec3f{static_cast<float>(spec.a.x), static_cast<float>(spec.a.y),
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

/// Vertical wall at `x`, spanning y/z in +-200.
std::vector<TriSpec> wall_x(double x) {
    std::vector<TriSpec> out;
    add_quad(out, {x, -200.0, -200.0}, {x, -200.0, 200.0}, {x, 200.0, 200.0},
             {x, 200.0, -200.0});
    return out;
}

// --- synthetic weapons -----------------------------------------------------

gm::Weapon make_weapon(int slot, int fire_ticks, bool hitscan) {
    gm::Weapon weapon;
    weapon.name = "TEST";
    weapon.icon = "WICON0";
    weapon.slot = slot;
    weapon.id = 1000u + static_cast<std::uint32_t>(slot);
    weapon.fire_mode = 1;
    weapon.fire_period_ms = 100; // not used once fire_ticks is set explicitly
    weapon.fire_ticks = fire_ticks;
    weapon.ammo_type = 0;
    weapon.ammo_per_shot = 1;
    weapon.projectile_id = 1000000u + static_cast<std::uint32_t>(slot);
    weapon.spread = 0.0;
    weapon.spread_y = 0.0;
    weapon.range = 100.0;
    weapon.muzzle = 0;
    weapon.damage = 10.0;
    weapon.magazine = 0;
    weapon.reload_ticks = 0;
    weapon.hitscan = hitscan;
    weapon.projectile_speed = 100.0;
    return weapon;
}

gm::Arsenal make_arsenal(std::uint32_t mask, int current, std::uint32_t ammo) {
    gm::Arsenal arsenal;
    arsenal.mask = mask;
    arsenal.current = current;
    for (std::uint32_t& value : arsenal.ammo) {
        value = ammo;
    }
    return arsenal;
}

// --- synthetic tests -------------------------------------------------------

void test_fire_rate_quantisation() {
    CHECK_EQ(gm::fire_ticks_from_ms(0), 0);
    CHECK_EQ(gm::fire_ticks_from_ms(1), 1);
    CHECK_EQ(gm::fire_ticks_from_ms(7), 1);
    CHECK_EQ(gm::fire_ticks_from_ms(17), 2);
    CHECK_EQ(gm::fire_ticks_from_ms(20), 2);
    CHECK_EQ(gm::fire_ticks_from_ms(50), 5);
    CHECK_EQ(gm::fire_ticks_from_ms(80), 8);
    CHECK_EQ(gm::fire_ticks_from_ms(400), 37);
    CHECK_EQ(gm::fire_ticks_from_ms(1000), 91);
    CHECK_EQ(gm::fire_ticks_from_ms(-5), 0);
}

void test_single_and_auto() {
    gm::Weapon single = make_weapon(0, 10, true);
    gm::Weapon automatic = make_weapon(1, 1, true);
    gm::Weapons table({single, automatic});
    gm::CollisionWorld world;

    gm::Arsenal arsenal = make_arsenal(0x3, 0, 5);
    gm::ShotResult first = gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(first.ok);
    CHECK(first.fired);
    CHECK_EQ(first.cooldown_left, 10);
    CHECK_EQ(arsenal.shot_index, std::uint64_t(1));

    // A slow weapon cannot be re-fired on the same or the next few ticks.
    gm::ShotResult again = gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(!again.ok);
    CHECK(again.error == gm::FireError::cooling_down);
    CHECK_EQ(arsenal.shot_index, std::uint64_t(1));
    gm::tick(table, arsenal, 9);
    CHECK_EQ(arsenal.cooldown[0], 1);
    CHECK(!gm::can_fire(table, arsenal, 0));
    gm::tick(table, arsenal, 1);
    CHECK(gm::can_fire(table, arsenal, 0));
    CHECK_EQ(arsenal.shot_index, std::uint64_t(1));

    // An automatic weapon fires after a single tick.
    gm::FireError why = gm::FireError::none;
    CHECK(gm::switch_slot(table, arsenal, 1, &why));
    CHECK_EQ(arsenal.current, 1);
    CHECK_EQ(arsenal.ammo[1], std::uint32_t(5));
    const gm::ShotResult burst =
        gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, world);
    CHECK(burst.ok);
    CHECK(!gm::can_fire(table, arsenal, 1));
    gm::tick(table, arsenal, 1);
    CHECK(gm::can_fire(table, arsenal, 1));

    // A zero-period weapon has no cooldown at all.
    gm::Weapon instant = make_weapon(0, 0, true);
    gm::Weapons instant_table({instant});
    gm::Arsenal instant_arsenal = make_arsenal(0x1, 0, 2);
    CHECK_EQ(instant_table.at(0)->fire_ticks, 0);
    CHECK(gm::fire(instant_table, instant_arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world).ok);
    CHECK(gm::can_fire(instant_table, instant_arsenal, 0));
}

void test_empty_ammo() {
    gm::Weapon weapon = make_weapon(0, 1, true);
    gm::Weapons table({weapon});
    gm::CollisionWorld world;
    gm::Arsenal arsenal = make_arsenal(0x1, 0, 0);

    gm::FireError why = gm::FireError::none;
    CHECK(!gm::can_fire(table, arsenal, 0, &why));
    CHECK(why == gm::FireError::empty_ammo);

    const gm::ShotResult shot =
        gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(!shot.ok);
    CHECK(!shot.fired);
    CHECK(shot.error == gm::FireError::empty_ammo);
    CHECK_EQ(arsenal.cooldown[0], 0);
    CHECK_EQ(arsenal.shot_index, std::uint64_t(0));

    arsenal.ammo[0] = 1;
    const gm::ShotResult last =
        gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(last.ok);
    CHECK_EQ(last.ammo_left, std::uint32_t(0));
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(0));
    gm::tick(table, arsenal, 1);
    CHECK(!gm::can_fire(table, arsenal, 0));
}

void test_magazine_and_reload() {
    gm::Weapon weapon = make_weapon(0, 1, true);
    weapon.magazine = 5;
    weapon.reload_ticks = 7;
    gm::Weapons table({weapon});
    gm::CollisionWorld world;

    gm::Arsenal arsenal = make_arsenal(0x1, 0, 0);
    arsenal.ammo[0] = 3;
    gm::FireError why = gm::FireError::none;
    CHECK(!gm::can_fire(table, arsenal, 0, &why));
    CHECK(why == gm::FireError::empty_magazine);

    CHECK(gm::reload(table, arsenal, 0, &why));
    CHECK(arsenal.reloading[0]);
    CHECK_EQ(arsenal.reload_left[0], 7);
    CHECK(!gm::can_fire(table, arsenal, 0, &why));
    CHECK(why == gm::FireError::reloading);
    gm::tick(table, arsenal, 6);
    CHECK(arsenal.reloading[0]);
    CHECK_EQ(arsenal.reload_left[0], 1);
    gm::tick(table, arsenal, 1);
    CHECK(!arsenal.reloading[0]);
    CHECK_EQ(arsenal.magazine[0], std::uint32_t(3));
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(0));
    CHECK(gm::can_fire(table, arsenal, 0));

    const gm::ShotResult shot =
        gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(shot.ok);
    CHECK_EQ(shot.ammo_left, std::uint32_t(2));
    CHECK_EQ(arsenal.magazine[0], std::uint32_t(2));

    // Empty reserve and full magazine are clean errors.
    CHECK(!gm::reload(table, arsenal, 0, &why));
    CHECK(why == gm::FireError::empty_ammo);
    arsenal.ammo[0] = 10;
    arsenal.magazine[0] = 5;
    CHECK(!gm::reload(table, arsenal, 0, &why));
    CHECK(why == gm::FireError::magazine_full);

    // Direct-feed weapons have no magazine to reload.
    gm::Weapon direct = make_weapon(1, 1, true);
    gm::Weapons direct_table({weapon, direct});
    gm::Arsenal direct_arsenal = make_arsenal(0x3, 1, 5);
    CHECK(!gm::reload(direct_table, direct_arsenal, 1, &why));
    CHECK(why == gm::FireError::no_magazine);

    // reload_ticks == 0 transfers immediately.
    gm::Weapon fast = weapon;
    fast.slot = 2;
    fast.reload_ticks = 0;
    gm::Weapons fast_table({weapon, direct, fast});
    gm::Arsenal fast_arsenal = make_arsenal(0x7, 2, 5);
    fast_arsenal.ammo[2] = 2;
    fast_arsenal.magazine[2] = 1;
    CHECK(gm::reload(fast_table, fast_arsenal, 2, &why));
    CHECK(!fast_arsenal.reloading[2]);
    CHECK_EQ(fast_arsenal.magazine[2], std::uint32_t(3));
    CHECK_EQ(fast_arsenal.ammo[2], std::uint32_t(0));
}

void test_switch_slot_and_mask() {
    gm::Weapons table({make_weapon(0, 1, true), make_weapon(1, 1, true)});
    gm::Arsenal arsenal = make_arsenal(0x3, 0, 5);
    gm::FireError why = gm::FireError::none;

    CHECK(gm::switch_slot(table, arsenal, 1, &why));
    CHECK_EQ(arsenal.current, 1);
    CHECK(gm::switch_slot(table, arsenal, 1, &why)); // same slot is a no-op
    CHECK(!gm::switch_slot(table, arsenal, 2, &why));
    CHECK(why == gm::FireError::not_available);
    CHECK(!gm::switch_slot(table, arsenal, 12, &why));
    CHECK(why == gm::FireError::slot_out_of_range);
    CHECK(!gm::switch_slot(table, arsenal, -1, &why));
    CHECK(why == gm::FireError::slot_out_of_range);

    arsenal.mask |= 1u << 2;
    // The table has no entry for slot 2 even though the mask bit exists.
    CHECK(!gm::switch_slot(table, arsenal, 2, &why));
    CHECK(why == gm::FireError::no_weapon);

    arsenal.current = 12;
    gm::CollisionWorld world;
    const gm::ShotResult out = gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(!out.ok);
    CHECK(out.error == gm::FireError::slot_out_of_range);
}

void test_hitscan_hit_miss() {
    gm::Weapon weapon = make_weapon(0, 1, true);
    weapon.damage = 25.0;
    weapon.range = 100.0;
    gm::Weapons table({weapon});
    gm::CollisionWorld world = build_world(wall_x(10.0));

    gm::Arsenal hit_arsenal = make_arsenal(0x1, 0, 5);
    const gm::ShotResult hit =
        gm::fire(table, hit_arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(hit.ok);
    CHECK(hit.hit);
    CHECK(!hit.projectile);
    CHECK_NEAR(hit.distance, 10.0, 1e-12);
    CHECK_NEAR(hit.point.x, 10.0, 1e-9);
    CHECK_NEAR(hit.point.y, 0.0, 1e-9);
    CHECK_NEAR(hit.point.z, 0.0, 1e-9);
    CHECK_NEAR(hit.time, 0.0, 0.0);
    CHECK_NEAR(hit.damage, 25.0, 0.0);

    // Target behind the shooter: a +X wall is invisible to a -X ray.
    gm::Arsenal miss_arsenal = make_arsenal(0x1, 0, 5);
    const gm::ShotResult behind =
        gm::fire(table, miss_arsenal, {0.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, world);
    CHECK(behind.ok);
    CHECK(!behind.hit);
    CHECK_NEAR(behind.distance, 100.0, 1e-12);
    CHECK_NEAR(behind.point.x, -100.0, 1e-12);
    CHECK_NEAR(behind.normal.x, 0.0, 0.0);

    gm::CollisionWorld empty;
    gm::Arsenal empty_arsenal = make_arsenal(0x1, 0, 5);
    const gm::ShotResult no_world =
        gm::fire(table, empty_arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, empty);
    CHECK(no_world.ok);
    CHECK(!no_world.hit);
    CHECK_NEAR(no_world.distance, 100.0, 1e-12);
}

void test_spread_determinism() {
    gm::Weapon weapon = make_weapon(0, 1, true);
    weapon.spread = 0.1;
    weapon.spread_y = 0.05;
    weapon.range = 1000.0;
    gm::Weapons table({weapon});
    gm::CollisionWorld world;

    const gm::Arsenal base = make_arsenal(0x1, 0, 10);
    gm::Arsenal a = base;
    gm::Arsenal b = base;
    const gm::ShotResult first_a =
        gm::fire(table, a, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    const gm::ShotResult first_b =
        gm::fire(table, b, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(first_a.ok && first_b.ok);
    CHECK(first_a.direction.x == first_b.direction.x);
    CHECK(first_a.direction.y == first_b.direction.y);
    CHECK(first_a.direction.z == first_b.direction.z);
    CHECK(first_a.spread_x == first_b.spread_x);
    CHECK(first_a.spread_y == first_b.spread_y);
    CHECK(first_a.spread_x >= -1.0 && first_a.spread_x <= 1.0);
    CHECK(first_a.spread_y >= -1.0 && first_a.spread_y <= 1.0);

    // The stream advances: shot 2 differs from shot 1 but matches its twin.
    gm::tick(table, a, 1);
    gm::tick(table, b, 1);
    const gm::ShotResult second_a =
        gm::fire(table, a, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    const gm::ShotResult second_b =
        gm::fire(table, b, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(second_a.ok && second_b.ok);
    CHECK(second_a.spread_x == second_b.spread_x);
    CHECK(second_a.spread_y == second_b.spread_y);
    CHECK(second_a.spread_x != first_a.spread_x);

    // A different seed gives a different direction for the same shot index.
    gm::Arsenal c = base;
    c.seed ^= 0xffffffffffffffffULL;
    const gm::ShotResult other =
        gm::fire(table, c, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(other.ok);
    CHECK(other.spread_x != first_a.spread_x);

    // Zero spread preserves the normalized input direction exactly.
    gm::Weapon exact = make_weapon(1, 1, true);
    gm::Weapons exact_table({weapon, exact});
    gm::Arsenal exact_arsenal = make_arsenal(0x3, 1, 5);
    const gm::ShotResult straight =
        gm::fire(exact_table, exact_arsenal, {0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, world);
    CHECK(straight.ok);
    CHECK_NEAR(straight.direction.x, 1.0, 0.0);
    CHECK_NEAR(straight.direction.y, 0.0, 0.0);
    CHECK_NEAR(straight.direction.z, 0.0, 0.0);
    CHECK_NEAR(straight.spread_x, 0.0, 0.0);
    CHECK_NEAR(straight.spread_y, 0.0, 0.0);
}

void test_projectile() {
    gm::Weapon weapon = make_weapon(0, 1, false);
    weapon.projectile_speed = 100.0;
    weapon.range = 100.0;
    gm::Weapons table({weapon});
    gm::CollisionWorld world = build_world(wall_x(50.0));

    gm::Arsenal arsenal = make_arsenal(0x1, 0, 5);
    const gm::ShotResult hit =
        gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(hit.ok);
    CHECK(hit.projectile);
    CHECK(hit.hit);
    CHECK_NEAR(hit.distance, 50.0, 1e-12);
    CHECK_NEAR(hit.time, 0.5, 1e-12);

    gm::Arsenal miss_arsenal = make_arsenal(0x1, 0, 5);
    const gm::ShotResult miss =
        gm::fire(table, miss_arsenal, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, world);
    CHECK(miss.ok);
    CHECK(!miss.hit);
    CHECK_NEAR(miss.time, 1.0, 1e-12);

    // Speed 0 is rejected before ammo is spent.
    gm::Weapon frozen = weapon;
    frozen.slot = 1;
    frozen.projectile_speed = 0.0;
    gm::Weapons frozen_table({weapon, frozen});
    gm::Arsenal frozen_arsenal = make_arsenal(0x3, 1, 5);
    const gm::ShotResult bad =
        gm::fire(frozen_table, frozen_arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(!bad.ok);
    CHECK(bad.error == gm::FireError::bad_projectile_speed);
    CHECK_EQ(frozen_arsenal.ammo[1], std::uint32_t(5));
    CHECK_EQ(frozen_arsenal.cooldown[1], 0);

    // 0x46f0f0: speed = base + charge * operand[9], both in units/tick.
    gm::Weapon charged = weapon;
    charged.slot = 1;
    charged.projectile_charge = 10.0;
    gm::Weapons charged_table({weapon, charged});
    gm::Arsenal charged_arsenal = make_arsenal(0x3, 1, 5);
    const gm::ShotResult powered = gm::fire(
        charged_table, charged_arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world, 0.5);
    CHECK(powered.ok);
    CHECK_NEAR(powered.projectile_speed, 100.0 + 0.5 * 10.0 * 91.0, 1e-12);
    CHECK_NEAR(powered.time, 50.0 / (100.0 + 0.5 * 10.0 * 91.0), 1e-12);

    gm::Arsenal bad_charge_arsenal = make_arsenal(0x3, 1, 5);
    const gm::ShotResult bad_charge = gm::fire(
        charged_table, bad_charge_arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, world, -1.0);
    CHECK(!bad_charge.ok);
    CHECK(bad_charge.error == gm::FireError::bad_charge);
    CHECK_EQ(bad_charge_arsenal.ammo[1], std::uint32_t(5));
    CHECK_EQ(bad_charge_arsenal.cooldown[1], 0);
}

void test_recoil() {
    const gm::Vec3d forward = gm::recoil_velocity_delta(0.0, 0.0, 100.0);
    CHECK_NEAR(forward.x, 0.0, 0.0);
    CHECK_NEAR(forward.y, -100.0, 1e-12);
    CHECK_NEAR(forward.z, 0.0, 0.0);

    const gm::Vec3d right = gm::recoil_velocity_delta(kPi / 2.0, 0.0, 100.0);
    CHECK_NEAR(right.x, -100.0, 1e-12);
    CHECK_NEAR(right.y, 0.0, 1e-12);

    // Looking straight up/down projects nothing onto the X/Y plane.
    const gm::Vec3d vertical = gm::recoil_velocity_delta(0.4, kPi / 2.0, 100.0);
    CHECK_NEAR(vertical.x, 0.0, 1e-12);
    CHECK_NEAR(vertical.y, 0.0, 1e-12);

    const gm::Vec3d nan = gm::recoil_velocity_delta(
        std::numeric_limits<double>::quiet_NaN(), 0.0, 100.0);
    CHECK_NEAR(nan.x, 0.0, 0.0);
    CHECK_NEAR(nan.y, 0.0, 0.0);

    gm::PlayerState player;
    player.yaw = 0.0;
    player.vel = {1.0, 2.0, 3.0};
    gm::apply_recoil(player, 0.0, 10.0);
    CHECK_NEAR(player.vel.x, 1.0, 0.0);
    CHECK_NEAR(player.vel.y, -8.0, 1e-12);
    CHECK_NEAR(player.vel.z, 3.0, 0.0);
}

void test_damage_and_armor() {
    gm::Vitals vitals;
    vitals.armor = 10000;
    vitals.armor_max = 10000;
    vitals.shield_max = 8000;
    vitals.shields = {8000, 8000, 8000, 8000};

    gm::DamageResult result = gm::apply_damage(vitals, 10.0, gm::ShieldSide::front);
    CHECK(result.ok);
    CHECK_EQ(result.shield_absorbed, 1000);
    CHECK_EQ(result.armor_absorbed, 0);
    CHECK_EQ(result.overflow, 0);
    CHECK_EQ(vitals.shields[0], 7000);
    CHECK_EQ(vitals.armor, 10000);
    CHECK(!result.dead);

    // 100 points against the back quadrant: its full 8000 hundredths absorb
    // the first 80 points, the remaining 2000 go to the armor.
    result = gm::apply_damage(vitals, 100.0, gm::ShieldSide::back);
    CHECK(result.ok);
    CHECK_EQ(result.shield_absorbed, 8000);
    CHECK_EQ(result.armor_absorbed, 2000);
    CHECK_EQ(vitals.shields[2], 0); // back = -Y = index 2
    CHECK_EQ(vitals.armor, 8000);
    CHECK_EQ(vitals.shields[0], 7000); // other quadrants are untouched

    // Overkill clamps both pools and reports the overflow.
    result = gm::apply_damage(vitals, 1000.0, gm::ShieldSide::left);
    CHECK(result.ok);
    CHECK_EQ(result.shield_absorbed, 8000);
    CHECK_EQ(result.armor_absorbed, 8000);
    CHECK_EQ(result.overflow, 84000);
    CHECK_EQ(vitals.shields[3], 0); // left = -X = index 3
    CHECK_EQ(vitals.armor, 0);
    CHECK(result.dead);

    gm::Vitals bad = vitals;
    CHECK(!gm::apply_damage(bad, -1.0).ok);
    CHECK(!gm::apply_damage(bad, std::numeric_limits<double>::quiet_NaN()).ok);
    CHECK(!gm::apply_damage(bad, 1.0, static_cast<gm::ShieldSide>(9)).ok);
}

void test_damage_gauge() {
    auto light = [] {
        gm::Vitals v;
        v.armor = 10000;
        v.armor_max = 10000;
        v.shield_max = 8000;
        v.shields = {8000, 8000, 8000, 8000};
        return v;
    };

    // Vector 1: one front plate, no spill at exactly zero.
    gm::Vitals v = light();
    gm::GaugeDamageResult r = gm::apply_damage_gauge(v, 400, 0, 0, 0);
    CHECK(r.ok);
    CHECK_EQ(v.shields[0], 7600);
    CHECK_EQ(v.armor, 10000);
    CHECK(!r.broken[0]);
    CHECK_EQ(r.stun, 0x80000);

    // Vector 2/3: exact zero is not "broken"; the next hit spills into hull.
    for (int i = 0; i < 19; ++i) {
        r = gm::apply_damage_gauge(v, 400, 0, 0, 0);
        CHECK(r.ok);
    }
    CHECK_EQ(v.shields[0], 0);
    CHECK_EQ(v.armor, 10000);
    CHECK(!r.broken[0]);
    r = gm::apply_damage_gauge(v, 400, 0, 0, 0);
    CHECK_EQ(v.shields[0], 0);
    CHECK_EQ(v.armor, 9600);
    CHECK(!r.broken[0]);

    // Vector 4: a plate that crosses zero clamps and marks broken.
    v = light();
    v.shields[0] = 300;
    r = gm::apply_damage_gauge(v, 400, 0, 0, 0);
    CHECK_EQ(v.shields[0], 0);
    CHECK_EQ(v.armor, 9900);
    CHECK(r.broken[0]);

    // Vector 5/7: non-directional and full-plate flags.
    v = light();
    r = gm::apply_damage_gauge(v, 400, 0, 1, 0);
    CHECK_EQ(v.shields[0], 7900);
    CHECK_EQ(v.shields[1], 7900);
    CHECK_EQ(v.armor, 10000);
    CHECK(!r.broken[0]);

    v = light();
    r = gm::apply_damage_gauge(v, 400, 0, 3, 0);
    CHECK_EQ(v.shields[0], 7600);
    CHECK_EQ(v.shields[3], 7600);
    CHECK_EQ(v.armor, 10000);

    // Vector 10/11: half-health threshold and healing.
    v = light();
    v.armor = 6000;
    r = gm::apply_damage_gauge(v, 1500, 0, 0, 1);
    CHECK_EQ(v.armor, 6000); // selected plate absorbed it
    CHECK_EQ(v.shields[1], 6500);
    r = gm::apply_damage_gauge(v, 0, 0, 0, 0);
    CHECK(!r.low_health);

    v = light();
    v.armor = 6000;
    v.shields[1] = 0;
    r = gm::apply_damage_gauge(v, 1500, 0, 0, 1);
    CHECK_EQ(v.armor, 4500);
    CHECK(r.low_health);
    CHECK(!r.recovered);
    r = gm::apply_damage_gauge(v, 0, -1500, 0, 0);
    CHECK_EQ(v.armor, 6000);
    CHECK(r.recovered);

    // Vector 12/13: multipliers, then flags&4 bypasses them.
    v = light();
    v.mult_plate = 0.2;
    v.mult_hull = 0.1;
    r = gm::apply_damage_gauge(v, 400, 1500, 0, 0);
    CHECK_EQ(v.shields[0], 7920);
    CHECK_EQ(v.armor, 9850);
    CHECK(r.ok);

    v = light();
    v.mult_plate = 0.2;
    v.mult_hull = 0.1;
    r = gm::apply_damage_gauge(v, 400, 1500, 4, 0);
    CHECK_EQ(v.shields[0], 7600);
    CHECK_EQ(v.armor, 8500);

    // Vector 15: status -1 is burn, not damage.
    v = light();
    r = gm::apply_damage_gauge(v, -1, 150, 0, 0);
    CHECK(r.ok);
    CHECK_EQ(v.armor, 10000);
    CHECK_EQ(v.shields[0], 8000);
    CHECK_EQ(v.burn_timer, 136);
    CHECK((v.status_flags & 0x20U) != 0U);

    gm::Vitals bad = light();
    CHECK(!gm::apply_damage_gauge(bad, 400, 0, 0, 9).ok);
}

void test_blast_and_thing_damage() {
    gm::Blast blast;
    blast.plate = 10000;
    blast.hull = 0;
    blast.radius = 100;
    blast.event = 42;
    CHECK_EQ(gm::blast_damage(blast, 0), 10000);
    CHECK_EQ(gm::blast_damage(blast, 50), 5000);
    CHECK_EQ(gm::blast_damage(blast, 99), 100);
    CHECK_EQ(gm::blast_damage(blast, 100), 0);
    CHECK_EQ(gm::blast_damage(blast, 101), 0);

    blast.radius = 0;
    CHECK_EQ(gm::blast_damage(blast, 0), 0); // divide-by-zero guard
    blast.radius = 1000;
    blast.plate = -1;
    blast.hull = 150;
    CHECK_EQ(gm::blast_damage(blast, 999), -1); // status is not scaled
    CHECK_EQ(gm::blast_hull_damage(blast, 999), 150);

    // Spherical query: no geometry/LOS; source exclusion is bit 0.
    blast.plate = 1000;
    blast.radius = 100;
    const std::array<gm::Vec3d, 2> players{{{0.0, 0.0, 0.0}, {200.0, 0.0, 0.0}}};
    const std::array<gm::Vec3d, 2> things{{{50.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
    const std::array<bool, 2> alive{true, false};
    const gm::BlastResult hit_all =
        gm::apply_blast({0.0, 0.0, 0.0}, blast, -1, -1, players, things, alive);
    CHECK_EQ(hit_all.hits.size(), 2U); // player 0 + active thing 0
    CHECK(hit_all.chain_reaction);
    if (hit_all.hits.size() == 2) {
        CHECK(hit_all.hits[0].is_player);
        CHECK_EQ(hit_all.hits[0].plate, 1000);
        CHECK_EQ(hit_all.hits[1].distance, 50);
        CHECK_EQ(hit_all.hits[1].plate, 500);
    }
    blast.flags = 1;
    const gm::BlastResult hit_source =
        gm::apply_blast({0.0, 0.0, 0.0}, blast, 0, 0, players, things, alive);
    CHECK_EQ(hit_source.hits.size(), 0U); // both sources excluded

    // THING damage: a 1000 plate against a 500 shield spills only 500 to HP.
    gm::ThingVitals thing;
    thing.hp = 1000;
    thing.max_hp = 1000;
    thing.shields = {500, 0, 0, 0};
    thing.alive = true;
    gm::ThingDamageResult tr = gm::apply_thing_damage(thing, 1000, 0, 0, 0);
    CHECK(tr.ok);
    CHECK_EQ(thing.shields[0], 0);
    CHECK_EQ(thing.hp, 500);
    CHECK(tr.broken[0]);
    CHECK(!tr.dead);
    CHECK(tr.damaged_event);
    CHECK(!tr.low_health_event); // flags2 bit 0x1000 is clear

    // Low-health event only when THING flags2 bit 0x1000 is set and the
    // integer half-health threshold is crossed (Ghidra 0x470500).
    thing.hp = 1000;
    thing.flags2 = 0x1000;
    tr = gm::apply_thing_damage(thing, 0, 600, 0, -1);
    CHECK(!tr.dead);
    CHECK(tr.low_health_event);
    CHECK_EQ(thing.hp, 400);

    // Lethal blast through the front quadrant.
    thing.hp = 800;
    thing.shields = {0, 500, 0, 500};
    tr = gm::apply_thing_damage(thing, 10000, 0, 0, 0);
    CHECK(tr.dead);
    CHECK_EQ(thing.hp, 0);
    CHECK_EQ(thing.shields[0], 0);

    // THING status path (0x470500): -1 burns, -2 freezes unless burning.
    gm::ThingVitals status;
    status.hp = 1000;
    status.max_hp = 1000;
    status.alive = true;
    gm::ThingDamageResult sr = gm::apply_thing_damage(status, -1, 200, 0, 0);
    CHECK(sr.ok);
    CHECK_EQ(status.burn_timer, 182); // 200 * 91 / 100
    CHECK_EQ(status.status_flags & 0x40U, 0x40U);
    gm::ThingVitals frozen;
    frozen.hp = 1000;
    frozen.max_hp = 1000;
    frozen.alive = true;
    sr = gm::apply_thing_damage(frozen, -2, 0, 0, 0);
    CHECK(sr.ok);
    CHECK_EQ(frozen.status_flags & 0x18U, 0x18U);
    sr = gm::apply_thing_damage(frozen, -2, 0, 0, 0);
    CHECK(sr.ok);
    CHECK_EQ(frozen.status_flags & 0x18U, 0x18U);

    // Runtime Thing bridge: the same gauges go in and out of world/things.
    gm::Thing runtime_thing;
    runtime_thing.health = 1000;
    runtime_thing.max_health = 1000;
    runtime_thing.shields = {500, 0, 0, 0};
    runtime_thing.shield_max = runtime_thing.shields;
    runtime_thing.alive = true;
    CHECK_EQ(gm::thing_vitals(runtime_thing).hp, 1000);
    tr = gm::apply_thing_damage(runtime_thing, 1000, 0, 0, 0);
    CHECK(tr.ok);
    CHECK_EQ(runtime_thing.health, 500);
    CHECK_EQ(runtime_thing.shields[0], 0);
    CHECK(runtime_thing.alive);
    runtime_thing.alive = false;
    CHECK(!gm::apply_thing_damage(runtime_thing, 1, 0, 0, 0).ok);

    // max_health == 0 is the original damage-immune marker.
    gm::ThingVitals immune;
    immune.hp = 100;
    immune.max_hp = 0;
    immune.alive = true;
    tr = gm::apply_thing_damage(immune, 1000, 0, 0, 0);
    CHECK(tr.ok);
    CHECK(!tr.dead);
    CHECK_EQ(immune.hp, 100);
    CHECK_EQ(immune.shields[0], 0);

    // The same immunity must survive the Thing bridge (DMG-1).
    gm::Thing immune_thing;
    immune_thing.id = 12;
    immune_thing.health = 0;
    immune_thing.max_health = 0;
    immune_thing.alive = true;
    tr = gm::apply_thing_damage(immune_thing, 100, 100, 0, -1);
    CHECK(tr.ok);
    CHECK(!tr.dead);
    CHECK(immune_thing.alive);
    CHECK_EQ(immune_thing.health, 0);
}

void test_runtime_blast_bridge() {
    std::string error;
    const auto table = gm::Weapons::load(kGame / "level0.sbt", &error);
    CHECK(table.has_value());
    if (!table) {
        return;
    }
    const gm::Weapon* rocket = nullptr;
    const gm::Weapon* plasma = nullptr;
    const gm::Weapon* rgm = nullptr;
    const gm::Weapon* nuke = nullptr;
    const gm::Weapon* emp = nullptr;
    for (const gm::Weapon& weapon : table->table()) {
        if (weapon.name == "ROCKET") {
            rocket = &weapon;
        } else if (weapon.name == "PLASMA CANNON") {
            plasma = &weapon;
        } else if (weapon.name == "RGM") {
            rgm = &weapon;
        } else if (weapon.name == "TAC NUKE") {
            nuke = &weapon;
        } else if (weapon.name == "EMP GUN") {
            emp = &weapon;
        }
    }
    CHECK(rocket != nullptr);
    if (rocket == nullptr) {
        return;
    }

    const std::array<gm::Vec3d, 1> things{{{50.0, 0.0, 0.0}}};
    const std::array<bool, 1> alive{true};
    std::array<gm::ThingVitals, 1> vitals{};
    vitals[0].hp = 1000;
    vitals[0].max_hp = 1000;
    vitals[0].alive = true;
    std::span<const gm::Vec3d> no_players;
    std::span<gm::Vitals> no_player_vitals;

    // Air impact: direct 10000@80 plus the air child 12000@256.
    const gm::BlastDamageResult air = gm::apply_weapon_blast(
        *rocket, false, {0.0, 0.0, 0.0}, false, true, true, -1, -1,
        no_players, no_player_vitals, things, alive, vitals);
    CHECK_EQ(air.hits.size(), 2U);
    CHECK(vitals[0].hp <= 0);
    CHECK(!vitals[0].alive || vitals[0].hp == 0);

    // Water impact: direct neutral blast + 10000@512, not the 256 air child.
    vitals[0].hp = 20000;
    vitals[0].alive = true;
    const gm::BlastDamageResult water = gm::apply_weapon_blast(
        *rocket, false, {0.0, 0.0, 0.0}, true, true, true, -1, -1,
        no_players, no_player_vitals, things, alive, vitals);
    CHECK_EQ(water.hits.size(), 2U);
    if (water.hits.size() == 2) {
        CHECK_EQ(water.hits[0].hit.plate, 3750); // 10000 * (80-50)/80
        CHECK_EQ(water.hits[1].hit.plate, 9023); // 10000 * (512-50)/512
    }

    // The expire vector uses the same bridge (collide == expire for ROCKET).
    vitals[0].hp = 20000;
    vitals[0].alive = true;
    const gm::BlastDamageResult expire = gm::apply_weapon_blast(
        *rocket, true, {0.0, 0.0, 0.0}, false, true, true, -1, -1,
        no_players, no_player_vitals, things, alive, vitals);
    CHECK_EQ(expire.hits.size(), 2U);

    // PLASMA power-up picks TT36/37/38 and its own expire vector.
    if (plasma != nullptr) {
        const auto at_power = [&](double power) {
            vitals[0].hp = 20000;
            vitals[0].alive = true;
            return gm::apply_weapon_blast(
                *plasma, true, {0.0, 0.0, 0.0}, false, true, true, -1, -1,
                no_players, no_player_vitals, things, alive, vitals, power);
        };
        const gm::BlastDamageResult full = at_power(1.0);
        const gm::BlastDamageResult mid = at_power(0.5);
        const gm::BlastDamageResult low = at_power(0.1);
        CHECK_EQ(full.hits.size(), 2U);
        CHECK_EQ(mid.hits.size(), 2U);
        CHECK_EQ(low.hits.size(), 2U);
        if (full.hits.size() == 2 && mid.hits.size() == 2 &&
            low.hits.size() == 2) {
            CHECK_EQ(full.hits[1].hit.plate, 8100);  // 10800@200, d=50
            CHECK_EQ(mid.hits[1].hit.plate, 3333);   // 5000@150
            CHECK_EQ(low.hits[1].hit.plate, 1466);   // 2200@150
        }
    }

    // RGM has no collide damage: only the expire vector fires.
    if (rgm != nullptr) {
        vitals[0].hp = 20000;
        vitals[0].alive = true;
        const gm::BlastDamageResult rgm_blasts = gm::apply_weapon_blast(
            *rgm, true, {0.0, 0.0, 0.0}, false, true, true, -1, -1,
            no_players, no_player_vitals, things, alive, vitals);
        CHECK_EQ(rgm_blasts.hits.size(), 2U);
        if (rgm_blasts.hits.size() == 2) {
            CHECK_EQ(rgm_blasts.hits[0].hit.plate, 0);
            CHECK_EQ(rgm_blasts.hits[1].hit.plate, 12070); // 15000@256
        }
    }

    // EMP rings run at the shot centre, not at a later hit point.
    if (emp != nullptr) {
        vitals[0].hp = 20000;
        vitals[0].alive = true;
        const gm::BlastDamageResult rings = gm::apply_weapon_inline(
            *emp, {0.0, 0.0, 0.0}, false, true, true, -1, -1,
            no_players, no_player_vitals, things, alive, vitals);
        CHECK_EQ(rings.hits.size(), 5U);
        CHECK(vitals[0].hp < 20000);
    }

    // TAC NUKE's guarded default branch contributes exactly three blasts.
    if (nuke != nullptr) {
        vitals[0].hp = 200000;
        vitals[0].alive = true;
        const gm::BlastDamageResult nuke_blasts = gm::apply_weapon_blast(
            *nuke, false, {0.0, 0.0, 0.0}, false, false, false, -1, -1,
            no_players, no_player_vitals, things, alive, vitals);
        CHECK_EQ(nuke_blasts.hits.size(), 3U);
    }
}

void test_weapon_corpus() {
    const auto find = [](const gm::Weapons& table,
                         std::string_view name) -> const gm::Weapon* {
        for (const gm::Weapon& weapon : table.table()) {
            if (weapon.name == name) {
                return &weapon;
            }
        }
        return nullptr;
    };
    std::size_t levels = 0;
    for (const auto& entry : fs::directory_iterator(kGame)) {
        if (entry.path().extension() != ".sbt") {
            continue;
        }
        std::string error;
        const auto table = gm::Weapons::load(entry.path(), &error);
        if (!table) {
            std::fprintf(stderr, "corpus load %s: %s\n",
                         entry.path().filename().string().c_str(),
                         error.c_str());
            ++g_failures;
            continue;
        }
        ++levels;

        const gm::Weapon* rgm = find(*table, "RGM");
        CHECK(rgm != nullptr);
        if (rgm != nullptr) {
            CHECK(!rgm->hitscan);
            CHECK_EQ(rgm->thing_type, 32);
            CHECK_EQ(rgm->impact_blasts.size(), 0U);
            CHECK_EQ(rgm->expire_blasts.size(), 3U);
            CHECK_NEAR(rgm->projectile_speed, 18.0 * 91.0, 1e-9);
        }
        const gm::Weapon* rocket = find(*table, "ROCKET");
        CHECK(rocket != nullptr);
        if (rocket != nullptr) {
            CHECK_EQ(rocket->thing_type, 31);
            CHECK_EQ(rocket->select_blasts(false, false).size(), 2U);
            CHECK_EQ(rocket->select_blasts(false, true).size(), 2U);
            CHECK_NEAR(rocket->projectile_speed, 18.0 * 91.0, 1e-9);
        }
        const gm::Weapon* nuke = find(*table, "TAC NUKE");
        CHECK(nuke != nullptr);
        if (nuke != nullptr) {
            CHECK_EQ(nuke->thing_type, 33);
            const std::string file = entry.path().filename().string();
            const std::size_t imp =
                file == "level20.sbt" ? 13U : (file == "level24.sbt" ? 16U : 15U);
            const std::size_t air =
                file == "level24.sbt" ? 4U : 3U;
            const std::size_t water =
                file == "level20.sbt" ? 2U : (file == "level24.sbt" ? 4U : 3U);
            CHECK_EQ(nuke->impact_blasts.size(), imp);
            CHECK_EQ(nuke->select_blasts(false, false, false, false).size(),
                     air);
            CHECK_EQ(nuke->select_blasts(false, true, false, false).size(),
                     water);
        }
        const gm::Weapon* plasma = find(*table, "PLASMA CANNON");
        CHECK(plasma != nullptr);
        if (plasma != nullptr) {
            CHECK_EQ(plasma->thing_type, 36);
            CHECK_EQ(plasma->thing_variants.size(), 3U);
            if (plasma->thing_variants.size() == 3) {
                CHECK_EQ(plasma->thing_variants[0].thing_type, 36);
                CHECK_EQ(plasma->thing_variants[1].thing_type, 37);
                CHECK_EQ(plasma->thing_variants[2].thing_type, 38);
            }
        }
        const gm::Weapon* cannon = find(*table, "CANNON");
        CHECK(cannon != nullptr);
        if (cannon != nullptr) {
            CHECK_EQ(cannon->impact_blasts.size(), 3U);
            CHECK_NEAR(cannon->damage, 15.0, 0.0);
        }
    }
    CHECK_EQ(levels, 36U);
}

void test_negatives_synthetic() {
    gm::Weapons table({make_weapon(0, 1, false)});
    gm::CollisionWorld world;
    gm::Arsenal arsenal = make_arsenal(0x1, 0, 5);

    const gm::ShotResult nan = gm::fire(
        table, arsenal, {0.0, 0.0, 0.0},
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, world);
    CHECK(!nan.ok);
    CHECK(nan.error == gm::FireError::bad_direction);
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(5));

    const gm::ShotResult zero =
        gm::fire(table, arsenal, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, world);
    CHECK(!zero.ok);
    CHECK(zero.error == gm::FireError::bad_direction);

    const gm::ShotResult origin = gm::fire(
        table, arsenal,
        {std::numeric_limits<double>::infinity(), 0.0, 0.0}, {1.0, 0.0, 0.0}, world);
    CHECK(!origin.ok);
    CHECK(origin.error == gm::FireError::bad_origin);
}

void test_real_weapon_modes() {
    std::string error;
    const auto table = gm::Weapons::load(kGame / "level0.sbt", &error);
    CHECK(table.has_value());
    if (!table) {
        std::fprintf(stderr, "weapons modes: %s\n", error.c_str());
        return;
    }
    const auto find = [&](std::string_view name) -> const gm::Weapon* {
        for (const gm::Weapon& weapon : table->table()) {
            if (weapon.name == name) {
                return &weapon;
            }
        }
        return nullptr;
    };
    const gm::Weapon* mg = find("MACHINE GUN");
    const gm::Weapon* cannon = find("CANNON");
    const gm::Weapon* auto_cannon = find("AUTOCANNON");
    const gm::Weapon* rail = find("RAILGUN");
    const gm::Weapon* rocket = find("ROCKET");
    CHECK(mg != nullptr && cannon != nullptr && auto_cannon != nullptr &&
          rail != nullptr && rocket != nullptr);
    if (mg != nullptr) {
        CHECK(mg->hitscan);
        CHECK_NEAR(mg->damage, 4.0, 0.0);
    }
    if (cannon != nullptr) {
        CHECK(cannon->hitscan);
        CHECK_NEAR(cannon->damage, 15.0, 0.0);
    }
    if (auto_cannon != nullptr) {
        CHECK(auto_cannon->hitscan);
        CHECK_NEAR(auto_cannon->damage, 15.0, 0.0);
    }
    if (rail != nullptr) {
        CHECK(rail->hitscan);
        CHECK_NEAR(rail->damage, 60.0, 0.0);
    }
    const gm::Weapon* fusion = find("FUSIONBEAM");
    const gm::Weapon* microwave = find("MICROWAVE GUN");
    const gm::Weapon* emp = find("EMP GUN");
    const gm::Weapon* disruptor = find("DISRUPTORFIELD");
    const gm::Weapon* meson = find("MESON GUN");
    const gm::Weapon* disint = find("DISINTEGRATOR");
    if (cannon != nullptr) {
        const auto air = cannon->select_blasts(false, false);
        const auto water = cannon->select_blasts(false, true);
        CHECK_EQ(air.size(), 2U);
        CHECK_EQ(water.size(), 2U);
        if (air.size() == 2) {
            CHECK_EQ(air[0].radius, 50);
            CHECK_EQ(air[1].plate, 1500);
            CHECK_EQ(air[1].radius, 200);
        }
        if (water.size() == 2) {
            CHECK_EQ(water[1].radius, 400);
        }
    }
    if (fusion != nullptr) {
        CHECK(fusion->hitscan);
        CHECK_NEAR(fusion->damage, 3.0, 0.0);
        CHECK_EQ(fusion->select_blasts(false, false).size(), 2U);
        CHECK_EQ(fusion->select_blasts(false, true).size(), 2U);
    }
    if (microwave != nullptr) {
        CHECK(microwave->hitscan);
        CHECK_NEAR(microwave->damage, 0.0, 0.0);
        CHECK_EQ(microwave->impact_blasts.size(), 2U);
        if (microwave->impact_blasts.size() == 2) {
            CHECK_EQ(microwave->impact_blasts[1].plate, 0);
            CHECK_EQ(microwave->impact_blasts[1].hull, 500);
            CHECK_EQ(microwave->impact_blasts[1].radius, 128);
        }
    }
    if (emp != nullptr) {
        CHECK(emp->hitscan);
        CHECK_NEAR(emp->damage, 0.0, 0.0);
        CHECK_EQ(emp->impact_blasts.size(), 0U);
        CHECK_EQ(emp->inline_blasts.size(), 5U);
        if (emp->inline_blasts.size() == 5) {
            CHECK_EQ(emp->inline_blasts[0].hull, 0);
            CHECK_EQ(emp->inline_blasts[0].radius, 4500);
            CHECK_EQ(emp->inline_blasts[1].hull, 1500);
            CHECK_EQ(emp->inline_blasts[1].radius, 76);
            CHECK_EQ(emp->inline_blasts[2].hull, 2000);
            CHECK_EQ(emp->inline_blasts[2].radius, 256);
            CHECK_EQ(emp->inline_blasts[3].radius, 512);
            CHECK_EQ(emp->inline_blasts[4].hull, 1500);
            CHECK_EQ(emp->inline_blasts[4].radius, 720);
        }
    }
    if (disruptor != nullptr) {
        CHECK(disruptor->hitscan);
        CHECK_NEAR(disruptor->damage, 0.0, 0.0);
        CHECK_EQ(disruptor->impact_blasts.size(), 1U);
        if (!disruptor->impact_blasts.empty()) {
            CHECK_EQ(disruptor->impact_blasts[0].plate, 12000);
            CHECK_EQ(disruptor->impact_blasts[0].radius, 128);
            CHECK(disruptor->impact_blasts[0].no_direction);
        }
    }
    if (meson != nullptr) {
        CHECK(meson->hitscan);
        CHECK_NEAR(meson->damage, 0.0, 0.0);
        CHECK_EQ(meson->impact_blasts.size(), 2U);
    }
    if (disint != nullptr) {
        CHECK(!disint->hitscan);
        CHECK_EQ(disint->thing_type, 34);
        CHECK_NEAR(disint->projectile_speed, 16.0 * 91.0, 1e-9);
        CHECK_NEAR(disint->secondary_damage, 400.0, 0.0);
        CHECK_NEAR(disint->secondary_range, 200.0, 0.0);
        CHECK_EQ(disint->impact_blasts.size(), 1U);
        if (!disint->impact_blasts.empty()) {
            CHECK_EQ(disint->impact_blasts[0].radius, 200);
        }
    }
    const gm::Weapon* plasma = find("PLASMA CANNON");
    if (plasma != nullptr) {
        CHECK_EQ(plasma->thing_type, 36);
        CHECK_EQ(plasma->thing_variants.size(), 3U);
        if (plasma->thing_variants.size() == 3) {
            CHECK_NEAR(plasma->thing_variants[0].min_power_up, 0.66, 1e-12);
            CHECK_EQ(plasma->thing_variants[0].thing_type, 36);
            CHECK_EQ(plasma->thing_variants[1].thing_type, 37);
            CHECK_EQ(plasma->thing_variants[2].thing_type, 38);
            CHECK_EQ(plasma->thing_variants[0].expire_blasts.size(), 2U);
            CHECK_EQ(plasma->thing_variants[1].expire_blasts.size(), 2U);
            CHECK_EQ(plasma->thing_variants[2].expire_blasts.size(), 2U);
            if (plasma->thing_variants[0].expire_blasts.size() == 2) {
                CHECK_EQ(plasma->thing_variants[0].expire_blasts[1].plate,
                         10800);
                CHECK_EQ(plasma->thing_variants[1].expire_blasts[1].plate,
                         5000);
                CHECK_EQ(plasma->thing_variants[2].expire_blasts[1].plate,
                         2200);
            }
        }
        const gm::ThingVariant* full = plasma->variant_for_power(1.0);
        const gm::ThingVariant* mid = plasma->variant_for_power(0.5);
        const gm::ThingVariant* low = plasma->variant_for_power(0.1);
        CHECK(full != nullptr && full->thing_type == 36);
        CHECK(mid != nullptr && mid->thing_type == 37);
        CHECK(low != nullptr && low->thing_type == 38);
        const auto full_blasts =
            plasma->select_blasts_at_power(1.0, true, false);
        const auto mid_blasts =
            plasma->select_blasts_at_power(0.5, true, false);
        const auto low_blasts =
            plasma->select_blasts_at_power(0.1, true, false);
        CHECK_EQ(full_blasts.size(), 2U);
        CHECK_EQ(mid_blasts.size(), 2U);
        CHECK_EQ(low_blasts.size(), 2U);
        if (full_blasts.size() == 2 && mid_blasts.size() == 2 &&
            low_blasts.size() == 2) {
            CHECK_EQ(full_blasts[1].plate, 10800);
            CHECK_EQ(mid_blasts[1].plate, 5000);
            CHECK_EQ(low_blasts[1].plate, 2200);
        }
    }
    const gm::Weapon* mrl = find("MRL");
    if (mrl != nullptr) {
        CHECK_NEAR(mrl->projectile_speed, 25.0 * 91.0, 1e-9);
    }
    const gm::Weapon* nuke = find("TAC NUKE");
    if (nuke != nullptr) {
        CHECK_NEAR(nuke->projectile_speed, 10.0 * 91.0, 1e-9);
        CHECK_EQ(nuke->impact_blasts.size(), 15U); // all guarded branches kept
        CHECK_EQ(nuke->select_blasts(true, false, false, false).size(), 3U);
        CHECK_EQ(nuke->select_blasts(true, true, false, false).size(), 3U);
    }
    const gm::Weapon* rgm = find("RGM");
    if (rgm != nullptr) {
        CHECK(!rgm->hitscan);
        CHECK_EQ(rgm->thing_type, 32);
        CHECK_NEAR(rgm->projectile_speed, 18.0 * 91.0, 1e-9);
        CHECK_EQ(rgm->impact_event, 0U);
        CHECK_EQ(rgm->impact_blasts.size(), 0U);
        CHECK_EQ(rgm->expire_blasts.size(), 3U);
        if (rgm->expire_blasts.size() == 3) {
            CHECK_EQ(rgm->expire_blasts[0].radius, 50);
            CHECK_EQ(rgm->expire_blasts[0].plate, 0);
            CHECK_EQ(rgm->expire_blasts[1].plate, 15000);
            CHECK_EQ(rgm->expire_blasts[1].radius, 256);
            CHECK(!rgm->expire_blasts[1].air);
            CHECK_EQ(rgm->expire_blasts[2].plate, 15000);
            CHECK_EQ(rgm->expire_blasts[2].radius, 256);
            CHECK(rgm->expire_blasts[2].air);
            CHECK(!rgm->expire_blasts[2].water);
        }
    }
    if (rocket != nullptr) {
        CHECK(!rocket->hitscan);
        CHECK_EQ(rocket->thing_type, 31);
        CHECK_NEAR(rocket->projectile_speed, 18.0 * 91.0, 1e-9);
        CHECK_EQ(rocket->impact_blasts.size(), 3U);
        if (rocket->impact_blasts.size() == 3) {
            const auto& direct = rocket->impact_blasts[0];
            CHECK_EQ(direct.plate, 10000);
            CHECK_EQ(direct.radius, 80);
            CHECK_EQ(direct.event, 0xffff8a41u);
            CHECK(direct.air); // neutral op: runs in both media
            CHECK(direct.water);
            CHECK_EQ(rocket->impact_blasts[1].plate, 10000);
            CHECK_EQ(rocket->impact_blasts[1].radius, 512);
            CHECK(!rocket->impact_blasts[1].air);
            CHECK(rocket->impact_blasts[1].water);
            CHECK_EQ(rocket->impact_blasts[2].plate, 12000);
            CHECK_EQ(rocket->impact_blasts[2].radius, 256);
            CHECK(rocket->impact_blasts[2].air);
            CHECK(!rocket->impact_blasts[2].water);
        }
        CHECK_EQ(rocket->expire_blasts.size(), rocket->impact_blasts.size());
        const auto air = rocket->select_blasts(false, false);
        CHECK_EQ(air.size(), 2U);
        if (air.size() == 2) {
            CHECK_EQ(air[0].radius, 80);
            CHECK_EQ(air[1].radius, 256);
        }
        const auto water = rocket->select_blasts(false, true);
        CHECK_EQ(water.size(), 2U);
        if (water.size() == 2) {
            CHECK_EQ(water[0].radius, 80);
            CHECK_EQ(water[1].radius, 512);
        }
    }
}

// --- real saves ------------------------------------------------------------

struct GoldenSave {
    const char* file;
    std::int32_t current;
    std::array<std::uint32_t, 2> ammo;
    int fire_ticks;
    double range;
    std::uint32_t ammo_left;
    double spread_x;
    double spread_y;
    double damage;
    bool hitscan;
};

constexpr std::array<GoldenSave, 2> kSaves = {{
    {"saved0.sbt", 0, {{2000, 90}}, 17, 7000.0, 1999, -0.2421790113617004,
     -0.646618239477736, 4.0, true},
    {"saved1.sbt", 1, {{1975, 87}}, 80, 8000.0, 86, 0.26468357609715865,
     0.61488239516758347, 15.0, true},
}};

std::vector<std::uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out;
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return out;
}

std::optional<gm::GameState> load_state(const fs::path& save_path) {
    std::vector<std::uint8_t> bytes = read_all(save_path);
    if (bytes.empty()) {
        return std::nullopt;
    }
    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    if (!save) {
        std::fprintf(stderr, "parse_save(%s): %s\n", save_path.c_str(), error.c_str());
        return std::nullopt;
    }
    std::optional<gm::GameState> state =
        gm::GameState::from_save(std::move(*save), "level0", error);
    if (!state) {
        std::fprintf(stderr, "from_save(%s): %s\n", save_path.c_str(), error.c_str());
    }
    return state;
}

void test_real_tables(const gm::Weapons& table) {
    CHECK_EQ(table.size(), std::size_t(19));
    CHECK_EQ(table.ammo_types().size(), std::size_t(15));

    const gm::Weapon* mg = table.at(0);
    CHECK(mg != nullptr);
    if (mg != nullptr) {
        CHECK_EQ(mg->name, std::string("MACHINE GUN"));
        CHECK_EQ(mg->icon, std::string("WICON1"));
        CHECK_EQ(mg->id, std::uint32_t(0xffff8ab5u));
        CHECK_EQ(mg->fire_mode, 1);
        CHECK_EQ(mg->fire_period_ms, 187);
        CHECK_EQ(mg->fire_ticks, 17);
        CHECK_EQ(mg->ammo_type, 0);
        CHECK_EQ(mg->ammo_per_shot, 1);
        CHECK_EQ(mg->projectile_id, std::uint32_t(1000000));
        CHECK_EQ(mg->projectile_alt, std::uint32_t(0));
        CHECK_EQ(mg->weight, std::uint32_t(15));
        CHECK_EQ(mg->flags, std::uint32_t(0));
        CHECK_EQ(mg->class_index, 0);
        CHECK_NEAR(mg->charge_rate, 0.0, 0.0);
        CHECK_NEAR(mg->spread, 0.14054083470239143, 1e-15);
        CHECK_NEAR(mg->spread_y, 0.46630765815499853, 1e-15);
        CHECK_NEAR(mg->range, 7000.0, 0.0);
        CHECK_EQ(mg->muzzle, 0);
        // Resolved from the entity child script: BULLET hitscan, 400/100.
        CHECK_NEAR(mg->damage, 4.0, 0.0);
        CHECK_EQ(mg->magazine, 0);
        CHECK_EQ(mg->reload_ticks, 0);
        CHECK(mg->hitscan);
        CHECK_NEAR(mg->projectile_speed, 12000.0, 0.0);
    }

    const gm::Weapon* cannon = table.at(1);
    CHECK(cannon != nullptr);
    if (cannon != nullptr) {
        CHECK_EQ(cannon->name, std::string("CANNON"));
        CHECK_EQ(cannon->icon, std::string("WICON3"));
        CHECK_EQ(cannon->fire_mode, 1);
        CHECK_EQ(cannon->fire_period_ms, 879);
        CHECK_EQ(cannon->fire_ticks, 80);
        CHECK_EQ(cannon->ammo_type, 1);
        CHECK_EQ(cannon->price, std::uint32_t(50000));
        CHECK_EQ(cannon->weight, std::uint32_t(600));
        CHECK_EQ(cannon->flags, std::uint32_t(3));
        CHECK_EQ(cannon->class_index, 1);
        CHECK_NEAR(cannon->range, 8000.0, 0.0);
        CHECK_EQ(cannon->muzzle, 1);
    }

    const gm::Weapon* laser = table.at(11);
    CHECK(laser != nullptr);
    if (laser != nullptr) {
        CHECK(laser->hitscan);
        CHECK_NEAR(laser->damage, 3.0, 0.0); // air branch, not underwater 1.00
    }
    const gm::Weapon* grenade = table.at(4);
    CHECK(grenade != nullptr);
    if (grenade != nullptr) {
        CHECK(!grenade->hitscan);
        CHECK_EQ(grenade->thing_type, 28);
        CHECK_NEAR(grenade->projectile_charge, 16.0, 0.0); // air, not water 12
        CHECK(grenade->impact_blasts.empty());
        CHECK_EQ(grenade->expire_blasts.size(), 2U);
        if (grenade->expire_blasts.size() == 2) {
            CHECK_EQ(grenade->expire_blasts[0].plate, 8000);
            CHECK_EQ(grenade->expire_blasts[0].radius, 500);
            CHECK(!grenade->expire_blasts[0].air);
            CHECK(grenade->expire_blasts[0].water);
            CHECK_EQ(grenade->expire_blasts[1].plate, 8000);
            CHECK_EQ(grenade->expire_blasts[1].radius, 350);
            CHECK(grenade->expire_blasts[1].air);
            CHECK(!grenade->expire_blasts[1].water);
        }
        CHECK(grenade->select_blasts(false, false).empty());
        const auto air = grenade->select_blasts(true, false);
        CHECK_EQ(air.size(), 1U);
        if (!air.empty()) {
            CHECK_EQ(air[0].radius, 350);
        }
        const auto water = grenade->select_blasts(true, true);
        CHECK_EQ(water.size(), 1U);
        if (!water.empty()) {
            CHECK_EQ(water[0].radius, 500);
        }
    }

    CHECK_EQ(table.ammo_types()[0].name, std::string("BULLETS"));
    CHECK_EQ(table.ammo_types()[0].max, std::uint32_t(5000));
    CHECK_EQ(table.ammo_types()[1].name, std::string("CANNON SHELLS"));
    CHECK_EQ(table.ammo_types()[1].max, std::uint32_t(180));
    CHECK_EQ(table.ammo_types()[2].name, std::string("FLECHETTES"));
    CHECK_EQ(table.ammo_types()[2].max, std::uint32_t(100));
    CHECK_EQ(table.ammo_max_for(mg != nullptr ? mg->ammo_type : 0), std::uint32_t(5000));
    CHECK_EQ(table.ammo_max_for(-1), std::uint32_t(0));
    CHECK_EQ(table.ammo_max_for(15), std::uint32_t(0));
}

void test_real_save(const GoldenSave& golden) {
    const fs::path save_path = kGame / "SAVEGAME" / golden.file;
    std::optional<gm::GameState> state = load_state(save_path);
    CHECK(state.has_value());
    if (!state) {
        return;
    }
    std::string error;
    std::optional<gm::Weapons> table =
        gm::Weapons::load(*state, kGame / "level0.sbt", &error);
    CHECK(table.has_value());
    if (!table) {
        std::fprintf(stderr, "Weapons::load: %s\n", error.c_str());
        return;
    }
    test_real_tables(*table);

    gm::Arsenal arsenal = table->initial();
    CHECK_EQ(arsenal.current, golden.current);
    CHECK_EQ(arsenal.mask, std::uint32_t(3));
    CHECK_EQ(arsenal.ammo[0], golden.ammo[0]);
    CHECK_EQ(arsenal.ammo[1], golden.ammo[1]);
    for (int i = 2; i < gm::kWeaponSlots; ++i) {
        CHECK_EQ(arsenal.ammo[static_cast<std::size_t>(i)], std::uint32_t(0));
    }

    gm::FireError why = gm::FireError::none;
    CHECK(gm::can_fire(*table, arsenal, 0, &why));
    CHECK(gm::can_fire(*table, arsenal, 1, &why));
    CHECK(!gm::can_fire(*table, arsenal, 2, &why));
    CHECK(why == gm::FireError::not_available);

    const gm::Weapon* weapon = table->at(golden.current);
    CHECK(weapon != nullptr);
    if (weapon == nullptr) {
        return;
    }

    // The shipped data has no magazine/reload layer: reload is a clean error.
    CHECK(!gm::reload(*table, arsenal, golden.current, &why));
    CHECK(why == gm::FireError::no_magazine);

    gm::CollisionWorld empty;
    const gm::ShotResult first =
        gm::fire(*table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, empty);
    CHECK(first.ok);
    CHECK(first.fired);
    CHECK(first.projectile == !golden.hitscan);
    CHECK(!first.hit);
    CHECK_NEAR(first.distance, golden.range, 1e-12);
    CHECK_NEAR(first.time, golden.hitscan ? 0.0 : golden.range / gm::kDefaultProjectileSpeed,
               1e-12);
    CHECK_NEAR(first.damage, golden.damage, 0.0);
    CHECK_EQ(first.consumed, std::uint32_t(1));
    CHECK_EQ(first.ammo_left, golden.ammo_left);
    CHECK_EQ(first.cooldown_left, golden.fire_ticks);
    CHECK_EQ(arsenal.ammo[static_cast<std::size_t>(golden.current)],
             golden.ammo_left);
    CHECK_EQ(arsenal.shot_index, std::uint64_t(1));

    // First-shot spread golden (implementation-pinned SplitMix64 stream;
    // the seed mixes in slot+1, so saved1's slot 1 is a different stream).
    CHECK_NEAR(first.spread_x, golden.spread_x, 1e-15);
    CHECK_NEAR(first.spread_y, golden.spread_y, 1e-15);

    // The cooldown is exactly the quantised period.
    CHECK(!gm::can_fire(*table, arsenal, golden.current, &why));
    CHECK(why == gm::FireError::cooling_down);
    gm::tick(*table, arsenal, static_cast<std::uint32_t>(golden.fire_ticks - 1));
    CHECK(!gm::can_fire(*table, arsenal, golden.current, &why));
    gm::tick(*table, arsenal, 1);
    CHECK(gm::can_fire(*table, arsenal, golden.current, &why));

    std::printf("weapons: %s slot %d ammo0=%u ammo1=%u first ammo_left=%u "
                "cooldown=%d distance=%.17g time=%.17g damage=%.17g\n",
                golden.file, golden.current, arsenal.ammo[0], arsenal.ammo[1],
                first.ammo_left, first.cooldown_left, first.distance, first.time,
                first.damage);

    // Switching is mask-driven: slot 1 is available, slot 2 is not.
    CHECK(gm::switch_slot(*table, arsenal, 1, &why));
    CHECK_EQ(arsenal.current, 1);
    if (golden.current == 0) {
        CHECK(!gm::switch_slot(*table, arsenal, 2, &why));
        CHECK(why == gm::FireError::not_available);
    }
}

void test_real_negatives() {
    const fs::path save_path = kGame / "SAVEGAME" / "saved0.sbt";
    std::optional<gm::GameState> state = load_state(save_path);
    CHECK(state.has_value());
    if (!state) {
        return;
    }
    std::string error;
    std::optional<gm::Weapons> table =
        gm::Weapons::load(*state, kGame / "level0.sbt", &error);
    CHECK(table.has_value());
    if (!table) {
        return;
    }

    gm::CollisionWorld empty;
    gm::Arsenal arsenal = table->initial();
    gm::ShotResult shot =
        gm::fire(*table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, empty);
    CHECK(shot.ok);
    CHECK_EQ(shot.ammo_left, std::uint32_t(1999));

    shot = gm::fire(*table, arsenal, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, empty);
    CHECK(!shot.ok);
    CHECK(shot.error == gm::FireError::cooling_down);

    // Bad directions never consume the round.
    gm::Arsenal nan_arsenal = table->initial();
    shot = gm::fire(*table, nan_arsenal, {0.0, 0.0, 0.0},
                    {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, empty);
    CHECK(!shot.ok);
    CHECK(shot.error == gm::FireError::bad_direction);
    CHECK_EQ(nan_arsenal.ammo[0], std::uint32_t(2000));
    CHECK_EQ(nan_arsenal.cooldown[0], 0);

    // A selected slot outside the save block is rejected before lookup.
    gm::Arsenal oob = table->initial();
    oob.current = 12;
    shot = gm::fire(*table, oob, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, empty);
    CHECK(!shot.ok);
    CHECK(shot.error == gm::FireError::slot_out_of_range);

    oob.current = -1;
    shot = gm::fire(*table, oob, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, empty);
    CHECK(!shot.ok);
    CHECK(shot.error == gm::FireError::slot_out_of_range);
}

} // namespace


void test_thing_runtime_damage() {
    std::ifstream in(kGame / "level0.sbt", std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    std::string error;
    const auto level = gm::parse_sbt(bytes, error);
    CHECK(level.has_value());
    if (!level) {
        return;
    }
    const gm::VmState vm = gm::vm_state_from_level(*level);
    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    gm::Things things = gm::Things::load(*level, vm, options);
    CHECK(things.ok());
    const gm::Thing* first = things.things().empty() ? nullptr : &things.things()[0];
    CHECK(first != nullptr);
    if (first == nullptr) {
        return;
    }
    const int id = first->id;
    gm::Thing* thing = things.by_id(id);
    CHECK(thing != nullptr);
    if (thing == nullptr) {
        return;
    }
    thing->health = 1000;
    thing->max_health = 1000;
    thing->shields.fill(0);
    thing->shield_max.fill(0);
    thing->alive = true;
    thing->damaged_event = 777;
    thing->destroy_event = 778;

    gm::EventQueue queue;
    const gm::ThingDamageResult hurt = gm::apply_thing_damage(
        things, id, 0, 500, 0, -1, &queue);
    CHECK(hurt.ok);
    CHECK_EQ(thing->health, 500);
    const gm::EventParseResult damaged = queue.drain();
    CHECK_EQ(damaged.events.size(), 1U);
    if (damaged.events.size() == 1) {
        CHECK_EQ(damaged.events[0].id, 777);
    }

    thing->health = 100;
    const gm::ThingDamageResult killed = gm::apply_thing_damage(
        things, id, 0, 2000, 0, -1, &queue);
    CHECK(killed.ok);
    CHECK(killed.dead);
    CHECK(!thing->alive);
    const gm::EventParseResult destroyed = queue.drain();
    CHECK_EQ(destroyed.events.size(), 1U);
    if (destroyed.events.size() == 1) {
        CHECK_EQ(destroyed.events[0].id, 778);
    }

    // Unknown id is rejected without touching the queue.
    const gm::ThingDamageResult missing =
        gm::apply_thing_damage(things, 0x7fffffff, 0, 1, 0, -1, &queue);
    CHECK(!missing.ok);
    CHECK(queue.empty());
}


void test_weapon_impact_event() {
    gm::Weapon weapon = make_weapon(0, 1, true);
    weapon.impact_event = 1234;
    gm::ShotResult shot;
    shot.ok = true;
    shot.hit = true;
    shot.point = {1.0, 2.0, 3.0};
    gm::EventQueue queue;
    CHECK(gm::post_weapon_impact(queue, weapon, shot, 0x11));
    const gm::EventParseResult parsed = queue.drain();
    CHECK_EQ(parsed.events.size(), 1U);
    if (parsed.events.size() == 1) {
        CHECK_EQ(parsed.events[0].id, 1234);
        CHECK_EQ(parsed.events[0].x, 1);
        CHECK_EQ(parsed.events[0].y, 2);
        CHECK_EQ(parsed.events[0].z, 3);
        CHECK_EQ(parsed.events[0].extra, 0x11U);
    }
    weapon.impact_event = 0;
    CHECK(!gm::post_weapon_impact(queue, weapon, shot));
    shot.ok = false;
    weapon.impact_event = 1234;
    CHECK(!gm::post_weapon_impact(queue, weapon, shot));
    shot.ok = true;
    shot.hit = false;
    CHECK(!gm::post_weapon_impact(queue, weapon, shot));
    shot.hit = true;
    gm::Weapon not_hitscan = weapon;
    not_hitscan.hitscan = false;
    CHECK(!gm::post_weapon_impact(queue, not_hitscan, shot));
}


void test_spawned_thing_damage() {
    gm::Things things;
    gm::Thing prototype;
    prototype.id = -1;
    prototype.type = 7;
    prototype.position = std::array<int, 3>{4, 5, 6};
    prototype.health = 1000;
    prototype.max_health = 1000;
    prototype.alive = true;
    prototype.damaged_event = 880;
    prototype.destroy_event = 881;
    const int id = things.spawn(prototype);
    CHECK_EQ(id, 0);
    gm::EventQueue queue;
    const gm::ThingDamageResult hurt =
        gm::apply_thing_damage(things, id, 0, 250, 0, -1, &queue);
    CHECK(hurt.ok);
    CHECK_EQ(things.by_id(id)->health, 750);
    const gm::EventParseResult damaged = queue.drain();
    CHECK_EQ(damaged.events.size(), 1U);
    if (damaged.events.size() == 1) {
        CHECK_EQ(damaged.events[0].id, 880);
        CHECK_EQ(damaged.events[0].x, 4);
    }
    const gm::ThingDamageResult killed =
        gm::apply_thing_damage(things, id, 0, 2000, 0, -1, &queue);
    CHECK(killed.dead);
    CHECK(!things.by_id(id)->alive);
    const gm::EventParseResult destroyed = queue.drain();
    CHECK_EQ(destroyed.events.size(), 1U);
    if (destroyed.events.size() == 1) {
        CHECK_EQ(destroyed.events[0].id, 881);
    }
    const gm::ThingDamageResult again =
        gm::apply_thing_damage(things, id, 0, 1, 0, -1, &queue);
    CHECK(!again.ok);
    CHECK(queue.empty());
}


void test_projectile_core() {
    const gm::ProjectileTargets no_targets;

    // Straight flight: 10 ticks at 2 units/tick, no gravity, no lifetime hit.
    gm::ProjectileState straight;
    straight.spawn.position = {0.0, 0.0, 0.0};
    straight.spawn.direction = {1.0, 0.0, 0.0};
    straight.spawn.speed = 2.0;
    straight.spawn.life_ticks = 364;
    straight.position = straight.spawn.position;
    straight.direction = straight.spawn.direction;
    straight.speed = straight.spawn.speed;
    straight.life_left = straight.spawn.life_ticks;
    for (int i = 0; i < 10; ++i) {
        const gm::ProjectileTickResult r =
            gm::projectile_step(straight, no_targets, gm::CollisionWorld{}, 1);
        CHECK(r.status == gm::ProjectileStatus::flying);
        CHECK_EQ(r.events.size(), 0U);
    }
    CHECK_NEAR(straight.position.x, 20.0, 1e-12);
    CHECK_NEAR(straight.position.z, 0.0, 0.0);
    CHECK_EQ(straight.life_left, 354);

    // Semi-implicit gravity: z = -0.2*n*(n+1)/2 after n ticks.
    gm::ProjectileState falling;
    falling.spawn.gravity = 1.0;
    falling.spawn.level_gravity = 1.0;
    falling.spawn.direction = {1.0, 0.0, 0.0};
    falling.position = {0.0, 0.0, 0.0};
    falling.direction = falling.spawn.direction;
    for (int i = 0; i < 10; ++i) {
        gm::projectile_step(falling, no_targets, gm::CollisionWorld{}, 1);
    }
    CHECK_NEAR(falling.position.z, -11.0, 1e-12);
    CHECK_NEAR(falling.vertical_speed, 2.0, 1e-12);

    // Floor at z=0, sphere radius 10: one 8-tick step reaches the contact.
    std::vector<TriSpec> floor;
    add_quad(floor, {-100.0, 100.0, 0.0}, {-100.0, -100.0, 0.0},
             {100.0, -100.0, 0.0}, {100.0, 100.0, 0.0});
    const gm::CollisionWorld world = build_world(floor);

    gm::ProjectileState bounce;
    bounce.spawn.direction = {0.0, 0.0, -1.0};
    bounce.spawn.speed = 2.0;
    bounce.spawn.radius = 10.0;
    bounce.spawn.damping = 0.8;
    bounce.spawn.collide_event = 77;
    bounce.position = {0.0, 0.0, 25.0};
    bounce.direction = {0.0, 0.0, -1.0};
    bounce.speed = 2.0;
    bounce.damping = 0.8;
    gm::ProjectileTickResult bounced;
    for (int i = 0; i < 8; ++i) {
        bounced = gm::projectile_step(bounce, no_targets, world, 1);
        if (bounced.status == gm::ProjectileStatus::bounced) {
            break;
        }
    }
    CHECK(bounced.status == gm::ProjectileStatus::bounced);
    CHECK_NEAR(bounce.position.z, 10.0, 1e-3);
    CHECK_NEAR(bounce.direction.z, 1.0, 1e-12);
    CHECK_NEAR(bounce.speed, 0.8, 1e-12); // 2 * 0.8 * 0.5 (vertical damping)
    CHECK_EQ(bounced.events.size(), 1U);
    if (bounced.events.size() == 1) {
        CHECK_EQ(bounced.events[0].id, 77U);
        CHECK_NEAR(bounced.events[0].position.z, 11.0, 1e-12);
    }

    // hits_left == 1: the first contact expires and posts no collide event.
    gm::ProjectileState one_hit;
    one_hit.spawn.hits_left = 1;
    one_hit.spawn.radius = 10.0;
    one_hit.spawn.damping = 0.8;
    one_hit.spawn.expire_event = 88;
    one_hit.spawn.collide_event = 77;
    one_hit.position = {0.0, 0.0, 25.0};
    one_hit.direction = {0.0, 0.0, -1.0};
    one_hit.speed = 2.0;
    one_hit.hits_left = 1;
    gm::ProjectileTickResult expired;
    for (int i = 0; i < 8; ++i) {
        expired = gm::projectile_step(one_hit, no_targets, world, 1);
        if (expired.status == gm::ProjectileStatus::expired) {
            break;
        }
    }
    CHECK(expired.status == gm::ProjectileStatus::expired);
    CHECK_EQ(one_hit.hits_left, 0);
    CHECK_EQ(expired.events.size(), 1U);
    if (expired.events.size() == 1) {
        CHECK_EQ(expired.events[0].id, 88U);
    }

    // Lifetime is decremented before movement; the exact boundary expires.
    gm::ProjectileState timed;
    timed.position = {0.0, 0.0, 0.0};
    timed.direction = {1.0, 0.0, 0.0};
    timed.spawn.life_ticks = 2;
    timed.spawn.expire_event = 99;
    timed.life_left = 2;
    CHECK(gm::projectile_step(timed, no_targets, gm::CollisionWorld{}, 1).status ==
          gm::ProjectileStatus::flying);
    const gm::ProjectileTickResult last =
        gm::projectile_step(timed, no_targets, gm::CollisionWorld{}, 1);
    CHECK(last.status == gm::ProjectileStatus::expired);
    CHECK(timed.spent);
    CHECK_EQ(last.events.size(), 1U);
    CHECK(gm::projectile_step(timed, no_targets, gm::CollisionWorld{}, 1).events.empty());

    // Proximity: the player is checked at the current position, movement waits.
    const std::array<gm::Vec3d, 1> player_pos = {{{20.0, 0.0, 0.0}}};
    const std::array<double, 1> player_radius = {{5.0}};
    const std::array<std::uint8_t, 1> player_kind = {{1}};
    const std::array<std::uint8_t, 1> player_object = {{1}};
    gm::ProjectileTargets targets;
    targets.player_positions = player_pos;
    targets.player_radii = player_radius;
    targets.player_kind = player_kind;
    targets.player_has_object = player_object;
    gm::ProjectileState grenade;
    grenade.spawn.thing_id = 3;
    grenade.spawn.proximity.push_back(gm::ProximityEntry{123, 0, 20, 0, {}});
    grenade.position = {0.0, 0.0, 0.0};
    grenade.direction = {1.0, 0.0, 0.0};
    grenade.speed = 2.0;
    const gm::ProjectileTickResult prox =
        gm::projectile_step(grenade, targets, gm::CollisionWorld{}, 1);
    CHECK(prox.status == gm::ProjectileStatus::proximity);
    CHECK_NEAR(grenade.position.x, 0.0, 0.0);
    CHECK_EQ(prox.events.size(), 1U);
    if (prox.events.size() == 1) {
        CHECK_EQ(prox.events[0].id, 123U);
        CHECK_EQ(prox.events[0].extra, (0U << 16) | (8U + 3U));
    }

    // A thing target uses the fixed 8+ slot, not ``thing_count + index`` and
    // therefore cannot be mistaken for player 1.
    const std::array<gm::Vec3d, 2> far_players = {
        gm::Vec3d{1000.0, 0.0, 0.0}, gm::Vec3d{2000.0, 0.0, 0.0}};
    const std::array<double, 2> far_radii = {{5.0, 5.0}};
    const std::array<std::uint8_t, 2> kinds = {{1, 1}};
    const std::array<std::uint8_t, 2> objects = {{1, 1}};
    const std::array<gm::Vec3d, 1> thing_pos = {gm::Vec3d{20.0, 0.0, 0.0}};
    const std::array<double, 1> thing_radii = {{5.0}};
    const std::array<std::uint8_t, 1> alive = {{1}};
    const std::array<std::uint32_t, 1> flags = {{0}};
    const std::array<int, 1> moves = {{1}};
    const std::array<int, 1> objects_present = {{1}};
    const std::array<int, 1> owners = {{-1}};
    const std::array<int, 1> parents = {{-1}};
    gm::ProjectileTargets thing_targets;
    thing_targets.player_positions = far_players;
    thing_targets.player_radii = far_radii;
    thing_targets.player_kind = kinds;
    thing_targets.player_has_object = objects;
    thing_targets.thing_positions = thing_pos;
    thing_targets.thing_radii = thing_radii;
    thing_targets.thing_alive = alive;
    thing_targets.thing_flags2 = flags;
    thing_targets.thing_movement_type = moves;
    thing_targets.thing_has_object = objects_present;
    thing_targets.thing_owner = owners;
    thing_targets.thing_parent = parents;
    gm::ProjectileState thing_grenade = grenade;
    thing_grenade.position = {0.0, 0.0, 0.0};
    const gm::ProjectileTickResult thing_prox =
        gm::projectile_step(thing_grenade, thing_targets, gm::CollisionWorld{}, 1);
    CHECK(thing_prox.status == gm::ProjectileStatus::proximity);
    CHECK_EQ(thing_prox.events.size(), 1U);
    if (thing_prox.events.size() == 1) {
        CHECK_EQ(thing_prox.events[0].extra >> 16, 8U);
    }

    // The owner class is deterministic and posts create/expire events.
    gm::ProjectileSpawn spawn;
    spawn.thing_id = 5;
    spawn.position = {0.0, 0.0, 0.0};
    spawn.direction = {1.0, 0.0, 0.0};
    spawn.speed = 2.0;
    spawn.life_ticks = 2;
    spawn.create_event = 55;
    spawn.expire_event = 66;
    spawn.destroy_event = 67;
    gm::Projectiles projectiles;
    gm::EventQueue queue;
    CHECK_EQ(projectiles.spawn(spawn, &queue), 0);
    CHECK_EQ(projectiles.spawn(spawn, &queue), -1);
    CHECK(projectiles.by_thing_id(5) != nullptr);
    const gm::EventParseResult created = queue.drain();
    CHECK_EQ(created.events.size(), 1U);
    if (created.events.size() == 1) {
        CHECK_EQ(created.events[0].id, 55);
    }
    projectiles.tick(1, no_targets, gm::CollisionWorld{}, &queue);
    CHECK(queue.empty());
    CHECK(projectiles.by_thing_id(5)->life_left == 1);
    projectiles.tick(1, no_targets, gm::CollisionWorld{}, &queue);
    const gm::EventParseResult expired_owner = queue.drain();
    CHECK_EQ(expired_owner.events.size(), 1U);
    if (expired_owner.events.size() == 1) {
        CHECK_EQ(expired_owner.events[0].id, 66);
    }
    CHECK(projectiles.by_thing_id(5)->spent);
    CHECK(projectiles.remove(5, &queue));
    const gm::EventParseResult destroy = queue.drain();
    CHECK_EQ(destroy.events.size(), 1U);
    if (destroy.events.size() == 1) {
        CHECK_EQ(destroy.events[0].id, 67);
    }
    CHECK(projectiles.by_thing_id(5)->dead);
    CHECK(!projectiles.remove(5, &queue));

    // A removed projectile id may be reused by a later spawn; the slot is
    // recycled instead of growing the vector forever.
    gm::ProjectileSpawn respawn = spawn;
    respawn.create_event = 68;
    respawn.life_ticks = 1;
    CHECK_EQ(projectiles.spawn(respawn, &queue), 0);
    const gm::ProjectileState* reused = projectiles.by_thing_id(5);
    CHECK(reused != nullptr);
    if (reused != nullptr) {
        CHECK(!reused->dead);
        CHECK_EQ(reused->spawn.create_event, 68U);
    }
    const gm::EventParseResult reused_event = queue.drain();
    CHECK_EQ(reused_event.events.size(), 1U);
    if (reused_event.events.size() == 1) {
        CHECK_EQ(reused_event.events[0].id, 68);
    }

    // The Thing adapter copies only the proven THING_TYPES fields.
    gm::Thing source;
    source.id = 9;
    source.gravity = 1.0;
    source.damping = 0.8;
    source.collision_shape = 2;
    source.radius = 10.0;
    source.box = {4.0, 10.0, 4.0};
    source.hits_left = 1;
    source.expire_ticks = 20;
    source.collide_event = 1;
    source.expire_event = 2;
    source.destroy_event = 3;
    source.timeout_event = 4;
    source.create_event = 5;
    const gm::ProjectileState from_thing =
        gm::projectile_spawn_from_thing(source, {1.0, 2.0, 3.0},
                                        {0.0, 0.0, 1.0}, 7.0, 0, -1);
    CHECK_EQ(from_thing.spawn.thing_id, 9);
    CHECK_NEAR(from_thing.spawn.gravity, 1.0, 0.0);
    CHECK_NEAR(from_thing.spawn.damping, 0.8, 0.0);
    CHECK_EQ(from_thing.spawn.shape, 2);
    CHECK_NEAR(from_thing.spawn.box[1], 10.0, 0.0);
    CHECK_EQ(from_thing.hits_left, 1);
    CHECK_EQ(from_thing.life_left, 20);
}


void test_projectile_rgm_homing() {
    const gm::ProjectileTargets no_targets;
    const gm::CollisionWorld empty_world;

    // RGM (`movement_type == 8`): the target is to the side, so the round has
    // to turn.  A 0.2 rad/tick cap gives a visible arc, and the reach ends in
    // destroy (the +0x1f8 record), never expire (+0x1ec).
    gm::ProjectileState rgm;
    rgm.spawn.thing_id = 77;
    rgm.spawn.destroy_event = 501;
    rgm.spawn.expire_event = 502;
    rgm.homing = true;
    rgm.destroy_on_contact = true;
    rgm.homing_turn = 0.2;
    rgm.target_point = {40.0, 20.0, 0.0};
    rgm.has_target_point = true;
    rgm.position = {0.0, 0.0, 0.0};
    rgm.direction = {1.0, 0.0, 0.0};
    rgm.speed = 2.0;

    double previous_distance = 44.721359549995796; // |(40,20,0)|
    bool destroyed = false;
    gm::ProjectileTickResult last;
    for (int tick = 0; tick < 100; ++tick) {
        last = gm::projectile_step(rgm, no_targets, empty_world, 1);
        const double distance =
            gm::length(gm::Vec3d{40.0, 20.0, 0.0} - rgm.position);
        CHECK(distance <= previous_distance + 1e-9);
        previous_distance = distance;
        if (tick == 0) {
            // The first tick already rotates toward +y instead of flying
            // straight along +x.
            CHECK(rgm.direction.x < 1.0);
            CHECK(rgm.direction.x > 0.0);
            CHECK(rgm.direction.y > 0.0);
        }
        if (last.status == gm::ProjectileStatus::destroyed) {
            destroyed = true;
            break;
        }
        CHECK(last.status == gm::ProjectileStatus::flying);
        CHECK(last.events.empty());
    }
    CHECK(destroyed);
    CHECK(rgm.dead);
    CHECK(rgm.spent);
    CHECK_NEAR(rgm.position.x, 40.0, 1e-9);
    CHECK_NEAR(rgm.position.y, 20.0, 1e-9);
    CHECK_NEAR(rgm.position.z, 0.0, 1e-12);
    CHECK_EQ(last.events.size(), 1U);
    if (last.events.size() == 1) {
        CHECK_EQ(last.events[0].id, 501U); // destroy, not 502 (expire)
    }
    // Once dead, further steps are silent and produce no second destroy.
    const gm::ProjectileTickResult after =
        gm::projectile_step(rgm, no_targets, empty_world, 1);
    CHECK(after.status == gm::ProjectileStatus::destroyed);
    CHECK(after.events.empty());

    // target_index resolves through ProjectileTargets (things start at slot
    // 8).  The decoy target_point is behind the round, so using it instead of
    // the index would immediately turn the direction to -x.
    const std::array<gm::Vec3d, 1> thing_pos = {{gm::Vec3d{0.0, 30.0, 0.0}}};
    gm::ProjectileTargets targets;
    targets.thing_positions = thing_pos;
    gm::ProjectileState indexed;
    indexed.homing = true;
    indexed.homing_turn = 0.2;
    indexed.target_index = 8;
    indexed.target_point = {-100.0, 0.0, 0.0};
    indexed.has_target_point = true;
    indexed.position = {0.0, 0.0, 0.0};
    indexed.direction = {1.0, 0.0, 0.0};
    indexed.speed = 2.0;
    const gm::ProjectileTickResult index_tick =
        gm::projectile_step(indexed, targets, empty_world, 1);
    CHECK(index_tick.status == gm::ProjectileStatus::flying);
    CHECK(indexed.direction.x > 0.0);
    CHECK(indexed.direction.y > 0.0);
    CHECK(indexed.position.y > 0.0);

    // Without a target the same branch keeps the current heading: straight
    // flight, no events, no contact.
    gm::ProjectileState lost;
    lost.spawn.destroy_event = 503;
    lost.spawn.expire_event = 504;
    lost.homing = true;
    lost.destroy_on_contact = true;
    lost.target_index = -1;
    lost.homing_turn = 0.5;
    lost.position = {0.0, 0.0, 0.0};
    lost.direction = {1.0, 0.0, 0.0};
    lost.speed = 2.0;
    for (int tick = 0; tick < 10; ++tick) {
        const gm::ProjectileTickResult r =
            gm::projectile_step(lost, no_targets, empty_world, 1);
        CHECK(r.status == gm::ProjectileStatus::flying);
        CHECK(r.events.empty());
    }
    CHECK_NEAR(lost.position.x, 20.0, 1e-12);
    CHECK_NEAR(lost.position.y, 0.0, 1e-12);
    CHECK_NEAR(lost.position.z, 0.0, 1e-12);

    // The homing fields are inert without `homing`: movement_type 6 keeps the
    // old straight/gravity/hits behavior even when a target is present.
    gm::ProjectileState legacy;
    legacy.homing = false;
    legacy.homing_turn = 1.0;
    legacy.has_target_point = true;
    legacy.target_point = {0.0, 50.0, 0.0};
    legacy.position = {0.0, 0.0, 0.0};
    legacy.direction = {1.0, 0.0, 0.0};
    legacy.speed = 2.0;
    const gm::ProjectileTickResult legacy_tick =
        gm::projectile_step(legacy, no_targets, empty_world, 1);
    CHECK(legacy_tick.status == gm::ProjectileStatus::flying);
    CHECK_NEAR(legacy.position.x, 2.0, 1e-12);
    CHECK_NEAR(legacy.position.y, 0.0, 1e-12);

    // World contact: destroy at the contact point, not expire, and exactly
    // one destroy record through the owner.
    const gm::CollisionWorld wall = build_world(wall_x(20.0));
    gm::ProjectileSpawn spawn;
    spawn.thing_id = 31;
    spawn.position = {0.0, 0.0, 0.0};
    spawn.direction = {1.0, 0.0, 0.0};
    spawn.speed = 5.0;
    spawn.radius = 1.0;
    spawn.homing = true;
    spawn.target_point = {100.0, 0.0, 0.0};
    spawn.has_target_point = true;
    spawn.destroy_on_contact = true;
    spawn.collide_event = 603;
    spawn.expire_event = 602;
    spawn.destroy_event = 601;
    gm::Projectiles projectiles;
    gm::EventQueue queue;
    CHECK_EQ(projectiles.spawn(spawn, &queue), 0);
    CHECK(queue.empty());
    projectiles.tick(10, no_targets, wall, &queue);
    const gm::EventParseResult contact = queue.drain();
    CHECK_EQ(contact.events.size(), 1U);
    if (contact.events.size() == 1) {
        CHECK_EQ(contact.events[0].id, 601); // neither expire nor collide
    }
    gm::ProjectileState* hit = projectiles.by_thing_id(31);
    CHECK(hit != nullptr);
    if (hit != nullptr) {
        CHECK(hit->dead);
        CHECK(hit->spent);
        CHECK_NEAR(hit->position.x, 19.0, 1e-6);
    }
    CHECK(!projectiles.remove(31, &queue)); // destroy already posted once
    CHECK(queue.empty());
    projectiles.tick(1, no_targets, wall, &queue);
    CHECK(queue.empty());

    // The Thing adapter selects the homing branch from movement_type 8; the
    // target still has to come from the caller.
    gm::Thing rgm_type;
    rgm_type.id = 78;
    rgm_type.movement_type = 8;
    rgm_type.destroy_event = 505;
    const gm::ProjectileState from_type = gm::projectile_spawn_from_thing(
        rgm_type, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, 2.0, 0, -1);
    CHECK(from_type.homing);
    CHECK(!from_type.has_target_point);
    CHECK(from_type.target_index == -1);
}

void test_projectile_shot_bridge() {
    const gm::CollisionWorld empty_world;
    const gm::ProjectileTargets no_targets;

    // Charged projectile shot: 182 u/s base + 0.5 * 10 * 91 u/s.
    gm::Weapon weapon = make_weapon(0, 1, false);
    weapon.projectile_speed = 182.0;   // 2 units/tick
    weapon.projectile_charge = 10.0;   // operand[9]
    weapon.range = 0.0;                // unlimited: the shot itself misses
    gm::Weapons table({weapon});
    gm::Arsenal arsenal = make_arsenal(0x1, 0, 5);
    const gm::ShotResult shot = gm::fire(
        table, arsenal, {10.0, 20.0, 30.0}, {0.0, 1.0, 0.0}, empty_world, 0.5);
    CHECK(shot.ok);
    CHECK(shot.projectile);
    CHECK_NEAR(shot.projectile_speed, 182.0 + 0.5 * 10.0 * 91.0, 1e-12);

    gm::Thing grenade;
    grenade.id = 42;
    grenade.type = 28;
    grenade.movement_type = 6;
    grenade.gravity = 1.0;
    grenade.damping = 0.8;
    grenade.collision_shape = 1;
    grenade.radius = 10.0;
    grenade.hits_left = 3;
    grenade.expire_ticks = 364;
    grenade.collide_event = 71;
    grenade.expire_event = 72;
    grenade.destroy_event = 73;
    grenade.timeout_event = 74;
    grenade.create_event = 75;

    const std::optional<gm::ProjectileSpawn> spawn =
        gm::projectile_spawn_from_shot(weapon, shot, &grenade, 0, 42, 1.0);
    CHECK(spawn.has_value());
    if (spawn) {
        CHECK_EQ(spawn->thing_id, 42);
        CHECK_EQ(spawn->owner_player, 0);
        CHECK_EQ(spawn->owner_thing, 42);
        // 637 u/s / 91 Hz == 7 units/tick.
        CHECK_NEAR(spawn->speed, 7.0, 1e-12);
        CHECK_NEAR(spawn->position.x, 10.0, 0.0);
        CHECK_NEAR(spawn->position.y, 20.0, 0.0);
        CHECK_NEAR(spawn->position.z, 30.0, 0.0);
        CHECK_NEAR(spawn->direction.y, 1.0, 1e-12);
        CHECK_NEAR(spawn->gravity, 1.0, 0.0);
        CHECK_NEAR(spawn->damping, 0.8, 0.0);
        CHECK_NEAR(spawn->level_gravity, 1.0, 0.0);
        CHECK_EQ(spawn->shape, 1);
        CHECK_NEAR(spawn->radius, 10.0, 0.0);
        CHECK_EQ(spawn->hits_left, 3);
        CHECK_EQ(spawn->life_ticks, 364);
        CHECK_EQ(spawn->collide_event, 71U);
        CHECK_EQ(spawn->expire_event, 72U);
        CHECK_EQ(spawn->destroy_event, 73U);
        CHECK_EQ(spawn->timeout_event, 74U);
        CHECK_EQ(spawn->create_event, 75U);
    }

    // Without a Thing the factory still works: straight flight, no events.
    const std::optional<gm::ProjectileSpawn> bare =
        gm::projectile_spawn_from_shot(weapon, shot, nullptr, -1, -1);
    CHECK(bare.has_value());
    if (bare) {
        CHECK_EQ(bare->thing_id, -1);
        CHECK_EQ(bare->owner_player, -1);
        CHECK_EQ(bare->owner_thing, -1);
        CHECK_NEAR(bare->speed, 7.0, 1e-12);
        CHECK_EQ(bare->shape, 1);
        CHECK_NEAR(bare->radius, 0.0, 0.0);
        CHECK_NEAR(bare->box[0], 0.0, 0.0);
        CHECK_NEAR(bare->gravity, 0.0, 0.0);
        CHECK_NEAR(bare->damping, 0.0, 0.0);
        CHECK_NEAR(bare->level_gravity, 0.0, 0.0);
        CHECK_EQ(bare->hits_left, 0);
        CHECK_EQ(bare->life_ticks, 0);
        CHECK_EQ(bare->collide_event, 0U);
        CHECK_EQ(bare->expire_event, 0U);
        CHECK_EQ(bare->destroy_event, 0U);
        CHECK_EQ(bare->timeout_event, 0U);
        CHECK_EQ(bare->create_event, 0U);
    }

    // Box shape and zero direction sanitisation.
    gm::Thing rocket = grenade;
    rocket.id = 43;
    rocket.collision_shape = 2;
    rocket.radius = 0.0;
    rocket.box = {4.0, 18.0, 6.0};
    gm::ShotResult odd = shot;
    odd.direction = {0.0, 0.0, 0.0};
    const std::optional<gm::ProjectileSpawn> boxed =
        gm::projectile_spawn_from_shot(weapon, odd, &rocket, 1, 43);
    CHECK(boxed.has_value());
    if (boxed) {
        CHECK_EQ(boxed->shape, 2);
        CHECK_NEAR(boxed->box[1], 18.0, 0.0);
        CHECK_NEAR(boxed->direction.z, 1.0, 1e-12); // safe default
    }

    // Speed fallback to the weapon when the shot carries none.
    gm::ShotResult no_shot_speed = shot;
    no_shot_speed.projectile_speed = 0.0;
    const std::optional<gm::ProjectileSpawn> fallback =
        gm::projectile_spawn_from_shot(weapon, no_shot_speed, nullptr, 0, -1);
    CHECK(fallback.has_value());
    if (fallback) {
        CHECK_NEAR(fallback->speed, 2.0, 1e-12);
    }

    // Negatives: rejected shot, hitscan shot, no usable speed anywhere.
    gm::ShotResult rejected = shot;
    rejected.ok = false;
    CHECK(!gm::projectile_spawn_from_shot(weapon, rejected, &grenade, 0, 42)
               .has_value());
    gm::ShotResult hitscan = shot;
    hitscan.projectile = false;
    CHECK(!gm::projectile_spawn_from_shot(weapon, hitscan, &grenade, 0, 42)
               .has_value());
    gm::Weapon frozen = weapon;
    frozen.projectile_speed = 0.0;
    gm::ShotResult nan_speed = shot;
    nan_speed.projectile_speed = std::numeric_limits<double>::quiet_NaN();
    CHECK(!gm::projectile_spawn_from_shot(frozen, nan_speed, &grenade, 0, 42)
               .has_value());

    // --- runtime scenario: spawn -> bounce -> expire, posted exactly once ---
    std::vector<TriSpec> floor;
    add_quad(floor, {-100.0, 100.0, 0.0}, {-100.0, -100.0, 0.0},
             {100.0, -100.0, 0.0}, {100.0, 100.0, 0.0});
    const gm::CollisionWorld world = build_world(floor);

    gm::Weapon dropper = make_weapon(0, 1, false);
    dropper.projectile_speed = 182.0; // 2 units/tick
    dropper.range = 0.0;
    gm::Weapons drop_table({dropper});
    gm::Arsenal drop_arsenal = make_arsenal(0x1, 0, 1);
    const gm::ShotResult down =
        gm::fire(drop_table, drop_arsenal, {0.0, 0.0, 25.0}, {0.0, 0.0, -1.0},
                 empty_world);
    CHECK(down.ok);

    gm::Thing bouncy;
    bouncy.id = 100;
    bouncy.type = 28;
    bouncy.movement_type = 6;
    bouncy.damping = 0.8;
    bouncy.collision_shape = 1;
    bouncy.radius = 10.0;
    bouncy.hits_left = 0;
    bouncy.expire_ticks = 10;
    bouncy.collide_event = 501;
    bouncy.expire_event = 502;

    const std::optional<gm::ProjectileSpawn> drop_spawn =
        gm::projectile_spawn_from_shot(dropper, down, &bouncy, 0, 100);
    CHECK(drop_spawn.has_value());
    if (drop_spawn) {
        gm::Projectiles projectiles;
        gm::EventQueue queue;
        CHECK_EQ(projectiles.spawn(*drop_spawn, &queue), 0);
        CHECK(queue.empty()); // create_event is 0
        std::size_t collide_events = 0;
        std::size_t expire_events = 0;
        for (int tick = 0; tick < 20; ++tick) {
            projectiles.tick(1, no_targets, world, &queue);
            const gm::EventParseResult drained = queue.drain();
            for (const gm::Event& event : drained.events) {
                if (event.id == 501) {
                    ++collide_events;
                } else if (event.id == 502) {
                    ++expire_events;
                }
            }
        }
        CHECK_EQ(collide_events, 1U);
        CHECK_EQ(expire_events, 1U);
        const gm::ProjectileState* state = projectiles.by_thing_id(100);
        CHECK(state != nullptr);
        if (state != nullptr) {
            CHECK(state->spent);
            CHECK_EQ(state->life_left, 0);
            CHECK_NEAR(state->position.z, 10.8, 1e-3); // one post-bounce step
        }
        // A spent projectile is skipped: no re-tick, no second expire.
        projectiles.tick(5, no_targets, world, &queue);
        CHECK(queue.empty());
    }

    // Manual path: projectile_step + post_projectile_events, still one expiry.
    if (drop_spawn) {
        gm::ProjectileState manual;
        manual.spawn = *drop_spawn;
        manual.spawn.hits_left = 0;
        manual.spawn.life_ticks = 2;
        manual.position = {0.0, 0.0, 0.0};
        manual.direction = {1.0, 0.0, 0.0};
        manual.speed = 0.0;
        manual.life_left = 2;
        gm::EventQueue manual_queue;
        gm::ProjectileTickResult result = gm::projectile_step(
            manual, no_targets, gm::CollisionWorld{}, 1);
        CHECK(result.status == gm::ProjectileStatus::flying);
        CHECK_EQ(gm::post_projectile_events(manual_queue, result), 0U);
        result = gm::projectile_step(manual, no_targets, gm::CollisionWorld{}, 1);
        CHECK(result.status == gm::ProjectileStatus::expired);
        CHECK_EQ(result.events.size(), 1U);
        CHECK_EQ(gm::post_projectile_events(manual_queue, result), 1U);
        result = gm::projectile_step(manual, no_targets, gm::CollisionWorld{}, 1);
        CHECK(result.status == gm::ProjectileStatus::expired);
        CHECK(result.events.empty());
        CHECK_EQ(gm::post_projectile_events(manual_queue, result), 0U);
        const gm::EventParseResult drained = manual_queue.drain();
        CHECK_EQ(drained.events.size(), 1U);
        if (drained.events.size() == 1) {
            CHECK_EQ(drained.events[0].id, 502);
        }
    }
}


void test_projectile_floor_bounces() {
    const gm::ProjectileTargets no_targets;

    std::vector<TriSpec> floor;
    add_quad(floor, {-100.0, 100.0, 0.0}, {-100.0, -100.0, 0.0},
             {100.0, -100.0, 0.0}, {100.0, 100.0, 0.0});
    const gm::CollisionWorld world = build_world(floor);

    // A fast, heavy sphere dropped straight down: the first bounces survive the
    // vertical `damping *= 0.5`, so three of them have to leave the floor.
    gm::ProjectileState state;
    state.spawn.thing_id = 7;
    state.spawn.position = {0.0, 0.0, 100.0};
    state.spawn.direction = {0.0, 0.0, -1.0};
    state.spawn.speed = 40.0;
    state.spawn.gravity = 1.0;
    state.spawn.level_gravity = 1.0;
    state.spawn.damping = 0.8;
    state.spawn.radius = 10.0;
    state.spawn.hits_left = 5;
    state.spawn.collide_event = 601;
    state.spawn.expire_event = 602;
    state.position = state.spawn.position;
    state.direction = state.spawn.direction;
    state.speed = state.spawn.speed;
    state.damping = state.spawn.damping;
    state.hits_left = state.spawn.hits_left;

    struct Bounce {
        int tick = -1;
        double z = 0.0;
        double damping_before = 0.0;
        double damping_after = 0.0;
        double speed_before = 0.0;
        double speed_after = 0.0;
        int hits_before = 0;
        int hits_after = 0;
    };
    std::vector<Bounce> bounces;
    std::size_t collide_events = 0;
    std::size_t stray_events = 0;
    // Position reached on the tick right after each of the first three
    // bounces; `-1` marks a slot that the next tick has not filled yet.
    std::array<double, 3> rise_z = {{-1.0, -1.0, -1.0}};
    std::array<bool, 3> rise_is_bounce = {{false, false, false}};

    for (int tick = 0; tick < 400 && bounces.size() < 4; ++tick) {
        const gm::ProjectileState before = state;
        const gm::ProjectileTickResult result =
            gm::projectile_step(state, no_targets, world, 1);

        // After every bounce at least the next tick has to move away from the
        // floor: this is the old "sticking" regression (the sphere used to be
        // left exactly on the contact point, so the next cast hit it at t=0).
        for (std::size_t i = 0; i < bounces.size() && i < rise_z.size(); ++i) {
            if (bounces[i].tick + 1 == tick && rise_z[i] < 0.0) {
                rise_z[i] = state.position.z;
                rise_is_bounce[i] =
                    result.status == gm::ProjectileStatus::bounced;
            }
        }

        CHECK(result.events.size() <= 1U);
        if (result.status == gm::ProjectileStatus::bounced) {
            Bounce bounce;
            bounce.tick = tick;
            bounce.z = state.position.z;
            bounce.damping_before = before.damping;
            bounce.damping_after = state.damping;
            bounce.speed_before = before.speed;
            bounce.speed_after = state.speed;
            bounce.hits_before = before.hits_left;
            bounce.hits_after = state.hits_left;
            bounces.push_back(bounce);
            for (const gm::ProjectileEvent& event : result.events) {
                if (event.id == 601) {
                    ++collide_events;
                }
            }
        } else {
            // A non-contact tick must not halve damping, spend a hit or post
            // anything: the old code did all three on every resting tick.
            CHECK_NEAR(state.damping, before.damping, 0.0);
            CHECK_EQ(state.hits_left, before.hits_left);
            if (!result.events.empty()) {
                ++stray_events;
            }
        }
    }

    CHECK(bounces.size() >= 3U);
    CHECK_EQ(stray_events, 0U);
    CHECK(collide_events >= 3U);
    CHECK(collide_events <= bounces.size());
    for (const Bounce& bounce : bounces) {
        // Contact skin: the centre sits just outside radius 10, never on 10.
        CHECK(bounce.z > 10.0);
        CHECK_NEAR(bounce.z, 10.0, 1e-3);
        // `damping *= 0.5` is a per-contact rule, not a per-tick one.
        CHECK_NEAR(bounce.damping_after, bounce.damping_before * 0.5, 1e-12);
        // hits > 0 is spent one per bounce and only on a bounce.
        CHECK_EQ(bounce.hits_after, bounce.hits_before - 1);
    }
    if (bounces.size() >= 3U) {
        CHECK_NEAR(bounces.front().speed_before, 40.0, 1e-12);
        CHECK_NEAR(bounces.front().speed_after, 40.0 * 0.4, 1e-12);
        CHECK_NEAR(bounces.at(1).speed_after, bounces.front().speed_after * 0.2,
                   1e-12);
        CHECK_NEAR(bounces.at(2).speed_after, bounces.at(1).speed_after * 0.1,
                   1e-12);
        // The first three bounces leave the floor on the very next tick and
        // that tick is a flight, not another contact.
        for (std::size_t i = 0; i < rise_z.size(); ++i) {
            CHECK(rise_z[i] > bounces[i].z);
            CHECK(!rise_is_bounce[i]);
        }
    }

    // The last bounce leaves too little speed to overcome one gravity step, so
    // the following contacts are rests: frozen damping/hits/position with no
    // events at all.  Twenty-five ticks used to burn the remaining hit and
    // decay `damping` to zero.
    const double rest_z = state.position.z;
    const double rest_damping = state.damping;
    const double rest_speed = state.speed;
    const int rest_hits = state.hits_left;
    CHECK(rest_hits > 0);
    for (int tick = 0; tick < 25; ++tick) {
        const gm::ProjectileTickResult rest =
            gm::projectile_step(state, no_targets, world, 1);
        CHECK(rest.status == gm::ProjectileStatus::flying);
        CHECK(rest.events.empty());
    }
    CHECK_NEAR(state.position.z, rest_z, 1e-12);
    CHECK_NEAR(state.damping, rest_damping, 1e-12);
    CHECK_NEAR(state.speed, rest_speed, 1e-12);
    CHECK_EQ(state.hits_left, rest_hits);
    CHECK(!state.spent);

    // The owner class has to follow the same path: the same number of collide
    // records, no expire (hits left) and no per-tick event stream.
    gm::ProjectileSpawn spawn;
    spawn.thing_id = 7;
    spawn.position = {0.0, 0.0, 100.0};
    spawn.direction = {0.0, 0.0, -1.0};
    spawn.speed = 40.0;
    spawn.gravity = 1.0;
    spawn.level_gravity = 1.0;
    spawn.damping = 0.8;
    spawn.radius = 10.0;
    spawn.hits_left = 5;
    spawn.collide_event = 601;
    spawn.expire_event = 602;
    gm::Projectiles projectiles;
    gm::EventQueue queue;
    CHECK_EQ(projectiles.spawn(spawn, &queue), 0);
    CHECK(queue.empty());
    std::size_t owner_collide = 0;
    std::size_t owner_expire = 0;
    for (int tick = 0; tick < 400; ++tick) {
        projectiles.tick(1, no_targets, world, &queue);
        for (const gm::Event& event : queue.drain().events) {
            if (event.id == 601) {
                ++owner_collide;
            } else if (event.id == 602) {
                ++owner_expire;
            }
        }
    }
    CHECK_EQ(owner_collide, collide_events);
    CHECK_EQ(owner_expire, 0U);
    const gm::ProjectileState* owner = projectiles.by_thing_id(7);
    CHECK(owner != nullptr);
    if (owner != nullptr) {
        CHECK_NEAR(owner->position.z, rest_z, 1e-9);
        CHECK_NEAR(owner->damping, rest_damping, 1e-12);
        CHECK_EQ(owner->hits_left, rest_hits);
        CHECK(!owner->spent);
        CHECK(!owner->dead);
    }

    // Gravity only presses a sphere against a floor.  A slow wall contact is
    // still a bounce even when its approach is below one gravity step, so the
    // rest rule must not swallow walls.
    const gm::CollisionWorld wall_world = build_world(wall_x(6.0));
    gm::ProjectileState slow_wall;
    slow_wall.spawn.position = {0.0, 0.0, 0.0};
    slow_wall.spawn.direction = {1.0, 0.0, 0.0};
    slow_wall.spawn.speed = 0.1;
    slow_wall.spawn.gravity = 1.0;
    slow_wall.spawn.level_gravity = 1.0;
    slow_wall.spawn.damping = 0.8;
    slow_wall.spawn.radius = 5.0;
    slow_wall.position = slow_wall.spawn.position;
    slow_wall.direction = slow_wall.spawn.direction;
    slow_wall.speed = 0.1;
    slow_wall.damping = 0.8;
    gm::ProjectileTickResult wall_hit;
    for (int tick = 0; tick < 20; ++tick) {
        wall_hit = gm::projectile_step(slow_wall, no_targets, wall_world, 1);
        if (wall_hit.status != gm::ProjectileStatus::flying) {
            break;
        }
    }
    CHECK(wall_hit.status == gm::ProjectileStatus::bounced);
    CHECK(slow_wall.direction.x < 0.0);
    CHECK_NEAR(slow_wall.position.x, 1.0 + 1e-4, 1e-3);
}

void test_projectile_rest_contact() {
    const gm::ProjectileTargets no_targets;

    std::vector<TriSpec> floor;
    add_quad(floor, {-100.0, 100.0, 0.0}, {-100.0, -100.0, 0.0},
             {100.0, -100.0, 0.0}, {100.0, 100.0, 0.0});
    const gm::CollisionWorld world = build_world(floor);

    // A projectile already at rest on the floor: the upward speed (0.05) is
    // smaller than one gravity step (0.2), so every tick touches the floor with
    // an approach of 0.15 and the rest branch has to keep it there.
    gm::ProjectileState rest;
    rest.spawn.thing_id = 20;
    rest.spawn.position = {0.0, 0.0, 10.0001};
    rest.spawn.direction = {0.0, 0.0, 1.0};
    rest.spawn.speed = 0.05;
    rest.spawn.gravity = 1.0;
    rest.spawn.level_gravity = 1.0;
    rest.spawn.damping = 0.8;
    rest.spawn.radius = 10.0;
    rest.spawn.hits_left = 5;
    rest.spawn.collide_event = 601;
    rest.spawn.expire_event = 602;
    rest.position = rest.spawn.position;
    rest.direction = rest.spawn.direction;
    rest.speed = rest.spawn.speed;
    rest.damping = rest.spawn.damping;
    rest.hits_left = rest.spawn.hits_left;

    // `rest_ticks = 0` (default) is the compatibility path: more than twenty
    // rest ticks spend no hit, leave damping/speed/position alone, post nothing
    // and never expire.
    {
        gm::ProjectileState idle = rest;
        for (int tick = 0; tick < 25; ++tick) {
            const gm::ProjectileTickResult step =
                gm::projectile_step(idle, no_targets, world, 1);
            CHECK(step.status == gm::ProjectileStatus::flying);
            CHECK(step.events.empty());
        }
        CHECK_NEAR(idle.position.z, 10.0001, 1e-9);
        CHECK_NEAR(idle.speed, 0.05, 1e-15);
        CHECK_NEAR(idle.damping, 0.8, 1e-15);
        CHECK_EQ(idle.hits_left, 5);
        CHECK(!idle.spent);
        CHECK(!idle.dead);
    }

    // With a timeout the rest is bounded: exactly one expire, and no
    // hit/damping change before it.  Four 1-tick rest steps expire on the
    // fourth; the counter follows the accumulated-dt rule.
    {
        gm::ProjectileState timed = rest;
        timed.spawn.rest_ticks = 4;
        timed.rest_left = 4;
        for (int tick = 0; tick < 3; ++tick) {
            const gm::ProjectileTickResult step =
                gm::projectile_step(timed, no_targets, world, 1);
            CHECK(step.status == gm::ProjectileStatus::flying);
            CHECK(step.events.empty());
        }
        CHECK_EQ(timed.rest_left, 1);
        CHECK_EQ(timed.hits_left, 5);
        CHECK_NEAR(timed.damping, 0.8, 1e-15);

        const gm::ProjectileTickResult last =
            gm::projectile_step(timed, no_targets, world, 1);
        CHECK(last.status == gm::ProjectileStatus::expired);
        CHECK_EQ(last.events.size(), 1U);
        if (last.events.size() == 1) {
            CHECK_EQ(last.events[0].id, 602U); // the ordinary expire event
        }
        CHECK(timed.spent);
        CHECK(!timed.dead); // expire never removes the record
        CHECK_EQ(timed.hits_left, 5);
        CHECK_NEAR(timed.damping, 0.8, 1e-15);
        CHECK_NEAR(timed.position.z, 10.0001, 1e-9);
        const gm::ProjectileTickResult replay =
            gm::projectile_step(timed, no_targets, world, 1);
        CHECK(replay.already_spent);
        CHECK(replay.events.empty());
    }

    // A single two-tick step spends two rest ticks: 4 -> 2 -> 0.
    {
        gm::ProjectileState bulk = rest;
        bulk.spawn.rest_ticks = 4;
        bulk.rest_left = 4;
        CHECK(gm::projectile_step(bulk, no_targets, world, 2).status ==
              gm::ProjectileStatus::flying);
        CHECK_EQ(bulk.rest_left, 2);
        CHECK(!bulk.spent);
        const gm::ProjectileTickResult step =
            gm::projectile_step(bulk, no_targets, world, 2);
        CHECK(step.status == gm::ProjectileStatus::expired);
        CHECK_EQ(step.events.size(), 1U);
        CHECK(bulk.spent);
    }

    // The owner path has to agree: one expire record through EventQueue, and
    // the record is spent but still present.
    {
        gm::ProjectileSpawn spawn = rest.spawn;
        spawn.thing_id = 21;
        spawn.rest_ticks = 4;
        gm::Projectiles projectiles;
        gm::EventQueue queue;
        CHECK_EQ(projectiles.spawn(spawn, &queue), 0);
        std::size_t expire_events = 0;
        for (int step = 0; step < 4; ++step) {
            projectiles.tick(1, no_targets, world, &queue);
            for (const gm::Event& event : queue.drain().events) {
                if (event.id == 602) {
                    ++expire_events;
                }
            }
        }
        CHECK_EQ(expire_events, 1U);
        const gm::ProjectileState* owner = projectiles.by_thing_id(21);
        CHECK(owner != nullptr);
        if (owner != nullptr) {
            CHECK(owner->spent);
            CHECK(!owner->dead);
            CHECK_EQ(owner->hits_left, 5);
        }
    }

    // A nearly horizontal shot along the same floor used to be swallowed by
    // the rest rule: the sweep reports the contact almost immediately, so the
    // horizontal part of the step was thrown away (0.0025 units/tick, no
    // damping, no events -- a frozen "rest" for ever).  Now the rest branch
    // keeps the surface-parallel remainder and applies ground friction, so the
    // shot slides and slows down; hits and events stay untouched because this
    // is still not a hit.
    {
        gm::ProjectileState graze;
        graze.spawn.thing_id = 22;
        graze.spawn.position = {0.0, 0.0, 10.0001};
        graze.spawn.direction = {1.0, 0.0, 0.0};
        graze.spawn.speed = 5.0;
        graze.spawn.gravity = 1.0;
        graze.spawn.level_gravity = 1.0;
        graze.spawn.damping = 0.8;
        graze.spawn.radius = 10.0;
        graze.spawn.hits_left = 5;
        graze.spawn.collide_event = 601;
        graze.spawn.expire_event = 602;
        graze.position = graze.spawn.position;
        graze.direction = graze.spawn.direction;
        graze.speed = graze.spawn.speed;
        graze.damping = graze.spawn.damping;
        graze.hits_left = graze.spawn.hits_left;

        const gm::ProjectileTickResult first =
            gm::projectile_step(graze, no_targets, world, 1);
        CHECK(first.status == gm::ProjectileStatus::flying);
        CHECK(first.events.empty());
        // The whole horizontal step survives (old code kept ~0.0025).
        CHECK(graze.position.x > 4.99);
        CHECK_NEAR(graze.position.x, 5.0, 1e-3);
        // Still on the contact skin, vertical speed cleared by the rest.
        CHECK_NEAR(graze.position.z, 10.0001, 1e-9);
        CHECK_NEAR(graze.vertical_speed, 0.0, 0.0);
        // One ground-friction tick: speed *= damping, damping itself is not
        // halved and no hit is spent.
        CHECK_NEAR(graze.speed, 4.0, 1e-12);
        CHECK_NEAR(graze.damping, 0.8, 1e-15);
        CHECK_EQ(graze.hits_left, 5);

        double previous_speed = graze.speed;
        for (int tick = 0; tick < 49; ++tick) {
            const gm::ProjectileTickResult step =
                gm::projectile_step(graze, no_targets, world, 1);
            CHECK(step.status == gm::ProjectileStatus::flying);
            CHECK(step.events.empty());
            CHECK(graze.speed < previous_speed); // friction, not a freeze
            previous_speed = graze.speed;
        }
        // Bounded slide: the speed decays below one gravity step and the total
        // travel stays finite (5/(1-0.8) = 25 units, not an endless creep).
        CHECK(graze.speed < 0.2);
        CHECK(graze.position.x > 20.0);
        CHECK(graze.position.x < 25.01);
        CHECK_NEAR(graze.position.z, 10.0001, 1e-6);
        CHECK_EQ(graze.hits_left, 5);
        CHECK_NEAR(graze.damping, 0.8, 1e-15);
        CHECK(!graze.spent);
    }

    // The slide is swept too: the primary cast stops on the floor, so a wall
    // further along the same step must not be jumped over.  The sphere slides
    // until the wall (contact centre x = 5 for a radius of 1), then bounces.
    {
        std::vector<TriSpec> room = wall_x(6.0);
        add_quad(room, {-100.0, 100.0, 0.0}, {-100.0, -100.0, 0.0},
                 {100.0, -100.0, 0.0}, {100.0, 100.0, 0.0});
        const gm::CollisionWorld room_world = build_world(room);

        gm::ProjectileState slide;
        slide.spawn.thing_id = 24;
        slide.spawn.position = {0.0, 0.0, 1.0001};
        slide.spawn.direction = {1.0, 0.0, 0.0};
        slide.spawn.speed = 3.0;
        slide.spawn.gravity = 1.0;
        slide.spawn.level_gravity = 1.0;
        slide.spawn.damping = 0.8;
        slide.spawn.radius = 1.0;
        slide.spawn.hits_left = 5;
        slide.spawn.collide_event = 601;
        slide.position = slide.spawn.position;
        slide.direction = slide.spawn.direction;
        slide.speed = slide.spawn.speed;
        slide.damping = slide.spawn.damping;
        slide.hits_left = slide.spawn.hits_left;

        double max_x = slide.position.x;
        bool bounced = false;
        for (int tick = 0; tick < 10; ++tick) {
            const gm::ProjectileTickResult step =
                gm::projectile_step(slide, no_targets, room_world, 1);
            max_x = std::max(max_x, slide.position.x);
            if (step.status == gm::ProjectileStatus::bounced) {
                bounced = true;
            }
        }
        CHECK(bounced);
        CHECK(max_x <= 5.001); // never crossed the wall at x = 6
        CHECK(slide.direction.x < 0.0);
    }

    // A surface whose normal is mostly vertical but below the floor threshold
    // (z = 0.3, ~72 degrees) is a ramp, not a floor: the same slow contact that
    // the old `normal.z > 0` rule swallowed as a rest has to bounce, spend one
    // hit and halve damping.
    {
        const double nz = 0.3;
        const double nx = std::sqrt(1.0 - nz * nz);
        const gm::Vec3d normal{nx, 0.0, nz};
        const gm::Vec3d u{0.0, 1.0, 0.0};
        const gm::Vec3d v = gm::cross(normal, u); // (-nz, 0, nx)
        std::vector<TriSpec> ramp;
        add_quad(ramp, u * -50.0 + v * -50.0, u * -50.0 + v * 50.0,
                 u * 50.0 + v * 50.0, u * 50.0 + v * -50.0);
        const gm::CollisionWorld ramp_world = build_world(ramp);
        CHECK(normal.z < gm::kProjectileRestNormalZ);

        gm::ProjectileState slow;
        slow.spawn.thing_id = 23;
        // 0.55 from the plane with radius 0.5: a 0.05 gap, so the first tick
        // touches with an approach of 0.1 + 0.2*0.3 = 0.16 <= one gravity step.
        slow.spawn.position = normal * 0.55;
        slow.spawn.direction = normal * -1.0;
        slow.spawn.speed = 0.1;
        slow.spawn.gravity = 1.0;
        slow.spawn.level_gravity = 1.0;
        slow.spawn.damping = 0.8;
        slow.spawn.radius = 0.5;
        slow.spawn.hits_left = 5;
        slow.spawn.collide_event = 601;
        slow.spawn.rest_ticks = 2;
        slow.position = slow.spawn.position;
        slow.direction = slow.spawn.direction;
        slow.speed = slow.spawn.speed;
        slow.damping = slow.spawn.damping;
        slow.hits_left = slow.spawn.hits_left;
        slow.rest_left = 2;

        const gm::ProjectileTickResult ramp_hit =
            gm::projectile_step(slow, no_targets, ramp_world, 1);
        CHECK(ramp_hit.status == gm::ProjectileStatus::bounced);
        CHECK_EQ(slow.hits_left, 4);
        CHECK_NEAR(slow.speed, 0.08, 1e-12); // damping applied on the bounce
        CHECK(gm::dot(slow.direction, normal) > 0.0); // reflected out
        // A real contact restarts the rest window instead of counting it.
        CHECK_EQ(slow.rest_left, 2);
    }
}

void test_projectile_accumulated_dt() {
    const gm::ProjectileTargets no_targets;

    // Hybrid Euler with `dt = 5` is one step, not five: vz = 1, z = -5 after a
    // single 5-tick step against z = -3 after five 1-tick steps.
    gm::ProjectileState state;
    state.spawn.position = {0.0, 0.0, 0.0};
    state.spawn.direction = {1.0, 0.0, 0.0};
    state.spawn.speed = 2.0;
    state.spawn.gravity = 1.0;
    state.spawn.level_gravity = 1.0;
    state.spawn.life_ticks = 100;
    state.position = {0.0, 0.0, 0.0};
    state.direction = {1.0, 0.0, 0.0};
    state.speed = 2.0;
    state.life_left = 100;

    gm::ProjectileState single = state;
    for (int tick = 0; tick < 5; ++tick) {
        gm::projectile_step(single, no_targets, gm::CollisionWorld{}, 1);
    }
    CHECK_NEAR(single.position.x, 10.0, 1e-12);
    CHECK_NEAR(single.position.z, -3.0, 1e-12);
    CHECK_NEAR(single.vertical_speed, 1.0, 1e-12);
    CHECK_EQ(single.life_left, 95);

    gm::ProjectileSpawn spawn;
    spawn.thing_id = 11;
    spawn.position = state.position;
    spawn.direction = state.direction;
    spawn.speed = 2.0;
    spawn.gravity = 1.0;
    spawn.level_gravity = 1.0;
    spawn.life_ticks = 100;
    gm::Projectiles projectiles;
    CHECK_EQ(projectiles.spawn(spawn), 0);
    projectiles.tick(5, no_targets, gm::CollisionWorld{}, nullptr);

    const gm::ProjectileState& by_tick = *projectiles.by_thing_id(11);
    const gm::ProjectileTickResult by_step =
        gm::projectile_step(state, no_targets, gm::CollisionWorld{}, 5);
    CHECK(by_step.status == gm::ProjectileStatus::flying);
    CHECK(by_step.events.empty());
    CHECK_NEAR(by_step.position.x, 10.0, 1e-12);
    CHECK_NEAR(by_step.position.z, -5.0, 1e-12);
    CHECK_NEAR(by_step.vertical_speed, 1.0, 1e-12);
    CHECK_NEAR(by_step.speed, 2.0, 1e-12);
    CHECK_EQ(by_step.hits_left, 0);
    CHECK_EQ(by_step.life_left, 95);
    CHECK_NEAR(by_tick.position.x, by_step.position.x, 1e-12);
    CHECK_NEAR(by_tick.position.y, by_step.position.y, 1e-12);
    CHECK_NEAR(by_tick.position.z, by_step.position.z, 1e-12);
    CHECK_NEAR(by_tick.direction.x, by_step.direction.x, 1e-12);
    CHECK_NEAR(by_tick.direction.y, by_step.direction.y, 1e-12);
    CHECK_NEAR(by_tick.direction.z, by_step.direction.z, 1e-12);
    CHECK_NEAR(by_tick.speed, by_step.speed, 1e-12);
    CHECK_NEAR(by_tick.vertical_speed, by_step.vertical_speed, 1e-12);
    CHECK_EQ(by_tick.hits_left, by_step.hits_left);
    CHECK_EQ(by_tick.life_left, by_step.life_left);
    CHECK_NEAR(by_tick.position.z, -5.0, 1e-12);
    CHECK(by_tick.position.z < single.position.z); // one step, not five

    // A bounce inside the same 5-tick step is identical through both paths and
    // posts exactly one collide record.
    std::vector<TriSpec> floor;
    add_quad(floor, {-100.0, 100.0, 0.0}, {-100.0, -100.0, 0.0},
             {100.0, -100.0, 0.0}, {100.0, 100.0, 0.0});
    const gm::CollisionWorld world = build_world(floor);
    // `speed = 18` also satisfies the original collide gate (`dt <=
    // |vz_old| + speed_old`, 5 <= 18), so both paths post the same record.
    gm::ProjectileState bulk_bounce;
    bulk_bounce.spawn.thing_id = 12;
    bulk_bounce.spawn.position = {0.0, 0.0, 25.0};
    bulk_bounce.spawn.direction = {0.0, 0.0, -1.0};
    bulk_bounce.spawn.speed = 18.0;
    bulk_bounce.spawn.damping = 0.8;
    bulk_bounce.spawn.radius = 10.0;
    bulk_bounce.spawn.collide_event = 604;
    bulk_bounce.position = bulk_bounce.spawn.position;
    bulk_bounce.direction = bulk_bounce.spawn.direction;
    bulk_bounce.speed = 18.0;
    bulk_bounce.damping = 0.8;
    const gm::ProjectileTickResult bulk_result =
        gm::projectile_step(bulk_bounce, no_targets, world, 5);
    CHECK(bulk_result.status == gm::ProjectileStatus::bounced);
    CHECK_EQ(bulk_result.events.size(), 1U);
    CHECK_NEAR(bulk_bounce.position.z, 10.0, 1e-3);
    CHECK_NEAR(bulk_bounce.direction.z, 1.0, 1e-12);
    CHECK_NEAR(bulk_bounce.speed, 18.0 * 0.8 * 0.5, 1e-12);

    gm::ProjectileSpawn bouncer;
    bouncer.thing_id = 12;
    bouncer.position = bulk_bounce.spawn.position;
    bouncer.direction = bulk_bounce.spawn.direction;
    bouncer.speed = 18.0;
    bouncer.damping = 0.8;
    bouncer.radius = 10.0;
    bouncer.collide_event = 604;
    gm::Projectiles bouncers;
    gm::EventQueue bounce_queue;
    CHECK_EQ(bouncers.spawn(bouncer, &bounce_queue), 0);
    bouncers.tick(5, no_targets, world, &bounce_queue);
    const gm::EventParseResult bounced_events = bounce_queue.drain();
    CHECK_EQ(bounced_events.events.size(), 1U);
    if (bounced_events.events.size() == 1) {
        CHECK_EQ(bounced_events.events[0].id, 604);
    }
    CHECK_NEAR(bouncers.by_thing_id(12)->position.z, bulk_bounce.position.z, 1e-12);
    CHECK_NEAR(bouncers.by_thing_id(12)->speed, bulk_bounce.speed, 1e-12);

    // Lifetime boundary with the accumulated dt: exactly one expire, one event.
    gm::ProjectileSpawn timed;
    timed.thing_id = 13;
    timed.position = {0.0, 0.0, 0.0};
    timed.direction = {1.0, 0.0, 0.0};
    timed.speed = 0.0;
    timed.life_ticks = 5;
    timed.expire_event = 702;
    gm::Projectiles expiring;
    gm::EventQueue queue;
    CHECK_EQ(expiring.spawn(timed, &queue), 0);
    CHECK(queue.empty());
    expiring.tick(5, no_targets, gm::CollisionWorld{}, &queue);
    const gm::EventParseResult expired = queue.drain();
    CHECK_EQ(expired.events.size(), 1U);
    if (expired.events.size() == 1) {
        CHECK_EQ(expired.events[0].id, 702);
    }
    const gm::ProjectileState* spent = expiring.by_thing_id(13);
    CHECK(spent != nullptr);
    if (spent != nullptr) {
        CHECK(spent->spent);
        CHECK(!spent->dead);
        CHECK_EQ(spent->life_left, 0);
    }
    // A spent record is skipped by `tick` and posts nothing again.
    expiring.tick(5, no_targets, gm::CollisionWorld{}, &queue);
    CHECK(queue.empty());
    expiring.tick(1, no_targets, gm::CollisionWorld{}, &queue);
    CHECK(queue.empty());

    // Manual path: the expire is in the result once and posting it once yields
    // one record; re-stepping the spent state returns an empty event list.
    gm::ProjectileState manual;
    manual.spawn = timed;
    manual.position = timed.position;
    manual.direction = timed.direction;
    manual.life_left = timed.life_ticks;
    gm::EventQueue manual_queue;
    const gm::ProjectileTickResult expire_result =
        gm::projectile_step(manual, no_targets, gm::CollisionWorld{}, 5);
    CHECK(expire_result.status == gm::ProjectileStatus::expired);
    CHECK_EQ(expire_result.events.size(), 1U);
    CHECK_EQ(gm::post_projectile_events(manual_queue, expire_result), 1U);
    const gm::EventParseResult manual_events = manual_queue.drain();
    CHECK_EQ(manual_events.events.size(), 1U);
    if (manual_events.events.size() == 1) {
        CHECK_EQ(manual_events.events[0].id, 702);
    }
    const gm::ProjectileTickResult again =
        gm::projectile_step(manual, no_targets, gm::CollisionWorld{}, 5);
    CHECK(again.status == gm::ProjectileStatus::expired);
    CHECK(again.events.empty());
    CHECK_EQ(gm::post_projectile_events(manual_queue, again), 0U);
    CHECK(manual_queue.empty());
}

void test_projectile_remove_events() {
    const gm::ProjectileTargets no_targets;

    gm::ProjectileSpawn spawn;
    spawn.thing_id = 21;
    spawn.position = {0.0, 0.0, 0.0};
    spawn.direction = {1.0, 0.0, 0.0};
    spawn.speed = 1.0;
    spawn.life_ticks = 1;
    spawn.create_event = 801;
    spawn.expire_event = 802;
    spawn.destroy_event = 803;

    gm::Projectiles projectiles;
    gm::EventQueue queue;
    CHECK_EQ(projectiles.spawn(spawn, &queue), 0);
    const gm::EventParseResult created = queue.drain();
    CHECK_EQ(created.events.size(), 1U);
    if (created.events.size() == 1) {
        CHECK_EQ(created.events[0].id, 801);
    }

    projectiles.tick(1, no_targets, gm::CollisionWorld{}, &queue);
    const gm::EventParseResult expired = queue.drain();
    CHECK_EQ(expired.events.size(), 1U);
    if (expired.events.size() == 1) {
        CHECK_EQ(expired.events[0].id, 802);
    }
    const gm::ProjectileState* state = projectiles.by_thing_id(21);
    CHECK(state != nullptr);
    if (state != nullptr) {
        CHECK(state->spent);
        CHECK(!state->dead);
    }
    // `spent` means the expire was posted: ticking again must not repeat it.
    for (int tick = 0; tick < 3; ++tick) {
        projectiles.tick(1, no_targets, gm::CollisionWorld{}, &queue);
        CHECK(queue.empty());
    }

    // Destroy is a separate record and exactly one per projectile.
    CHECK(projectiles.remove(21, &queue));
    const gm::EventParseResult destroyed = queue.drain();
    CHECK_EQ(destroyed.events.size(), 1U);
    if (destroyed.events.size() == 1) {
        CHECK_EQ(destroyed.events[0].id, 803);
    }
    if (state != nullptr) {
        CHECK(state->dead);
        CHECK(state->spent);
    }
    CHECK(!projectiles.remove(21, &queue));
    CHECK(queue.empty());
    projectiles.tick(5, no_targets, gm::CollisionWorld{}, &queue);
    CHECK(queue.empty());

    // Removing before the lifetime expires also posts destroy once, and the
    // dead record produces nothing afterwards.
    gm::ProjectileSpawn live = spawn;
    live.thing_id = 22;
    live.life_ticks = 100;
    live.create_event = 0;
    CHECK_EQ(projectiles.spawn(live, &queue), 1);
    CHECK(queue.empty());
    projectiles.tick(1, no_targets, gm::CollisionWorld{}, &queue);
    CHECK(queue.empty());
    CHECK(projectiles.remove(22, &queue));
    const gm::EventParseResult destroyed_live = queue.drain();
    CHECK_EQ(destroyed_live.events.size(), 1U);
    if (destroyed_live.events.size() == 1) {
        CHECK_EQ(destroyed_live.events[0].id, 803);
    }
    CHECK(!projectiles.remove(22, &queue));
    CHECK(queue.empty());
    for (int tick = 0; tick < 3; ++tick) {
        projectiles.tick(1, no_targets, gm::CollisionWorld{}, &queue);
        CHECK(queue.empty());
    }
}

void test_projectile_blast_bridge() {
    const gm::ProjectileTargets no_targets;

    std::vector<TriSpec> floor;
    add_quad(floor, {-100.0, 100.0, 0.0}, {-100.0, -100.0, 0.0},
             {100.0, -100.0, 0.0}, {100.0, 100.0, 0.0});
    const gm::CollisionWorld world = build_world(floor);

    // Synthetic grenade: the two branches carry distinguishable event ids so a
    // hit proves which vector was selected.
    gm::Weapon grenade;
    grenade.name = "GRENADE";
    grenade.thing_type = 28;
    gm::Blast impact;
    impact.plate = 100;
    impact.hull = 50;
    impact.radius = 50;
    impact.event = 11;
    grenade.impact_blasts.push_back(impact);
    gm::Blast expiry;
    expiry.plate = 200;
    expiry.hull = 100;
    expiry.radius = 50;
    expiry.event = 22;
    grenade.expire_blasts.push_back(expiry);

    std::array<gm::Vec3d, 1> thing_pos{{{0.0, 0.0, 10.0}}};
    std::array<bool, 1> thing_alive{true};
    std::array<gm::ThingVitals, 1> vitals{};
    vitals[0].hp = 1000;
    vitals[0].max_hp = 1000;
    vitals[0].alive = true;
    gm::ProjectileBlastTargets targets;
    targets.thing_positions = thing_pos;
    targets.thing_alive = thing_alive;
    targets.thing_vitals = vitals;
    gm::ProjectileBlastContext context;

    // 1. Bounce -> exactly one blast through the impact branch at
    //    result.position.
    gm::ProjectileState bounce;
    bounce.spawn.direction = {0.0, 0.0, -1.0};
    bounce.spawn.speed = 2.0;
    bounce.spawn.radius = 10.0;
    bounce.spawn.damping = 0.8;
    bounce.spawn.hits_left = 0;
    bounce.position = {0.0, 0.0, 25.0};
    bounce.direction = {0.0, 0.0, -1.0};
    bounce.speed = 2.0;
    bounce.damping = 0.8;
    gm::ProjectileTickResult bounced;
    for (int i = 0; i < 8; ++i) {
        bounced = gm::projectile_step(bounce, no_targets, world, 1);
        if (bounced.status == gm::ProjectileStatus::bounced) {
            break;
        }
    }
    CHECK(bounced.status == gm::ProjectileStatus::bounced);
    CHECK(!bounced.already_spent);
    CHECK_NEAR(bounced.position.z, 10.0, 1e-3);
    thing_pos[0] = bounced.position; // distance 0: both blasts cover it
    const std::int32_t hp_before_bounce = vitals[0].hp;
    gm::BlastDamageResult applied_bounce;
    const std::size_t bounce_hits = gm::apply_projectile_blast(
        grenade, bounced, false, context, targets, &applied_bounce);
    CHECK_EQ(bounce_hits, 1U);
    CHECK_EQ(applied_bounce.hits.size(), 1U);
    if (!applied_bounce.hits.empty()) {
        CHECK_EQ(applied_bounce.hits[0].hit.event, 11U);
    }
    CHECK(vitals[0].hp < hp_before_bounce);
    CHECK(applied_bounce.chain_reaction); // plate > 0 detonates type -4

    // A flying / destroyed result never blasts.
    gm::ProjectileState flying;
    flying.position = bounced.position;
    flying.direction = {1.0, 0.0, 0.0};
    flying.speed = 1.0;
    const gm::ProjectileTickResult flight =
        gm::projectile_step(flying, no_targets, gm::CollisionWorld{}, 1);
    CHECK(flight.status == gm::ProjectileStatus::flying);
    CHECK_EQ(gm::apply_projectile_blast(grenade, flight, false, context, targets),
             0U);
    gm::ProjectileState dead;
    dead.dead = true;
    const gm::ProjectileTickResult destroyed =
        gm::projectile_step(dead, no_targets, world, 1);
    CHECK(destroyed.status == gm::ProjectileStatus::destroyed);
    CHECK_EQ(gm::apply_projectile_blast(grenade, destroyed, true, context,
                                        targets),
             0U);

    // 2. Expiry (last hit) -> one blast through the expire branch.
    gm::ProjectileState one_hit;
    one_hit.spawn.direction = {0.0, 0.0, -1.0};
    one_hit.spawn.speed = 2.0;
    one_hit.spawn.radius = 10.0;
    one_hit.spawn.damping = 0.8;
    one_hit.spawn.hits_left = 1;
    one_hit.position = {0.0, 0.0, 25.0};
    one_hit.direction = {0.0, 0.0, -1.0};
    one_hit.speed = 2.0;
    one_hit.damping = 0.8;
    one_hit.hits_left = 1;
    gm::ProjectileTickResult expired;
    for (int i = 0; i < 8; ++i) {
        expired = gm::projectile_step(one_hit, no_targets, world, 1);
        if (expired.status == gm::ProjectileStatus::expired) {
            break;
        }
    }
    CHECK(expired.status == gm::ProjectileStatus::expired);
    CHECK(!expired.already_spent);
    CHECK(one_hit.spent);
    thing_pos[0] = expired.position;
    const std::int32_t hp_before_expiry = vitals[0].hp;
    gm::BlastDamageResult applied_expiry;
    const std::size_t expiry_hits = gm::apply_projectile_blast(
        grenade, expired, true, context, targets, &applied_expiry);
    CHECK_EQ(expiry_hits, 1U);
    CHECK_EQ(applied_expiry.hits.size(), 1U);
    if (!applied_expiry.hits.empty()) {
        CHECK_EQ(applied_expiry.hits[0].hit.event, 22U);
    }
    CHECK(vitals[0].hp < hp_before_expiry);

    // 3. The spent record replays `expired` on every later step; the bridge
    //    must not blast again (this is the double-blast regression).
    const gm::ProjectileTickResult replay =
        gm::projectile_step(one_hit, no_targets, world, 1);
    CHECK(replay.status == gm::ProjectileStatus::expired);
    CHECK(replay.already_spent);
    CHECK(replay.events.empty());
    const std::int32_t hp_after_expiry = vitals[0].hp;
    CHECK_EQ(gm::apply_projectile_blast(grenade, replay, true, context, targets),
             0U);
    CHECK_EQ(vitals[0].hp, hp_after_expiry);

    // A lifetime expiry takes the same path: one blast, then silence.
    gm::ProjectileState timed;
    timed.position = bounced.position;
    timed.direction = {1.0, 0.0, 0.0};
    timed.spawn.life_ticks = 1;
    timed.life_left = 1;
    const gm::ProjectileTickResult timed_out =
        gm::projectile_step(timed, no_targets, gm::CollisionWorld{}, 1);
    CHECK(timed_out.status == gm::ProjectileStatus::expired);
    CHECK(!timed_out.already_spent);
    CHECK_EQ(gm::apply_projectile_blast(grenade, timed_out, true, context,
                                        targets),
             1U);
    const gm::ProjectileTickResult timed_replay =
        gm::projectile_step(timed, no_targets, gm::CollisionWorld{}, 1);
    CHECK(timed_replay.already_spent);
    CHECK_EQ(gm::apply_projectile_blast(grenade, timed_replay, true, context,
                                        targets),
             0U);

    // 4. Proximity: no blast by default, one when the caller opts in.
    const std::array<gm::Vec3d, 1> player_pos = {bounced.position};
    const std::array<double, 1> player_radius = {{5.0}};
    const std::array<std::uint8_t, 1> player_kind = {{1}};
    const std::array<std::uint8_t, 1> player_object = {{1}};
    gm::ProjectileTargets prox_targets;
    prox_targets.player_positions = player_pos;
    prox_targets.player_radii = player_radius;
    prox_targets.player_kind = player_kind;
    prox_targets.player_has_object = player_object;
    gm::ProjectileState prox;
    prox.spawn.thing_id = 3;
    prox.spawn.owner_player = -1;
    prox.spawn.proximity.push_back(gm::ProximityEntry{123, 0, 20, 0, {}});
    prox.position = bounced.position;
    prox.direction = {1.0, 0.0, 0.0};
    prox.speed = 2.0;
    const gm::ProjectileTickResult proximity =
        gm::projectile_step(prox, prox_targets, world, 1);
    CHECK(proximity.status == gm::ProjectileStatus::proximity);
    std::array<gm::Vitals, 1> player_vitals{};
    player_vitals[0].armor = 1000;
    player_vitals[0].armor_max = 1000;
    gm::ProjectileBlastTargets prox_blast_targets;
    prox_blast_targets.player_positions = player_pos;
    prox_blast_targets.player_vitals = player_vitals;
    const std::int32_t armor_before = player_vitals[0].armor;
    CHECK_EQ(gm::apply_projectile_blast(grenade, proximity, false, context,
                                        prox_blast_targets),
             0U);
    CHECK_EQ(player_vitals[0].armor, armor_before);
    gm::ProjectileBlastContext proximity_context;
    proximity_context.allow_proximity = true;
    CHECK_EQ(gm::apply_projectile_blast(grenade, proximity, false,
                                        proximity_context, prox_blast_targets),
             1U);
    CHECK(player_vitals[0].armor < armor_before);

    // 5. Real ROCKET from level0: the same bridge picks its two collide/expire
    //    blasts (docs/WEAPONS_CPP.md §7).
    std::optional<gm::Weapons> table;
    if (fs::exists(kGame / "level0.sbt")) {
        std::string error;
        table = gm::Weapons::load(kGame / "level0.sbt", &error);
        CHECK(table.has_value());
    }
    if (table) {
        const gm::Weapon* rocket = nullptr;
        for (const gm::Weapon& weapon : table->table()) {
            if (weapon.name == "ROCKET") {
                rocket = &weapon;
                break;
            }
        }
        CHECK(rocket != nullptr);
        if (rocket != nullptr) {
            std::array<gm::Vec3d, 1> rocket_pos{{{0.0, 0.0, 0.0}}};
            std::array<bool, 1> rocket_alive{true};
            std::array<gm::ThingVitals, 1> rocket_vitals{};
            rocket_vitals[0].hp = 200000;
            rocket_vitals[0].max_hp = 200000;
            rocket_vitals[0].alive = true;
            gm::ProjectileBlastTargets rocket_targets;
            rocket_targets.thing_positions = rocket_pos;
            rocket_targets.thing_alive = rocket_alive;
            rocket_targets.thing_vitals = rocket_vitals;

            gm::ProjectileState rocket_bounce;
            rocket_bounce.spawn.direction = {0.0, 0.0, -1.0};
            rocket_bounce.spawn.speed = 2.0;
            rocket_bounce.spawn.radius = 10.0;
            rocket_bounce.position = {0.0, 0.0, 25.0};
            rocket_bounce.direction = {0.0, 0.0, -1.0};
            rocket_bounce.speed = 2.0;
            gm::ProjectileTickResult rocket_bounced;
            for (int i = 0; i < 8; ++i) {
                rocket_bounced = gm::projectile_step(
                    rocket_bounce, no_targets, world, 1);
                if (rocket_bounced.status == gm::ProjectileStatus::bounced) {
                    break;
                }
            }
            CHECK(rocket_bounced.status == gm::ProjectileStatus::bounced);
            rocket_pos[0] = rocket_bounced.position;
            const std::int32_t rocket_hp = rocket_vitals[0].hp;
            CHECK_EQ(gm::apply_projectile_blast(*rocket, rocket_bounced, false,
                                                context, rocket_targets),
                     2U);
            CHECK(rocket_vitals[0].hp < rocket_hp);

            gm::ProjectileState rocket_expire;
            rocket_expire.spawn.direction = {0.0, 0.0, -1.0};
            rocket_expire.spawn.speed = 2.0;
            rocket_expire.spawn.radius = 10.0;
            rocket_expire.spawn.hits_left = 1;
            rocket_expire.position = {0.0, 0.0, 25.0};
            rocket_expire.direction = {0.0, 0.0, -1.0};
            rocket_expire.speed = 2.0;
            rocket_expire.hits_left = 1;
            gm::ProjectileTickResult rocket_expired;
            for (int i = 0; i < 8; ++i) {
                rocket_expired = gm::projectile_step(
                    rocket_expire, no_targets, world, 1);
                if (rocket_expired.status == gm::ProjectileStatus::expired) {
                    break;
                }
            }
            CHECK(rocket_expired.status == gm::ProjectileStatus::expired);
            rocket_pos[0] = rocket_expired.position;
            rocket_vitals[0].hp = 200000;
            CHECK_EQ(gm::apply_projectile_blast(*rocket, rocket_expired, true,
                                                context, rocket_targets),
                     2U);
            const gm::ProjectileTickResult rocket_replay =
                gm::projectile_step(rocket_expire, no_targets, world, 1);
            CHECK(rocket_replay.already_spent);
            CHECK_EQ(gm::apply_projectile_blast(*rocket, rocket_replay, true,
                                                context, rocket_targets),
                     0U);
        }
    }
}


int main() {
    test_fire_rate_quantisation();
    test_single_and_auto();
    test_empty_ammo();
    test_magazine_and_reload();
    test_switch_slot_and_mask();
    test_hitscan_hit_miss();
    test_spread_determinism();
    test_projectile();
    test_recoil();
    test_damage_and_armor();
    test_damage_gauge();
    test_negatives_synthetic();
    test_projectile_core();
    test_projectile_rgm_homing();
    test_projectile_shot_bridge();
    test_projectile_floor_bounces();
    test_projectile_rest_contact();
    test_projectile_accumulated_dt();
    test_projectile_remove_events();
    test_projectile_blast_bridge();
    test_weapon_impact_event();
    test_spawned_thing_damage();

    if (fs::exists(kGame / "SAVEGAME" / "saved0.sbt") &&
        fs::exists(kGame / "SAVEGAME" / "saved1.sbt") &&
        fs::exists(kGame / "level0.sbt")) {
        test_real_weapon_modes();
        test_weapon_corpus();
        test_runtime_blast_bridge();
        test_thing_runtime_damage();
        for (const GoldenSave& golden : kSaves) {
            test_real_save(golden);
        }
        test_real_negatives();
    } else {
        std::printf("weapons: game install not found, real-save checks skipped\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "weapons: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("weapons: ok\n");
    return 0;
}
