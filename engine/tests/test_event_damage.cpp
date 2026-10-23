// Event bridge of damage to things (`M6.2.5`, docs/EVENT_DAMAGE.md).
//
// The original posts a thing's `damaged_event` (`THING_TYPES+0x1f4`, instance
// `+0x518`) after a non-lethal hit (`0x470995`).  `event_dispatch` (`0x4799b0`)
// turns it into a `GENERATE`, the entity sub-script runs, and its op 75
// `CAUSE_BLAST_RADIUS_DAMAGE` (`0x45775b` -> `0x477280`) damages the
// neighbourhood: chain explosions.
//
// This test pins
//   * the level-wide `build_event_registry()` census and registry on the real
//     `dm1`/`level0` SCRIPT records (counts + the same keys as
//     `Weapons::load()`'s collide/expire collection);
//   * the shipped-data answer: the `damaged_event` ids of every THING_TYPES
//     record of `dm1`/`level0` never reach a blast (0/...), so the chain is a
//     documented negative on the shipped scripts;
//   * the mechanism itself: `chase_damaged_events()` numbers for a real
//     registry id, a synthetic `damaged_event` graph with a cycle and a depth
//     chain, a missing id and an empty registry;
//   * the runtime bridge: a projectile expiry blast whose survivors have a
//     `damaged_event` re-blasts them, and `chain_damaged_events=false` keeps
//     the primary blast only.
#include "game/events.h"

#include "game/runtime.h"
#include "game/weapons.h"
#include "res/sbt.h"
#include "res/vm_exec.h"
#include "world/things.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
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
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

std::optional<gm::SbtLevel> load_level(const char* stem) {
    const fs::path path = kGame / (std::string(stem) + ".sbt");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    std::string error;
    auto level = gm::parse_sbt(bytes, error);
    if (!level) {
        std::fprintf(stderr, "event_damage: %s: %s\n", path.c_str(), error.c_str());
    }
    return level;
}

gm::Blast make_blast(std::int32_t plate, int radius,
                     std::int32_t hull = 0, std::uint32_t event = 0) {
    gm::Blast blast;
    blast.plate = plate;
    blast.hull = hull;
    blast.radius = radius;
    blast.event = event;
    return blast;
}

/// A thing span for the free `chase_damaged_events()`: alive, full HP, no
/// shields, so the intake lands in HP only (`0x470500`).
struct ThingSpan {
    std::vector<gm::Vec3d> positions;
    std::vector<gm::ThingVitals> vitals;
    std::vector<std::uint32_t> damaged_events;
    std::unique_ptr<bool[]> alive;

    explicit ThingSpan(std::vector<gm::Vec3d> points, std::int32_t hp = 10000) {
        positions = std::move(points);
        vitals.resize(positions.size());
        alive = std::make_unique<bool[]>(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            alive[i] = true;
            vitals[i].hp = hp;
            vitals[i].max_hp = hp;
            vitals[i].alive = true;
        }
        damaged_events.assign(positions.size(), 0);
    }

    std::span<const bool> alive_span() const {
        return std::span<const bool>(alive.get(), positions.size());
    }
};

std::vector<gm::ThingVitals> thing_vitals(gm::GameRuntime& runtime) {
    std::vector<gm::ThingVitals> out;
    out.reserve(runtime.things().things().size());
    for (const gm::Thing& thing : runtime.things().things()) {
        out.push_back(gm::thing_vitals(thing));
    }
    return out;
}

