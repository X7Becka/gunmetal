// Player input -> weapon tick seam (docs/WEAPONS_CPP.md §3.6).
//
// Synthetic tables pin the rules taken from `0x4453d0`:
//   * fire mode 0 shoots on the trigger edge only (0x445d85, `+0x3344 & 0x20`);
//   * fire mode 1 shoots while held, the rate is the weapon period `+0x2c`
//     (0x445d64 + cooldown gate 0x445aa2);
//   * fire mode 2 charges and shoots on release, mode 3 auto-shoots at full
//     (`charge = 1.0`, 0x445d37);
//   * a slot request goes through the mask and resets the accumulator
//     (`FUN_00445300`, 0x445300);
//   * a shot consumes ammo from `+0x329c` and a spawned round posts its create
//     event, exactly like the existing `weapons` bridge.
//
// No real game data is needed: the real weapon/charge goldens live in
// `test_weapons.cpp`.
#include "game/trigger.h"

#include "game/events.h"
#include "game/projectile.h"
#include "game/runtime.h"
#include "game/weapons.h"
#include "render/collide.h"
#include "world/things.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

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

// --- synthetic data --------------------------------------------------------

/// One table record.  `slot` must match its index (`fire()` reads
/// `arsenal.current` and the table by the same number).
gm::Weapon make_weapon(int slot, int fire_mode, int fire_ticks) {
    gm::Weapon weapon;
    weapon.name = "TRIGGER" + std::to_string(slot);
    weapon.icon = "WICON0";
    weapon.slot = slot;
    weapon.id = 2000u + static_cast<std::uint32_t>(slot);
    weapon.fire_mode = fire_mode;
    weapon.fire_period_ms = fire_ticks * 1000 / 91;
    weapon.fire_ticks = fire_ticks;
    weapon.ammo_type = 0;
    weapon.ammo_per_shot = 1;
    weapon.projectile_id = 3000u + static_cast<std::uint32_t>(slot);
    weapon.projectile_alt = 4000u + static_cast<std::uint32_t>(slot);
    weapon.state = 5000u + static_cast<std::uint32_t>(slot);
    weapon.class_index = 0;
    weapon.spread = 0.0;
    weapon.spread_y = 0.0;
    weapon.range = 0.0; // unlimited
    weapon.muzzle = 0;
    // Hypothesis layer: `fire()` resolves a projectile when !hitscan.
    weapon.damage = 10.0;
    weapon.magazine = 0;
    weapon.reload_ticks = 0;
    weapon.hitscan = true;
    weapon.projectile_speed = 100.0;
    weapon.projectile_charge = 10.0;
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

gm::PlayerWeaponInput pull(bool held, bool edge) {
    gm::PlayerWeaponInput input;
    input.trigger_held = held;
    input.trigger_edge = edge;
    input.aim_direction = {1.0, 0.0, 0.0};
    return input;
}

// --- tests -----------------------------------------------------------------

void test_semi_auto_edge_only() {
    // Mode 0 = `0x445d85`: the `+0x3344 & 0x20` edge bit alone decides.
    gm::Weapon semi = make_weapon(0, 0, 5);
    gm::Weapons table({semi});
    gm::Arsenal arsenal = make_arsenal(0x1, 0, 10);
    gm::ChargeState charge;
    const gm::CollisionWorld world;

    const gm::WeaponActionResult first =
        gm::update_player_weapon(table, arsenal, charge, pull(true, true),
                                 {0.0, 0.0, 0.0}, world);
    CHECK(first.fired());
    CHECK(first.shot.ok);
    CHECK_EQ(first.slot, 0);
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(9));
    CHECK_EQ(arsenal.cooldown[0], 5);
    CHECK_EQ(arsenal.shot_index, std::uint64_t(1));

    // Still held, no edge, cooldown ticks down: no shot and no ammo spent.
    const gm::WeaponActionResult held =
        gm::update_player_weapon(table, arsenal, charge, pull(true, false),
                                 {0.0, 0.0, 0.0}, world);
    CHECK(!held.fired());
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(9));
    CHECK_EQ(arsenal.cooldown[0], 4);

    // A new edge during the cooldown is rejected by `fire()` (0x445aa2 gate
    // means the charge machine does not even ask; the ammo stays intact).
    const gm::WeaponActionResult cooling =
        gm::update_player_weapon(table, arsenal, charge, pull(true, true),
                                 {0.0, 0.0, 0.0}, world);
    CHECK(!cooling.fired());
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(9));

    // After the period expires an edge shoots again.
    for (int i = 0; i < 4; ++i) {
        (void)gm::update_player_weapon(table, arsenal, charge, pull(false, false),
                                       {0.0, 0.0, 0.0}, world);
    }
    CHECK_EQ(arsenal.cooldown[0], 0);
    const gm::WeaponActionResult again =
        gm::update_player_weapon(table, arsenal, charge, pull(true, true),
                                 {0.0, 0.0, 0.0}, world);
    CHECK(again.fired());
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(8));
}

