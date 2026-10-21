// Minimal thing AI state machine (docs/AI_CPP.md).
//
// Synthetic part: a hand-built SBT/SCRIPT with two absolute things and one
// source-relative thing (so the attach filter is exercised), an empty collision
// world and a single wall.  It pins idle -> alert -> attack, the 91 Hz timers,
// line-of-sight blocking, loss of sight, leash/return, death (dead does not
// tick), two-run determinism and the negative inputs.
//
// Real part: dm1.sbt / level0.sbt from the shipped game.  Attach counts, the
// first absolute agent and its golden position after 100 ticks are pinned.  The
// AI never mutates `Things`; `--full` is intentionally not needed here (the
// thing corpus lives in CTest `things_full`).
#include "world/ai.h"
#include "world/ai_move.h"

#include "res/sbt.h"
#include "res/sbw.h"
#include "res/vm_exec.h"
#include "world/things.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

void put_u32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    std::memcpy(data.data() + offset, &value, 4);
}

void append_u32(std::vector<std::uint8_t>& code, std::uint32_t value) {
    const std::size_t at = code.size();
    code.resize(at + 4);
    put_u32(code, at, value);
}

void append_i32(std::vector<std::uint8_t>& code, int value) {
    append_u32(code, static_cast<std::uint32_t>(value));
}

void append_f64(std::vector<std::uint8_t>& code, double value) {
    const std::size_t at = code.size();
    code.resize(at + 8);
    std::memcpy(code.data() + at, &value, 8);
}

// --- synthetic SBT ---------------------------------------------------------

struct Row {
    std::uint32_t flags = 0x3FF;
    int id = 0;
    std::uint32_t offset = 0;
};

struct TypeSpec {
    const char* name;
    std::uint32_t model_index;
    int history_offset = 0; ///< THING_TYPES +0x1d0
    int max_health = 0;    ///< THING_TYPES +0x1d4, 0 = damage immune
    std::array<int, 4> shields{}; ///< THING_TYPES +0x1d8..+0x1e4
    int hits_left = 0;     ///< THING_TYPES +0x1c8
    double search_radius = 6000.0; ///< THING_TYPES +0x58
};

gm::SbtLevel make_sbt_level(const std::vector<std::uint8_t>& code,
                            const std::vector<Row>& rows,
                            const std::vector<TypeSpec>& types,
                            const std::vector<std::string>& models) {
    gm::SbtLevel level;
    level.script_code = code;
    level.script.handles = static_cast<std::uint32_t>(rows.size());
    level.script_handles_data.resize(rows.size() * 12);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        put_u32(level.script_handles_data, i * 4, rows[i].flags);
        put_u32(level.script_handles_data, (rows.size() + i) * 4,
                static_cast<std::uint32_t>(rows[i].id));
        put_u32(level.script_handles_data, (2 * rows.size() + i) * 4, rows[i].offset);
    }

    gm::SbtRecord types_record;
    types_record.type = 4;
    types_record.payload.resize(4 + types.size() * 936, 0);
    put_u32(types_record.payload, 0, static_cast<std::uint32_t>(types.size()));
    for (std::size_t i = 0; i < types.size(); ++i) {
        const std::size_t base = 4 + i * 936;
        const std::size_t length = std::min<std::size_t>(std::strlen(types[i].name), 15);
        std::memcpy(types_record.payload.data() + base, types[i].name, length);
        put_u32(types_record.payload, base + 0x1A4, types[i].model_index);
        std::memcpy(types_record.payload.data() + base + 0x58,
                    &types[i].search_radius, sizeof(double));
        put_u32(types_record.payload, base + 0x1C8,
                static_cast<std::uint32_t>(types[i].hits_left));
        put_u32(types_record.payload, base + 0x1D0,
                static_cast<std::uint32_t>(types[i].history_offset));
        put_u32(types_record.payload, base + 0x1D4,
                static_cast<std::uint32_t>(types[i].max_health));
        for (std::size_t s = 0; s < types[i].shields.size(); ++s) {
            put_u32(types_record.payload, base + 0x1D8 + s * 4,
                    static_cast<std::uint32_t>(types[i].shields[s]));
        }
    }
    level.records.push_back(std::move(types_record));

    gm::SbtRecord script_record;
    script_record.type = 11;
    script_record.payload = code;
    level.records.push_back(std::move(script_record));

    for (const std::string& model : models) {
        gm::SbtSob sob;
        sob.name = model;
        level.sobs.push_back(sob);
    }
    return level;
}

void add_thing(std::vector<std::uint8_t>& code, std::uint32_t type, int x, int y, int z) {
    code.push_back(0xC9);
    code.push_back(1); // ADD_THING GLLLAAA
    append_u32(code, type);
    append_i32(code, x);
    append_i32(code, y);
    append_i32(code, z);
    append_f64(code, 0.0);
    append_f64(code, 0.0);
    append_f64(code, 0.0);
}

void add_thing_at_source(std::vector<std::uint8_t>& code, std::uint32_t type, int owner) {
    code.push_back(0xC9);
    code.push_back(3); // ADD_THING_AT_SOURCE LGAAA
    append_i32(code, owner);
    append_u32(code, type);
    append_f64(code, 0.0);
    append_f64(code, 0.0);
    append_f64(code, 0.0);
}

gm::Things make_synthetic_things(int history_offset = 0, int max_health = 0,
                                 double search_radius = 6000.0) {
    std::vector<std::uint8_t> code;
    add_thing(code, 0, 0, 0, 0);        // agent 0, home (0,0,0)
    add_thing(code, 0, 1000, 0, 0);     // agent 1, home (1000,0,0)
    add_thing_at_source(code, 0, -1);   // no absolute position -> skipped
    code.push_back(0xFF);

    std::vector<Row> rows{{0x3FF, 42, 0}};
    TypeSpec spec{"TEST_THING", 0, history_offset, max_health, {10, 20, 30, 40}, 3,
                  search_radius};
    const std::vector<TypeSpec> types{spec};
    const std::vector<std::string> models{"TEST_MODEL"};
    const gm::SbtLevel level = make_sbt_level(code, rows, types, models);
    const gm::VmState vm = gm::vm_state_from_level(level);

    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    return gm::Things::load(level, vm, options);
}

gm::SbtLevel load_real(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    std::string error;
    const auto level = gm::parse_sbt(bytes, error);
    if (!level) {
        std::fprintf(stderr, "ai: parse_sbt(%s): %s\n", path.c_str(), error.c_str());
        return {};
    }
    return *level;
}

// --- synthetic collision world --------------------------------------------

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
            const float value = (point == 0) ? 0.0f : (component == point - 1 ? 1.0f : 0.0f);
            put_f32(header.data() + 8 * (point * 3 + component) + 4, value);
        }
    }
}

gm::SbwLevel make_sbw_level(const std::vector<TriSpec>& specs) {
    gm::SbwLevel level;
    level.header.sectors.resize(1);
    for (const TriSpec& spec : specs) {
        gm::SbwObjectRef object;
        write_identity_header(object.header);
        level.header.sectors.at(0).objects.push_back(object);

        gm::SbwGeometry geometry;
        geometry.n_verts = 3;
        geometry.vertices = {gm::Vec3f{static_cast<float>(spec.a.x), static_cast<float>(spec.a.y),
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

gm::CollisionWorld wall_between(double y) {
    std::vector<TriSpec> specs;
    add_quad(specs, {-50.0, y, -20.0}, {50.0, y, -20.0}, {50.0, y, 40.0},
             {-50.0, y, 40.0});
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_sbw_level(specs), &error));
    CHECK(error.empty());
    return world;
}

// A wall that stops the body sphere (z = 0, radius 10) but stays below the
// eye ray from (0, 0, 10) to (0, 300, 0), which passes z = 6.67 at y = 100.
gm::CollisionWorld low_wall_between(double y) {
    std::vector<TriSpec> specs;
    add_quad(specs, {-50.0, y, -20.0}, {50.0, y, -20.0}, {50.0, y, 5.0},
             {-50.0, y, 5.0});
    gm::CollisionWorld world;
    std::string error;
    CHECK(world.build(make_sbw_level(specs), &error));
    CHECK(error.empty());
    return world;
}

bool same_agent(const gm::AiAgent& a, const gm::AiAgent& b) {
    return a.thing_id == b.thing_id && a.state == b.state && a.target == b.target &&
           a.alertness == b.alertness && a.timer == b.timer && a.leash == b.leash &&
           a.home.x == b.home.x && a.home.y == b.home.y && a.home.z == b.home.z &&
           a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.health == b.health &&
           a.distance == b.distance && a.visible == b.visible &&
           a.returning == b.returning && a.has_home == b.has_home;
}

bool has_event(const gm::Ai& ai, gm::AiEventType type, int thing_id) {
    for (const gm::AiEvent& event : ai.events()) {
        if (event.type == type && event.thing_id == thing_id) {
            return true;
        }
    }
    return false;
}

std::size_t events_for(const gm::Ai& ai, int thing_id) {
    std::size_t count = 0;
    for (const gm::AiEvent& event : ai.events()) {
        if (event.thing_id == thing_id) {
            ++count;
        }
    }
    return count;
}

// --- tests ------------------------------------------------------------------

void test_attach_and_transitions() {
    const gm::Things things = make_synthetic_things();
    CHECK_EQ(things.things().size(), 3u);
    CHECK(!things.things()[2].absolute());

    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 2u);
    CHECK_EQ(ai.stats().attached, 2u);
    CHECK_EQ(ai.stats().skipped_without_position, 1u);
    CHECK_EQ(ai.stats().idle, 2u);

    const gm::AiAgent* first = ai.agent(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->thing_id, 0);
        CHECK(first->has_home);
        CHECK_EQ(first->state, gm::AiState::idle);
        CHECK_NEAR(first->home.x, 0.0, 0.0);
        CHECK_NEAR(first->home.y, 0.0, 0.0);
        CHECK_NEAR(first->home.z, 0.0, 0.0);
        CHECK_NEAR(first->timer, ai.config().patrol_after_seconds, 1e-12);
    }

    const gm::CollisionWorld empty;
    CHECK(ai.set_player({0.0, 60.0, 10.0}));
    ai.tick(empty);

    first = ai.agent(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->state, gm::AiState::alert);
        CHECK_EQ(first->target, 0);
        CHECK_NEAR(first->distance, 60.0, 1e-12);
        CHECK(first->visible);
        CHECK_NEAR(first->timer, ai.config().alert_seconds, 1e-12);
    }
    const gm::AiAgent* second = ai.agent(1);
    CHECK(second != nullptr);
    if (second != nullptr) {
        CHECK_EQ(second->state, gm::AiState::idle);
        CHECK(!second->visible);
        CHECK(second->distance > ai.config().sight_range);
    }
    CHECK_EQ(ai.stats().los_checks, 1u);
    CHECK(has_event(ai, gm::AiEventType::sight, 0));

    // 0.5 s at 91 Hz = 46 ticks (45.5) until the first attack step.
    for (int i = 0; i < 46; ++i) {
        ai.tick(empty);
    }
    first = ai.agent(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->state, gm::AiState::attack);
        CHECK_EQ(first->target, 0);
        CHECK_NEAR(first->timer, ai.config().attack_period, 1e-12);
    }

    const std::uint64_t before = ai.stats().attacks;
    for (int i = 0; i < 69; ++i) {
        ai.tick(empty);
    }
    CHECK(ai.stats().attacks > before);
    CHECK(has_event(ai, gm::AiEventType::attack, 0));
    const gm::AiEvent* attack_event = nullptr;
    for (const gm::AiEvent& event : ai.events()) {
        if (event.type == gm::AiEventType::attack && event.thing_id == 0) {
            attack_event = &event;
        }
    }
    CHECK(attack_event != nullptr);
    if (attack_event != nullptr) {
        CHECK_EQ(attack_event->target, 0);
        CHECK_EQ(attack_event->target_thing, -1);
        CHECK_NEAR(attack_event->aim.y, 60.0, 0.0);
    }
    CHECK_EQ(ai.stats().ticks, 116u);
}