/// Census + registry of the shipped SCRIPT records.  The numbers are the
/// docs/EVENT_DAMAGE.md §2 table: 355 entity ids / 253 GENERATE / 83 blast ops
/// on dm1, 346 / 223 / 78 on level0.
void test_build_registry_real() {
    struct Expected {
        const char* stem;
        std::size_t handles;
        std::size_t ids;
        std::size_t decoded;
        std::size_t generates;
        std::size_t blast_commands;
        std::size_t registered_ids;
        std::size_t registered_blasts;
    };
    const std::array<Expected, 2> expected{{
        {"dm1", 363, 355, 355, 241, 83, 61, 121},
        {"level0", 350, 346, 346, 212, 78, 56, 114},
    }};
    for (const Expected& want : expected) {
        const auto level = load_level(want.stem);
        CHECK(level.has_value());
        if (!level) {
            continue;
        }
        gm::EventBlastRegistry registry;
        gm::EventRegistryBuildStats stats;
        std::string error;
        const bool ok = gm::build_event_registry(*level, registry, &error, &stats);
        CHECK(ok);
        CHECK(error.empty());
        CHECK_EQ(stats.entity_blocks, want.handles);
        CHECK_EQ(stats.ids, want.ids);
        CHECK_EQ(stats.scripts_decoded, want.decoded);
        CHECK_EQ(stats.generate_commands, want.generates);
        CHECK_EQ(stats.blast_commands, want.blast_commands);
        CHECK_EQ(stats.decode_failures, std::size_t(0));
        CHECK_EQ(registry.size(), registry.event_ids().size());
        CHECK_EQ(registry.blast_count(), stats.blasts);
        for (std::uint32_t id : registry.event_ids()) {
            CHECK(registry.contains(id));
            CHECK(!registry.find(id).empty());
        }
        if (want.registered_ids != 0) {
            CHECK_EQ(stats.ids_with_blasts, want.registered_ids);
            CHECK_EQ(stats.blasts, want.registered_blasts);
        }
        std::printf("event_damage: %s SCRIPT handles=%zu ids=%zu decoded=%zu "
                    "GENERATE=%zu blast_ops=%zu -> registry ids=%zu blasts=%zu "
                    "(cycles=%zu depth_cut=%zu)\n",
                    want.stem, stats.entity_blocks, stats.ids,
                    stats.scripts_decoded, stats.generate_commands,
                    stats.blast_commands, registry.size(),
                    registry.blast_count(), stats.chain_cycles,
                    stats.chain_depth_cut);
    }
}

/// The registry built from the level must agree with the weapon path:
/// `Weapons::load(level0)` resolved ROCKET's THING_TYPES collide/expire event
/// ids through `collect_event_blasts()`, so the level-wide scan has to store
/// the same number of blasts under those exact ids.
void test_registry_matches_weapon_path() {
    const auto level = load_level("level0");
    CHECK(level.has_value());
    if (!level) {
        return;
    }
    const fs::path path = kGame / "level0.sbt";
    std::string error;
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
    gm::EventBlastRegistry registry;
    CHECK(gm::build_event_registry(*level, registry, &error));
    CHECK(registry.contains(events->collide_event));
    CHECK(registry.contains(events->expire_event));
    CHECK_EQ(registry.find(events->collide_event).size(),
             rocket->impact_blasts.size());
    CHECK_EQ(registry.find(events->expire_event).size(),
             rocket->expire_blasts.size());
    std::printf("event_damage: level0 ROCKET collide=%d (%zu blasts) expire=%d "
                "(%zu blasts) match Weapons::load\n",
                static_cast<int>(events->collide_event),
                registry.find(events->collide_event).size(),
                static_cast<int>(events->expire_event),
                registry.find(events->expire_event).size());
}

/// The shipped answer: every THING_TYPES `damaged_event` of dm1/level0 is
/// either an event id whose block is absent or one whose block (and its
/// GENERATE chain) has no `CAUSE_BLAST_RADIUS_DAMAGE`.  The registry lookup is
/// therefore empty for all of them, and that is a no-op, not an error.
void test_real_damaged_events_have_no_blast() {
    for (const char* stem : {"dm1", "level0"}) {
        const auto level = load_level(stem);
        CHECK(level.has_value());
        if (!level) {
            continue;
        }
        gm::EventBlastRegistry registry;
        std::string error;
        CHECK(gm::build_event_registry(*level, registry, &error));

        const gm::VmState vm = gm::vm_state_from_level(*level);
        gm::ThingsOptions options;
        options.build_static = false;
        options.execute = false;
        gm::Things things = gm::Things::load(*level, vm, options);
        const gm::ThingTypeTable& types = things.type_table();
        CHECK(!types.damaged_events.empty());

        std::set<std::uint32_t> unique;
        std::size_t with_event = 0;
        std::size_t with_blast = 0;
        std::uint32_t sample_id = 0;
        std::string sample_name;
        for (std::size_t i = 0; i < types.damaged_events.size(); ++i) {
            const std::uint32_t id = types.damaged_events[i];
            if (id == 0) {
                continue;
            }
            ++with_event;
            unique.insert(id);
            if (registry.contains(id)) {
                ++with_blast;
            }
            if (sample_id == 0) {
                sample_id = id;
                sample_name = i < types.names.size() ? types.names[i] : "";
            }
        }
        CHECK(with_event > 0);
        CHECK_EQ(with_blast, std::size_t(0));
        CHECK(!registry.contains(sample_id));
        std::printf("event_damage: %s THING_TYPES damaged_event: %zu records, "
                    "%zu distinct ids, %zu with a blast (sample type '%s' id=%d)\n",
                    stem, with_event, unique.size(), with_blast,
                    sample_name.c_str(), static_cast<int>(sample_id));
    }
}

