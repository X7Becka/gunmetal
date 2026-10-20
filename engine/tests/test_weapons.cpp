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
#include "game/weapons.h"

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

#include "game/state.h"
#include "player/controller.h"
#include "render/collide.h"
#include "res/save.h"
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
}

void test_runtime_blast_bridge() {
    std::string error;
    const auto table = gm::Weapons::load(kGame / "level0.sbt", &error);
    CHECK(table.has_value());
    if (!table) {
        return;
    }
    const gm::Weapon* rocket = nullptr;
    for (const gm::Weapon& weapon : table->table()) {
        if (weapon.name == "ROCKET") {
            rocket = &weapon;
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
        CHECK_NEAR(disint->secondary_damage, 400.0, 0.0);
        CHECK_NEAR(disint->secondary_range, 200.0, 0.0);
        CHECK_EQ(disint->impact_blasts.size(), 1U);
        if (!disint->impact_blasts.empty()) {
            CHECK_EQ(disint->impact_blasts[0].radius, 200);
        }
    }
    const gm::Weapon* nuke = find("TAC NUKE");
    if (nuke != nullptr) {
        CHECK_EQ(nuke->impact_blasts.size(), 15U); // all guarded branches kept
        CHECK_EQ(nuke->select_blasts(true, false, false, false).size(), 3U);
        CHECK_EQ(nuke->select_blasts(true, true, false, false).size(), 3U);
    }
    const gm::Weapon* rgm = find("RGM");
    if (rgm != nullptr) {
        CHECK(!rgm->hitscan);
        CHECK_EQ(rgm->thing_type, 32);
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

    if (fs::exists(kGame / "SAVEGAME" / "saved0.sbt") &&
        fs::exists(kGame / "SAVEGAME" / "saved1.sbt") &&
        fs::exists(kGame / "level0.sbt")) {
        test_real_weapon_modes();
        test_runtime_blast_bridge();
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