void test_line_of_sight_and_loss() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld wall = wall_between(100.0);

    gm::Ai blind;
    CHECK_EQ(blind.attach(things), 2u);
    CHECK(blind.set_player({0.0, 200.0, 10.0}));
    blind.tick(wall);

    const gm::AiAgent* agent = blind.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::idle);
        CHECK(!agent->visible);
        CHECK_NEAR(agent->distance, 200.0, 1e-12);
    }
    CHECK_EQ(blind.stats().los_checks, 1u);
    CHECK_EQ(blind.stats().los_blocked, 1u);
    CHECK(!has_event(blind, gm::AiEventType::sight, 0));

    // Acquire the target in the open, then hide it behind the same wall.
    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 60.0, 10.0}));
    const gm::CollisionWorld empty;
    ai.tick(empty);
    CHECK_EQ(ai.agent(0)->state, gm::AiState::alert);

    CHECK(ai.set_player({0.0, 200.0, 10.0}));
    for (int i = 0; i < 274; ++i) {
        ai.tick(wall);
    }
    agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::patrol);
        CHECK_EQ(agent->target, -1);
        CHECK_EQ(agent->alertness, 0.0);
        CHECK(!agent->visible);
    }
    CHECK(has_event(ai, gm::AiEventType::lost_sight, 0));
}

void test_leash_and_return() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 60.0, 10.0}));
    ai.tick(empty);
    CHECK_EQ(ai.agent(0)->state, gm::AiState::alert);

    CHECK(ai.place(0, {1000.0, 0.0, 0.0}));
    ai.tick(empty);
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::idle);
        CHECK(agent->returning);
        CHECK_EQ(agent->target, -1);
        CHECK(agent->position.x < 1000.0);
        CHECK(agent->position.x > 0.0);
    }
    CHECK(has_event(ai, gm::AiEventType::leash, 0));

    // No player: the return walk must not immediately re-acquire a target.
    ai.clear_player();
    int ticks = 0;
    while (ai.agent(0)->returning && ticks < 1000) {
        ai.tick(empty);
        ++ticks;
    }
    agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK(!agent->returning);
        CHECK_NEAR(agent->position.x, 0.0, 0.0);
        CHECK_NEAR(agent->position.y, 0.0, 0.0);
        CHECK_NEAR(agent->position.z, 0.0, 0.0);
        CHECK_EQ(agent->state, gm::AiState::patrol);
    }
    CHECK(ticks > 0);
    CHECK(ticks < 1000);
    CHECK(has_event(ai, gm::AiEventType::arrived, 0));
}

void test_death_and_negatives() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.damage(0, 1000.0));
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::dead);
        CHECK_NEAR(agent->health, 0.0, 0.0);
        CHECK_EQ(agent->target, -1);
        CHECK_EQ(agent->target_thing, -1);
        CHECK(!agent->target_is_thing);
        CHECK_NEAR(agent->timer, 0.0, 0.0);
    }
    CHECK_EQ(ai.stats().deaths, 1u);
    CHECK_EQ(ai.stats().dead, 1u);
    CHECK(has_event(ai, gm::AiEventType::death, 0));

    const std::size_t events = events_for(ai, 0);
    const double timer = ai.agent(0)->timer;
    const double alertness = ai.agent(0)->alertness;
    CHECK(ai.set_player({0.0, 60.0, 10.0}));
    for (int i = 0; i < 300; ++i) {
        ai.tick(empty);
    }
    CHECK_EQ(events_for(ai, 0), events);
    CHECK_NEAR(ai.agent(0)->timer, timer, 0.0);
    CHECK_NEAR(ai.agent(0)->alertness, alertness, 0.0);
    CHECK_EQ(ai.agent(0)->state, gm::AiState::dead);

    CHECK(!ai.damage(0, 1.0));               // already dead
    CHECK(!ai.damage(-999, 1.0));            // unknown id
    CHECK(!ai.damage(1, -5.0));              // negative amount
    CHECK(!ai.damage(1, 0.0));               // zero amount
    CHECK(!ai.damage(1, std::numeric_limits<double>::quiet_NaN()));
    CHECK_EQ(ai.stats().invalid_damage, 5u);

    // Dead instance in Things is attached as dead and never ticks.
    gm::Things dead_things = make_synthetic_things();
    CHECK(dead_things.kill(0));
    gm::Ai dead_ai;
    CHECK_EQ(dead_ai.attach(dead_things), 2u);
    CHECK_EQ(dead_ai.agent(0)->state, gm::AiState::dead);
    dead_ai.tick(empty);
    CHECK_EQ(dead_ai.agent(0)->state, gm::AiState::dead);
    CHECK_EQ(dead_ai.stats().dead, 1u);
}

void test_dt_and_player_negatives() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(!ai.has_player());

    CHECK(!ai.set_player({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}));
    CHECK(!ai.has_player());
    CHECK(!ai.set_player({0.0, std::numeric_limits<double>::infinity(), 0.0}));
    CHECK(!ai.has_player());

    ai.tick(empty); // null player is safe
    CHECK_EQ(ai.stats().ticks, 1u);
    CHECK_EQ(ai.agent(0)->state, gm::AiState::idle);

    CHECK(ai.set_player({0.0, 60.0, 10.0}));
    ai.tick(empty, 1e9); // clamped to config().max_dt
    CHECK_EQ(ai.stats().ticks, 2u);
    CHECK_EQ(ai.stats().clamped_ticks, 1u);
    CHECK_EQ(ai.agent(0)->state, gm::AiState::alert);
    CHECK(std::isfinite(ai.agent(0)->timer));
    CHECK(std::isfinite(ai.agent(0)->position.x));

    ai.tick(empty, 0.0);
    ai.tick(empty, -1.0);
    ai.tick(empty, std::numeric_limits<double>::quiet_NaN());
    CHECK_EQ(ai.stats().ignored_ticks, 3u);
    CHECK_EQ(ai.stats().ticks, 2u);
}

void test_two_runs() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld wall = wall_between(100.0);
    const gm::CollisionWorld empty;

    gm::Ai a;
    gm::Ai b;
    CHECK_EQ(a.attach(things), 2u);
    CHECK_EQ(b.attach(things), 2u);
    CHECK(a.set_player({0.0, 60.0, 10.0}));
    CHECK(b.set_player({0.0, 60.0, 10.0}));

    for (int i = 0; i < 300; ++i) {
        // Alternate the worlds so both LOS branches participate.
        const gm::CollisionWorld& world = (i % 7 == 0) ? wall : empty;
        a.tick(world);
        b.tick(world);
    }
    CHECK_EQ(a.agents().size(), b.agents().size());
    for (std::size_t i = 0; i < a.agents().size() && i < b.agents().size(); ++i) {
        CHECK(same_agent(a.agents()[i], b.agents()[i]));
    }
    CHECK_EQ(a.events().size(), b.events().size());
    for (std::size_t i = 0; i < a.events().size() && i < b.events().size(); ++i) {
        CHECK_EQ(a.events()[i].type, b.events()[i].type);
        CHECK_EQ(a.events()[i].thing_id, b.events()[i].thing_id);
        CHECK_EQ(a.events()[i].tick, b.events()[i].tick);
    }
    CHECK_EQ(a.stats().ticks, 300u);
    CHECK_EQ(a.stats().attacks, b.stats().attacks);
}

