// Doors, switches and lifts (docs/DOORS_CPP.md).
//
// Synthetic part: a hand-written `.sbt` with DOOR/SWITCH/LIFT records and a
// three-object `.sbw`, exercising the state machine exactly (phases, reverse,
// hold timer, lock, lift call, huge tick, malformed records).
//
// Real part: `dm1.sbt` + `dm1.sbw` from the shipped game.  Counts come from
// `python3 tools/re/doors.py --level dm1` (61 doors, 46 sliding, 4 linked,
// 129 switches, 0 lifts).  `--full` also sweeps all 36 levels and pins the
// corpus totals 1779 / 3964 / 45 and 1779/1779 object bindings.
#include "world/doors.h"

#include "game/events.h"

#include "res/sbt.h"
#include "res/sbw.h"
#include "res/save.h"
#include "res/scene.h"

#include <algorithm>
#include <array>
#include <cmath>
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
constexpr double kEps = 1e-15;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

void put_i32(std::vector<std::uint8_t>& data, std::size_t offset, int value) {
    std::memcpy(data.data() + offset, &value, 4);
}

void put_u32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    std::memcpy(data.data() + offset, &value, 4);
}

void put_f64(std::vector<std::uint8_t>& data, std::size_t offset, double value) {
    std::memcpy(data.data() + offset, &value, 8);
}

// --- synthetic records ------------------------------------------------------

struct DoorSpec {
    int id = 0;
    int sector = 0;
    int object = 0;
    int flags = 1;
    std::array<int, 5> ids{};
    std::array<double, 3> closed{};
    std::array<double, 3> open{};
    std::uint32_t phase = 0;
    int state = 0;
    int step = 1500;
    std::array<double, 3> size{};
    std::array<double, 3> axis{};
    std::array<int, 3> volume{};
    std::array<int, 3> sample{{-1, -1, -1}};
};

std::vector<std::uint8_t> make_door_payload(const std::vector<DoorSpec>& specs) {
    std::vector<std::uint8_t> payload(4 + specs.size() * 0xB8, 0);
    put_u32(payload, 0, static_cast<std::uint32_t>(specs.size()));
    for (std::size_t k = 0; k < specs.size(); ++k) {
        const DoorSpec& spec = specs[k];
        const std::size_t base = 4 + k * 0xB8;
        for (int axis = 0; axis < 3; ++axis) {
            put_f64(payload, base + static_cast<std::size_t>(axis) * 8,
                    spec.closed[static_cast<std::size_t>(axis)]);
            put_f64(payload, base + 0x18 + static_cast<std::size_t>(axis) * 8,
                    spec.open[static_cast<std::size_t>(axis)]);
            put_f64(payload, base + 0x30 + static_cast<std::size_t>(axis) * 8,
                    spec.size[static_cast<std::size_t>(axis)]);
            put_f64(payload, base + 0x48 + static_cast<std::size_t>(axis) * 8,
                    spec.axis[static_cast<std::size_t>(axis)]);
            put_i32(payload, base + 0xA0 + static_cast<std::size_t>(axis) * 4,
                    spec.volume[static_cast<std::size_t>(axis)]);
            put_i32(payload, base + 0xAC + static_cast<std::size_t>(axis) * 4,
                    spec.sample[static_cast<std::size_t>(axis)]);
        }
        put_f64(payload, base + 0x60, 0.03);
        put_i32(payload, base + 0x68, spec.flags);
        for (int i = 0; i < 5; ++i) {
            put_i32(payload, base + 0x6C + static_cast<std::size_t>(i) * 4,
                    spec.ids[static_cast<std::size_t>(i)]);
        }
        put_i32(payload, base + 0x80, spec.state);
        put_i32(payload, base + 0x84, spec.sector);
        put_i32(payload, base + 0x88, spec.object);
        put_u32(payload, base + 0x8C, spec.phase);
        put_i32(payload, base + 0x94, spec.step);
    }
    return payload;
}

struct SwitchSpec {
    int kind = 259;
    int target = 0;
    int radius = 128;
    std::array<int, 3> pos{};
    int aux = 0;
    int sound = -1;
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
        put_i32(payload, base + 0x18, specs[k].aux);
        put_i32(payload, base + 0x1C, specs[k].sound);
    }
    return payload;
}

/// The original helper: one transformed (kind 259) switch per target.
std::vector<std::uint8_t> make_switch_payload(const std::vector<int>& targets) {
    std::vector<SwitchSpec> specs;
    specs.reserve(targets.size());
    for (std::size_t k = 0; k < targets.size(); ++k) {
        SwitchSpec spec;
        spec.kind = 259; // transformed, as in dm1
        spec.target = targets[k];
        spec.radius = 128;
        spec.pos = static_cast<std::array<int, 3>>(std::array<int, 3>{
            static_cast<int>(k), 0, 0});
        specs.push_back(spec);
    }
    return make_switch_payload(specs);
}

std::vector<std::uint8_t> make_lift_payload() {
    constexpr std::size_t kRecord = 0xA8;
    constexpr std::size_t kStops = 2;
    std::vector<std::uint8_t> payload(4 + kRecord + 4 + kStops * 12, 0);
    put_u32(payload, 0, 1);
    const std::size_t base = 4;
    put_i32(payload, base + 0x34, 1);   // kind
    put_i32(payload, base + 0x3C, 10);  // x
    put_i32(payload, base + 0x40, 20);  // y
    put_i32(payload, base + 0x44, 30);  // z
    put_i32(payload, base + 0x54, 10);  // speed per tick
    put_i32(payload, base + 0x5C, 0);   // height
    put_i32(payload, base + 0x60, 0);   // target
    put_i32(payload, base + 0x68, 0);   // moving
    put_i32(payload, base + 0x6C, 0);   // stop index
    put_i32(payload, base + 0x70, 500); // call id
    put_i32(payload, base + 0x9C, static_cast<int>(kStops));
    put_i32(payload, base + 0xA0, 0);

    const std::size_t columns = base + kRecord + 4;
    put_u32(payload, base + kRecord, static_cast<std::uint32_t>(kStops));
    put_i32(payload, columns + 0, 500);            // floor ids
    put_i32(payload, columns + 4, 501);
    put_i32(payload, columns + kStops * 4 + 0, 0); // heights
    put_i32(payload, columns + kStops * 4 + 4, 100);
    put_i32(payload, columns + kStops * 8 + 0, 0); // sounds
    put_i32(payload, columns + kStops * 8 + 4, 7);
    return payload;
}

