// Event-driven blast damage: THING_TYPES collide/expire event id -> the
// `CAUSE_BLAST_RADIUS_DAMAGE` ops of that event's sub-script -> player/thing
// damage.
//
// The original chain (docs/PROJECTILE_CPP.md §7):
//   collide 0x4752d0 / expire 0x4703a0 -> event_post 0x4795b0
//   -> event_dispatch 0x4799b0 -> generate_event 0x479960
//   -> generate_runner 0x45fa00 -> VM 0x457390 -> op 75 0x45775b
//   -> 0x477280 -> player 0x453b50 / thing 0x470500.
//
// The synthetic cases pin the registry lookup itself, the exact plate/hull
// arithmetic of `0x477280`/`0x453b50`/`0x470500`, the per-target radius, and
// the runtime bridge; the shipped `level0.sbt` case proves the registry keys
// are the real THING_TYPES collide/expire ids.
#include "game/events.h"

#include "game/runtime.h"
#include "game/weapons.h"
#include "res/sbt.h"
#include "world/things.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
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

constexpr const char* kGame = "/mnt/d/games/Gunmetal 3dfx";

void put_u32(std::vector<std::uint8_t>& out, std::size_t offset,
             std::uint32_t value) {
    std::memcpy(out.data() + offset, &value, 4);
}

void put_i32(std::vector<std::uint8_t>& out, std::size_t offset, int value) {
    std::memcpy(out.data() + offset, &value, 4);
}

/// One SWITCH record: `WorldState::build` rejects a completely empty level and
/// the blast tests do not press anything.
gm::SbtLevel make_level() {
    gm::SbtLevel level;
    gm::SbtRecord switches;
    switches.type = 1;
    switches.payload.assign(4 + 0x20, 0);
    put_u32(switches.payload, 0, 1);
    put_i32(switches.payload, 4, 259);
    put_i32(switches.payload, 8, 999);
    put_i32(switches.payload, 12, 128);
    put_i32(switches.payload, 16, 0);
    level.records.push_back(std::move(switches));
    return level;
}

void write_identity_object_header(std::array<std::uint8_t, 96>& header) {
    header.fill(0);
    for (int point = 0; point < 4; ++point) {
        for (int component = 0; component < 3; ++component) {
            const float value =
                (point == 0) ? 0.0f : (component == point - 1 ? 1.0f : 0.0f);
            std::memcpy(header.data() + 8 * (point * 3 + component) + 4,
                        &value, sizeof(value));
        }
    }
}

/// Floor at z = 0 (400x400, two triangles) so a round can bounce.
gm::SbwLevel make_floor_geometry() {
    gm::SbwLevel geometry;
    geometry.header.sectors.resize(1);
    const double s = 200.0;
    const std::array<gm::Vec3d, 4> corners = {
        gm::Vec3d{-s, s, 0.0}, gm::Vec3d{-s, -s, 0.0},
        gm::Vec3d{s, -s, 0.0}, gm::Vec3d{s, s, 0.0}};
    const int triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (const auto& triangle : triangles) {
        gm::SbwObjectRef object;
        write_identity_object_header(object.header);
        geometry.header.sectors[0].objects.push_back(object);

        gm::SbwGeometry floor;
        for (int corner : triangle) {
            floor.vertices.push_back(gm::Vec3f{
                static_cast<float>(corners[static_cast<std::size_t>(corner)].x),
                static_cast<float>(corners[static_cast<std::size_t>(corner)].y),
                static_cast<float>(corners[static_cast<std::size_t>(corner)].z)});
        }
        floor.n_verts = 3;
        gm::SbwPoly poly;
        poly.indices = {0, 1, 2};
        floor.polys.push_back(poly);
        floor.n_polys = 1;
        geometry.geometry.push_back(std::move(floor));
    }
    geometry.header.object_count = 2;
    return geometry;
}

gm::Blast make_blast(std::int32_t hull, int radius, std::uint32_t event = 0,
                     std::uint32_t flags = 0) {
    gm::Blast blast;
    blast.hull = hull;
    blast.radius = radius;
    blast.event = event;
    blast.flags = flags;
    return blast;
}