void test_target_history() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    // Empty history: no player, no entries, no lag-1 position.
    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.target_history().empty());
    CHECK_EQ(ai.target_history().size(), 0u);
    gm::Vec3d out{};
    CHECK(!ai.target_position(0, out));
    CHECK(!ai.target_position(1, out));

    // A tick without a player does not invent a target position.
    ai.tick(empty);
    CHECK(ai.target_history().empty());
    CHECK_EQ(ai.stats().ticks, 1u);

    // Three distinct eye positions, one recorded per accepted tick.
    const gm::Vec3d a{10.0, 20.0, 30.0};
    const gm::Vec3d b{40.0, 50.0, 60.0};
    const gm::Vec3d c{70.0, 80.0, 90.0};
    CHECK(ai.set_player(a));
    ai.tick(empty);
    CHECK_EQ(ai.target_history().size(), 1u);
    CHECK(ai.target_position(0, out));
    CHECK_NEAR(out.x, a.x, 0.0);
    CHECK(!ai.target_position(1, out)); // two entries are needed for lag 1

    CHECK(ai.set_player(b));
    ai.tick(empty);
    CHECK(ai.set_player(c));
    ai.tick(empty);
    CHECK_EQ(ai.target_history().size(), 3u);

    CHECK(ai.target_position(0, out)); // "current" = live player eye
    CHECK_NEAR(out.x, c.x, 0.0);
    CHECK_NEAR(out.y, c.y, 0.0);
    CHECK_NEAR(out.z, c.z, 0.0);
    CHECK(ai.target_position(1, out)); // one tick back = b
    CHECK_NEAR(out.x, b.x, 0.0);
    CHECK_NEAR(out.y, b.y, 0.0);
    CHECK_NEAR(out.z, b.z, 0.0);
    CHECK(ai.target_position(2, out)); // two ticks back = a
    CHECK_NEAR(out.x, a.x, 0.0);
    CHECK_NEAR(out.y, a.y, 0.0);
    CHECK_NEAR(out.z, a.z, 0.0);
    CHECK(!ai.target_position(3, out)); // only three entries

    // Negative lag is clamped to 0, i.e. the current position.
    CHECK(ai.target_position(-5, out));
    CHECK_NEAR(out.x, c.x, 0.0);

    // Data field clamp: 0 keeps "now"; positive values live in 1..239.
    CHECK_EQ(gm::ai_history_lag(-5), 0);
    CHECK_EQ(gm::ai_history_lag(0), 0);
    CHECK_EQ(gm::ai_history_lag(1), 1);
    CHECK_EQ(gm::ai_history_lag(10), 10);
    CHECK_EQ(gm::ai_history_lag(239), 239);
    CHECK_EQ(gm::ai_history_lag(240), 239);
    CHECK_EQ(gm::ai_history_lag(1000000), 239);

    // A full ring wraps: 240 positions, lag 239 reaches the oldest slot.
    ai.reset_target_history();
    CHECK(ai.target_history().empty());
    for (int i = 0; i < gm::kAiHistorySlots; ++i) {
        CHECK(ai.record_target_position({static_cast<double>(i), 0.0, 0.0}));
    }
    CHECK_EQ(ai.target_history().size(), 240u);
    CHECK(ai.target_history().at(0, out));
    CHECK_NEAR(out.x, 239.0, 0.0);
    CHECK(ai.target_position(239, out));
    CHECK_NEAR(out.x, 0.0, 0.0);
    CHECK(ai.target_position(240, out)); // clamped to 239
    CHECK_NEAR(out.x, 0.0, 0.0);
    CHECK(!ai.record_target_position(
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}));
    CHECK_EQ(ai.target_history().size(), 240u); // rejected, ring unchanged

    // `attach` clears the ring.
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.target_history().empty());

    // Data wiring: +0x1d0 is copied through ai_history_lag; the default
    // (use_thing_fields=false) keeps lag 0 regardless of the record.
    gm::AiConfig typed_config;
    typed_config.use_thing_fields = true;
    const gm::Things lag3 = make_synthetic_things(3);
    gm::Ai typed(typed_config);
    CHECK_EQ(typed.attach(lag3), 2u);
    CHECK_EQ(typed.agent(0)->history_lag, 3);
    const gm::Things lag500 = make_synthetic_things(500);
    gm::Ai clamped(typed_config);
    CHECK_EQ(clamped.attach(lag500), 2u);
    CHECK_EQ(clamped.agent(0)->history_lag, 239);
    const gm::Things lag_negative = make_synthetic_things(-4);
    gm::Ai negative(typed_config);
    CHECK_EQ(negative.attach(lag_negative), 2u);
    CHECK_EQ(negative.agent(0)->history_lag, 0);
    gm::Ai plain;
    CHECK_EQ(plain.attach(lag3), 2u);
    CHECK_EQ(plain.agent(0)->history_lag, 0);

    // The agent's aim point follows +0x1d0: lag 3 means the eye from three
    // accepted ticks ago.  The live distance still drives LOS/range.
    const gm::Things aimed_things = make_synthetic_things(3);
    gm::Ai aimed(typed_config);
    CHECK_EQ(aimed.attach(aimed_things), 2u);
    const gm::CollisionWorld clear_world;
    for (int i = 1; i <= 5; ++i) {
        CHECK(aimed.set_player({static_cast<double>(i), 0.0, 10.0}));
        aimed.tick(clear_world);
    }
    const gm::AiAgent* aimer = aimed.agent(0);
    CHECK(aimer != nullptr);
    if (aimer != nullptr) {
        CHECK(aimer->has_aim);
        CHECK_EQ(aimer->history_lag, 3);
        CHECK_NEAR(aimer->aim_point.x, 2.0, 0.0);
        CHECK_NEAR(aimer->aim_point.y, 0.0, 0.0);
        CHECK_NEAR(aimer->aim_distance, 2.0, 1e-12);
    }
    aimed.clear_player();
    aimed.tick(clear_world);
    // With no player the original falls through to things 8..0x10b.
    CHECK(aimed.agent(0)->target_is_thing);
    CHECK_EQ(aimed.agent(0)->target_thing, 1);
    CHECK_NEAR(aimed.agent(0)->distance, std::sqrt(1000.0 * 1000.0 + 10.0 * 10.0),
               1e-12);
    CHECK(aimed.agent(0)->has_aim);
    CHECK_NEAR(aimed.agent(0)->aim_point.x, 1000.0, 0.0);

    // Multiple player eyes: the scan picks the nearest visible target, and
    // each slot has its own 240-slot history (original 0x473a00).
    gm::Ai multi;
    CHECK_EQ(multi.attach(things), 2u);
    const std::array<gm::Vec3d, 2> eyes{
        {{0.0, 60.0, 10.0}, {0.0, 30.0, 10.0}}};
    CHECK(multi.set_players(eyes));
    CHECK_EQ(multi.player_count(), 2);
    multi.tick(empty);
    const gm::AiAgent* nearest = multi.agent(0);
    CHECK(nearest != nullptr);
    if (nearest != nullptr) {
        CHECK_EQ(nearest->target, 1);
        CHECK_NEAR(nearest->distance, 30.0, 1e-12);
        CHECK(nearest->has_aim);
        CHECK_NEAR(nearest->aim_point.y, 30.0, 1e-12);
    }
    CHECK_EQ(multi.target_history(0).size(), 1u);
    CHECK_EQ(multi.target_history(1).size(), 1u);
    gm::Vec3d slot{};
    CHECK(multi.target_position(1, 0, slot));
    CHECK_NEAR(slot.y, 30.0, 1e-12);
    CHECK(!multi.target_position(2, 0, slot));
    CHECK(!multi.set_players({}));
    CHECK_EQ(multi.player_count(), 2); // rejected empty set leaves it intact

    // Determinism: two runs record identical rings over 300 ticks.
    gm::Ai first;
    gm::Ai second;
    CHECK_EQ(first.attach(things), 2u);
    CHECK_EQ(second.attach(things), 2u);
    for (int i = 0; i < 300; ++i) {
        const gm::Vec3d eye{static_cast<double>(i), static_cast<double>(-i), 10.0};
        CHECK(first.set_player(eye));
        CHECK(second.set_player(eye));
        first.tick(empty);
        second.tick(empty);
    }
    CHECK_EQ(first.target_history().size(), 240u);
    CHECK_EQ(second.target_history().size(), 240u);
    for (int lag = 0; lag <= gm::kAiHistoryMaxLag; ++lag) {
        gm::Vec3d left{};
        gm::Vec3d right{};
        CHECK_EQ(first.target_history().at(lag, left),
                 second.target_history().at(lag, right));
        CHECK_NEAR(left.x, right.x, 0.0);
        CHECK_NEAR(left.y, right.y, 0.0);
        CHECK_NEAR(left.z, right.z, 0.0);
    }
}