gm::SbtLevel make_level() {
    gm::SbtLevel level;
    gm::SbtRecord doors;
    doors.type = 2;
    doors.payload = make_door_payload({
        {100, 0, 0, 1, {0, 100, 0, 0, 0}, {0, 0, 0}, {40, 0, 0}},
        {200, 0, 1, 3, {0, 200, 0, 201, 0}, {0, 0, 0}, {0, 0, 0}},
        {300, 0, 2, 1, {0, 300, 0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    });
    gm::SbtRecord switches;
    switches.type = 1;
    switches.payload = make_switch_payload({100, 200, 300, 999, 500});
    gm::SbtRecord lifts;
    lifts.type = 18;
    lifts.payload = make_lift_payload();
    level.records = {std::move(doors), std::move(switches), std::move(lifts)};
    return level;
}

gm::SbwLevel make_world(std::size_t objects = 3) {
    gm::SbwLevel world;
    world.header.sectors.resize(1);
    world.header.sectors[0].objects.resize(objects);
    world.header.object_count = objects;
    return world;
}

gm::WorldState build_synthetic(std::string* error = nullptr) {
    gm::WorldState state;
    const gm::SbwLevel world = make_world();
    const gm::SbtLevel level = make_level();
    state.build(level, world, error);
    return state;
}

// --- synthetic state machine -------------------------------------------------

void test_synthetic() {
    std::string error;
    gm::WorldState state = build_synthetic(&error);
    CHECK(error.empty());
    const gm::WorldStats& stats = state.stats();
    CHECK_EQ(stats.doors, 3u);
    CHECK_EQ(stats.switches, 5u);
    CHECK_EQ(stats.lifts, 1u);
    CHECK_EQ(stats.doors_bound, 3u);
    CHECK_EQ(stats.doors_distinct_objects, 3u);
    CHECK_EQ(stats.doors_sliding, 1u);
    CHECK_EQ(stats.doors_linked, 1u);

    // Opening: 1500 phase units per 91 Hz tick.
    CHECK(state.interact(0));
    gm::Door* door = state.find_door(100);
    CHECK(door != nullptr);
    CHECK(door->state == gm::DoorState::Opening);
    state.tick(10);
    CHECK_EQ(door->phase, 15000u);
    CHECK_NEAR(door->open_fraction, 15000.0 / 65536.0, kEps);
    {
        const auto mapped = state.apply_to_objects();
        const auto it = std::find_if(mapped.begin(), mapped.end(), [](const auto& entry) {
            return entry.door_id == 100;
        });
        CHECK(it != mapped.end());
        CHECK_NEAR(it->open_fraction, door->open_fraction, 0.0);
        CHECK(it->state == gm::DoorState::Opening);
    }

    // A second press reverses the motion; a third one reverses it again.
    CHECK(state.interact(0));
    CHECK(door->state == gm::DoorState::Closing);
    state.tick(5);
    CHECK_EQ(door->phase, 7500u);
    CHECK(state.interact(0));
    CHECK(door->state == gm::DoorState::Opening);
    state.tick(100);
    CHECK_EQ(door->phase, gm::kDoorPhaseFull);
    CHECK(door->state == gm::DoorState::Closed);
    CHECK_NEAR(door->open_fraction, 1.0, kEps);
    CHECK_EQ(door->timer, 0u);

    // Hold timer: opt-in auto-close after the open end.
    state.set_hold_ticks(100, 12);
    CHECK(state.interact(0)); // close first
    state.tick(100);
    CHECK_EQ(door->phase, 0u);
    CHECK(state.interact(0)); // then open fully and arm the timer
    state.tick(44);
    CHECK_EQ(door->timer, 12u);
    state.tick(11);
    CHECK_EQ(door->timer, 1u);
    CHECK_EQ(door->phase, gm::kDoorPhaseFull);
    state.tick(1);
    CHECK_EQ(door->timer, 0u);
    CHECK(door->state == gm::DoorState::Closing);
    state.tick(100);
    CHECK_EQ(door->phase, 0u);
    CHECK_NEAR(door->open_fraction, 0.0, kEps);

    // Locked door: the switch is found, but the leaf does not move.
    state.set_locked(300, true);
    CHECK(state.interact(2));
    CHECK(door->state == gm::DoorState::Closed); // 100 is untouched
    gm::Door* locked = state.find_door(300);
    CHECK(locked != nullptr);
    CHECK(locked->state == gm::DoorState::Closed);
    CHECK_EQ(state.stats().locked_hits, 1u);
    state.set_locked(300, false);
    CHECK(state.interact(2));
    CHECK(locked->state == gm::DoorState::Opening);

    // Unmatched target and missing switch id.
    CHECK(state.interact(3));
    CHECK_EQ(state.stats().unmatched_targets, 1u);
    CHECK(!state.interact(99));
    CHECK(!state.interact(-1));

    // Lift: call id 500 moves one stop up at 10 height units per tick.
    CHECK(state.interact(4));
    gm::Lift* lift = state.find_lift(0);
    CHECK(lift != nullptr);
    CHECK(lift->moving);
    CHECK_EQ(lift->target, 100);
    state.tick(5);
    CHECK_EQ(lift->height, 50);
    state.tick(5);
    CHECK_EQ(lift->height, 100);
    CHECK(!lift->moving);
    CHECK_EQ(lift->stop_index, 1);
    CHECK(state.interact(4));
    CHECK_EQ(lift->target, 0);
    state.tick(1000);
    CHECK_EQ(lift->height, 0);
    CHECK(!lift->moving);

    // Huge tick: closed form, no loop, no NaN.
    CHECK(state.interact(0));
    state.tick(1'000'000'000ull);
    CHECK(door->state == gm::DoorState::Closed);
    CHECK(std::isfinite(door->open_fraction));
    CHECK(door->open_fraction >= 0.0 && door->open_fraction <= 1.0);

    // apply_to_objects mirrors the runtime state, deterministically.
    const auto objects = state.apply_to_objects();
    CHECK_EQ(objects.size(), 3u);
    const auto found = std::find_if(objects.begin(), objects.end(), [](const auto& entry) {
        return entry.door_id == 100;
    });
    CHECK(found != objects.end());
    CHECK(found->bound);
    CHECK(found->sliding);
    CHECK_EQ(found->sector, 0);
    CHECK_EQ(found->object, 0);

    gm::WorldState again = build_synthetic(&error);
    CHECK(again.interact(0));
    again.tick(17);
    gm::WorldState twice = build_synthetic(&error);
    CHECK(twice.interact(0));
    twice.tick(17);
    CHECK_EQ(again.find_door(100)->phase, twice.find_door(100)->phase);
    CHECK_NEAR(again.find_door(100)->open_fraction,
               twice.find_door(100)->open_fraction, 0.0);

    // SwitchPressed -> type-2 Trigger Switch record.
    state.clear_events();
    CHECK(state.interact(0));
    gm::EventQueue switch_queue;
    CHECK_EQ(state.post_switch_events(switch_queue, 7), 1U);
    const gm::EventParseResult switch_event = switch_queue.drain();
    CHECK_EQ(switch_event.events.size(), 1U);
    if (switch_event.events.size() == 1) {
        CHECK(switch_event.events[0].type == gm::EventType::trigger_switch);
        CHECK_EQ(switch_event.events[0].switch_index, 0U);
        CHECK_EQ(switch_event.events[0].source, 7);
    }

    // tick() clears the event log; the cursor must reset with it.
    state.tick(1);
    CHECK(state.interact(0));
    gm::EventQueue after_tick;
    CHECK_EQ(state.post_switch_events(after_tick, 1), 1U);
    const gm::EventParseResult after_tick_events = after_tick.drain();
    CHECK_EQ(after_tick_events.events.size(), 1U);

    // Without a failed post the log stays frame-local: an unconsumed event
    // must not accumulate across ticks.
    state.tick(1);
    CHECK(state.interact(0));
    state.tick(1);
    CHECK(state.events().empty());
    gm::EventQueue unposted;
    CHECK_EQ(state.post_switch_events(unposted, 2), 0U);

    // A full queue must leave the pending switch in the log; after draining,
    // the same cursor posts it exactly once.
    state.tick(1);
    CHECK(state.interact(0));
    gm::EventQueue full;
    while (full.post_switch(0, 0)) {
    }
    CHECK_EQ(state.post_switch_events(full, 2), 0U);
    state.tick(1); // pending tail survives the tick
    const gm::EventParseResult full_events = full.drain();
    CHECK(!full_events.events.empty()); // queue was reset by drain
    // A retry from another caller keeps the source of the original attempt.
    CHECK_EQ(state.post_switch_events(full, 3), 1U);
    const gm::EventParseResult retried = full.drain();
    CHECK_EQ(retried.events.size(), 1U);
    if (retried.events.size() == 1) {
        CHECK_EQ(retried.events[0].source, 2);
    }
}

// QA F4: a pending tail that spans several presses must post every record with
// the source of *its own* press.  The old code kept one source for the whole
// tail (the first caller's), so the second press inherited source 2 instead of
// its own 7.
void test_switch_source_tail() {
    std::string error;
    gm::WorldState state = build_synthetic(&error);
    CHECK(error.empty());

    // A deliberately tiny queue: two type-2 records fit, the third is refused.
    gm::EventQueue queue(gm::EventQueue::kType2Size);
    CHECK(queue.post_switch(9, 0)); // filler, keeps the queue full
    CHECK(queue.post_switch(9, 0));
    CHECK(!queue.post_switch(9, 0));

    // First press (source 2) cannot be queued: it stays pending with source 2.
    CHECK(state.interact(0)); // switch 0 -> door 100
    CHECK_EQ(state.post_switch_events(queue, 2), 0U);
    // Second press (source 7) also stays pending, but with *its* source.
    CHECK(state.interact(1)); // switch 1 -> door 200
    CHECK_EQ(state.post_switch_events(queue, 7), 0U);
    state.tick(1); // both pending records survive the tick
    CHECK_EQ(state.post_switch_events(queue, 3), 0U); // still full, still blocked

    const gm::EventParseResult filler = queue.drain();
    CHECK_EQ(filler.events.size(), 2u);

    // Retry from a third caller: the per-record sources win over `source=5`.
    CHECK_EQ(state.post_switch_events(queue, 5), 2U);
    const gm::EventParseResult tail = queue.drain();
    CHECK_EQ(tail.events.size(), 2u);
    if (tail.events.size() == 2) {
        CHECK(tail.events[0].type == gm::EventType::trigger_switch);
        CHECK_EQ(tail.events[0].switch_index, 0U);
        CHECK_EQ(tail.events[0].source, 2);
        CHECK(tail.events[1].type == gm::EventType::trigger_switch);
        CHECK_EQ(tail.events[1].switch_index, 1U);
        CHECK_EQ(tail.events[1].source, 7);
    }
    // Fully drained: no duplicate records on a further call.
    CHECK_EQ(state.post_switch_events(queue, 9), 0U);
    CHECK(queue.empty());

    // Same rule without a full queue: each press is posted with its own source.
    state.clear_events();
    CHECK(state.interact(2));
    gm::EventQueue single;
    CHECK_EQ(state.post_switch_events(single, 4), 1U);
    const gm::EventParseResult first = single.drain();
    CHECK_EQ(first.events.size(), 1u);
    if (first.events.size() == 1u) {
        CHECK_EQ(first.events[0].source, 4);
    }
}

void test_negatives() {
    // Out-of-range object: the door exists, is counted, and is left unbound.
    {
        gm::SbtLevel level;
        gm::SbtRecord doors;
        doors.type = 2;
        doors.payload = make_door_payload(
            {{1, 0, 99, 1, {0, 1, 0, 0, 0}, {0, 0, 0}, {0, 0, 0}}});
        level.records.push_back(std::move(doors));
        gm::WorldState state;
        std::string error;
        CHECK(state.build(level, make_world(), &error));
        CHECK_EQ(state.stats().doors, 1u);
        CHECK_EQ(state.stats().doors_bound, 0u);
        CHECK_EQ(state.stats().bad_bindings, 1u);
        CHECK(!state.apply_to_objects()[0].bound);
    }

    // Truncated door table: count says one record, payload has none.
    {
        gm::SbtLevel level;
        gm::SbtRecord doors;
        doors.type = 2;
        doors.payload = {1, 0, 0, 0};
        gm::SbtRecord switches;
        switches.type = 1;
        switches.payload = make_switch_payload({1});
        level.records = {std::move(doors), std::move(switches)};
        gm::WorldState state;
        CHECK(state.build(level, make_world(), nullptr));
        CHECK_EQ(state.stats().doors, 0u);
        CHECK(state.stats().bad_records >= 1u);
    }

    // Giant counts in every record type: clamped, no OOB, no allocation storm.
    {
        gm::SbtLevel level;
        for (std::uint8_t type : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{18}}) {
            gm::SbtRecord record;
            record.type = type;
            record.payload = {0xFF, 0xFF, 0xFF, 0xFF};
            level.records.push_back(std::move(record));
        }
        gm::WorldState state;
        CHECK(state.build(level, make_world(), nullptr));
        CHECK_EQ(state.stats().doors, 0u);
        CHECK_EQ(state.stats().switches, 0u);
        CHECK_EQ(state.stats().lifts, 0u);
        CHECK(state.stats().bad_records >= 3u);
        state.tick(1'000'000'000ull);
    }

    // Invalid state word in a DOOR record is rejected, not clamped.
    {
        gm::SbtLevel level;
        gm::SbtRecord doors;
        doors.type = 2;
        doors.payload = make_door_payload({{7, 0, 0, 1, {0, 7, 0, 0, 0}, {}, {}, 0, 5}});
        level.records.push_back(std::move(doors));
        gm::WorldState state;
        CHECK(state.build(level, make_world(), nullptr));
        CHECK_EQ(state.stats().doors, 0u);
        CHECK_EQ(state.stats().bad_records, 1u);
    }

    // No source at all -> build fails with an error, stats stay zero.
    {
        gm::SbtLevel level;
        gm::WorldState state;
        std::string error;
        CHECK(!state.build(level, make_world(), &error));
        CHECK(!error.empty());
        CHECK_EQ(state.stats().doors, 0u);
        state.tick(5);
    }
}

struct LoadedLevel {
    gm::SbtLevel sbt;
    gm::SbwLevel sbw;
    bool ok = false;
};

LoadedLevel load_real(const fs::path& sbt_path, const fs::path& sbw_path) {
    LoadedLevel out;
    std::string error;
    const auto sbt_bytes = read_file(sbt_path);
    const auto sbw_bytes = read_file(sbw_path);
    if (sbt_bytes.empty() || sbw_bytes.empty()) {
        return out;
    }
    const auto sbt = gm::parse_sbt(sbt_bytes, error);
    if (!sbt) {
        std::fprintf(stderr, "parse_sbt(%s): %s\n", sbt_path.c_str(), error.c_str());
        return out;
    }
    const auto sbw = gm::parse_sbw(sbw_bytes, error);
    if (!sbw) {
        std::fprintf(stderr, "parse_sbw(%s): %s\n", sbw_path.c_str(), error.c_str());
        return out;
    }
    out.sbt = *sbt;
    out.sbw = *sbw;
    out.ok = true;
    return out;
}

// --- geometry: leaf transform, motion delta, cabin --------------------------

constexpr double kPi = 3.14159265358979323846;

bool orthonormal(const std::array<double, 9>& b, double eps) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k) {
                dot += b[static_cast<std::size_t>(i * 3 + k)] *
                       b[static_cast<std::size_t>(j * 3 + k)];
            }
            if (std::fabs(dot - (i == j ? 1.0 : 0.0)) > eps) {
                return false;
            }
        }
    }
    return true;
}