/// Eight players: the first one at `near`, the rest far outside every radius.
std::array<gm::Vitals, 8> make_players(std::array<gm::Vec3d, 8>& eyes,
                                       gm::Vec3d near) {
    std::array<gm::Vitals, 8> players{};
    for (std::size_t i = 0; i < eyes.size(); ++i) {
        eyes[i] = gm::Vec3d{1000.0 + 100.0 * static_cast<double>(i), 0.0, 0.0};
        players[i].armor = 10000;
        players[i].armor_max = 10000;
        players[i].shield_max = 0;
    }
    eyes[0] = near;
    return players;
}

std::vector<gm::ThingVitals> thing_vitals(gm::GameRuntime& runtime) {
    std::vector<gm::ThingVitals> vitals;
    for (const gm::Thing& thing : runtime.things().things()) {
        vitals.push_back(gm::thing_vitals(thing));
    }
    return vitals;
}

/// Registry map semantics: sorted event ids, merge on add, replace on set.
void test_registry() {
    gm::EventBlastRegistry registry;
    CHECK(registry.empty());
    CHECK(registry.find(7).empty());

    const gm::Blast a = make_blast(100, 10, 5);
    const gm::Blast b = make_blast(200, 20, 6);
    CHECK(registry.add(7, a));
    CHECK(registry.add(7, b));
    CHECK(registry.add(5, a));
    CHECK_EQ(registry.size(), std::size_t(2));
    CHECK_EQ(registry.blast_count(), std::size_t(3));
    CHECK_EQ(registry.find(7).size(), std::size_t(2));
    CHECK_EQ(registry.find(7)[1].hull, 200);
    const std::vector<std::uint32_t> ids = registry.event_ids();
    CHECK_EQ(ids.size(), std::size_t(2));
    if (ids.size() == 2) {
        CHECK_EQ(ids[0], 5U);
        CHECK_EQ(ids[1], 7U);
    }

    registry.set(7, {b});
    CHECK_EQ(registry.find(7).size(), std::size_t(1));
    CHECK_EQ(registry.find(7)[0].hull, 200);
    CHECK(registry.remove(5));
    CHECK(!registry.remove(5));
    CHECK_EQ(registry.size(), std::size_t(1));
    registry.clear();
    CHECK(registry.empty());
}

/// A bounce uses the round's `collide_event`: player 0 and the crate inside
/// the radius take the hull damage, the other seven players do not.
void test_collide_event_damages_player_and_thing() {
    gm::GameRuntime runtime;
    std::string error;
    CHECK(runtime.build_world(make_level(), make_floor_geometry(), &error));
    CHECK(error.empty());

    gm::Thing crate;
    crate.id = -1;
    crate.position = std::array<int, 3>{20, 0, 10};
    crate.max_health = 10000;
    crate.health = 10000;
    CHECK_EQ(runtime.spawn_thing(crate), 0);

    std::array<gm::Vec3d, 8> eyes{};
    std::array<gm::Vitals, 8> players = make_players(eyes, {0.0, 0.0, 10.0});
    CHECK(runtime.set_player_eyes(eyes));

    gm::EventBlastRegistry registry;
    registry.add(700, make_blast(1000, 50));

    gm::ProjectileSpawn round;
    round.thing_id = 50;
    round.position = {0.0, 0.0, 25.0};
    round.direction = {0.0, 0.0, -1.0};
    round.speed = 2.0;
    round.gravity = 1.0;
    round.level_gravity = 1.0;
    round.damping = 0.8;
    round.radius = 10.0;
    round.hits_left = 5;
    round.collide_event = 700;
    CHECK_EQ(runtime.spawn_projectile(round), 0);

    bool bounced = false;
    for (int i = 0; i < 100 && !bounced; ++i) {
        runtime.tick(1);
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        if (!records.empty()) {
            bounced = records[0].status == gm::ProjectileStatus::bounced;
            if (bounced) {
                CHECK_EQ(records[0].collide_event, 700U);
            }
        }
    }
    CHECK(bounced);
    // Two event records (the collision and its event) are dispatched by the
    // same tick, so the queue is empty before the blast is applied.
    CHECK_EQ(runtime.events().queue().size(), std::size_t(0));

    std::vector<gm::ThingVitals> things = thing_vitals(runtime);
    gm::ProjectileBlastContext context;
    const gm::ProjectileBlastBatch first = runtime.apply_projectile_blasts(
        registry, context, players, things);
    CHECK_EQ(first.blasts, std::size_t(1));
    CHECK_EQ(first.already_applied, std::size_t(0));
    CHECK_EQ(first.skipped_no_event, std::size_t(0));
    CHECK_EQ(first.damage.hits.size(), std::size_t(2)); // player 0 + crate
    CHECK_EQ(players[0].armor, 9000);                   // 10000 - 1000
    for (std::size_t i = 1; i < players.size(); ++i) {
        CHECK_EQ(players[i].armor, 10000);
    }
    CHECK_EQ(things[0].hp, 9400); // 10000 - 1000*(50-20)/50
    CHECK_EQ(first.posted_events, std::size_t(0)); // blast.event == 0

    // The same generation is never blasted twice.
    const gm::ProjectileBlastBatch repeat = runtime.apply_projectile_blasts(
        registry, context, players, things);
    CHECK_EQ(repeat.blasts, std::size_t(0));
    CHECK_EQ(repeat.already_applied, std::size_t(1));
    CHECK(repeat.damage.hits.empty());
    CHECK_EQ(players[0].armor, 9000);
    CHECK_EQ(things[0].hp, 9400);
}