void test_vitals_sync() {
    // The AI mirrors the vitals of the owning thing.  The runtime damage
    // path (apply_thing_damage) writes the thing, the AI picks the result up
    // on the next tick; the AI never mutates `Things`.
    gm::Things things = make_synthetic_things(0, 100, 0.0);
    gm::AiConfig config;
    config.use_thing_fields = true;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);

    const gm::AiAgent* first = ai.agent(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_NEAR(first->health, 100.0, 0.0);
        CHECK(!first->immune);
        CHECK_EQ(first->max_health, 100.0);
    }

    // The thing takes damage somewhere else.
    gm::Thing* thing = things.by_id(0);
    CHECK(thing != nullptr);
    if (thing != nullptr) {
        thing->health = 40;
        thing->shields = {1, 2, 3, 4};
        thing->hits_left = 1;
        thing->burn_timer = 91;
        thing->status_flags = 0x40;
    }
    const gm::CollisionWorld empty;
    ai.tick(empty);
    first = ai.agent(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_NEAR(first->health, 40.0, 0.0);
        CHECK_EQ(first->shields[0], 1);
        CHECK_EQ(first->shields[3], 4);
        CHECK_EQ(first->hits_left, 1);
        CHECK_EQ(first->burn_timer, 91);
        CHECK_EQ(first->status_flags, 0x40U);
        CHECK_EQ(first->state, gm::AiState::idle);
    }
    CHECK_EQ(ai.stats().vitals_syncs, 1u);

    // The thing dies: the agent follows on the next tick.
    CHECK(things.kill(0));
    ai.tick(empty);
    first = ai.agent(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->state, gm::AiState::dead);
    }
    CHECK_EQ(ai.stats().thing_deaths, 1u);
    // A dead agent stays dead and is not counted twice.
    ai.tick(empty);
    CHECK_EQ(ai.stats().thing_deaths, 1u);
    CHECK_EQ(ai.stats().dead, 1u);
}

void test_real_level(const fs::path& path, const char* name, std::size_t expect_things,
                     std::size_t expect_attached, int first_id, double x, double y,
                     double z) {
    if (!fs::exists(path)) {
        std::printf("ai: %s not installed, real-level test skipped\n", name);
        return;
    }
    const gm::SbtLevel level = load_real(path);
    const gm::VmState vm = gm::vm_state_from_level(level);
    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    const gm::Things things = gm::Things::load(level, vm, options);
    CHECK(things.ok());
    CHECK_EQ(things.things().size(), expect_things);

    gm::Ai ai;
    CHECK_EQ(ai.attach(things), expect_attached);
    CHECK_EQ(ai.stats().attached, expect_attached);
    CHECK_EQ(ai.stats().skipped_without_position, things.things().size() - expect_attached);
    CHECK_EQ(ai.stats().idle, expect_attached);

    const gm::AiAgent* first = ai.agent(first_id);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->thing_id, first_id);
        CHECK_EQ(first->state, gm::AiState::idle);
        CHECK_NEAR(first->home.x, x, 0.0);
        CHECK_NEAR(first->home.y, y, 0.0);
        CHECK_NEAR(first->home.z, z, 0.0);
        CHECK_NEAR(first->position.x, x, 0.0);
        CHECK_NEAR(first->position.y, y, 0.0);
        CHECK_NEAR(first->position.z, z, 0.0);
    }

    // Without a player the agent stays idle; its position must not move and
    // the idle timer must count down from the patrol delay.
    const gm::CollisionWorld empty;
    for (int i = 0; i < 100; ++i) {
        ai.tick(empty);
    }
    first = ai.agent(first_id);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->state, gm::AiState::idle);
        CHECK_NEAR(first->position.x, x, 0.0);
        CHECK_NEAR(first->position.y, y, 0.0);
        CHECK_NEAR(first->position.z, z, 0.0);
        CHECK_NEAR(first->timer, ai.config().patrol_after_seconds - 100.0 / 91.0, 1e-9);
    }
    std::printf("ai: %s attached %zu, first id %d at (%.17g, %.17g, %.17g)\n", name,
                ai.stats().attached, first_id, x, y, z);
}

void test_real_dm1() {
    if (!fs::exists(kGame / "dm1.sbt")) {
        std::printf("ai: game not installed, real levels skipped\n");
        return;
    }
    test_real_level(kGame / "dm1.sbt", "dm1", 141u, 5u, 115, -350.0, 1304.0, 40.0);
    test_real_level(kGame / "level0.sbt", "level0", 121u, 6u, 115, 24.0, 2708.0,
                    -92.0);

    // Last absolute thing on dm1 is the OILDRUM at (5688,216,264).
    const gm::SbtLevel level = load_real(kGame / "dm1.sbt");
    const gm::VmState vm = gm::vm_state_from_level(level);
    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    const gm::Things things = gm::Things::load(level, vm, options);
    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 5u);
    const gm::AiAgent* last = ai.agent(140);
    CHECK(last != nullptr);
    if (last != nullptr) {
        CHECK_NEAR(last->home.x, 5688.0, 0.0);
        CHECK_NEAR(last->home.y, 216.0, 0.0);
        CHECK_NEAR(last->home.z, 264.0, 0.0);
    }

    // Opt-in THING_TYPES fields: telrad is immune; OILDRUM has 1000 HP.
    gm::AiConfig typed_config;
    typed_config.use_thing_fields = true;
    gm::Ai typed(typed_config);
    CHECK_EQ(typed.attach(things), 5u);
    const gm::AiAgent* telrad = typed.agent(115);
    CHECK(telrad != nullptr);
    if (telrad != nullptr) {
        CHECK_EQ(telrad->movement_type, 0);
        CHECK_NEAR(telrad->search_radius, 0.0, 0.0);
        CHECK_EQ(telrad->target_zone, 200);
        CHECK_EQ(telrad->max_health, 0.0);
        CHECK(telrad->immune);
        CHECK(!typed.damage(115, 10.0));
        CHECK_EQ(typed.stats().immune_hits, 1u);
    }
    const gm::AiAgent* drum = typed.agent(140);
    CHECK(drum != nullptr);
    if (drum != nullptr) {
        CHECK_EQ(drum->movement_type, 18);
        CHECK_NEAR(drum->search_radius, 0.0, 0.0);
        CHECK_EQ(drum->max_health, 1000.0);
        CHECK(!drum->immune);
    }

    // dm1 THING_TYPES has no nonzero +0x1d0, so history_lag stays 0 for every
    // attached agent; still pin the copy through the shared clamp helper.
    for (const gm::Thing& thing : things.things()) {
        if (!thing.absolute()) {
            continue;
        }
        const gm::AiAgent* attached = typed.agent(thing.id);
        CHECK(attached != nullptr);
        if (attached != nullptr) {
            CHECK_EQ(attached->history_lag,
                     gm::ai_history_lag(thing.target_history_offset));
            CHECK_EQ(attached->history_lag, 0);
        }
    }
}

} // namespace


void test_octagonal_distance() {
    CHECK_NEAR(gm::octagonal_distance({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}),
               0.0, 0.0);
    CHECK_NEAR(gm::octagonal_distance({0.0, 0.0, 0.0}, {3.0, 4.0, 0.0}),
               4.75, 0.0);
    CHECK_NEAR(gm::octagonal_distance({3.0, 4.0, 0.0}, {0.0, 0.0, 0.0}),
               4.75, 0.0);
    CHECK_NEAR(gm::octagonal_distance({1.0, 2.0, 3.0}, {4.0, 6.0, 3.0}),
               4.75, 1e-12);
}


void test_seek_step() {
    const gm::CollisionWorld empty;
    gm::AiMoveParams params;
    params.speed_per_tick = 2.0;
    params.radius = 0.0;
    params.arrive_radius = 0.1;

    gm::Vec3d position{0.0, 0.0, 0.0};
    bool arrived = false;
    for (int i = 0; i < 91; ++i) {
        const gm::AiSeekResult step = gm::ai_seek_step(
            position, {0.0, 182.0, 0.0}, empty, params, 1.0 / 91.0);
        position = step.position;
        arrived = step.arrived;
    }
    CHECK_NEAR(position.y, 182.0, 1e-9);
    CHECK(arrived);

    gm::AiMoveParams stop = params;
    stop.arrive_radius = 50.0;
    const gm::AiSeekResult stopped =
        gm::ai_seek_step({0.0, 0.0, 0.0}, {0.0, 100.0, 0.0}, empty, stop,
                         1.0 / 91.0);
    CHECK(!stopped.arrived);
    CHECK_NEAR(stopped.position.y, 2.0, 1e-12);

    const gm::CollisionWorld wall = wall_between(100.0);
    gm::AiMoveParams blocked_params = params;
    blocked_params.radius = 10.0;
    gm::Vec3d blocked_position{0.0, 0.0, 0.0};
    bool blocked = false;
    for (int i = 0; i < 91; ++i) {
        const gm::AiSeekResult step = gm::ai_seek_step(
            blocked_position, {0.0, 182.0, 0.0}, wall, blocked_params,
            1.0 / 91.0);
        blocked_position = step.position;
        blocked = blocked || step.blocked;
    }
    CHECK(blocked);
    CHECK(blocked_position.y <= 90.0 + 1e-9);
    CHECK(blocked_position.y > 80.0);
}

// A4: pure `0x475f10` automaton over a linear waypoint list.  Four points,
// loop 0,1,2,3,0; expire on the last; empty list; repeated advance.
void test_path_loop() {
    const std::array<gm::Vec3d, 4> points{{
        {10.0, 0.0, 0.0},
        {20.0, 0.0, 0.0},
        {30.0, 0.0, 0.0},
        {40.0, 0.0, 0.0},
    }};
    const int expected[] = {0, 1, 2, 3, 0, 1};

    gm::AiPathState state;
    for (int i = 0; i < 6; ++i) {
        const gm::AiPathStep step =
            gm::path_advance(state, points, gm::AiPathMode::loop);
        CHECK(step.selected);
        CHECK(step.waiting);
        CHECK(!step.arrived);
        CHECK(!step.expired);
        CHECK_EQ(step.index, expected[i]);
        CHECK_NEAR(step.point.x,
                   points[static_cast<std::size_t>(expected[i])].x, 0.0);
        CHECK_EQ(step.state.index, expected[i]);
        CHECK(step.state.waiting);

        // The body reaches the point: `0x476a50` clears `+0x4f4`.
        state = step.state;
        gm::path_mark_arrived(state);
        CHECK(!state.waiting);
    }
    CHECK_EQ(state.index, 1);
}