/// The shipped answer on a live hit: take a real thing `damaged_event`
/// (`dm1` type 41 `SenTop` -> -30129), a real registered blast as the primary
/// hit, and show that the survivors' chain adds exactly zero (the event id is
/// not in the registry).  This is the negative case with numbers.
void test_real_damaged_event_chain_is_zero() {
    const auto level = load_level("dm1");
    CHECK(level.has_value());
    if (!level) {
        return;
    }
    gm::EventBlastRegistry registry;
    std::string error;
    CHECK(gm::build_event_registry(*level, registry, &error));
    CHECK(!registry.empty());
    const std::uint32_t primary_id = registry.event_ids().front();

    const gm::VmState vm = gm::vm_state_from_level(*level);
    gm::ThingsOptions options;
    options.build_static = false;
    options.execute = false;
    gm::Things things = gm::Things::load(*level, vm, options);
    const gm::ThingTypeTable& types = things.type_table();
    std::uint32_t damaged_id = 0;
    std::string type_name;
    for (std::size_t i = 0; i < types.damaged_events.size(); ++i) {
        if (types.damaged_events[i] != 0 && !registry.contains(types.damaged_events[i])) {
            damaged_id = types.damaged_events[i];
            type_name = i < types.names.size() ? types.names[i] : "";
            break;
        }
    }
    CHECK(damaged_id != 0);
    if (damaged_id == 0) {
        return;
    }

    ThingSpan span({{0.0, 0.0, 0.0}, {20.0, 0.0, 0.0}}, 1000000);
    span.damaged_events = {damaged_id, damaged_id};
    const gm::BlastDamageResult primary = gm::apply_event_blast(
        registry, primary_id, {0.0, 0.0, 0.0}, false, true, true, -1, -1, {},
        std::span<gm::Vitals>{}, span.positions, span.alive_span(), span.vitals);
    CHECK(!primary.hits.empty());
    const std::int32_t after_primary = span.vitals[0].hp;
    CHECK(after_primary < 1000000);
    const gm::EventDamageChainResult chain = gm::chase_damaged_events(
        registry, primary, span.damaged_events, span.positions, span.alive_span(),
        {}, span.vitals);
    CHECK_EQ(chain.stats.events, std::size_t(0));
    CHECK_EQ(chain.stats.skipped_missing, std::size_t(2));
    CHECK_EQ(span.vitals[0].hp, after_primary);
    std::printf("event_damage: real damaged_event dm1 type '%s' id=%d (event "
                "%d primary): hp 1000000 -> %d, chain events=%zu -> %d\n",
                type_name.c_str(), static_cast<int>(damaged_id),
                static_cast<int>(primary_id), after_primary, chain.stats.events,
                span.vitals[0].hp);
}

/// A `damaged_event` that is not in the registry and an empty registry are
/// both no-ops: no hits, no state change, no error.
void test_missing_event_and_empty_registry() {
    ThingSpan span({{0.0, 0.0, 0.0}, {50.0, 0.0, 0.0}});
    span.damaged_events = {900, 0};

    gm::EventBlastRegistry empty;
    gm::EventBlastRegistry partial;
    partial.add(300, make_blast(1000, 100));

    const gm::BlastDamageResult primary = gm::apply_event_blast(
        partial, 300, {0.0, 0.0, 0.0}, false, true, true, -1, -1, {},
        std::span<gm::Vitals>{}, span.positions, span.alive_span(), span.vitals);
    CHECK_EQ(primary.hits.size(), std::size_t(2));
    const std::int32_t after_primary = span.vitals[0].hp;
    CHECK(after_primary < 10000);

    const gm::EventDamageChainResult missing = gm::chase_damaged_events(
        partial, primary, span.damaged_events, span.positions, span.alive_span(),
        {}, span.vitals);
    CHECK_EQ(missing.stats.events, std::size_t(0));
    CHECK_EQ(missing.stats.skipped_missing, std::size_t(1));
    CHECK_EQ(missing.stats.skipped_no_event, std::size_t(1));
    CHECK_EQ(missing.stats.hits, std::size_t(0));
    CHECK(missing.damage.hits.empty());
    CHECK_EQ(span.vitals[0].hp, after_primary);

    // Empty registry: the primary blast itself applies nothing.
    const gm::BlastDamageResult none = gm::apply_event_blast(
        empty, 300, {0.0, 0.0, 0.0}, false, true, true, -1, -1, {},
        std::span<gm::Vitals>{}, span.positions, span.alive_span(), span.vitals);
    CHECK(none.hits.empty());
    const gm::EventDamageChainResult no_chain = gm::chase_damaged_events(
        empty, primary, span.damaged_events, span.positions, span.alive_span(),
        {}, span.vitals);
    CHECK_EQ(no_chain.stats.events, std::size_t(0));
    CHECK_EQ(no_chain.stats.hits, std::size_t(0));
    CHECK_EQ(span.vitals[0].hp, after_primary);
}

