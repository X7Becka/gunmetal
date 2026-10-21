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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        }
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

std::vector<std::uint8_t> make_switch_payload(const std::vector<int>& targets) {
    std::vector<std::uint8_t> payload(4 + targets.size() * 0x20, 0);
    put_u32(payload, 0, static_cast<std::uint32_t>(targets.size()));
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const std::size_t base = 4 + k * 0x20;
        put_i32(payload, base, 259); // kind (transformed), as in dm1
        put_i32(payload, base + 4, targets[k]);
        put_i32(payload, base + 8, 128);
        put_i32(payload, base + 12, static_cast<int>(k));
    }
    return payload;
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

// --- real dm1 + corpus ------------------------------------------------------

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
}

} // namespace

int main(int argc, char** argv) {
    const bool full = argc > 1 && std::string(argv[1]) == "--full";
    test_synthetic();
    test_negatives();
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