/// An expiry uses `expire_event`; a lethal blast kills the crate and the
/// player, and the hit event goes through the runtime queue.
void test_expire_event_kills() {
    gm::GameRuntime runtime;

    gm::Thing crate;
    crate.id = -1;
    crate.position = std::array<int, 3>{0, 0, 0};
    crate.max_health = 500;
    crate.health = 500;
    CHECK_EQ(runtime.spawn_thing(crate), 0);

    std::array<gm::Vec3d, 8> eyes{};
    std::array<gm::Vitals, 8> players = make_players(eyes, {0.0, 0.0, 0.0});
    for (gm::Vitals& player : players) {
        player.armor = 1000;
    }
    CHECK(runtime.set_player_eyes(eyes));
    CHECK(runtime.set_player_vitals(players));

    gm::EventBlastRegistry registry;
    registry.add(701, make_blast(2000, 100, /*event=*/42));

    gm::ProjectileSpawn round;
    round.thing_id = 51;
    round.position = {0.0, 0.0, 0.0};
    round.speed = 0.0;
    round.life_ticks = 1;
    round.expire_event = 701;
    CHECK_EQ(runtime.spawn_projectile(round), 0);

    runtime.tick(1);
    {
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() == 1) {
            CHECK(records[0].status == gm::ProjectileStatus::expired);
            CHECK(!records[0].already_spent);
            CHECK_EQ(records[0].expire_event, 701U);
        }
    }

    std::vector<gm::ThingVitals> things = thing_vitals(runtime);
    gm::ProjectileBlastContext context;
    const gm::ProjectileBlastBatch batch =
        runtime.apply_projectile_blasts(registry, context, players, things);
    CHECK_EQ(batch.blasts, std::size_t(1));
    CHECK_EQ(batch.damage.hits.size(), std::size_t(2));
    CHECK_EQ(players[0].armor, 0);
    CHECK_EQ(things[0].hp, 0);
    CHECK(batch.damage.hits.size() == 2 && batch.damage.hits[0].player_damage.dead);
    CHECK(batch.damage.hits.size() == 2 && batch.damage.hits[1].thing_damage.dead);
    CHECK_EQ(batch.posted_events, std::size_t(2)); // event 42 per target
    CHECK_EQ(runtime.events().queue().size(),
             std::size_t(2) * gm::EventQueue::kType3Size);

    // The caller-owned thing span is what actually kills the world thing.
    gm::store_thing_vitals(*runtime.things().by_id(0), things[0]);
    CHECK(!runtime.things().by_id(0)->alive);
}