void test_geometry() {
    // Translation-only leaf (1422 of the shipped doors: every angle operand 0).
    // The runtime record holds corner+size, so origin at phase 0 is the raw
    // corner D1..D3 and the basis stays identity.
    {
        DoorSpec spec;
        spec.id = 10;
        spec.ids = {0, 10, 0, 0, 0};
        spec.size = {1.0, 2.0, 3.0};
        spec.closed = {11.0, 22.0, 33.0}; // D1 + size
        spec.open = {111.0, 222.0, 333.0};
        gm::SbtLevel level;
        gm::SbtRecord doors;
        doors.type = 2;
        doors.payload = make_door_payload({spec});
        level.records.push_back(std::move(doors));
        gm::WorldState state;
        CHECK(state.build(level, make_world(1), nullptr));
        const gm::Door& door = state.doors().at(0);
        CHECK_EQ(gm::door_rotation_axis(door), -1);
        const gm::DoorTransform at0 = gm::door_transform(door);
        CHECK_NEAR(at0.origin[0], 10.0, kEps);
        CHECK_NEAR(at0.origin[1], 20.0, kEps);
        CHECK_NEAR(at0.origin[2], 30.0, kEps);
        CHECK(orthonormal(at0.basis, kEps));
        const gm::DoorTransform half = gm::door_transform_at(door, 0x8000);
        CHECK_NEAR(half.center[0], 11.0 + (111.0 - 11.0) * (32768.0 / 65535.0), kEps);
        CHECK_NEAR(half.origin[0], half.center[0] - 1.0, kEps);
        const gm::DoorTransform at1 = gm::door_transform_at(door, 0xFFFF);
        CHECK_NEAR(at1.origin[0], 110.0, kEps);
        CHECK_NEAR(at1.origin[1], 220.0, kEps);
        CHECK_NEAR(at1.origin[2], 330.0, kEps);
        // The full-open phase is 0xffff / 65535 == 1.0 exactly, even though the
        // model's own phase counter goes to 65536.
        const gm::DoorTransform clamped = gm::door_transform_at(door, gm::kDoorPhaseFull);
        CHECK_NEAR(clamped.center[0], at1.center[0], 0.0);
        CHECK_NEAR(door.motion_fraction(), 0.0, kEps);

        // Per-tick motion vector written to obj+0x90 by 0x44f2e0.
        const std::array<double, 3> delta =
            gm::door_motion_delta(door, 0, 1500);
        CHECK_NEAR(delta[0], (111.0 - 11.0) * (1500.0 / 65535.0), kEps);
        CHECK_NEAR(delta[1], (222.0 - 22.0) * (1500.0 / 65535.0), kEps);
        CHECK_NEAR(delta[2], (333.0 - 33.0) * (1500.0 / 65535.0), kEps);
        const std::array<double, 3> none = gm::door_motion_delta(door, 1500, 1500);
        CHECK_NEAR(none[0], 0.0, 0.0);
        CHECK_NEAR(none[2], 0.0, 0.0);
        // Clamp: going past 0xffff must not move the leaf further.
        const std::array<double, 3> clipped = gm::door_motion_delta(door, 0xFFFF, 0x20000);
        CHECK_NEAR(clipped[0], 0.0, 0.0);
    }

    // Hinged leaf: one angle operand, closed == open (the leaf spins in place).
    {
        DoorSpec spec;
        spec.id = 11;
        spec.ids = {0, 11, 0, 0, 0};
        spec.size = {1.0, 2.0, 3.0};
        spec.closed = {5.0, 5.0, 5.0};
        spec.open = {5.0, 5.0, 5.0};
        spec.axis = {0.0, (kPi / 2.0) / 65535.0, 0.0};
        gm::SbtLevel level;
        gm::SbtRecord doors;
        doors.type = 2;
        doors.payload = make_door_payload({spec});
        level.records.push_back(std::move(doors));
        gm::WorldState state;
        CHECK(state.build(level, make_world(1), nullptr));
        const gm::Door& door = state.doors().at(0);
        CHECK_EQ(gm::door_rotation_axis(door), 1);
        const gm::DoorTransform at0 = gm::door_transform(door);
        CHECK_NEAR(at0.angle, 0.0, 0.0);
        CHECK(orthonormal(at0.basis, kEps));
        const gm::DoorTransform quarter = gm::door_transform_at(door, 0x4000);
        CHECK_NEAR(quarter.angle, (kPi / 2.0) * (16384.0 / 65535.0), 1e-15);
        CHECK(orthonormal(quarter.basis, kEps));
        const gm::DoorTransform full = gm::door_transform_at(door, 0xFFFF);
        CHECK_NEAR(full.angle, kPi / 2.0, 1e-12);
        CHECK(orthonormal(full.basis, kEps));
        // Rotation by -90 degrees about +y: row 0 is (0, 0, 1) up to sign.
        CHECK_NEAR(std::fabs(full.basis[2]), 1.0, 1e-12);
        CHECK_NEAR(full.basis[4], 1.0, 1e-12);
        // origin = center - basis^T * size
        for (int row = 0; row < 3; ++row) {
            double shifted = 0.0;
            for (int k = 0; k < 3; ++k) {
                shifted += spec.size[static_cast<std::size_t>(k)] *
                           full.basis[static_cast<std::size_t>(k * 3 + row)];
            }
            CHECK_NEAR(full.origin[static_cast<std::size_t>(row)],
                       full.center[static_cast<std::size_t>(row)] - shifted, kEps);
        }
        // A pure rotation must not translate the centre.
        for (int k = 0; k < 3; ++k) {
            CHECK_NEAR(full.center[static_cast<std::size_t>(k)], 5.0, kEps);
        }
    }

    // Axis priority of 0x44dfc0: delta[2] wins over delta[0], which wins over
    // delta[1]; two non-zero operands are legal in the data (357 doors rotate).
    {
        gm::Door door;
        door.axis = {0.5, 0.5, 0.5};
        CHECK_EQ(gm::door_rotation_axis(door), 2);
        door.axis = {0.5, 0.5, 0.0};
        CHECK_EQ(gm::door_rotation_axis(door), 0);
        door.axis = {0.0, 0.25, 0.0};
        CHECK_EQ(gm::door_rotation_axis(door), 1);
        door.axis = {0.0, 0.0, 0.0};
        CHECK_EQ(gm::door_rotation_axis(door), -1);
    }

    // Cabin: 0x44e6f0/0x41e025.  At height 0 the base angles are used, at
    // 65535 the base+delta sum, and the position is pos + pos_delta*fraction.
    {
        gm::Lift lift;
        lift.kind = 1;
        lift.x = 100;
        lift.y = 200;
        lift.z = 300;
        lift.pos_delta = {10, -20, 30};
        lift.basis = {0.0, 0.0, 0.0, 0.0, kPi / 2.0, 0.0};
        lift.height = 0;
        const gm::LiftTransform low = gm::lift_transform(lift);
        CHECK_NEAR(low.origin[0], 100.0, 0.0);
        CHECK_NEAR(low.origin[1], 200.0, 0.0);
        CHECK_NEAR(low.origin[2], 300.0, 0.0);
        CHECK(orthonormal(low.basis, kEps));
        CHECK_NEAR(low.basis[0], 1.0, kEps);
        CHECK_NEAR(low.basis[4], 1.0, kEps);
        CHECK_NEAR(low.basis[8], 1.0, kEps);
        CHECK_NEAR(lift_fraction(lift), 0.0, 0.0);

        lift.height = 0xFFFF;
        const gm::LiftTransform high = gm::lift_transform(lift);
        CHECK_NEAR(high.origin[0], 110.0, kEps);
        CHECK_NEAR(high.origin[1], 180.0, kEps);
        CHECK_NEAR(high.origin[2], 330.0, kEps);
        CHECK(orthonormal(high.basis, 1e-12));
        CHECK_NEAR(lift_fraction(lift), 1.0, 0.0);
        // value[1] (basis[1] + basis[4]) goes into the first angle, so the
        // stored second pair drives angles[0].
        CHECK_NEAR(high.angles[0], kPi / 2.0, 1e-12);
        CHECK_NEAR(high.angles[1], 0.0, 0.0);
        CHECK_NEAR(high.angles[2], 0.0, 0.0);

        // Clamp outside 0..65535.
        lift.height = -5;
        CHECK_NEAR(lift_fraction(lift), 0.0, 0.0);
        lift.height = 1 << 20;
        CHECK_NEAR(lift_fraction(lift), 1.0, 0.0);
    }
}