/// A level without a SCRIPT record is rejected with an error; the registry
/// stays empty and the call is repeatable.
void test_build_registry_without_script() {
    gm::SbtLevel empty;
    gm::EventBlastRegistry registry;
    registry.add(7, make_blast(1, 1));
    std::string error;
    gm::EventRegistryBuildStats stats;
    CHECK(!gm::build_event_registry(empty, registry, &error, &stats));
    CHECK(!error.empty());
    CHECK(registry.empty());
    CHECK_EQ(registry.blast_count(), std::size_t(0));
    CHECK_EQ(stats.entity_blocks, std::size_t(0));
    CHECK_EQ(stats.blast_commands, std::size_t(0));
}

/// Cycle cut: A's damaged_event and B's damaged_event map to two real blasts
/// that hit each other, so an unguarded recursion never terminates.  The
/// event-id stack allows exactly one application per path.
void test_cycle_is_cut() {
    ThingSpan span({{0.0, 0.0, 0.0}, {50.0, 0.0, 0.0}});
    span.damaged_events = {300, 301};

    gm::EventBlastRegistry registry;
    registry.add(300, make_blast(100, 500));
    registry.add(301, make_blast(100, 500));

    const gm::BlastDamageResult primary = gm::apply_event_blast(
        registry, 300, {0.0, 0.0, 0.0}, false, true, true, -1, -1, {},
        std::span<gm::Vitals>{}, span.positions, span.alive_span(), span.vitals);
    CHECK_EQ(primary.hits.size(), std::size_t(2));

    const gm::EventDamageChainResult chain = gm::chase_damaged_events(
        registry, primary, span.damaged_events, span.positions, span.alive_span(),
        {}, span.vitals);
    // Path A: 300 -> hits A (300 in stack) and B (301 -> hits A: 300 in stack,
    // B: 301 in stack).  Path B: 301 -> the same.  Two applications, four
    // cycle cuts, depth 2.
    CHECK_EQ(chain.stats.events, std::size_t(4));
    CHECK_EQ(chain.stats.blasts, std::size_t(4));
    CHECK_EQ(chain.stats.depth_max, std::size_t(2));
    CHECK_EQ(chain.stats.skipped_cycle, std::size_t(6));
    CHECK_EQ(chain.stats.hits, std::size_t(8));
    std::printf("event_damage: cycle graph events=%zu blasts=%zu hits=%zu "
                "cycles=%zu hp=(%d,%d)\n",
                chain.stats.events, chain.stats.blasts, chain.stats.hits,
                chain.stats.skipped_cycle, span.vitals[0].hp, span.vitals[1].hp);
}