/// A bounce without a collide id is a no-op: the blast must not fall back to
/// any weapon table.
void test_bounce_without_event_does_nothing() {
    gm::GameRuntime runtime;
    std::string error;
    CHECK(runtime.build_world(make_level(), make_floor_geometry(), &error));
    CHECK(error.empty());

    gm::Thing crate;
    crate.id = -1;
    crate.position = std::array<int, 3>{0, 0, 10};
    crate.max_health = 1000;
    crate.health = 1000;
    CHECK_EQ(runtime.spawn_thing(crate), 0);

    std::array<gm::Vec3d, 8> eyes{};
    std::array<gm::Vitals, 8> players = make_players(eyes, {0.0, 0.0, 10.0});
    CHECK(runtime.set_player_eyes(eyes));

    // The registry has a blast for 700, but the round carries no collide id.
    gm::EventBlastRegistry registry;
    registry.add(700, make_blast(1000, 50));

    gm::ProjectileSpawn round;
    round.thing_id = 52;
    round.position = {0.0, 0.0, 25.0};
    round.direction = {0.0, 0.0, -1.0};
    round.speed = 2.0;
    round.gravity = 1.0;
    round.level_gravity = 1.0;
    round.radius = 10.0;
    CHECK_EQ(runtime.spawn_projectile(round), 0);

    bool bounced = false;
    for (int i = 0; i < 100 && !bounced; ++i) {
        runtime.tick(1);
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        if (!records.empty()) {
            bounced = records[0].status == gm::ProjectileStatus::bounced;
        }
    }
    CHECK(bounced);

    std::vector<gm::ThingVitals> things = thing_vitals(runtime);
    const gm::ProjectileBlastBatch batch = runtime.apply_projectile_blasts(
        registry, gm::ProjectileBlastContext{}, players, things);
    CHECK_EQ(batch.blasts, std::size_t(0));
    CHECK_EQ(batch.skipped_no_event, std::size_t(1));
    CHECK_EQ(batch.damage.hits.size(), std::size_t(0));
    CHECK_EQ(players[0].armor, 10000);
    CHECK_EQ(things[0].hp, 1000);
}

/// A registered blast with no target inside the radius runs (one blast) but
/// applies no damage.
void test_radius_leaves_far_targets_alone() {
    gm::EventBlastRegistry registry;
    registry.add(702, make_blast(5000, 10));

    const std::array<gm::Vec3d, 1> players{{{100.0, 0.0, 0.0}}};
    std::array<gm::Vitals, 1> vitals{};
    vitals[0].armor = 10000;
    vitals[0].armor_max = 10000;
    const std::array<gm::Vec3d, 1> things{{{0.0, 200.0, 0.0}}};
    const std::array<bool, 1> alive{true};
    std::array<gm::ThingVitals, 1> thing_vitals{};
    thing_vitals[0].hp = 1000;
    thing_vitals[0].max_hp = 1000;
    thing_vitals[0].alive = true;

    std::size_t matched = 0;
    const gm::BlastDamageResult none = gm::apply_event_blast(
        registry, 702, {0.0, 0.0, 0.0}, false, true, true, -1, -1, players,
        vitals, things, alive, thing_vitals, &matched);
    CHECK_EQ(matched, std::size_t(1));
    CHECK(none.hits.empty());
    CHECK_EQ(vitals[0].armor, 10000);
    CHECK_EQ(thing_vitals[0].hp, 1000);

    // An unknown event id is an empty lookup, not an error.
    const gm::BlastDamageResult missing = gm::apply_event_blast(
        registry, 703, {0.0, 0.0, 0.0}, false, true, true, -1, -1, players,
        vitals, things, alive, thing_vitals, &matched);
    CHECK_EQ(matched, std::size_t(0));
    CHECK(missing.hits.empty());
}