void test_auto_held_rate() {
    // Mode 1 = `0x445d64`: held OR edge sets the shot flag; the `+0x3318`
    // cooldown (weapon `+0x2c`) limits the rate to one shot per period.
    gm::Weapon automatic = make_weapon(1, 1, 3);
    gm::Weapons table({automatic});
    gm::Arsenal arsenal = make_arsenal(0x1, 0, 10);
    gm::ChargeState charge;
    const gm::CollisionWorld world;

    int shots = 0;
    const gm::WeaponActionResult first =
        gm::update_player_weapon(table, arsenal, charge, pull(true, true),
                                 {0.0, 0.0, 0.0}, world);
    shots += first.fired() ? 1 : 0;
    CHECK(first.fired());
    CHECK_EQ(arsenal.cooldown[0], 3);

    // Ticks 2 and 3 are held but cooling -> no shot.
    for (int i = 0; i < 2; ++i) {
        const gm::WeaponActionResult held =
            gm::update_player_weapon(table, arsenal, charge, pull(true, false),
                                     {0.0, 0.0, 0.0}, world);
        shots += held.fired() ? 1 : 0;
        CHECK(!held.fired());
    }
    CHECK_EQ(arsenal.cooldown[0], 1);

    // Tick 4: cooldown reaches zero and the held trigger shoots.
    const gm::WeaponActionResult fourth =
        gm::update_player_weapon(table, arsenal, charge, pull(true, false),
                                 {0.0, 0.0, 0.0}, world);
    shots += fourth.fired() ? 1 : 0;
    CHECK(fourth.fired());
    CHECK_EQ(shots, 2);
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(8));
}

void test_charge_threshold_and_release() {
    // Mode 2 = charge, shot on release (0x445af6..0x445b0e).  `charge_rate`
    // 0.5 needs two ticks to reach exactly 1.0; the exact 1.0 boundary is
    // "not over" (0x445cf1 `FCOMP 1.0`), so the shot happens on release with
    // the latched `player[+0x2fa0]` = 1.0.
    gm::Weapon charged = make_weapon(0, 2, 1);
    charged.hitscan = false;
    charged.charge_rate = 0.5;
    gm::Weapons table({charged});
    gm::Arsenal arsenal = make_arsenal(0x1, 0, 10);
    gm::ChargeState charge;
    const gm::CollisionWorld world;

    const gm::WeaponActionResult first =
        gm::update_player_weapon(table, arsenal, charge, pull(true, true),
                                 {0.0, 0.0, 0.0}, world);
    CHECK(!first.fired());
    CHECK_NEAR(charge.accumulated, 0.5, 0.0);
    CHECK(first.charge.started);
    CHECK_EQ(first.charge.start_event, charged.projectile_alt);
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(10));

    const gm::WeaponActionResult second =
        gm::update_player_weapon(table, arsenal, charge, pull(true, false),
                                 {0.0, 0.0, 0.0}, world);
    CHECK(!second.fired());
    CHECK_NEAR(charge.accumulated, 1.0, 0.0);
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(10));

    const gm::WeaponActionResult release =
        gm::update_player_weapon(table, arsenal, charge, pull(false, false),
                                 {0.0, 0.0, 0.0}, world);
    CHECK(release.fired());
    CHECK_NEAR(release.shot.damage, 10.0, 0.0); // synthetic weapon damage
    CHECK(release.shot.projectile);
    // 0x46f0f0: speed = base + charge * operand[9], both units/tick.
    CHECK_NEAR(release.shot.projectile_speed, 100.0 + 1.0 * 10.0 * 91.0, 1e-12);
    CHECK_EQ(arsenal.ammo[0], std::uint32_t(9));

    // Mode 3 auto-fires when the accumulator crosses 1.0 and latches
    // `charge = 1.0` (0x445d37).
    gm::Weapon auto_charge = make_weapon(0, 3, 1);
    auto_charge.hitscan = false;
    auto_charge.charge_rate = 0.6;
    gm::Weapons auto_table({auto_charge});
    gm::Arsenal auto_arsenal = make_arsenal(0x1, 0, 10);
    gm::ChargeState auto_state;
    const gm::WeaponActionResult below =
        gm::update_player_weapon(auto_table, auto_arsenal, auto_state,
                                 pull(true, true), {0.0, 0.0, 0.0}, world);
    CHECK(!below.fired());
    CHECK_NEAR(auto_state.accumulated, 0.6, 1e-15);
    const gm::WeaponActionResult over =
        gm::update_player_weapon(auto_table, auto_arsenal, auto_state,
                                 pull(true, false), {0.0, 0.0, 0.0}, world);
    CHECK(over.fired());
    CHECK_NEAR(auto_state.charge, 1.0, 0.0);
    CHECK_NEAR(over.shot.projectile_speed, 100.0 + 1.0 * 10.0 * 91.0, 1e-12);
    CHECK_EQ(auto_arsenal.ammo[0], std::uint32_t(9));
}