void test_path_expire() {
    const std::array<gm::Vec3d, 4> points{{
        {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0},
        {3.0, 0.0, 0.0},
        {4.0, 0.0, 0.0},
    }};
    gm::AiPathState state;
    for (int i = 0; i < 4; ++i) {
        const gm::AiPathStep step =
            gm::path_advance(state, points, gm::AiPathMode::expire);
        CHECK(step.selected);
        CHECK(!step.expired);
        CHECK_EQ(step.index, i);
        state = step.state;
        gm::path_mark_arrived(state);
    }

    // Reaching the last point is not the expiry: the *next* advance is.
    const gm::AiPathStep expired =
        gm::path_advance(state, points, gm::AiPathMode::expire);
    CHECK(!expired.selected);
    CHECK(expired.expired);
    CHECK(!expired.waiting);
    CHECK_EQ(expired.index, 3);
    CHECK_NEAR(expired.point.x, 4.0, 0.0);
    CHECK(expired.state.expired);

    // `0x4703a0` is posted once; the dead state stays terminal.
    const gm::AiPathStep again =
        gm::path_advance(expired.state, points, gm::AiPathMode::expire);
    CHECK(!again.expired); // no second expiry event
    CHECK(again.state.expired);
    CHECK(!again.selected);
    CHECK_EQ(again.index, 3);
}

void test_path_empty() {
    const std::span<const gm::Vec3d> none;
    for (const gm::AiPathMode mode :
         {gm::AiPathMode::path, gm::AiPathMode::loop}) {
        const gm::AiPathState state;
        const gm::AiPathStep step = gm::path_advance(state, none, mode);
        CHECK(!step.selected);
        CHECK(!step.waiting);
        CHECK(!step.arrived);
        CHECK(!step.expired);
        CHECK_EQ(step.index, -1);
        CHECK_EQ(step.state.index, -1);
        CHECK(!step.state.expired);
    }

    // The original's `path_count < 1` branch: only pathexpire dies.
    const gm::AiPathStep expired =
        gm::path_advance({}, none, gm::AiPathMode::expire);
    CHECK(expired.expired);
    CHECK(expired.state.expired);
    CHECK(!expired.selected);

    gm::AiPathState dead;
    dead.expired = true;
    const gm::AiPathStep dead_step =
        gm::path_advance(dead, none, gm::AiPathMode::expire);
    CHECK(!dead_step.expired);
    CHECK(dead_step.state.expired);
}

void test_path_repeat_advance() {
    const std::array<gm::Vec3d, 4> points{{
        {0.0, 0.0, 0.0},
        {0.0, 10.0, 0.0},
        {0.0, 20.0, 0.0},
        {0.0, 30.0, 0.0},
    }};

    const gm::AiPathStep first =
        gm::path_advance({}, points, gm::AiPathMode::loop);
    CHECK(first.selected);
    CHECK_EQ(first.index, 0);

    // Pending point: no second selection, the same waypoint comes back.
    const gm::AiPathStep pending =
        gm::path_advance(first.state, points, gm::AiPathMode::loop);
    CHECK(!pending.selected);
    CHECK(pending.waiting);
    CHECK_EQ(pending.index, 0);
    CHECK_NEAR(pending.point.y, 0.0, 0.0);
    CHECK_EQ(pending.state.index, 0);

    // `path` mode: after the last point the agent stands there for good.
    gm::AiPathState path_state;
    for (int i = 0; i < 4; ++i) {
        const gm::AiPathStep step =
            gm::path_advance(path_state, points, gm::AiPathMode::path);
        CHECK(step.selected);
        CHECK_EQ(step.index, i);
        path_state = step.state;
        gm::path_mark_arrived(path_state);
    }
    const gm::AiPathStep done =
        gm::path_advance(path_state, points, gm::AiPathMode::path);
    CHECK(done.arrived);
    CHECK(!done.selected);
    CHECK(!done.expired);
    CHECK_EQ(done.index, 3);
    const gm::AiPathStep done_again =
        gm::path_advance(done.state, points, gm::AiPathMode::path);
    CHECK(done_again.arrived);
    CHECK(!done_again.selected);
    CHECK_EQ(done_again.index, 3);

    // Corrupt index restarts from the beginning, never reads out of bounds.
    gm::AiPathState broken;
    broken.index = 99;
    const gm::AiPathStep restarted =
        gm::path_advance(broken, points, gm::AiPathMode::loop);
    CHECK(restarted.selected);
    CHECK_EQ(restarted.index, 0);

    // More than 32 points: the original's `0x1f < i` guard truncates.
    std::vector<gm::Vec3d> long_path(gm::kAiPathMaxPoints + 8,
                                     gm::Vec3d{0.0, 0.0, 0.0});
    gm::AiPathState long_state;
    long_state.index = gm::kAiPathMaxPoints - 1;
    const gm::AiPathStep wrapped =
        gm::path_advance(long_state, long_path, gm::AiPathMode::loop);
    CHECK(wrapped.selected);
    CHECK_EQ(wrapped.index, 0);

    // A4 fields are opt-in data; A2/A3 defaults must not change.
    const gm::AiConfig config;
    CHECK(!config.use_path);
    CHECK(config.path_mode == gm::AiPathMode::loop);
}

// A4 wiring: default `use_path=false` must ignore an assigned route and keep
// the historical patrol behaviour (walk home / stand); `use_path=true` walks
// it.  Also pins `set_path` validation and the 32-point truncation.
void test_tick_path_opt_in() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;
    const std::array<gm::Vec3d, 2> route{{
        {40.0, 0.0, 0.0},
        {40.0, 40.0, 0.0},
    }};

    gm::AiConfig base;
    base.use_move_step = true;
    base.move_speed = 2.0;
    base.move_arrive_radius = 1.0;
    base.patrol_after_seconds = 0.01;

    gm::Ai off(base);
    CHECK_EQ(off.attach(things), 2u);
    CHECK(off.set_path(0, route));
    for (int i = 0; i < 200; ++i) {
        off.tick(empty);
    }
    CHECK_EQ(off.agent(0)->state, gm::AiState::patrol);
    CHECK_NEAR(off.agent(0)->position.x, 0.0, 0.0);
    CHECK_NEAR(off.agent(0)->position.y, 0.0, 0.0);
    CHECK_EQ(off.agent(0)->path.index, -1); // route never entered
    CHECK(!off.agent(0)->path.waiting);
    CHECK_EQ(off.stats().move_steps, 0u);

    gm::AiConfig on = base;
    on.use_path = true;
    gm::Ai ai(on);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_path(0, route));
    CHECK_EQ(ai.agent(0)->path.index, -1);
    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    CHECK(ai.agent(0)->position.x > 0.0);
    CHECK(ai.agent(0)->path.index >= 0);
    CHECK(ai.stats().move_steps > 0u);
    // No route on agent 1: patrol falls back to `home`, no path cursor.
    CHECK_EQ(ai.agent(1)->path.index, -1);
    CHECK_NEAR(ai.agent(1)->position.x, 1000.0, 0.0);

    // Validation: unknown id and NaN leave the route untouched.
    const int before = ai.agent(0)->path.index;
    CHECK(before >= 0);
    CHECK(!ai.set_path(99, route));
    const gm::Vec3d nan{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    CHECK(!ai.set_path(0, std::span<const gm::Vec3d>(&nan, 1)));
    CHECK_EQ(ai.agent(0)->path.index, before); // still on the same leg
    // More than 32 points is truncated and restarts the cursor.
    std::vector<gm::Vec3d> long_route(40, gm::Vec3d{0.0, 0.0, 0.0});
    CHECK(ai.set_path(0, long_route));
    CHECK_EQ(ai.agent(0)->path_waypoints.size(),
             static_cast<std::size_t>(gm::kAiPathMaxPoints));
    CHECK_EQ(ai.agent(0)->path.index, -1);
}

// A4 `loop`: the patrol walks all four points and wraps 3 -> 0.  Runs without
// `use_move_step`, because the route is its own opt-in.
void test_tick_path_loop() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;
    const std::array<gm::Vec3d, 4> route{{
        {40.0, 0.0, 0.0},
        {40.0, 40.0, 0.0},
        {0.0, 40.0, 0.0},
        {0.0, 0.0, 0.0},
    }};
    const int expected[] = {0, 1, 2, 3, 0};

    gm::AiConfig config;
    config.use_path = true;
    config.path_mode = gm::AiPathMode::loop;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.patrol_after_seconds = 0.01;
    CHECK(!config.use_move_step);
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_path(0, route));

    std::vector<int> arrived;
    for (int i = 0; i < 4000 && arrived.size() < 5; ++i) {
        ai.tick(empty);
        const gm::AiAgent* agent = ai.agent(0);
        CHECK(agent != nullptr);
        if (agent == nullptr) {
            break;
        }
        if (!agent->path.waiting && agent->path.index >= 0 &&
            (arrived.empty() || arrived.back() != agent->path.index)) {
            arrived.push_back(agent->path.index);
            CHECK(gm::octagonal_distance(
                      agent->position,
                      route[static_cast<std::size_t>(agent->path.index)]) <=
                  1.0 + 1e-9);
        }
    }
    CHECK_EQ(arrived.size(), 5u);
    for (std::size_t i = 0; i < arrived.size() && i < 5; ++i) {
        CHECK_EQ(arrived[i], expected[i]);
    }
    CHECK_EQ(ai.agent(0)->state, gm::AiState::patrol);
    CHECK(ai.stats().move_steps > 0u);
}