/// Depth cut: six things on a line, every blast reaches its neighbours.  With
/// `max_depth = 4` the chase reaches depth 4 and cuts the rest; with
/// `max_depth = 1` exactly one level runs and the result is strictly smaller.
void test_depth_is_limited() {
    const auto run = [](int max_depth) {
        ThingSpan span({{0.0, 0.0, 0.0}, {100.0, 0.0, 0.0},
                        {200.0, 0.0, 0.0}, {300.0, 0.0, 0.0},
                        {400.0, 0.0, 0.0}, {500.0, 0.0, 0.0}});
        for (std::size_t i = 0; i < span.damaged_events.size(); ++i) {
            span.damaged_events[i] = static_cast<std::uint32_t>(500 + i);
        }
        gm::EventBlastRegistry registry;
        for (std::size_t i = 0; i < span.damaged_events.size(); ++i) {
            registry.add(static_cast<std::uint32_t>(500 + i),
                         make_blast(1, 250));
        }
        const gm::BlastDamageResult primary = gm::apply_event_blast(
            registry, 500, {0.0, 0.0, 0.0}, false, true, true, -1, -1, {},
            std::span<gm::Vitals>{}, span.positions, span.alive_span(),
            span.vitals);
        gm::EventDamageChainContext context;
        context.max_depth = max_depth;
        return gm::chase_damaged_events(registry, primary, span.damaged_events,
                                        span.positions, span.alive_span(), {},
                                        span.vitals, context)
            .stats;
    };
    const gm::EventDamageChainStats shallow = run(1);
    const gm::EventDamageChainStats deep = run(4);
    CHECK_EQ(shallow.depth_max, std::size_t(1));
    CHECK_EQ(deep.depth_max, std::size_t(4));
    CHECK(deep.events > shallow.events);
    CHECK(deep.skipped_depth > 0);
    CHECK(shallow.blasts == shallow.events);
    std::printf("event_damage: depth chain max=1 -> events=%zu blasts=%zu; "
                "max=4 -> events=%zu blasts=%zu hits=%zu depth_cut=%zu\n",
                shallow.events, shallow.blasts, deep.events, deep.blasts,
                deep.hits, deep.skipped_depth);
}

/// Real registry data through a synthetic `damaged_event` linkage: dm1's
/// THING_TYPES destroy events are the only shipped thing events that carry
/// blasts (docs/EVENT_DAMAGE.md §2).  Pick the first one by data, apply its
/// blasts directly, then let the chase re-run them from the survivors'
/// `damaged_event` field.
void test_real_registry_chain() {
    const auto level = load_level("dm1");
    CHECK(level.has_value());
    if (!level) {
        return;
    }
    gm::EventBlastRegistry registry;
    std::string error;
    CHECK(gm::build_event_registry(*level, registry, &error));

    const gm::VmState vm = gm::vm_state_from_level(*level);
    gm::ThingsOptions options;
    options.build_static = false;
    options.execute = false;
    gm::Things things = gm::Things::load(*level, vm, options);
    const gm::ThingTypeTable& types = things.type_table();
    std::uint32_t event_id = 0;
    std::string type_name;
    for (std::size_t i = 0; i < types.destroy_events.size(); ++i) {
        const std::uint32_t id = types.destroy_events[i];
        if (id != 0 && registry.contains(id)) {
            event_id = id;
            type_name = i < types.names.size() ? types.names[i] : "";
            break;
        }
    }
    CHECK(event_id != 0);
    if (event_id == 0) {
        return;
    }

    ThingSpan span({{0.0, 0.0, 0.0}, {20.0, 0.0, 0.0}}, 400000);
    span.damaged_events = {event_id, event_id};
    const std::vector<std::int32_t> before = {span.vitals[0].hp,
                                             span.vitals[1].hp};
    std::size_t matched = 0;
    const gm::BlastDamageResult primary = gm::apply_event_blast(
        registry, event_id, {0.0, 0.0, 0.0}, false, true, true, -1, -1, {},
        std::span<gm::Vitals>{}, span.positions, span.alive_span(), span.vitals,
        &matched);
    CHECK(matched > 0);
    CHECK(matched <= registry.find(event_id).size());
    CHECK(!primary.hits.empty());
    const std::int32_t after_primary = span.vitals[0].hp;
    const gm::EventDamageChainResult chain = gm::chase_damaged_events(
        registry, primary, span.damaged_events, span.positions, span.alive_span(),
        {}, span.vitals);
    CHECK_EQ(chain.stats.events, std::size_t(2));
    CHECK_EQ(chain.stats.blasts, std::size_t(2) * matched);
    CHECK(chain.stats.hits > chain.stats.events);
    CHECK(span.vitals[0].hp < after_primary);
    std::printf("event_damage: real chain dm1 type '%s' event=%d blasts=%zu: "
                "hp %d+%d -> %d (primary hits=%zu) -> %d (chain events=%zu "
                "blasts=%zu hits=%zu depth=%zu)\n",
                type_name.c_str(), static_cast<int>(event_id),
                registry.find(event_id).size(), before[0], before[1],
                after_primary, primary.hits.size(), span.vitals[0].hp,
                chain.stats.events, chain.stats.blasts, chain.stats.hits,
                chain.stats.depth_max);
}

