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

gm::Things make_synthetic_things() {
    std::vector<std::uint8_t> code;
    add_thing(code, 0, 0, 0, 0);        // agent 0, home (0,0,0)
    add_thing(code, 0, 1000, 0, 0);     // agent 1, home (1000,0,0)
    add_thing_at_source(code, 0, -1);   // no absolute position -> skipped
    code.push_back(0xFF);

    std::vector<Row> rows{{0x3FF, 42, 0}};
    const std::vector<TypeSpec> types{{"TEST_THING", 0}};
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
}

} // namespace

int main() {
    test_attach_and_transitions();
    test_line_of_sight_and_loss();
    test_leash_and_return();
    test_death_and_negatives();
    test_dt_and_player_negatives();
    test_two_runs();
    test_real_dm1();
    if (g_failures != 0) {
        std::fprintf(stderr, "ai: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ai: ok\n");
    return 0;
}