// A4 `path`: after the last point the agent stands there for good.
void test_tick_path_stand() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;
    const std::array<gm::Vec3d, 3> route{{
        {10.0, 0.0, 0.0},
        {20.0, 0.0, 0.0},
        {30.0, 0.0, 0.0},
    }};

    gm::AiConfig config;
    config.use_path = true;
    config.path_mode = gm::AiPathMode::path;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.patrol_after_seconds = 0.01;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_path(0, route));
    for (int i = 0; i < 300; ++i) {
        ai.tick(empty);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent == nullptr) {
        return;
    }
    CHECK_EQ(agent->state, gm::AiState::patrol);
    CHECK_EQ(agent->path.index, 2);
    CHECK(!agent->path.waiting);
    CHECK(!agent->path.expired);
    CHECK(gm::octagonal_distance(agent->position, route[2]) <= 1.0 + 1e-9);
    CHECK_NEAR(agent->move_delta.x, 0.0, 0.0);
    const gm::Vec3d rest = agent->position;
    const std::uint64_t steps = ai.stats().move_steps;
    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    CHECK_NEAR(agent->position.x, rest.x, 0.0);
    CHECK_NEAR(agent->position.y, rest.y, 0.0);
    CHECK_NEAR(agent->position.z, rest.z, 0.0);
    CHECK_EQ(agent->path.index, 2);
    CHECK_EQ(ai.stats().move_steps, steps); // terminal `path` never drifts
}

// A4 `expire`: reaching the last point is not the expiry; the next advance is.
// The agent dies once, the terminal latch survives and no second death is
// posted.
void test_tick_path_expire() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;
    const std::array<gm::Vec3d, 2> route{{
        {10.0, 0.0, 0.0},
        {20.0, 0.0, 0.0},
    }};

    gm::AiConfig config;
    config.use_path = true;
    config.path_mode = gm::AiPathMode::expire;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.patrol_after_seconds = 0.01;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.damage(1, 1000.0)); // isolate the expiry counter to agent 0
    CHECK(ai.set_path(0, route));

    int ticks = 0;
    while (ai.agent(0)->state != gm::AiState::dead && ticks < 1000) {
        ai.tick(empty);
        ++ticks;
    }
    CHECK(ticks > 0);
    CHECK(ticks < 1000);
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent == nullptr) {
        return;
    }
    CHECK_EQ(agent->state, gm::AiState::dead);
    CHECK(agent->path.expired);
    CHECK(!agent->path.waiting);
    CHECK_EQ(agent->path.index, 1);
    CHECK(gm::octagonal_distance(agent->position, route[1]) <= 1.0 + 1e-9);
    CHECK_EQ(ai.stats().path_expiries, 1u);
    CHECK(has_event(ai, gm::AiEventType::death, 0));

    const gm::Vec3d rest = agent->position;
    for (int i = 0; i < 100; ++i) {
        ai.tick(empty);
    }
    CHECK_NEAR(agent->position.x, rest.x, 0.0);
    CHECK_NEAR(agent->position.y, rest.y, 0.0);
    CHECK_EQ(ai.stats().path_expiries, 1u); // one-shot `0x4703a0`
}

// A wall clamps the route step to the swept-sphere contact.
void test_tick_path_wall() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld wall = low_wall_between(100.0);
    const std::array<gm::Vec3d, 1> route{{{0.0, 182.0, 0.0}}};

    gm::AiConfig config;
    config.use_path = true;
    config.path_mode = gm::AiPathMode::loop;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_radius = 10.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 0.0;
    config.patrol_after_seconds = 0.01;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_path(0, route));

    for (int i = 0; i < 200; ++i) {
        ai.tick(wall);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent == nullptr) {
        return;
    }
    CHECK(agent->position.y <= 90.0 + 1e-9); // 100 - radius
    CHECK(agent->position.y > 80.0);
    CHECK(agent->path.waiting); // still trying to reach the waypoint
    CHECK_EQ(agent->path.index, 0);
    CHECK(ai.stats().blocked_steps > 0u);
    CHECK_EQ(ai.stats().path_expiries, 0u);
}

// `alert`/`attack` must keep using the A2 aim-point seek: the route is not
// selected while a target is visible, even with `use_path=true`.
void test_tick_path_alert_ignores_route() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;
    const std::array<gm::Vec3d, 2> route{{
        {0.0, -500.0, 0.0},
        {0.0, -600.0, 0.0},
    }};

    gm::AiConfig config;
    config.use_path = true;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 0.0;
    config.patrol_after_seconds = 0.01;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_path(0, route));
    CHECK(ai.set_player({0.0, 300.0, 0.0}));
    ai.tick(empty);
    CHECK_EQ(ai.agent(0)->state, gm::AiState::alert);
    CHECK_EQ(ai.agent(0)->path.index, -1);
    CHECK(!ai.agent(0)->path.waiting);
    CHECK_NEAR(ai.agent(0)->position.y, 2.0, 1e-12); // toward the player
    for (int i = 0; i < 100; ++i) {
        ai.tick(empty);
    }
    CHECK(ai.agent(0)->position.y > 0.0); // route points at -y
    CHECK_EQ(ai.agent(0)->path.index, -1);
    CHECK(!ai.agent(0)->path.waiting);
}

// A2 wiring: `use_move_step` is opt-in, so the default scaffold must not move.
void test_move_step_opt_in() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    // A3: the shipped standoff default is 200 (+0x1cc) and the retreat band is
    // 60, but both only act through `use_move_step`.
    CHECK(!gm::AiConfig{}.use_move_step);
    CHECK_NEAR(gm::AiConfig{}.move_standoff, 200.0, 0.0);
    CHECK_NEAR(gm::AiConfig{}.move_standoff_backoff, 60.0, 0.0);

    gm::Ai legacy;
    CHECK_EQ(legacy.attach(things), 2u);
    CHECK(legacy.set_player({0.0, 300.0, 0.0}));
    for (int i = 0; i < 200; ++i) {
        legacy.tick(empty);
    }
    CHECK_EQ(legacy.agent(0)->state, gm::AiState::alert);
    CHECK_NEAR(legacy.agent(0)->position.x, 0.0, 0.0);
    CHECK_NEAR(legacy.agent(0)->position.y, 0.0, 0.0);
    CHECK_NEAR(legacy.agent(0)->position.z, 0.0, 0.0);
    CHECK_NEAR(legacy.agent(0)->move_delta.y, 0.0, 0.0);
    CHECK_EQ(legacy.stats().move_steps, 0u);
    CHECK_EQ(legacy.stats().blocked_steps, 0u);
    CHECK_EQ(legacy.stats().leash_clamps, 0u);
    CHECK_EQ(legacy.stats().standoff_holds, 0u);
    CHECK_EQ(legacy.stats().retreat_steps, 0u);

    // The same default config with the player *inside* the standoff window:
    // still no movement, no retreat.
    gm::Ai inside;
    CHECK_EQ(inside.attach(things), 2u);
    CHECK(inside.set_player({0.0, 60.0, 0.0}));
    for (int i = 0; i < 200; ++i) {
        inside.tick(empty);
    }
    CHECK_NEAR(inside.agent(0)->position.y, 0.0, 0.0);
    CHECK_EQ(inside.stats().move_steps, 0u);
    CHECK_EQ(inside.stats().retreat_steps, 0u);
}

// alert/attack seek `aim_point`; the step is `speed_per_tick * 91 * dt`.
void test_move_step_seek() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;     // units per tick
    config.move_arrive_radius = 5.0;
    config.move_leash = 0.0;     // clamp disabled
    config.move_standoff = 0.0;  // A3 window disabled: pure seek to the arrival radius
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 300.0, 0.0}));

    const double start_distance = ai.agent(0)->distance;
    ai.tick(empty);
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    double first_distance = 0.0;
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::alert);
        CHECK(agent->has_aim);
        CHECK_NEAR(agent->position.y, 2.0, 1e-12); // 2 units * 91 * (1/91)
        CHECK_NEAR(agent->position.x, 0.0, 0.0);
        CHECK_NEAR(agent->position.z, 0.0, 0.0);
        CHECK_NEAR(agent->move_delta.y, 2.0, 1e-12);
        CHECK_NEAR(agent->heading, std::atan2(1.0, 0.0), 1e-12);
        CHECK_NEAR(start_distance, 0.0, 0.0); // no target before the first tick
        first_distance = agent->distance;
    }
    CHECK_EQ(ai.stats().move_steps, 1u);

    for (int i = 0; i < 199; ++i) {
        ai.tick(empty);
    }
    agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        // 300 units at 2/tick: the octagonal arrival radius stops it at 296.
        CHECK_NEAR(agent->position.y, 296.0, 1e-6);
        CHECK(agent->state == gm::AiState::attack ||
              agent->state == gm::AiState::alert);
        CHECK(agent->distance < first_distance);
    }
    CHECK(ai.stats().move_steps > 0u);
    CHECK_EQ(ai.stats().leash_clamps, 0u);

    // A second run from the same inputs produces the same path (no rand).
    gm::Ai twin(config);
    CHECK_EQ(twin.attach(things), 2u);
    CHECK(twin.set_player({0.0, 300.0, 0.0}));
    for (int i = 0; i < 200; ++i) {
        twin.tick(empty);
    }
    CHECK_NEAR(twin.agent(0)->position.y, ai.agent(0)->position.y, 0.0);
    CHECK_EQ(twin.stats().move_steps, ai.stats().move_steps);
}