/// Runtime bridge: a round expires next to two crates whose `damaged_event`
/// maps to a second registered blast.  The primary blast damages both, the
/// survivors' event re-blasts them, and the cycles of the equal event ids are
/// cut.  The opt-out keeps the primary damage only.
void test_runtime_bridge() {
    const auto build = [](bool chain_enabled) {
        gm::GameRuntimeConfig config;
        config.chain_damaged_events = chain_enabled;
        gm::GameRuntime runtime(config);

        gm::Thing first;
        first.id = -1;
        first.position = std::array<int, 3>{0, 0, 0};
        first.max_health = 10000;
        first.health = 10000;
        first.damaged_event = 701;
        CHECK_EQ(runtime.spawn_thing(first), 0);
        gm::Thing second;
        second.id = -1;
        second.position = std::array<int, 3>{50, 0, 0};
        second.max_health = 10000;
        second.health = 10000;
        second.damaged_event = 701;
        CHECK_EQ(runtime.spawn_thing(second), 1);

        gm::EventBlastRegistry registry;
        registry.add(700, make_blast(1000, 100, 1000));
        registry.add(701, make_blast(100, 100, 100));

        gm::ProjectileSpawn round;
        round.thing_id = 61;
        round.position = {0.0, 0.0, 0.0};
        round.speed = 0.0;
        round.life_ticks = 1;
        round.expire_event = 700;
        CHECK_EQ(runtime.spawn_projectile(round), 0);
        runtime.tick(1);

        std::vector<gm::ThingVitals> vitals = thing_vitals(runtime);
        const std::span<const gm::ProjectileTickRecord> records =
            runtime.last_projectile_results();
        CHECK_EQ(records.size(), std::size_t(1));
        if (records.size() != 1 || records[0].status != gm::ProjectileStatus::expired) {
            std::fprintf(stderr, "event_damage: round did not expire\n");
            ++g_failures;
        }
        const gm::ProjectileBlastBatch batch = runtime.apply_projectile_blasts(
            registry, gm::ProjectileBlastContext{}, {}, vitals);
        return std::pair(batch, vitals);
    };

    const auto with_chain = build(true);
    CHECK_EQ(with_chain.first.blasts, std::size_t(1));
    CHECK_EQ(with_chain.first.damage.hits.size(), std::size_t(6));
    CHECK_EQ(with_chain.first.chain.events, std::size_t(2));
    CHECK_EQ(with_chain.first.chain.blasts, std::size_t(2));
    CHECK_EQ(with_chain.first.chain.depth_max, std::size_t(1));
    CHECK_EQ(with_chain.first.chain.skipped_cycle, std::size_t(4));
    CHECK(with_chain.second[0].hp < 10000);
    CHECK(with_chain.second[1].hp < 10000);

    const auto primary_only = build(false);
    CHECK_EQ(primary_only.first.blasts, std::size_t(1));
    CHECK_EQ(primary_only.first.chain.events, std::size_t(0));
    CHECK_EQ(primary_only.first.chain.hits, std::size_t(0));
    CHECK(primary_only.second[0].hp > with_chain.second[0].hp);
    CHECK(primary_only.second[1].hp > with_chain.second[1].hp);
    std::printf("event_damage: runtime chain=%d hp=(%d,%d) events=%zu hits=%zu; "
                "chain=0 hp=(%d,%d) events=%zu\n",
                1, with_chain.second[0].hp, with_chain.second[1].hp,
                with_chain.first.chain.events,
                with_chain.first.chain.hits, primary_only.second[0].hp,
                primary_only.second[1].hp, primary_only.first.chain.events);
}

} // namespace

int main() {
    if (!fs::exists(kGame / "dm1.sbt") || !fs::exists(kGame / "level0.sbt")) {
        std::printf("event_damage: SKIP real data (game not installed)\n");
        return 0;
    }
    test_build_registry_real();
    test_registry_matches_weapon_path();
    test_real_damaged_events_have_no_blast();
    test_real_damaged_event_chain_is_zero();
    test_missing_event_and_empty_registry();
    test_build_registry_without_script();
    test_cycle_is_cut();
    test_depth_is_limited();
    test_real_registry_chain();
    test_runtime_bridge();
    if (g_failures != 0) {
        std::fprintf(stderr, "event_damage: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("event_damage: all checks passed\n");
    return 0;
}