/// `0x477280` -> `0x453b50`: plate hits the four shield quadrants (quarter
/// each without the full-damage flag), shield deficits spill into the armor.
void test_player_shields_and_armor() {
    gm::EventBlastRegistry registry;
    registry.add(704, make_blast(0, 100));
    gm::Blast shield_blast = make_blast(0, 100);
    shield_blast.plate = 4000;
    registry.set(704, {shield_blast});

    const std::array<gm::Vec3d, 1> players{{{0.0, 0.0, 0.0}}};
    std::array<gm::Vitals, 1> vitals{};
    vitals[0].armor = 10000;
    vitals[0].armor_max = 10000;
    vitals[0].shield_max = 8000;
    vitals[0].shields = {8000, 8000, 8000, 8000};
    std::span<gm::ThingVitals> no_things;
    std::span<const gm::Vec3d> no_thing_positions;
    std::span<const bool> no_alive;

    const gm::BlastDamageResult first = gm::apply_event_blast(
        registry, 704, {0.0, 0.0, 0.0}, false, true, true, -1, -1, players,
        vitals, no_thing_positions, no_alive, no_things);
    CHECK_EQ(first.hits.size(), std::size_t(1));
    // 4000 >> 2 = 1000 per quadrant, no deficit, armor untouched.
    CHECK_EQ(vitals[0].shields[0], 7000);
    CHECK_EQ(vitals[0].shields[3], 7000);
    CHECK_EQ(vitals[0].armor, 10000);

    // A 32000 plate: 8000 per quadrant reaches zero, the 1000 deficit of every
    // quadrant spills into the armor exactly once.
    gm::Blast heavy = make_blast(0, 100);
    heavy.plate = 32000;
    registry.set(704, {heavy});
    const gm::BlastDamageResult second = gm::apply_event_blast(
        registry, 704, {0.0, 0.0, 0.0}, false, true, true, -1, -1, players,
        vitals, no_thing_positions, no_alive, no_things);
    CHECK_EQ(second.hits.size(), std::size_t(1));
    CHECK_EQ(vitals[0].shields[0], 0);
    CHECK_EQ(vitals[0].shields[3], 0);
    CHECK_EQ(vitals[0].armor, 6000); // 10000 - 4*1000
    CHECK_EQ(second.hits[0].player_damage.broken[0], true);
}

/// Real data: `Weapons::load(level0.sbt)` resolves ROCKET's THING_TYPES
/// collide/expire blasts; the registry must key them by the exact event ids of
/// that type (not by a weapon slot).
void test_real_level_registry() {
    const std::filesystem::path path =
        std::filesystem::path(kGame) / "level0.sbt";
    if (!std::filesystem::exists(path)) {
        return;
    }
    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    if (!in) {
        return;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    std::string error;
    const auto level = gm::parse_sbt(bytes, error);
    CHECK(level.has_value());
    if (!level) {
        return;
    }
    const auto weapons = gm::Weapons::load(path, &error);
    CHECK(weapons.has_value());
    if (!weapons) {
        return;
    }
    const gm::Weapon* rocket = nullptr;
    for (const gm::Weapon& weapon : weapons->table()) {
        if (weapon.name == "ROCKET") {
            rocket = &weapon;
        }
    }
    CHECK(rocket != nullptr);
    if (rocket == nullptr) {
        return;
    }
    const auto events = gm::thing_type_events(*level, rocket->thing_type);
    CHECK(events.has_value());
    if (!events) {
        return;
    }
    CHECK(events->collide_event != 0);
    CHECK(events->expire_event != 0);
    CHECK(!rocket->impact_blasts.empty());
    CHECK(!rocket->expire_blasts.empty());

    gm::EventBlastRegistry registry;
    CHECK(registry.add_from_weapons(*weapons, *level) > 0);
    CHECK_EQ(registry.find(events->collide_event).size(),
             rocket->impact_blasts.size());
    CHECK_EQ(registry.find(events->expire_event).size(),
             rocket->expire_blasts.size());

    // The real collide blast hits a crate 50 units away with the same falloff
    // the weapon-path test pins.
    const std::array<gm::Vec3d, 1> things{{{50.0, 0.0, 0.0}}};
    const std::array<bool, 1> alive{true};
    std::array<gm::ThingVitals, 1> vitals{};
    vitals[0].hp = 10000;
    vitals[0].max_hp = 10000;
    vitals[0].alive = true;
    std::span<const gm::Vec3d> no_players;
    std::span<gm::Vitals> no_player_vitals;
    const gm::BlastDamageResult result = gm::apply_event_blast(
        registry, events->collide_event, {0.0, 0.0, 0.0}, false, true, true,
        -1, -1, no_players, no_player_vitals, things, alive, vitals);
    // `impact_blasts` also carries the other medium/ground branches; the
    // registry applies the same guard filter as `Weapon::select_blasts`.
    CHECK_EQ(result.hits.size(),
             rocket->select_blasts(false, false, true, true).size());
    CHECK(vitals[0].hp < 10000);
}

} // namespace

int main() {
    test_registry();
    test_collide_event_damages_player_and_thing();
    test_expire_event_kills();
    test_bounce_without_event_does_nothing();
    test_radius_leaves_far_targets_alone();
    test_player_shields_and_armor();
    test_real_level_registry();
    if (g_failures != 0) {
        std::fprintf(stderr, "blast_damage: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("blast_damage: all checks passed\n");
    return 0;
}