void test_slot_change() {
    // `FUN_00445300` (0x445300): mask check, slot write, accumulator reset.
    gm::Weapon semi = make_weapon(0, 0, 1);
    gm::Weapon automatic = make_weapon(1, 1, 4);
    gm::Weapons table({semi, automatic});
    gm::Arsenal arsenal = make_arsenal(0x3, 0, 10);
    gm::ChargeState charge;
    charge.accumulated = 0.75;
    charge.state_event = 777;
    const gm::CollisionWorld world;

    gm::PlayerWeaponInput input = pull(false, false);
    input.slot = 1;
    input.slot_edge = true;
    const gm::WeaponActionResult switched =
        gm::update_player_weapon(table, arsenal, charge, input, {0.0, 0.0, 0.0},
                                 world);
    CHECK(switched.slot_changed);
    CHECK(!switched.fired());
    CHECK_EQ(switched.slot, 1);
    CHECK_EQ(arsenal.current, 1);
    CHECK_EQ(switched.switched_state_event, 777u);
    CHECK_EQ(charge.state_event, 0u);
    CHECK_NEAR(charge.accumulated, 0.0, 0.0);

    // The next edge fires the *new* weapon: its period is 4, not the old 1.
    input = pull(true, true);
    input.slot = 1;
    const gm::WeaponActionResult shot =
        gm::update_player_weapon(table, arsenal, charge, input, {0.0, 0.0, 0.0},
                                 world);
    CHECK(shot.fired());
    CHECK_EQ(shot.slot, 1);
    CHECK_EQ(shot.shot.weapon_id, automatic.id);
    CHECK_EQ(arsenal.cooldown[1], 4);

    // A slot without a mask bit is rejected and keeps the current weapon.
    gm::PlayerWeaponInput bad = pull(false, false);
    bad.slot = 2;
    bad.slot_edge = true;
    const gm::WeaponActionResult rejected =
        gm::update_player_weapon(table, arsenal, charge, bad, {0.0, 0.0, 0.0},
                                 world);
    CHECK(!rejected.slot_changed);
    CHECK(rejected.slot_rejected);
    CHECK(rejected.slot_error == gm::FireError::not_available);
    CHECK_EQ(arsenal.current, 1);

    // Selecting the current slot is a no-op and does not reset the charge.
    charge.accumulated = 0.5;
    gm::PlayerWeaponInput same = pull(false, false);
    same.slot = 1;
    same.slot_edge = true;
    const gm::WeaponActionResult unchanged =
        gm::update_player_weapon(table, arsenal, charge, same, {0.0, 0.0, 0.0},
                                 world);
    CHECK(!unchanged.slot_changed);
    CHECK(!unchanged.slot_rejected);
    CHECK_NEAR(charge.accumulated, 0.5, 0.0);
}