// A wall clamps the step to the swept-sphere contact, not through it.
void test_move_step_wall() {
    const gm::Things things = make_synthetic_things();
    // Low wall: the body sphere hits it, the eye ray still sees the target.
    const gm::CollisionWorld wall = low_wall_between(100.0);

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_radius = 10.0;
    config.move_arrive_radius = 5.0;
    config.move_standoff = 0.0; // pure seek: the wall, not the window, stops it
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 300.0, 0.0}));
    for (int i = 0; i < 200; ++i) {
        ai.tick(wall);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK(agent->position.y <= 90.0 + 1e-9); // 100 - radius
        CHECK(agent->position.y > 80.0);
    }
    CHECK(ai.stats().blocked_steps > 0u);
}

// Leash: a step that grows the anchor distance past `move_leash` is cancelled.
void test_move_step_leash() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_arrive_radius = 5.0;
    config.move_leash = 100.0;
    config.move_standoff = 0.0; // isolate the leash clamp
    config.leash = 10000.0; // keep the state-machine leash out of the way
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 500.0, 0.0}));
    CHECK_NEAR(ai.agent(0)->leash_anchor.x, 0.0, 0.0);
    CHECK_NEAR(ai.agent(0)->leash_anchor.y, 0.0, 0.0);
    CHECK_NEAR(ai.agent(0)->leash_distance, 100.0, 0.0);

    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::alert);
        CHECK_NEAR(agent->position.y, 100.0, 1e-9); // frozen on the leash sphere
        CHECK_NEAR(gm::octagonal_distance(agent->leash_anchor, agent->position),
                   100.0, 1e-9);
        CHECK(agent->move_delta.y == 0.0);
    }
    CHECK_EQ(ai.stats().move_steps, 50u); // 100 units at 2/tick
    CHECK(ai.stats().leash_clamps > 0u);
}

// patrol/return seek `home` through the same step; arrival snaps to home.
void test_move_step_return() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_arrive_radius = 5.0;
    config.move_standoff = 0.0; // the return path is unaffected by A3
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    // Reach `alert` first: the state-machine leash only breaks for engaged
    // agents, exactly like the default scaffold.
    CHECK(ai.set_player({0.0, 60.0, 10.0}));
    ai.tick(empty);
    CHECK_EQ(ai.agent(0)->state, gm::AiState::alert);
    CHECK(ai.place(0, {1000.0, 0.0, 0.0}));
    ai.tick(empty);
    CHECK(ai.agent(0)->returning);
    CHECK(ai.agent(0)->position.x < 1000.0);
    CHECK(has_event(ai, gm::AiEventType::leash, 0));
    ai.clear_player();

    int ticks = 0;
    while (ai.agent(0)->returning && ticks < 2000) {
        ai.tick(empty);
        ++ticks;
    }
    CHECK(ticks > 0);
    CHECK(ticks < 2000);
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK(!agent->returning);
        CHECK_NEAR(agent->position.x, 0.0, 0.0);
        CHECK_NEAR(agent->position.y, 0.0, 0.0);
        CHECK_NEAR(agent->position.z, 0.0, 0.0);
        CHECK_EQ(agent->state, gm::AiState::patrol);
        CHECK_NEAR(agent->move_delta.x, 0.0, 0.0);
    }
    CHECK(has_event(ai, gm::AiEventType::arrived, 0));
    CHECK(ai.stats().move_steps > 0u);
}

// Standoff (`+0x1cc`) stops the seek before the arrival radius.
void test_move_step_standoff() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 100.0;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 150.0, 0.0}));

    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    const double frozen = ai.agent(0)->position.y;
    CHECK_NEAR(frozen, 50.0, 1e-12); // 150 - standoff 100
    CHECK_EQ(ai.stats().move_steps, 25u);

    for (int i = 0; i < 100; ++i) {
        ai.tick(empty);
    }
    CHECK_NEAR(ai.agent(0)->position.y, frozen, 0.0);
    CHECK_EQ(ai.stats().move_steps, 25u); // no drifting inside the standoff
}

// A3, far side: outside the standoff the agent closes in and stops on the
// standoff sphere, then fires (the `attack` event) without moving.

// A3 must keep the historical 1.25x exit hysteresis; without it a target
// hovering on the standoff sphere flips alert/attack every tick.
void test_move_step_attack_hysteresis() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;
    gm::AiConfig config;
    config.use_move_step = true;
    // Do not let the agent walk toward the new target: the test must exercise
    // the exit threshold itself, not arrive inside it.
    config.move_speed = 0.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 200.0;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 150.0, 0.0}));
    for (int i = 0; i < 120; ++i) {
        ai.tick(empty);
    }
    CHECK(ai.agent(0)->state == gm::AiState::attack);
    CHECK_NEAR(ai.agent(0)->position.y, 0.0, 1e-12);
    // The state keeps its 1.25x hysteresis, but A3 must not fire outside the
    // standoff window itself.
    const std::size_t attacks_before = ai.stats().attacks;
    CHECK(ai.set_player({0.0, 240.0, 0.0})); // inside 200*1.25, outside 200
    // Longer than attack_period (0.75 s): without the window gate this would
    // produce a `weapon`/`attack` shot before the state leaves at 250.
    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    CHECK(ai.agent(0)->state == gm::AiState::attack);
    CHECK_EQ(ai.stats().attacks, attacks_before);
}

void test_move_step_standoff_far() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 200.0; // shipped default, set explicitly
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 500.0, 0.0}));

    for (int i = 0; i < 300; ++i) {
        ai.tick(empty);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_NEAR(agent->position.y, 300.0, 1e-9); // 500 - standoff 200
        CHECK_NEAR(agent->position.x, 0.0, 0.0);
        CHECK_NEAR(agent->position.z, 0.0, 0.0);
        CHECK_EQ(agent->state, gm::AiState::attack);
        CHECK(agent->visible);
    }
    CHECK_EQ(ai.stats().move_steps, 150u); // 300 units at 2/tick
    CHECK_EQ(ai.stats().retreat_steps, 0u);
    CHECK(ai.stats().attacks > 0u);
    CHECK(has_event(ai, gm::AiEventType::attack, 0));
}

// A3, inside the window but outside the retreat band: stand still and fire.
void test_move_step_standoff_window() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 200.0;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 150.0, 0.0})); // 150 + 60 > 200 -> hold

    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_NEAR(agent->position.y, 0.0, 0.0);
        CHECK_NEAR(agent->move_delta.y, 0.0, 0.0);
        CHECK_EQ(agent->state, gm::AiState::attack);
    }
    CHECK_EQ(ai.stats().move_steps, 0u);
    CHECK_EQ(ai.stats().retreat_steps, 0u);
    CHECK_EQ(ai.stats().standoff_holds, 200u); // every tick held the window
    CHECK(ai.stats().attacks > 0u);
    CHECK(has_event(ai, gm::AiEventType::attack, 0));
}

// A3, deep inside the standoff: `dist + 60 <= standoff` backs off until the
// retreat band is left, then holds.
void test_move_step_standoff_retreat() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld empty;

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 200.0;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 100.0, 0.0})); // 100 + 60 <= 200 -> retreat

    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        // Backs off until the octagonal distance is 142 (100 -> 200 - 60 + 2).
        CHECK_NEAR(agent->position.y, -42.0, 1e-9);
        CHECK_NEAR(agent->position.x, 0.0, 0.0);
        CHECK_NEAR(agent->position.z, 0.0, 0.0);
        CHECK_NEAR(gm::octagonal_distance(agent->position, agent->aim_point), 142.0,
                   1e-9);
    }
    CHECK_EQ(ai.stats().retreat_steps, 21u);
    CHECK_EQ(ai.stats().move_steps, 21u); // one count per committed retreat step
    CHECK_EQ(ai.stats().standoff_holds, 179u);
    CHECK(ai.stats().attacks > 0u);
}

// A3: a retreat never enters geometry; the wall cancels the back-off step.
void test_move_step_standoff_retreat_wall() {
    const gm::Things things = make_synthetic_things();
    const gm::CollisionWorld wall = wall_between(-50.0); // behind the agent

    gm::AiConfig config;
    config.use_move_step = true;
    config.move_speed = 2.0;
    config.move_radius = 10.0;
    config.move_arrive_radius = 1.0;
    config.move_standoff = 200.0;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 100.0, 0.0}));

    for (int i = 0; i < 200; ++i) {
        ai.tick(wall);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        // The body sphere (radius 10) rests on the wall plane y = -50 ...
        CHECK_NEAR(agent->position.y, -40.0, 1e-6);
        CHECK(agent->position.y > -50.0 + 1e-9); // ... never inside it
    }
    CHECK(ai.stats().blocked_steps > 0u);
    CHECK_EQ(ai.stats().retreat_steps, 20u); // 0 -> -40 at 2/tick, then blocked
}

// --- A5: AI weapon table ----------------------------------------------------

gm::Things make_weapon_things(int low = 0, int high = 200, int cooldown = 91) {
    gm::Things things = make_synthetic_things();
    gm::Thing* owner = things.by_id(0);
    CHECK(owner != nullptr);
    if (owner != nullptr) {
        owner->proximity.push_back(
            gm::ProximityEntry{41, low, high, cooldown, {1.0, 2.0, 3.0}});
    }
    return things;
}

const gm::AiEvent* last_event(const gm::Ai& ai, gm::AiEventType type, int thing_id) {
    const gm::AiEvent* found = nullptr;
    for (const gm::AiEvent& event : ai.events()) {
        if (event.type == type && event.thing_id == thing_id) {
            found = &event;
        }
    }
    return found;
}