// --- sounds: v0/n0 start, v1/n1 loop, v2/n2 stop ----------------------------

void test_door_sounds() {
    DoorSpec spec;
    spec.id = 20;
    spec.ids = {0, 20, 0, 0, 0};
    spec.size = {1.0, 1.0, 1.0};
    spec.closed = {1.0, 1.0, 1.0};
    spec.open = {11.0, 1.0, 1.0};
    spec.volume = {64, 32, 48};
    spec.sample = {130, 131, 132};
    gm::SbtLevel level;
    gm::SbtRecord doors;
    doors.type = 2;
    doors.payload = make_door_payload({spec});
    gm::SbtRecord switches;
    switches.type = 1;
    switches.payload = make_switch_payload(std::vector<int>{20});
    level.records = {std::move(doors), std::move(switches)};
    gm::WorldState state;
    CHECK(state.build(level, make_world(1), nullptr));
    const gm::Door& door = state.doors().at(0);
    CHECK_EQ(door.start_sound.sample, 130);
    CHECK_EQ(door.start_sound.volume, 64);
    CHECK_EQ(door.move_sound.sample, 131);
    CHECK_EQ(door.move_sound.volume, 32);
    CHECK_EQ(door.stop_sound.sample, 132);
    CHECK_EQ(door.stop_sound.volume, 48);
    CHECK_NEAR(door.sound_level, 0.03, 0.0);
    CHECK(!door.start_sound.silent());

    // 0x44f690 plays v0/n0 on the GENERATE press.
    state.clear_events();
    CHECK(state.interact(0));
    const auto& events = state.events();
    CHECK_EQ(events.size(), 2u); // SwitchPressed, then DoorOpening
    if (events.size() == 2u) {
        CHECK(events[0].kind == gm::WorldEvent::Kind::SwitchPressed);
        CHECK(events[1].kind == gm::WorldEvent::Kind::DoorOpening);
        CHECK_EQ(events[1].sample, 130);
        CHECK_EQ(events[1].volume, 64);
    }
    // 0x44f3f0 plays v2/n2 when the leaf reaches either end and posts ids[3]
    // (open) / ids[4] (closed), which are 0 here.
    state.tick(100);
    const auto& done = state.events();
    CHECK(!done.empty());
    if (!done.empty()) {
        CHECK(done.back().kind == gm::WorldEvent::Kind::DoorOpened);
        CHECK_EQ(done.back().sample, 132);
        CHECK_EQ(done.back().volume, 48);
        CHECK_EQ(done.back().target, door.event_open);
    }

    // A silent door (-1) stays silent.
    gm::Door quiet;
    quiet.start_sound = {-1, 0};
    CHECK(quiet.start_sound.silent());
    quiet.start_sound = {gm::kDoorSampleCount, 0};
    CHECK(quiet.start_sound.silent());
}

// --- triggers: 0x44fc30 proximity, 0x44fbd0 exact point ---------------------

gm::WorldState build_trigger_level() {
    std::vector<DoorSpec> doors;
    for (int k = 0; k < 4; ++k) {
        DoorSpec spec;
        spec.id = 100 + k;
        spec.sector = 0;
        spec.object = k;
        spec.ids = {0, 100 + k, 0, 0, 0};
        spec.size = {1.0, 1.0, 1.0};
        // Closed corner at (6k, 0, 0) + size, so the leaf origin is (6k, 0, 0).
        spec.closed = {6.0 * k + 1.0, 1.0, 1.0};
        spec.open = spec.closed;
        doors.push_back(spec);
    }
    std::vector<SwitchSpec> switches = {
        // 0: proximity to pos, radius 10.
        {0, 100, 10, {20, 0, 0}, 0, -1},
        // 1: proximity to the leaf origin of door 101 at (6, 0, 0).
        {1, 101, 10, {0, 0, 0}, 0, -1},
        // 2: exact point (3, 4, 5).
        {0x104, 102, 0, {3, 4, 5}, 0, -1},
        // 3: aimed trigger, never fired by a bare position.
        {0x101, 103, 1000, {0, 0, 0}, 0, -1},
        // 4: radius 10 around the origin — octagon probe.
        {0, 100, 10, {0, 0, 0}, 0, -1},
        // 5: unmatched target.
        {0, 999, 5, {0, 0, 0}, 0, -1},
    };
    gm::SbtLevel level;
    gm::SbtRecord door_record;
    door_record.type = 2;
    door_record.payload = make_door_payload(doors);
    gm::SbtRecord switch_record;
    switch_record.type = 1;
    switch_record.payload = make_switch_payload(switches);
    level.records = {std::move(door_record), std::move(switch_record)};
    gm::WorldState state;
    state.build(level, make_world(4), nullptr);
    return state;
}