void test_runtime_spawn_and_event() {
    // Full seam: input -> charge -> fire -> `spawn_player_shot()` -> queue.
    gm::Weapon launcher = make_weapon(0, 0, 1);
    launcher.hitscan = false; // THING-style projectile
    gm::Weapons table({launcher});

    gm::GameRuntime runtime;
    runtime.set_player_weapons(table, make_arsenal(0x1, 0, 5));

    gm::Thing grenade;
    grenade.id = -1; // runtime assigns 0x10000+
    grenade.type = 28;
    grenade.movement_type = 6;
    grenade.gravity = 1.0;
    grenade.damping = 0.8;
    grenade.collision_shape = 1;
    grenade.radius = 10.0;
    grenade.hits_left = 3;
    grenade.expire_ticks = 364;
    grenade.create_event = 901;
    runtime.set_player_projectile(grenade, 0);

    const gm::PlayerWeaponTick tick = runtime.update_player_weapon(
        pull(true, true), {10.0, 20.0, 30.0});
    CHECK(tick.action.fired());
    CHECK(tick.spawned >= 0);
    CHECK_EQ(runtime.player_arsenal().ammo[0], std::uint32_t(4));
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));
    if (tick.spawned >= 0) {
        const gm::ProjectileState& state = runtime.projectiles().list()[0];
        CHECK_EQ(state.spawn.thing_id, 0x10000);
        CHECK_EQ(state.spawn.owner_player, 0);
    }

    // The spawn posts its create event through the runtime queue.
    const gm::EventParseResult created = runtime.events().queue().drain();
    CHECK(created.ok);
    CHECK_EQ(created.events.size(), std::size_t(1));
    if (created.events.size() == 1) {
        CHECK_EQ(created.events[0].id, 901);
    }

    // No tables installed: the seam reports `no_weapon` and spawns nothing.
    gm::GameRuntime bare;
    const gm::PlayerWeaponTick none =
        bare.update_player_weapon(pull(true, true), {0.0, 0.0, 0.0});
    CHECK(!none.action.fired());
    CHECK(none.action.shot.error == gm::FireError::no_weapon);
    CHECK_EQ(none.spawned, -1);
}

void test_runtime_tick_phase() {
    // Opt-in phase 2c: one input installs, `tick()` runs the seam before the
    // projectile step, so the round is stepped and the create event dispatched
    // in the same tick.
    gm::Weapon launcher = make_weapon(0, 0, 1);
    launcher.hitscan = false;
    gm::Weapons table({launcher});

    gm::GameRuntimeConfig config;
    config.player_weapons_in_tick = true;
    gm::GameRuntime runtime(config);
    runtime.set_player_weapons(table, make_arsenal(0x1, 0, 5));

    gm::Thing rocket;
    rocket.id = -1;
    rocket.type = 28;
    rocket.movement_type = 6;
    rocket.gravity = 0.0;
    rocket.damping = 0.0;
    rocket.collision_shape = 1;
    rocket.radius = 10.0;
    rocket.hits_left = 0;
    rocket.create_event = 902;
    runtime.set_player_projectile(rocket, 3);

    runtime.set_player_weapon_input(pull(true, true), {0.0, 0.0, 0.0});
    CHECK(runtime.has_player_weapon_input());
    runtime.tick(1);
    CHECK(!runtime.has_player_weapon_input()); // consumed exactly once

    const gm::PlayerWeaponTick& tick = runtime.last_player_weapon_tick();
    CHECK(tick.action.fired());
    CHECK_EQ(tick.action.slot, 0);
    CHECK(tick.spawned >= 0);
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));
    if (!runtime.projectiles().list().empty()) {
        CHECK_EQ(runtime.projectiles().list()[0].spawn.owner_player, 3);
    }
    CHECK(runtime.last_events().ok);
    bool saw_create = false;
    for (const gm::Event& event : runtime.last_events().generated) {
        if (event.id == 902) {
            saw_create = true;
        }
    }
    CHECK(saw_create);

    // Without a new input the next tick does not repeat the trigger: the seam
    // does not run, so no ammo is spent and no second round appears.  (The
    // last-result accessor still describes the seam's most recent call.)
    runtime.tick(1);
    CHECK_EQ(runtime.player_arsenal().ammo[0], std::uint32_t(4));
    CHECK_EQ(runtime.projectiles().list().size(), std::size_t(1));
}

} // namespace

int main() {
    test_semi_auto_edge_only();
    test_auto_held_rate();
    test_charge_threshold_and_release();
    test_slot_change();
    test_runtime_spawn_and_event();
    test_runtime_tick_phase();

    if (g_failures != 0) {
        std::fprintf(stderr, "player_trigger: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("player_trigger: ok\n");
    return 0;
}