// Entry validation and the 8-slot clamp; no Ai involved.
void test_weapon_table_load() {
    gm::Thing thing;
    CHECK_EQ(gm::ai_weapon_count(thing), 0);
    gm::AiWeapon weapon;
    CHECK(!gm::ai_weapon_at(thing, 0, weapon));
    CHECK(!weapon.valid());

    // +0x218 counts weapons, but the runtime table has only 8 slots.
    for (int i = 0; i < 9; ++i) {
        thing.proximity.push_back(gm::ProximityEntry{
            static_cast<std::uint32_t>(100 + i), i * 10, 100 + i * 10, i, {}});
    }
    CHECK_EQ(gm::ai_weapon_count(thing), gm::kAiMaxWeapons);
    CHECK(gm::ai_weapon_at(thing, 0, weapon));
    if (weapon.valid()) {
        CHECK_EQ(weapon.count, gm::kAiMaxWeapons);
        CHECK_EQ(weapon.event, 100u);
        CHECK_EQ(weapon.low, 0);
        CHECK_EQ(weapon.high, 100);
        CHECK_EQ(weapon.cooldown, 0);
        CHECK_NEAR(weapon.offset.x, 0.0, 0.0);
    }
    CHECK(!gm::ai_weapon_at(thing, 8, weapon)); // beyond the runtime clamp
    CHECK(!gm::ai_weapon_at(thing, -1, weapon));

    // Inactive entry (event 0), inverted band, negative cooldown, NaN offset.
    thing.proximity = {gm::ProximityEntry{0, 0, 100, 0, {}}};
    CHECK(!gm::ai_weapon_at(thing, 0, weapon));
    thing.proximity = {gm::ProximityEntry{7, 100, 10, 0, {}}};
    CHECK(!gm::ai_weapon_at(thing, 0, weapon));
    thing.proximity = {gm::ProximityEntry{7, 0, 100, -1, {}}};
    CHECK(!gm::ai_weapon_at(thing, 0, weapon));
    thing.proximity = {gm::ProximityEntry{
        7, 0, 100, 0,
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}}};
    CHECK(!gm::ai_weapon_at(thing, 0, weapon));

    // Valid entry with an offset (data only, not used by the event yet).
    thing.proximity = {gm::ProximityEntry{7, 10, 20, 3, {1.0, -2.0, 0.5}}};
    CHECK(gm::ai_weapon_at(thing, 0, weapon));
    CHECK(weapon.valid());
    if (weapon.valid()) {
        CHECK_EQ(weapon.count, 1);
        CHECK_EQ(weapon.event, 7u);
        CHECK_EQ(weapon.low, 10);
        CHECK_EQ(weapon.high, 20);
        CHECK_EQ(weapon.cooldown, 3);
        CHECK_NEAR(weapon.offset.y, -2.0, 0.0);
    }
}

// Inclusive band, scan direction (+0x468) and "no match".
void test_weapon_selection() {
    gm::Thing thing;
    thing.proximity = {
        gm::ProximityEntry{11, 0, 100, 10, {}},
        gm::ProximityEntry{0, 0, 300, 0, {}}, // inactive: never selected
        gm::ProximityEntry{12, 0, 200, 20, {}},
    };
    gm::AiWeapon weapon;
    CHECK(gm::ai_weapon_select(thing, 75.0, false, weapon));
    CHECK_EQ(weapon.event, 11u);
    CHECK(gm::ai_weapon_select(thing, 75.0, true, weapon));
    CHECK_EQ(weapon.event, 12u);
    // The band is inclusive at both ends.
    CHECK(gm::ai_weapon_select(thing, 100.0, false, weapon));
    CHECK_EQ(weapon.event, 11u);
    CHECK(gm::ai_weapon_select(thing, 200.0, true, weapon));
    CHECK_EQ(weapon.event, 12u);
    // Below every band, above every band and non-finite distance.
    CHECK(!gm::ai_weapon_select(thing, -1.0, false, weapon));
    CHECK(!gm::ai_weapon_select(thing, 250.0, false, weapon));
    CHECK(!gm::ai_weapon_select(
        thing, std::numeric_limits<double>::quiet_NaN(), false, weapon));
}

// Default `use_weapon_table=false`: the table is loaded, but no event is posted
// and the attack/state stream is exactly the historical one.
void test_weapon_default_off() {
    const gm::Things things = make_weapon_things();
    gm::Ai ai;
    CHECK_EQ(ai.attach(things), 2u);
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->weapon.count, 1);
        CHECK_EQ(agent->weapon.event, 0u); // no selection yet
        CHECK_NEAR(agent->weapon_cooldown, 0.0, 0.0);
    }

    CHECK(ai.set_player({0.0, 60.0, 10.0}));
    const gm::CollisionWorld empty;
    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    CHECK(ai.stats().attacks > 0u);
    CHECK_EQ(ai.stats().weapon_events, 0u);
    CHECK(!has_event(ai, gm::AiEventType::weapon, 0));
    agent = ai.agent(0);
    if (agent != nullptr) {
        CHECK_EQ(agent->weapon.event, 0u);
        CHECK(agent->weapon_reverse); // untouched
    }
}

// Opt-in: alert before the A3 window posts nothing; the first `attack` tick
// posts one `weapon` event with the table id, and `+0x27c` gates the next one.
void test_weapon_opt_in_window_and_cooldown() {
    const gm::Things things = make_weapon_things(0, 200, 91);
    gm::AiConfig config;
    config.use_weapon_table = true;
    config.use_move_step = true;
    config.move_standoff = 200.0;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 150.0, 10.0}));
    const gm::CollisionWorld empty;

    // 20 ticks of `alert`: the band already contains the target, but the A3
    // window is not `attack` yet, so no weapon event is allowed.
    for (int i = 0; i < 20; ++i) {
        ai.tick(empty);
    }
    const gm::AiAgent* agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::alert);
    }
    CHECK_EQ(ai.stats().weapon_events, 0u);

    for (int i = 0; i < 40; ++i) {
        ai.tick(empty);
    }
    agent = ai.agent(0);
    CHECK(agent != nullptr);
    if (agent != nullptr) {
        CHECK_EQ(agent->state, gm::AiState::attack);
        CHECK_EQ(agent->weapon.count, 1);
        CHECK_EQ(agent->weapon.event, 41u);
        CHECK(!agent->weapon_reverse); // toggled after the first shot
        CHECK(agent->weapon_cooldown > 0.0);
    }
    CHECK_EQ(ai.stats().weapon_events, 1u);
    const gm::AiEvent* event =
        last_event(ai, gm::AiEventType::weapon, 0);
    CHECK(event != nullptr);
    if (event != nullptr) {
        CHECK_EQ(event->event_id, 41u);
        CHECK_EQ(event->target, 0);
        CHECK_EQ(event->target_thing, -1);
        // Octagonal metric: 150 + 0.25 * 10 = 152.5.
        CHECK_NEAR(event->value, 152.5, 1e-9);
        CHECK_NEAR(event->aim.y, 150.0, 0.0);
    }

    // The cooldown is 91 ticks = 1 s; 50 more ticks are not enough ...
    for (int i = 0; i < 50; ++i) {
        ai.tick(empty);
    }
    CHECK_EQ(ai.stats().weapon_events, 1u);
    // ... 45 more pass the 1 s boundary and post exactly one more.
    for (int i = 0; i < 45; ++i) {
        ai.tick(empty);
    }
    CHECK_EQ(ai.stats().weapon_events, 2u);
    // A3 still posts its own `attack` event inside the window.
    CHECK(has_event(ai, gm::AiEventType::attack, 0));
}

// `high` is a hard gate: the attack event still appears, the weapon does not.
void test_weapon_high_gate() {
    const gm::Things things = make_weapon_things(0, 50, 0);
    gm::AiConfig config;
    config.use_weapon_table = true;
    gm::Ai ai(config);
    CHECK_EQ(ai.attach(things), 2u);
    CHECK(ai.set_player({0.0, 60.0, 10.0})); // octagonal 62.5 > high 50
    const gm::CollisionWorld empty;
    for (int i = 0; i < 200; ++i) {
        ai.tick(empty);
    }
    CHECK(ai.stats().attacks > 0u);
    CHECK(has_event(ai, gm::AiEventType::attack, 0));
    CHECK_EQ(ai.stats().weapon_events, 0u);
    CHECK(!has_event(ai, gm::AiEventType::weapon, 0));
}

int main() {
    test_attach_and_transitions();
    test_line_of_sight_and_loss();
    test_leash_and_return();
    test_death_and_negatives();
    test_octagonal_distance();
    test_seek_step();
    test_path_loop();
    test_path_expire();
    test_path_empty();
    test_path_repeat_advance();
    test_tick_path_opt_in();
    test_tick_path_loop();
    test_tick_path_stand();
    test_tick_path_expire();
    test_tick_path_wall();
    test_tick_path_alert_ignores_route();
    test_move_step_opt_in();
    test_move_step_seek();
    test_move_step_wall();
    test_move_step_leash();
    test_move_step_return();
    test_move_step_standoff();
    test_move_step_attack_hysteresis();
    test_move_step_standoff_far();
    test_move_step_standoff_window();
    test_move_step_standoff_retreat();
    test_move_step_standoff_retreat_wall();
    test_weapon_table_load();
    test_weapon_selection();
    test_weapon_default_off();
    test_weapon_opt_in_window_and_cooldown();
    test_weapon_high_gate();
    test_dt_and_player_negatives();
    test_two_runs();
    test_target_history();
    test_vitals_sync();
    test_real_dm1();
    if (g_failures != 0) {
        std::fprintf(stderr, "ai: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ai: ok\n");
    return 0;
}