std::vector<int> fired_switches(const gm::WorldState& state) {
    std::vector<int> out;
    for (const gm::WorldEvent& event : state.events()) {
        if (event.kind == gm::WorldEvent::Kind::SwitchPressed) {
            out.push_back(event.switch_id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void test_triggers() {
    {
        gm::WorldState state = build_trigger_level();
        CHECK_EQ(state.switches().size(), 6u);
        // (20, 0, 0): only switch 0 (kind 0, pos 20, radius 10).
        state.clear_events();
        CHECK_EQ(state.poll_triggers({20.0, 0.0, 0.0}, 7), 1u);
        {
            const std::vector<int> fired = fired_switches(state);
            CHECK(fired.size() == 1u && fired[0] == 0);
        }
        CHECK_EQ(state.stats().trigger_presses, 1u);
        CHECK(state.switches()[0].inside);
        CHECK(!state.switches()[1].inside);
        CHECK(state.find_door(100)->state == gm::DoorState::Opening);

        // The source of the press is the one 0x479810 stores, not the poster's.
        gm::EventQueue queue;
        CHECK_EQ(state.post_switch_events(queue, 99), 1u);
        const gm::EventParseResult posted = queue.drain();
        CHECK_EQ(posted.events.size(), 1u);
        if (posted.events.size() == 1u) {
            CHECK_EQ(posted.events[0].switch_index, 0u);
            CHECK_EQ(posted.events[0].source, 7);
        }

        // Kind 1: proximity to the leaf origin of door 101 (6, 0, 0).
        state.clear_events();
        CHECK_EQ(state.poll_triggers({6.0, 0.0, 0.0}, 1), 2u); // switch 1 and 4
        CHECK(!state.switches()[0].inside);
        CHECK(state.switches()[1].inside);
        CHECK(state.switches()[4].inside); // (6,0,0) vs pos 0 radius 10
        CHECK_EQ(state.find_door(101)->state, gm::DoorState::Opening);

        // Octagon metric: (7.5, 7.5, 1) is 9.625 away by 0x4344df (< 10) but
        // 10.65 by Euclid, so a sphere would not fire switch 4 here.
        state.clear_events();
        CHECK_EQ(state.poll_triggers({-7.5, 7.5, 1.0}, 1), 1u);
        {
            const std::vector<int> fired = fired_switches(state);
            CHECK(fired.size() == 1u && fired[0] == 4);
        }

        // No latch: standing still posts again on the next poll.
        state.clear_events();
        CHECK_EQ(state.poll_triggers({-7.5, 7.5, 1.0}, 1), 1u);
        CHECK_EQ(state.stats().trigger_presses, 5u);

        // Kind 0x101 is aimed (0x43c51f); a position never fires it.
        CHECK(!state.switches()[3].inside);
        CHECK(state.find_door(103)->state == gm::DoorState::Closed);

        // Kind 0x104 (0x44fbd0): exact integer point match.
        state.clear_events();
        CHECK_EQ(state.trigger_at({3.0, 4.0, 5.0}, 5), 1u);
        {
            const std::vector<int> fired = fired_switches(state);
            CHECK(fired.size() == 1u && fired[0] == 2);
        }
        CHECK(state.find_door(102)->state == gm::DoorState::Opening);
        state.clear_events();
        CHECK_EQ(state.trigger_at({3.0, 4.0, 6.0}, 5), 0u);

        // An unmatched target only bumps the counter.
        const std::size_t unmatched = state.stats().unmatched_targets;
        state.clear_events();
        CHECK_EQ(state.poll_triggers({0.0, 0.0, 0.0}, 1), 3u); // switches 1, 4, 5
        CHECK_EQ(state.stats().unmatched_targets, unmatched + 1u);
    }

    // Doors of kind 0 are inert: 0x44f690 skips them and 0x44f2e0 never
    // animates them.
    {
        DoorSpec spec;
        spec.id = 40;
        spec.flags = 0;
        spec.ids = {0, 40, 0, 0, 0};
        gm::SbtLevel level;
        gm::SbtRecord doors;
        doors.type = 2;
        doors.payload = make_door_payload({spec});
        gm::SbtRecord switches;
        switches.type = 1;
        switches.payload = make_switch_payload(std::vector<int>{40});
        level.records = {std::move(doors), std::move(switches)};
        gm::WorldState state;
        CHECK(state.build(level, make_world(1), nullptr));
        CHECK(!state.doors().at(0).active());
        state.clear_events();
        CHECK(state.interact(0));
        CHECK_EQ(state.stats().unmatched_targets, 1u);
        CHECK(state.doors().at(0).state == gm::DoorState::Closed);
        state.doors_mutable()[0].state = gm::DoorState::Opening;
        state.tick(10);
        CHECK_EQ(state.doors().at(0).phase, 0u);
    }
}

// --- 0x450590: an entity blocks the leaf ------------------------------------

void test_block_contact() {
    std::vector<DoorSpec> doors;
    for (int kind = 1; kind <= 3; ++kind) {
        DoorSpec spec;
        spec.id = 50 + kind;
        spec.object = kind - 1;
        spec.flags = kind;
        spec.ids = {0, 50 + kind, 0, 0, 0};
        spec.size = {1.0, 1.0, 1.0};
        spec.closed = {1.0, 1.0, 1.0};
        spec.open = {21.0, 1.0, 1.0};
        spec.step = 1500;
        doors.push_back(spec);
    }
    std::vector<SwitchSpec> switches = {{259, 51, 128, {0, 0, 0}, 0, -1},
                                        {259, 52, 128, {0, 0, 0}, 0, -1},
                                        {259, 53, 128, {0, 0, 0}, 0, -1}};
    gm::SbtLevel level;
    gm::SbtRecord door_record;
    door_record.type = 2;
    door_record.payload = make_door_payload(doors);
    gm::SbtRecord switch_record;
    switch_record.type = 1;
    switch_record.payload = make_switch_payload(switches);
    level.records = {std::move(door_record), std::move(switch_record)};
    gm::WorldState state;
    std::string error;
    CHECK(state.build(level, make_world(3), &error));
    CHECK(error.empty());

    for (int k = 0; k < 3; ++k) {
        CHECK(state.interact(k));
    }
    state.tick(1);
    for (const gm::Door& door : state.doors()) {
        CHECK_EQ(door.phase, 1500u);
        CHECK_EQ(door.phase_prev, 0u);
        CHECK(door.state == gm::DoorState::Opening);
    }

    // Kind 1: ordinary door, 0x450590 returns without touching it.
    CHECK(!state.block_contact(0, 0));
    CHECK_EQ(state.doors().at(0).phase, 1500u);
    CHECK_EQ(state.stats().blocked_hits, 0u);

    // Kind 2: the tick's phase is dropped and the door reopens.
    state.clear_events();
    CHECK(state.block_contact(0, 1));
    CHECK_EQ(state.doors().at(1).phase, 0u);
    CHECK_EQ(state.doors().at(1).phase_next, 0u);
    CHECK(state.doors().at(1).state == gm::DoorState::Opening);
    CHECK_EQ(state.stats().blocked_hits, 1u);
    CHECK_EQ(state.events().size(), 1u);
    if (!state.events().empty()) {
        CHECK(state.events()[0].kind == gm::WorldEvent::Kind::DoorBlocked);
    }

    // Kind 3: the same, plus event_post(-22) — the crush event.
    state.clear_events();
    CHECK(state.block_contact(0, 2));
    CHECK_EQ(state.doors().at(2).phase, 0u);
    CHECK_EQ(state.stats().blocked_hits, 2u);
    CHECK_EQ(state.stats().crush_hits, 1u);
    CHECK_EQ(state.events().size(), 1u);
    if (!state.events().empty()) {
        CHECK(state.events()[0].kind == gm::WorldEvent::Kind::DoorCrush);
        CHECK_EQ(state.events()[0].door_id, 53);
    }

    // Unknown pair: nothing.
    CHECK(!state.block_contact(5, 5));
    CHECK_EQ(state.stats().blocked_hits, 2u);
}

// --- `.sbt` / save writer: records 1, 2, 18 --------------------------------

bool same_door_record(const gm::SbtDoorRecord& a, const gm::SbtDoorRecord& b) {
    if (a.closed != b.closed || a.open != b.open || a.size != b.size ||
        a.axis != b.axis || a.ids != b.ids || a.volume != b.volume ||
        a.sample != b.sample) {
        return false;
    }
    return a.sound_level == b.sound_level && a.flags == b.flags && a.state == b.state &&
           a.sector == b.sector && a.object == b.object &&
           a.phase_prev == b.phase_prev && a.phase_next == b.phase_next &&
           a.step == b.step && a.dwell == b.dwell && a.timer == b.timer;
}

bool same_switch_record(const gm::SbtSwitchRecord& a, const gm::SbtSwitchRecord& b) {
    return a.kind == b.kind && a.target == b.target && a.radius == b.radius &&
           a.pos == b.pos && a.aux == b.aux && a.sound == b.sound;
}

bool same_lift_record(const gm::SbtLiftRecord& a, const gm::SbtLiftRecord& b) {
    return a.basis == b.basis && a.handle == b.handle && a.kind == b.kind &&
           a.sob == b.sob && a.pos == b.pos && a.pos_delta == b.pos_delta &&
           a.speed == b.speed && a.dwell == b.dwell && a.height == b.height &&
           a.height_tick == b.height_tick && a.wait == b.wait &&
           a.direction == b.direction && a.stop_index == b.stop_index &&
           a.call_ids == b.call_ids && a.call_event == b.call_event &&
           a.call_radius == b.call_radius && a.volume_a == b.volume_a &&
           a.volume_b == b.volume_b && a.unknown_8c == b.unknown_8c &&
           a.sample_call == b.sample_call && a.sample_move == b.sample_move &&
           a.sample_arrive == b.sample_arrive && a.stop_count == b.stop_count &&
           a.stop_start == b.stop_start && a.unknown_a4 == b.unknown_a4;
}

void test_sbt_writer(bool full) {
    // Synthetic anchors: the payloads the rest of the tests use must survive
    // `encode(parse(x)) == x` byte-for-byte.
    {
        const auto door_payload = make_door_payload({
            {1, 0, 0, 3, {0, 1, 2, 3, 4}, {11, 22, 33}, {44, 55, 66}},
        });
        std::string error;
        std::vector<gm::SbtDoorRecord> parsed;
        CHECK(gm::parse_door_block(
            std::span<const std::uint8_t>(door_payload.data() + 4, door_payload.size() - 4),
            parsed, error));
        CHECK(error.empty());
        CHECK_EQ(parsed.size(), 1u);
        if (parsed.size() == 1u) {
            const gm::SbtDoorRecord& d = parsed[0];
            CHECK_EQ(d.flags, 3);
            CHECK_EQ(d.ids[4], 4);
            CHECK_NEAR(d.open[2], 66.0, 0.0);
            CHECK_EQ(d.sample[0], -1);
        }
        const auto body = gm::encode_door_block(parsed);
        CHECK(body.size() == door_payload.size() - 4);
        CHECK(std::memcmp(body.data(), door_payload.data() + 4, body.size()) == 0);
        const auto counted = gm::encode_counted(body, static_cast<std::uint32_t>(parsed.size()));
        CHECK(counted == door_payload);

        const auto switch_payload =
            make_switch_payload(std::vector<SwitchSpec>{{259, 7, 128, {1, 2, 3}, 5, 9}});
        std::vector<gm::SbtSwitchRecord> switch_parsed;
        CHECK(gm::parse_switch_block(
            std::span<const std::uint8_t>(switch_payload.data() + 4, switch_payload.size() - 4),
            switch_parsed, error));
        CHECK_EQ(switch_parsed.size(), 1u);
        if (switch_parsed.size() == 1u) {
            CHECK_EQ(switch_parsed[0].kind, 259);
            CHECK_EQ(switch_parsed[0].target, 7);
            CHECK_EQ(switch_parsed[0].aux, 5);
            CHECK_EQ(switch_parsed[0].sound, 9);
            const std::array<int, 3> expected_pos = {1, 2, 3};
            CHECK(switch_parsed[0].pos == expected_pos);
        }
        const auto switch_body = gm::encode_switch_block(switch_parsed);
        CHECK(std::memcmp(switch_body.data(), switch_payload.data() + 4, switch_body.size()) == 0);

        // Type 18 as the test level writes it.
        const auto lift_payload = make_lift_payload();
        gm::SbtLiftBlock lift_block;
        CHECK(gm::parse_lift_block(lift_payload, lift_block, error));
        CHECK_EQ(lift_block.lifts.size(), 1u);
        CHECK_EQ(lift_block.stops.size(), 2u);
        if (lift_block.lifts.size() == 1u) {
            CHECK_EQ(lift_block.lifts[0].kind, 1);
            CHECK_EQ(lift_block.lifts[0].speed, 10);
            CHECK_EQ(lift_block.lifts[0].stop_count, 2);
            CHECK_EQ(lift_block.lifts[0].stop_start, 0);
        }
        if (lift_block.stops.size() == 2u) {
            CHECK_EQ(lift_block.stops[0].floor_id, 500);
            CHECK_EQ(lift_block.stops[1].height, 100);
            CHECK_EQ(lift_block.stops[1].sound, 7);
        }
        const auto reencoded = gm::encode_lift_block(lift_block);
        CHECK(reencoded == lift_payload);
        // And the canonical form is stable.
        gm::SbtLiftBlock again;
        CHECK(gm::parse_lift_block(reencoded, again, error));
        CHECK(gm::encode_lift_block(again) == reencoded);

        // Out-of-range stop window is refused instead of written as garbage.
        gm::SbtLiftBlock broken = lift_block;
        CHECK(!broken.lifts.empty());
        if (!broken.lifts.empty()) {
            broken.lifts[0].stop_count = 3;
        }
        CHECK(gm::encode_lift_block(broken).empty());
        // Malformed bodies are rejected.
        std::vector<gm::SbtDoorRecord> rejected;
        CHECK(!gm::parse_door_block(
            std::span<const std::uint8_t>(door_payload.data() + 4, 0xB7), rejected, error));
        CHECK(!error.empty());
        CHECK(!gm::parse_lift_block(
            std::span<const std::uint8_t>(lift_payload.data(), lift_payload.size() - 4),
            again, error));
    }

    // Real save files: blocks 1 and 2 are real door/switch records, so
    // `encode(parse(y)) == y` is checked on bytes the game wrote.
    {
        for (const char* name : {"saved0.sbt", "saved1.sbt"}) {
            const fs::path path = kGame / "SAVEGAME" / name;
            if (!fs::exists(path)) {
                continue;
            }
            const auto bytes = read_file(path);
            std::string error;
            const auto save = gm::parse_save(bytes, error);
            if (!save) {
                std::fprintf(stderr, "doors: parse_save(%s): %s\n", name, error.c_str());
                ++g_failures;
                continue;
            }
            std::size_t door_count = 0;
            std::size_t switch_count = 0;
            for (const gm::SaveBlock& block : save->blocks) {
                // A block body is `u32 count` + records; the parsers take the
                // bare record body.
                if (block.raw.size() < 4) {
                    continue;
                }
                const std::span<const std::uint8_t> body(block.raw.data() + 4,
                                                         block.raw.size() - 4);
                if (block.id == 1) {
                    std::vector<gm::SbtSwitchRecord> records;
                    CHECK(gm::parse_switch_block(body, records, error));
                    CHECK_EQ(records.size(), block.count);
                    const auto encoded = gm::encode_switch_block(records);
                    CHECK(encoded.size() == body.size());
                    CHECK(encoded == std::vector<std::uint8_t>(body.begin(), body.end()));
                    CHECK(gm::encode_counted(encoded, block.count) == block.raw);
                    switch_count += records.size();
                } else if (block.id == 2) {
                    std::vector<gm::SbtDoorRecord> records;
                    CHECK(gm::parse_door_block(body, records, error));
                    CHECK_EQ(records.size(), block.count);
                    const auto encoded = gm::encode_door_block(records);
                    CHECK(encoded.size() == body.size());
                    CHECK(encoded == std::vector<std::uint8_t>(body.begin(), body.end()));
                    CHECK(gm::encode_counted(encoded, block.count) == block.raw);
                    // Save bytes are the *runtime* record: the folded corners
                    // are what the loader stored, and the phase lives in
                    // +0x8c/+0x90 as a 0..0xffff value.
                    for (const gm::SbtDoorRecord& record : records) {
                        CHECK(record.sector >= 0);
                        CHECK(record.object >= 0);
                    }
                    door_count += records.size();
                }
            }
            std::printf("doors: %s round-trip %zu doors, %zu switches\n", name,
                        door_count, switch_count);
            CHECK_EQ(door_count, 25u);
            CHECK_EQ(switch_count, 24u);
        }
    }

    // Shipped lifts (ADD_LIFT has no record-18 twin) become records and
    // survive the writer; the full run pins all 45 records / 151 stops, the
    // quick run only dm3 so it stays fast.
    if (fs::exists(kGame)) {
        std::size_t lifts = 0;
        std::size_t stops = 0;
        std::vector<fs::path> level_files;
        for (const auto& entry : fs::directory_iterator(kGame)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".sbt") {
                continue;
            }
            if (!full && entry.path().stem() != "dm3") {
                continue;
            }
            level_files.push_back(entry.path());
        }
        std::sort(level_files.begin(), level_files.end());
        for (const fs::path& path : level_files) {
            const fs::path sbw_path =
                path.parent_path() / (path.stem().string() + ".sbw");
            if (!fs::exists(sbw_path)) {
                continue;
            }
            const LoadedLevel level = load_real(path, sbw_path);
            if (!level.ok) {
                continue;
            }
            gm::WorldState state;
            std::string error;
            if (!state.build(level.sbt, level.sbw, &error)) {
                continue;
            }
            gm::SbtLiftBlock block;
            for (const gm::Lift& lift : state.lifts()) {
                gm::SbtLiftRecord record;
                record.kind = lift.kind;
                record.sob = lift.sob;
                record.pos = {lift.x, lift.y, lift.z};
                record.pos_delta = lift.pos_delta;
                record.basis = lift.basis;
                record.speed = lift.speed;
                record.dwell = lift.dwell;
                record.height = lift.height;
                record.height_tick = lift.height_tick;
                record.wait = lift.wait;
                record.direction = lift.direction;
                record.stop_index = lift.stop_index;
                record.call_ids = lift.call_ids;
                record.call_event = lift.call_event;
                record.call_radius = lift.call_radius;
                record.volume_a = lift.call_sound.volume;
                record.volume_b = lift.move_sound.volume;
                record.sample_call = lift.call_sound.sample;
                record.sample_move = lift.move_sound.sample;
                record.sample_arrive = lift.arrive_sound.sample;
                record.stop_count = static_cast<int>(lift.stops.size());
                record.stop_start = static_cast<int>(block.stops.size());
                for (const gm::LiftStop& stop : lift.stops) {
                    block.stops.push_back({stop.floor_id, stop.height, stop.sound});
                }
                block.lifts.push_back(record);
                ++lifts;
                stops += lift.stops.size();
            }
            if (block.lifts.empty()) {
                continue;
            }
            const auto payload = gm::encode_lift_block(block);
            CHECK(!payload.empty());
            gm::SbtLiftBlock parsed;
            std::string error2;
            if (!gm::parse_lift_block(payload, parsed, error2)) {
                std::fprintf(stderr, "doors: %s lifts: %s\n", path.c_str(),
                             error2.c_str());
                ++g_failures;
                continue;
            }
            CHECK_EQ(parsed.lifts.size(), block.lifts.size());
            CHECK_EQ(parsed.stops.size(), block.stops.size());
            CHECK(gm::encode_lift_block(parsed) == payload); // encode(parse(x)) == x
            for (std::size_t i = 0; i < parsed.lifts.size(); ++i) {
                CHECK(same_lift_record(parsed.lifts[i], block.lifts[i]));
            }
            for (std::size_t i = 0; i < parsed.stops.size(); ++i) {
                CHECK(parsed.stops[i].floor_id == block.stops[i].floor_id);
                CHECK(parsed.stops[i].height == block.stops[i].height);
                CHECK(parsed.stops[i].sound == block.stops[i].sound);
            }
            // The scripted lift now really moves: word 20 is the speed.
            for (const gm::Lift& lift : state.lifts()) {
                CHECK(lift.speed >= 0);
                CHECK(lift.speed < 1'000'000);
                CHECK_EQ(lift.id, static_cast<int>(&lift - state.lifts().data()));
                CHECK_EQ(lift.basis.size(), 6u);
            }
        }
        std::printf("doors: type-18 writer round-trip %zu lifts, %zu stops%s\n", lifts,
                    stops, full ? "" : " (dm3; --full pins 45/151)");
        if (full) {
            CHECK_EQ(lifts, 45u);
            CHECK_EQ(stops, 151u);
        } else {
            CHECK(lifts >= 1u);
            CHECK(stops >= 1u);
        }
    }
}


void test_door_events() {
    // `0x44f3f0` posts `ids[3]` when the leaf is fully open, `ids[4]` when it
    // is fully closed, and `0x450590` posts `-22` on a crush.  All of them go
    // through `0x4795b0` with `param_3 = object != 0`, i.e. type-1 records
    // (id, param = sector, a = object, x/y/z = object origin, extra = -1).
    DoorSpec spec;
    spec.sector = 0;
    spec.object = 2;
    spec.flags = 3; // animated, kind 3 (posts the crush event)
    spec.ids = {700, 0, 0, 500, 501};
    spec.closed = {1.0, 1.0, 1.0};
    spec.open = {101.0, 1.0, 1.0};
    spec.size = {1.0, 1.0, 1.0};
    spec.step = 0x8000; // half the phase per tick: 2 ticks to the end
    gm::SbtLevel level;
    gm::SbtRecord door_record;
    door_record.type = 2;
    door_record.payload = make_door_payload({spec});
    gm::SbtRecord switch_record;
    switch_record.type = 1;
    switch_record.payload = make_switch_payload({SwitchSpec{0, 700, 10, {0, 0, 0}, 0, -1}});
    level.records = {std::move(door_record), std::move(switch_record)};

    gm::WorldState state;
    CHECK(state.build(level, make_world(3), nullptr));
    gm::Door* door = state.find_door(700);
    CHECK(door != nullptr);
    if (door == nullptr) {
        return;
    }
    CHECK_EQ(door->event_open, 500);
    CHECK_EQ(door->event_close, 501);

    gm::EventQueue queue;
    CHECK_EQ(state.post_door_events(queue), 0u); // nothing happened yet
    CHECK(queue.empty());

    CHECK(state.interact(0)); // opening
    state.tick(1);
    CHECK_EQ(state.post_door_events(queue), 0u); // still moving
    state.tick(1);
    CHECK_EQ(door->phase, gm::kDoorPhaseFull);
    CHECK_EQ(state.post_door_events(queue), 1u);
    {
        const gm::EventParseResult parsed = queue.drain();
        CHECK(parsed.ok);
        CHECK_EQ(parsed.events.size(), 1u);
        if (!parsed.events.empty()) {
            const gm::Event& event = parsed.events[0];
            CHECK(event.type == gm::EventType::generate_short);
            CHECK_EQ(event.id, 500);
            CHECK_EQ(event.param, 0u); // sector
            CHECK_EQ(event.a, 2);
            CHECK_EQ(event.b, 0);
            CHECK_EQ(event.extra, 0xffffffffu);
        }
    }
    // Already posted: a second call posts nothing.
    CHECK_EQ(state.post_door_events(queue), 0u);
    CHECK(queue.empty());

    // Closing posts `ids[4]`.
    CHECK(state.interact(0));
    state.tick(2);
    CHECK_EQ(door->phase, 0u);
    CHECK_EQ(state.post_door_events(queue), 1u);
    {
        const gm::EventParseResult parsed = queue.drain();
        CHECK(parsed.ok);
        CHECK(parsed.events.size() == 1u &&
              parsed.events[0].id == 501 &&
              parsed.events[0].type == gm::EventType::generate_short);
    }

    // Crush posts `-22` (type 1 as well: object != 0).
    CHECK(state.interact(0)); // opening again
    state.tick(1);
    CHECK(state.block_contact(0, 2));
    CHECK_EQ(state.post_door_events(queue), 1u);
    {
        const gm::EventParseResult parsed = queue.drain();
        CHECK(parsed.ok);
        CHECK(parsed.events.size() == 1u);
        if (!parsed.events.empty()) {
            CHECK_EQ(parsed.events[0].id, -22);
            CHECK_EQ(parsed.events[0].type, gm::EventType::generate_short);
            CHECK_EQ(parsed.events[0].a, 2);
        }
    }
    // The crush record was posted once; the event log does not replay it.
    CHECK_EQ(state.post_door_events(queue), 0u);

    // A door without event ids posts nothing (the engine drops id 0).
    DoorSpec quiet = spec;
    quiet.ids = {701, 0, 0, 0, 0};
    level.records.clear();
    gm::SbtRecord quiet_record;
    quiet_record.type = 2;
    quiet_record.payload = make_door_payload({quiet});
    gm::SbtRecord quiet_switch;
    quiet_switch.type = 1;
    quiet_switch.payload =
        make_switch_payload({SwitchSpec{0, 701, 10, {0, 0, 0}, 0, -1}});
    level.records = {std::move(quiet_record), std::move(quiet_switch)};
    gm::WorldState silent;
    CHECK(silent.build(level, make_world(3), nullptr));
    CHECK(silent.interact(0));
    silent.tick(2);
    CHECK_EQ(silent.post_door_events(queue), 0u);
    CHECK(queue.empty());
}

void test_polygon_triggers() {
    // `ADD_SWITCH` operands 4..6 are a (sector, object, polygon) triple for
    // every kind >= 0x101 (docs/DOOR_FIELDS.md §2.2); `0x101` is the aim ray
    // (`0x44fc30`), `0x102`/`0x103` the blast proximity (`0x44fb30`, called
    // from opcode 75) and `0x104` the exact-triple trigger (`0x44fbd0`).
    std::vector<SwitchSpec> switches = {
        // 0: aimed, max ray length 100.
        {0x101, 100, 100, {2, 3, 4}, 0, -1},
        // 1: same polygon, tighter limit (boundary test for `<=`).
        {0x101, 101, 50, {2, 3, 4}, 0, -1},
        // 2: aimed at another polygon.
        {0x101, 102, 100, {5, 6, 7}, 0, -1},
        // 3: blast proximity, radius 40.
        {0x102, 103, 40, {2, 3, 4}, 0, -1},
        // 4: blast proximity, radius 10.
        {0x103, 104, 10, {2, 3, 4}, 0, -1},
        // 5: exact triple (0x44fbd0), never fires from a hit.
        {0x104, 105, 0, {2, 3, 4}, 0, -1},
        // 6: plain world position that happens to look like a triple.
        {0, 106, 10, {2, 3, 4}, 0, -1},
    };
    gm::SbtLevel level;
    gm::SbtRecord switch_record;
    switch_record.type = 1;
    switch_record.payload = make_switch_payload(switches);
    level.records = {std::move(switch_record)};

    gm::WorldState state;
    CHECK(state.build(level, make_world(4), nullptr));
    CHECK_EQ(state.switches().size(), switches.size());
    for (const gm::Switch& sw : state.switches()) {
        CHECK(sw.is_poly() == (sw.kind >= 0x101));
    }

    const auto fired = [&state]() { return fired_switches(state); };
    const auto hit = [](std::uint32_t s, std::uint32_t o, std::uint32_t p, double d) {
        gm::PolygonHit h;
        h.sector = s;
        h.object = o;
        h.polygon = p;
        h.distance = d;
        return h;
    };

    // The ray hit (2, 3, 4) at 60: only switch 0 covers it.
    CHECK_EQ(state.poll_aim(hit(2, 3, 4, 60.0), 3), 1u);
    {
        const std::vector<int> f = fired();
        CHECK(f.size() == 1u && f[0] == 0);
    }
    CHECK_EQ(state.stats().aim_presses, 1u);
    CHECK(state.switches()[0].pressed);

    // Exactly at the limit (50 <= 50): switches 0 and 1.
    state.clear_events();
    CHECK_EQ(state.poll_aim(hit(2, 3, 4, 50.0), 3), 2u);
    {
        const std::vector<int> f = fired();
        CHECK(f.size() == 2u && f[0] == 0 && f[1] == 1);
    }

    // Too far: no switch.
    state.clear_events();
    CHECK_EQ(state.poll_aim(hit(2, 3, 4, 100.5), 3), 0u);
    CHECK(fired().empty());

    // Right distance, wrong polygon.
    state.clear_events();
    CHECK_EQ(state.poll_aim(hit(2, 3, 5, 1.0), 3), 0u);
    CHECK(fired().empty());

    // The other polygon only matches switch 2.
    state.clear_events();
    CHECK_EQ(state.poll_aim(hit(5, 6, 7, 90.0), 3), 1u);
    {
        const std::vector<int> f = fired();
        CHECK(f.size() == 1u && f[0] == 2);
    }

    // The aim ray must not fire the blast kinds, nor kind 0/0x104.
    state.clear_events();
    CHECK_EQ(state.poll_aim(hit(2, 3, 4, 5.0), 3), 2u);
    {
        const std::vector<int> f = fired();
        CHECK(f.size() == 2u && f[0] == 0 && f[1] == 1);
    }

    // Blast proximity: 30 is inside 40 but outside 10.
    state.clear_events();
    CHECK_EQ(state.poll_blast(hit(2, 3, 4, 30.0), 4), 1u);
    {
        const std::vector<int> f = fired();
        CHECK(f.size() == 1u && f[0] == 3);
    }
    CHECK_EQ(state.stats().blast_presses, 1u);

    // 10 is the inclusive limit: both blast switches fire.
    state.clear_events();
    CHECK_EQ(state.poll_blast(hit(2, 3, 4, 10.0), 4), 2u);
    {
        const std::vector<int> f = fired();
        CHECK(f.size() == 2u && f[0] == 3 && f[1] == 4);
    }

    // Blast must not fire aimed switches, and a miss fires nothing.
    state.clear_events();
    CHECK_EQ(state.poll_blast(hit(2, 3, 4, 41.0), 4), 0u);
    CHECK_EQ(state.poll_blast(hit(9, 9, 9, 0.0), 4), 0u);
    CHECK(fired().empty());

    // 0x104 stays on `trigger_at`: an exact integer triple, not a hit.
    state.clear_events();
    CHECK_EQ(state.trigger_at({2.0, 3.0, 4.0}, 5), 1u);
    {
        const std::vector<int> f = fired();
        CHECK(f.size() == 1u && f[0] == 5);
    }

    // The kind-0 switch is untouched by both polygon paths.
    state.clear_events();
    CHECK_EQ(state.poll_aim(hit(2, 3, 4, 0.0), 3), 2u);
    CHECK_EQ(state.poll_blast(hit(2, 3, 4, 0.0), 3), 2u);
    CHECK(!state.switches()[6].pressed);

    // Determinism: the same call sequence twice yields the same counters.
    gm::WorldState a;
    gm::WorldState b;
    CHECK(a.build(level, make_world(4), nullptr));
    CHECK(b.build(level, make_world(4), nullptr));
    for (gm::WorldState* st : {&a, &b}) {
        st->poll_aim(hit(2, 3, 4, 20.0), 1);
        st->poll_blast(hit(2, 3, 4, 5.0), 2);
        st->trigger_at({2.0, 3.0, 4.0}, 3);
    }
    CHECK_EQ(a.stats().aim_presses, b.stats().aim_presses);
    CHECK_EQ(a.stats().blast_presses, b.stats().blast_presses);
    CHECK_EQ(a.stats().trigger_presses, b.stats().trigger_presses);
    CHECK_EQ(a.events().size(), b.events().size());
}

void test_real_dm1() {
    const fs::path sbt_path = kGame / "dm1.sbt";
    const fs::path sbw_path = kGame / "dm1.sbw";
    if (!fs::exists(sbt_path) || !fs::exists(sbw_path)) {
        std::printf("doors: dm1 not installed, real part skipped\n");
        return;
    }
    const LoadedLevel level = load_real(sbt_path, sbw_path);
    CHECK(level.ok);
    if (!level.ok) {
        return;
    }

    gm::WorldState state;
    std::string error;
    CHECK(state.build(level.sbt, level.sbw, &error));
    CHECK(error.empty());
    const gm::WorldStats& stats = state.stats();
    // tools/re/doors.py --level dm1
    CHECK_EQ(stats.doors, 61u);
    CHECK_EQ(stats.switches, 129u);
    CHECK_EQ(stats.lifts, 0u);
    CHECK_EQ(stats.doors_sliding, 46u);
    CHECK_EQ(stats.doors_linked, 4u);
    CHECK_EQ(stats.doors_bound, 61u);
    CHECK_EQ(stats.doors_distinct_objects, 61u);
    CHECK_EQ(stats.script_doors, 61u);
    CHECK_EQ(stats.script_switches, 129u);

    // Switch operands 4..6 are polymorphic: dm1 has 5 kind-0 world points and
    // 124 polygon switches (docs/DOOR_FIELDS.md §2.2, tools/re/doors.py).
    std::size_t poly = 0;
    std::size_t world_points = 0;
    for (const gm::Switch& sw : state.switches()) {
        if (sw.is_poly()) {
            ++poly;
        } else if (sw.kind == 0) {
            ++world_points;
        }
    }
    CHECK_EQ(poly, 124u);
    CHECK_EQ(world_points, 5u);
    {
        // An aimed hit on a real switch's polygon fires exactly that switch.
        // Uses a throwaway world: firing a switch opens doors, and the
        // geometry block below assumes the pristine phase 0.
        gm::WorldState probe;
        CHECK(probe.build(level.sbt, level.sbw, nullptr));
        const gm::Switch* aimed_ptr = nullptr;
        for (const gm::Switch& sw : probe.switches()) {
            if (sw.kind == 0x101) {
                aimed_ptr = &sw;
                break;
            }
        }
        CHECK(aimed_ptr != nullptr);
        if (aimed_ptr != nullptr) {
            const gm::Switch aimed = *aimed_ptr;
            gm::PolygonHit hit;
            hit.sector = static_cast<std::uint32_t>(aimed.pos[0]);
            hit.object = static_cast<std::uint32_t>(aimed.pos[1]);
            hit.polygon = static_cast<std::uint32_t>(aimed.pos[2]);
            hit.distance = static_cast<double>(aimed.radius);
            CHECK_EQ(probe.poll_aim(hit, 2), 1u);
            const std::vector<int> f = fired_switches(probe);
            CHECK(f.size() == 1u && f[0] == aimed.id);
            // ... and the same triple with a distance over the limit does not.
            probe.clear_events();
            hit.distance = static_cast<double>(aimed.radius) + 1.0;
            CHECK_EQ(probe.poll_aim(hit, 2), 0u);
        }
    }

    // Every door has a finite fraction in [0, 1] and a bound object.
    for (const gm::Door& door : state.doors()) {
        CHECK(door.bound);
        CHECK(std::isfinite(door.open_fraction));
        CHECK(door.open_fraction >= 0.0 && door.open_fraction <= 1.0);
    }
    CHECK_EQ(state.apply_to_objects().size(), 61u);

    // Golden: dm1 door #1 (-30231, step 1500) is driven by switch #51.
    gm::Door* door = state.find_door(-30231);
    CHECK(door != nullptr);
    if (door == nullptr) {
        return;
    }
    CHECK_EQ(door->phase, 0u);
    CHECK_EQ(door->step, 1500);
    CHECK_EQ(state.switches()[51].target, -30231);
    CHECK(state.interact(51));
    state.tick(10);
    CHECK_EQ(door->phase, 15000u);
    CHECK_NEAR(door->open_fraction, 15000.0 / 65536.0, kEps);
    CHECK(door->state == gm::DoorState::Opening);
    state.tick(34);
    CHECK_EQ(door->phase, gm::kDoorPhaseFull);
    CHECK_NEAR(door->open_fraction, 1.0, kEps);
    CHECK(door->state == gm::DoorState::Closed);
    CHECK(state.interact(51));
    CHECK(door->state == gm::DoorState::Closing);
    state.tick(44);
    CHECK_EQ(door->phase, 0u);
    CHECK_NEAR(door->open_fraction, 0.0, kEps);

    // Determinism over the whole level.
    gm::WorldState again;
    CHECK(again.build(level.sbt, level.sbw, &error));
    CHECK(again.interact(51));
    again.tick(17);
    gm::WorldState twice;
    CHECK(twice.build(level.sbt, level.sbw, &error));
    CHECK(twice.interact(51));
    twice.tick(17);
    CHECK_NEAR(again.find_door(-30231)->open_fraction,
               twice.find_door(-30231)->open_fraction, 0.0);

    // Geometry against the script operands: every shipped door starts at
    // phase 0, so the engine's origin is exactly the raw D1..D3 corner and the
    // size is D7..D9.  `closed` in the record is that corner + size (the
    // loader fold at 0x44e368).
    std::string scene_error;
    const auto scene = gm::extract_scene(level.sbt, scene_error, false);
    CHECK(scene.has_value());
    if (!scene) {
        return;
    }
    CHECK_EQ(scene->doors.size(), state.doors().size());
    std::size_t rotating = 0;
    for (const gm::SceneDoor& src : scene->doors) {
        const gm::Door* model = nullptr;
        for (const gm::Door& door : state.doors()) {
            if (door.sector == src.params[0] && door.object == src.params[1]) {
                model = &door;
                break;
            }
        }
        CHECK(model != nullptr);
        if (model == nullptr) {
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            CHECK_NEAR(model->size[static_cast<std::size_t>(k)],
                       src.values[static_cast<std::size_t>(6 + k)], 0.0);
            CHECK_NEAR(model->closed[static_cast<std::size_t>(k)],
                       src.values[static_cast<std::size_t>(k)] +
                           src.values[static_cast<std::size_t>(6 + k)],
                       0.0);
            CHECK_NEAR(model->axis[static_cast<std::size_t>(k)],
                       src.angles[static_cast<std::size_t>(k)] * gm::kDoorPhaseMaxToFraction,
                       1e-18);
        }
        const gm::DoorTransform at0 = gm::door_transform(*model);
        for (int k = 0; k < 3; ++k) {
            CHECK_NEAR(at0.origin[static_cast<std::size_t>(k)],
                       src.values[static_cast<std::size_t>(k)], 1e-9);
        }
        CHECK(orthonormal(at0.basis, kEps));
        const gm::DoorTransform at1 = gm::door_transform_at(*model, 0xFFFF);
        CHECK(orthonormal(at1.basis, 1e-12));
        if (gm::door_rotation_axis(*model) >= 0) {
            ++rotating;
        }
    }
    CHECK_EQ(rotating, 23u); // dm1: 23 of its 61 doors carry an angle operand
}

void test_corpus() {
    if (!fs::exists(kGame)) {
        std::printf("doors: game not installed, corpus sweep skipped\n");
        return;
    }
    std::vector<fs::path> levels;
    for (const auto& entry : fs::directory_iterator(kGame)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sbt") {
            levels.push_back(entry.path());
        }
    }
    std::sort(levels.begin(), levels.end());

    std::size_t doors = 0;
    std::size_t switches = 0;
    std::size_t lifts = 0;
    std::size_t bound = 0;
    std::size_t distinct = 0;
    std::size_t sliding = 0;
    std::size_t linked = 0;
    std::size_t parsed = 0;
    std::size_t rotating = 0;
    std::size_t transforms = 0;
    for (const fs::path& sbt_path : levels) {
        const fs::path sbw_path = sbt_path.parent_path() / (sbt_path.stem().string() + ".sbw");
        if (!fs::exists(sbw_path)) {
            continue;
        }
        const LoadedLevel level = load_real(sbt_path, sbw_path);
        if (!level.ok) {
            continue;
        }
        ++parsed;
        gm::WorldState state;
        std::string error;
        if (!state.build(level.sbt, level.sbw, &error)) {
            std::fprintf(stderr, "doors: build(%s): %s\n", sbt_path.c_str(), error.c_str());
            ++g_failures;
            continue;
        }
        const gm::WorldStats& stats = state.stats();
        doors += stats.doors;
        switches += stats.switches;
        lifts += stats.lifts;
        bound += stats.doors_bound;
        distinct += stats.doors_distinct_objects;
        sliding += stats.doors_sliding;
        linked += stats.doors_linked;
        CHECK_EQ(stats.doors_bound, stats.doors);
        for (const gm::Door& door : state.doors()) {
            if (!std::isfinite(door.open_fraction)) {
                std::fprintf(stderr, "doors: non-finite fraction in %s\n", sbt_path.c_str());
                ++g_failures;
            }
            // Every shipped door must produce a finite, orthonormal transform
            // at both ends of its travel.
            const gm::DoorTransform at0 = gm::door_transform_at(door, 0);
            const gm::DoorTransform at1 = gm::door_transform_at(door, 0xFFFF);
            if (!orthonormal(at0.basis, 1e-12) || !orthonormal(at1.basis, 1e-12) ||
                !std::isfinite(at0.origin[0]) || !std::isfinite(at1.origin[0])) {
                std::fprintf(stderr, "doors: bad transform in %s\n", sbt_path.c_str());
                ++g_failures;
            }
            ++transforms;
            if (gm::door_rotation_axis(door) >= 0) {
                ++rotating;
            }
        }
    }
    CHECK_EQ(parsed, 36u);
    CHECK_EQ(doors, 1779u);
    CHECK_EQ(switches, 3964u);
    CHECK_EQ(lifts, 45u);
    CHECK_EQ(bound, 1779u);
    CHECK_EQ(distinct, 1779u);
    CHECK_EQ(sliding, 1534u); // docs/FORMATS.md §12.4
    CHECK_EQ(linked, 200u);   // E4 != 0
    CHECK_EQ(transforms, 1779u);
    CHECK_EQ(rotating, 357u); // doors with a non-zero angle operand
}

} // namespace

int main(int argc, char** argv) {
    const bool full = argc > 1 && std::string(argv[1]) == "--full";
    test_synthetic();
    test_switch_source_tail();
    test_negatives();
    test_geometry();
    test_door_sounds();
    test_triggers();
    test_block_contact();
    test_polygon_triggers();
    test_door_events();
    test_sbt_writer(full);
    test_real_dm1();
    if (full) {
        test_corpus();
    } else {
        // dm1 is the quick representative; --full pins the 36-level totals.
        std::printf("doors: quick run (dm1); use --full for the 36-level corpus\n");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "doors: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("doors: ok\n");
    return 0;
}
