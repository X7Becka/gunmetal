// Thing runtime (docs/THINGS_CPP.md).
//
// Synthetic part: hand-built SBT/SCRIPT byte code with top-level things, the
// absolute ADD_THING / ADD_THING_EX variants, ADD_THING_AT_SOURCE, GENERATE
// child sub-scripts, a GENERATE cycle, nested IF/ELSE, RANDOM forking, a
// 0xFFFFFFFF type, an out-of-range type, a missing SOB model, duplicate entity
// rows, a 70-deep IF nest, truncated code and an unknown opcode.
//
// Real part: dm1.sbt / level0.sbt from the shipped game.  `--full` sweeps all
// 36 levels and pins the corpus totals 4 008 top-level / 7 563 recursive /
// 7 617 executed / 391 starts, 2 896 valid THING_TYPES model refs and 13
// 0xFFFFFFFF static things.  The per-level executed numbers are the
// `--contexts all --random-unknown` goldens from tools/re/nested_things.py.
#include "world/things.h"

#include "game/events.h"
#include "res/sbt.h"
#include "res/vm_exec.h"
#include "game_path.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <optional>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
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

#define CHECK_STR(a, b)                                                                      \
    do {                                                                                     \
        const std::string va = (a);                                                          \
        const std::string vb = (b);                                                          \
        if (va != vb) {                                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%s vs %s)\n", __FILE__, __LINE__,    \
                         #a, #b, va.c_str(), vb.c_str());                                     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

void put_u32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    std::memcpy(data.data() + offset, &value, 4);
}

void put_f64(std::vector<std::uint8_t>& data, std::size_t offset, double value) {
    std::memcpy(data.data() + offset, &value, 8);
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
    put_f64(code, at, value);
}

// --- synthetic SBT ----------------------------------------------------------

struct Row {
    std::uint32_t flags = 0x3FF;
    int id = 0;
    std::uint32_t offset = 0;
};

struct TypeSpec {
    const char* name;
    std::uint32_t model_index;
    int expire_ticks = 0;       ///< THING_TYPES +0x1e8
    std::uint32_t expire_event = 0; ///< THING_TYPES +0x1ec
    int timer_period = 0;       ///< THING_TYPES +0x210 -> instance +0x3ec
    int timer_step = 0;         ///< THING_TYPES +0x214 -> instance +0x3f0
};

gm::SbtLevel make_level(const std::vector<std::uint8_t>& code,
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
        put_u32(types_record.payload, base + 0x1E8,
                static_cast<std::uint32_t>(types[i].expire_ticks));
        put_u32(types_record.payload, base + 0x1EC, types[i].expire_event);
        put_u32(types_record.payload, base + 0x210,
                static_cast<std::uint32_t>(types[i].timer_period));
        put_u32(types_record.payload, base + 0x214,
                static_cast<std::uint32_t>(types[i].timer_step));
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

gm::SbtLevel load_real(const fs::path& path) {
    const std::vector<std::uint8_t> bytes = read_file(path);
    std::string error;
    const auto level = gm::parse_sbt(bytes, error);
    if (!level) {
        std::fprintf(stderr, "things: parse_sbt(%s): %s\n", path.c_str(), error.c_str());
        return {};
    }
    return *level;
}

// Condition encoders (ids/signatures from gm/sbtvm.py).
void add_thing(std::vector<std::uint8_t>& code, std::uint32_t type, int x, int y, int z,
               double a, double b, double c) {
    code.push_back(0xC9);
    code.push_back(1); // ADD_THING GLLLAAA
    append_u32(code, type);
    append_i32(code, x);
    append_i32(code, y);
    append_i32(code, z);
    append_f64(code, a);
    append_f64(code, b);
    append_f64(code, c);
}

void add_thing_ex(std::vector<std::uint8_t>& code, std::uint32_t type, int x, int y, int z,
                  int owner) {
    code.push_back(0xC9);
    code.push_back(2); // ADD_THING_EX LLLGLLLAAAL
    append_i32(code, 0);
    append_i32(code, 0);
    append_i32(code, owner);
    append_u32(code, type);
    append_i32(code, x);
    append_i32(code, y);
    append_i32(code, z);
    append_f64(code, 1.0);
    append_f64(code, 2.0);
    append_f64(code, 3.0);
    append_i32(code, 0);
}

void add_thing_at_source(std::vector<std::uint8_t>& code, std::uint32_t type, int owner) {
    code.push_back(0xC9);
    code.push_back(3); // ADD_THING_AT_SOURCE LGAAA
    append_i32(code, owner);
    append_u32(code, type);
    append_f64(code, 4.0);
    append_f64(code, 5.0);
    append_f64(code, 6.0);
}

void add_player_start(std::vector<std::uint8_t>& code, int slot, int x, int y, int z) {
    code.push_back(169); // SET_PLAYER_START LLLLAAA
    append_i32(code, slot);
    append_i32(code, x);
    append_i32(code, y);
    append_i32(code, z);
    append_f64(code, 10.0);
    append_f64(code, 20.0);
    append_f64(code, 30.0);
}

void generate(std::vector<std::uint8_t>& code, int id) {
    code.push_back(2); // GENERATE E
    append_u32(code, static_cast<std::uint32_t>(id));
}

/// IF RANDOM { then + ELSE } else_code END.
void append_random_if(std::vector<std::uint8_t>& code,
                      const std::function<void(std::vector<std::uint8_t>&)>& then_code,
                      const std::function<void(std::vector<std::uint8_t>&)>& else_code) {
    code.push_back(0xC7);
    code.push_back(24); // RANDOM D
    append_f64(code, 0.5);
    const std::size_t size_at = code.size();
    append_u32(code, 0);
    const std::size_t body_start = code.size();
    then_code(code);
    const std::size_t else_at = code.size();
    code.push_back(0xC8);
    append_u32(code, 0);
    const std::size_t then_size = code.size() - body_start;
    put_u32(code, size_at, static_cast<std::uint32_t>(4 + then_size));
    const std::size_t else_start = code.size();
    else_code(code);
    const std::size_t end = code.size();
    put_u32(code, else_at + 1, static_cast<std::uint32_t>(end - (else_at + 1)));
    code.push_back(0xFF);
}

// A block with the interesting top-level cases: absolute, source-relative,
// ADD_THING_EX, 0xffffffff, invalid type, missing model and a spawn.
std::vector<std::uint8_t> synthetic_code(std::vector<Row>* rows) {
    std::vector<std::uint8_t> code;

    const std::uint32_t a = static_cast<std::uint32_t>(code.size());
    add_thing(code, 0, 1, 2, 3, 0.25, 0.5, 0.75);
    add_thing_at_source(code, 1, 7);
    add_player_start(code, 0, 10, 20, 30);
    code.push_back(0xFF);

    const std::uint32_t b = static_cast<std::uint32_t>(code.size());
    append_random_if(
        code,
        [](std::vector<std::uint8_t>& out) { add_thing(out, 3, 4, 5, 6, 0, 0, 0); },
        [](std::vector<std::uint8_t>& out) { add_thing(out, 0, 7, 8, 9, 0, 0, 0); });

    const std::uint32_t c = static_cast<std::uint32_t>(code.size());
    add_thing(code, 2, 0, 0, 0, 0, 0, 0);                    // missing SOB model
    add_thing(code, 0xFFFFFFFFu, 0, 0, 0, 0, 0, 0);          // engine "none"
    code.push_back(0xFF);

    const std::uint32_t d = static_cast<std::uint32_t>(code.size());
    add_thing(code, 99, 0, 0, 0, 0, 0, 0); // out of range type
    code.push_back(0xFF);

    const std::uint32_t e = static_cast<std::uint32_t>(code.size());
    add_thing_ex(code, 1, 10, 11, 12, 9);
    code.push_back(0xFF);

    rows->push_back({0x3FF, 10, a});
    rows->push_back({0x2FF, 20, b});
    rows->push_back({0x1FF, 30, c});
    rows->push_back({0x0FF, 40, d});
    rows->push_back({0x00F, 50, e});
    return code;
}

const std::vector<TypeSpec> kSyntheticTypes = {
    {"THING_A", 0}, {"THING_B", 1}, {"THING_C", 99}, {"THING_D", 0}};
const std::vector<std::string> kSyntheticModels = {"MODEL_A", "MODEL_B"};

gm::Things load_synthetic(const std::vector<std::uint8_t>& code,
                          const std::vector<Row>& rows,
                          const gm::ThingsOptions& options = {}) {
    const gm::SbtLevel level = make_level(code, rows, kSyntheticTypes, kSyntheticModels);
    const gm::VmState vm = gm::vm_state_from_level(level);
    return gm::Things::load(level, vm, options);
}

gm::ThingsOptions single_context(bool follow_generate = false) {
    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    options.follow_generate = follow_generate;
    return options;
}

// --- tests ------------------------------------------------------------------

void test_synthetic() {
    std::vector<Row> rows;
    const std::vector<std::uint8_t> code = synthetic_code(&rows);
    const gm::SbtLevel level = make_level(code, rows, kSyntheticTypes, kSyntheticModels);
    const gm::VmState vm = gm::vm_state_from_level(level);
    const gm::Things things = gm::Things::load(level, vm);
    CHECK(things.ok());
    const gm::ThingStats& stats = things.stats();
    CHECK_EQ(stats.top_level, 7u);
    CHECK_EQ(stats.recursive, 8u);
    CHECK_EQ(stats.nested, 1u);
    CHECK_EQ(stats.executed, 8u);
    CHECK_EQ(stats.executed_top_level, 7u);
    CHECK_EQ(stats.executed_nested, 1u);
    CHECK_EQ(stats.spawns_static, 1u);
    CHECK_EQ(stats.spawns_executed, 1u);
    CHECK_EQ(stats.thing_types, 4u);
    CHECK_EQ(stats.valid_model_refs, 3u);
    CHECK_EQ(stats.missing_model_refs, 1u);
    CHECK_EQ(stats.null_type_static, 1u);
    CHECK_EQ(stats.null_type_things, 1u);
    CHECK_EQ(stats.invalid_type_things, 1u);
    CHECK_EQ(stats.missing_model_things, 1u);
    CHECK_EQ(stats.duplicate_ids, 0u);
    CHECK_EQ(stats.max_depth, 1u);
    CHECK_EQ(stats.blocks, 5u);
    CHECK_EQ(stats.reasons.count("unknown-opcode"), 0u);

    CHECK_EQ(things.things().size(), 8u);
    for (std::size_t i = 0; i < things.things().size(); ++i) {
        CHECK_EQ(things.things()[i].id, static_cast<int>(i));
        CHECK(things.things()[i].executed);
        CHECK(things.by_id(static_cast<int>(i)) == &things.things()[i]);
    }
    CHECK(things.by_id(-1) == nullptr);
    CHECK(things.by_id(8) == nullptr);

    // The absolute ADD_THING: type/model, position, angles, parent/script/flags.
    const gm::Thing* first = things.by_id(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_EQ(first->type, 0);
        CHECK_STR(first->type_name, "THING_A");
        CHECK_STR(first->model, "MODEL_A");
        CHECK_EQ(first->model_index, 0);
        CHECK(first->absolute());
        CHECK_EQ((*first->position)[0], 1);
        CHECK_EQ((*first->position)[1], 2);
        CHECK_EQ((*first->position)[2], 3);
        CHECK_EQ(first->angles.size(), 3u);
        CHECK_EQ(first->parent, 10);
        CHECK_EQ(first->flags, 0x3FFu);
        CHECK_EQ(first->script, rows[0].offset);
        CHECK_EQ(first->depth, 0);
        CHECK_EQ(first->occ, 0);
    }

    // AT_SOURCE keeps no world position; ADD_THING_EX is absolute.
    bool saw_source = false;
    bool saw_ex = false;
    bool saw_null = false;
    bool saw_invalid = false;
    bool saw_missing_model = false;
    bool saw_nested = false;
    for (const gm::Thing& thing : things.things()) {
        if (thing.kind == "ADD_THING_AT_SOURCE") {
            saw_source = true;
            CHECK(!thing.absolute());
            CHECK_EQ(thing.owner, 7);
            CHECK_EQ(thing.type, 1);
            CHECK_STR(thing.model, "MODEL_B");
        } else if (thing.kind == "ADD_THING_EX") {
            saw_ex = true;
            CHECK_EQ(thing.type, 1);
            CHECK_EQ(thing.owner, 9);
            CHECK(thing.absolute());
            CHECK_EQ((*thing.position)[0], 10);
            CHECK_EQ(thing.angles.size(), 3u);
            CHECK_EQ(thing.parent, 50);
        } else if (thing.type < 0) {
            saw_null = true;
            CHECK_EQ(thing.type, -1);
            CHECK(thing.type_name.empty());
            CHECK(thing.model.empty());
            CHECK(thing.absolute());
        } else if (thing.type == 99) {
            saw_invalid = true;
            CHECK(thing.type_name.empty());
            CHECK(thing.model.empty());
        } else if (thing.type == 2) {
            saw_missing_model = true;
            CHECK_STR(thing.type_name, "THING_C");
            CHECK(thing.model.empty());
            CHECK_EQ(thing.model_index, -1);
        }
        if (thing.depth == 1) {
            saw_nested = true;
            CHECK_EQ(thing.type, 3);
            CHECK_EQ(thing.parent, 20);
            CHECK_EQ(thing.depth, 1);
            CHECK_EQ(thing.path.size(), 1u);
            if (!thing.path.empty()) {
                CHECK_STR(thing.path[0], "IF RANDOM");
            }
        }
    }
    CHECK(saw_source);
    CHECK(saw_ex);
    CHECK(saw_null);
    CHECK(saw_invalid);
    CHECK(saw_missing_model);
    CHECK(saw_nested);

    // Static recursive has both IF branches; static top-level only the else
    // code (the compiler emits it as ordinary code after the IF).
    bool top_has_then = false;
    for (const gm::Thing& thing : things.static_top_level()) {
        if (thing.type == 3) {
            top_has_then = true;
        }
    }
    CHECK(!top_has_then);

    // children() is keyed by parent entity id.
    CHECK_EQ(things.children(10).size(), 2u);
    CHECK_EQ(things.child_ids(20).size(), 2u);
    CHECK(things.children(999).empty());

    // Minimal tick: age advances only for living things; kill/revive work.
    gm::Things ticking = gm::Things::load(level, vm);
    ticking.tick(91);
    CHECK_EQ(ticking.clock(), 91u);
    CHECK_EQ(ticking.stats().ticks, 91u);
    for (const gm::Thing& thing : ticking.things()) {
        CHECK_EQ(thing.age, 91u);
        CHECK(thing.alive);
    }
    CHECK(ticking.kill(0));
    CHECK(!ticking.by_id(0)->alive);
    ticking.tick(5);
    CHECK_EQ(ticking.by_id(0)->age, 91u);
    CHECK_EQ(ticking.by_id(1)->age, 96u);
    CHECK(ticking.revive(0));
    CHECK(ticking.by_id(0)->alive);
    ticking.tick(1);
    CHECK_EQ(ticking.by_id(0)->age, 92u);
    CHECK(!ticking.kill(999));
    CHECK(!ticking.revive(999));

    // Two loads are deterministic.
    const gm::Things again = gm::Things::load(level, vm);
    CHECK_EQ(again.stats().executed, things.stats().executed);
    CHECK_EQ(again.things().size(), things.things().size());
    for (std::size_t i = 0; i < things.things().size(); ++i) {
        CHECK_EQ(again.things()[i].site, things.things()[i].site);
        CHECK_EQ(again.things()[i].type, things.things()[i].type);
        CHECK_EQ(again.things()[i].parent, things.things()[i].parent);
    }
}

void test_generate_and_cycle() {
    // Child sub-script: block 1 GENERATEs entity 2, whose block builds a thing.
    std::vector<std::uint8_t> code;
    generate(code, 2);
    code.push_back(0xFF);
    const std::uint32_t child_offset = static_cast<std::uint32_t>(code.size());
    add_thing(code, 0, 11, 12, 13, 0, 0, 0);
    code.push_back(0xFF);
    std::vector<Row> rows = {{0x3FF, 1, 0}, {0x3FF, 2, child_offset}};
    gm::Things child = load_synthetic(code, rows, single_context(true));
    CHECK(child.ok());
    CHECK_EQ(child.stats().executed, 1u);
    CHECK_EQ(child.things().size(), 1u);
    if (!child.things().empty()) {
        CHECK_EQ(child.things()[0].parent, 2);
        CHECK_EQ(child.things()[0].script, child_offset);
        CHECK_EQ((*child.things()[0].position)[0], 11);
    }
    // Every entity block is still started on its own, exactly like
    // gm.scene.execute_things, so the child block builds the same site once.
    gm::Things plain = load_synthetic(code, rows, single_context(false));
    CHECK_EQ(plain.stats().top_level, 1u);
    CHECK_EQ(plain.stats().executed, 1u);
    CHECK_EQ(plain.things()[0].parent, 2);

    // GENERATE self-cycle stops with the VM's recursion reason, not a hang.
    // The row must not be a one-shot: 0x200 is cleared before the call
    // (0x45fa9e), which turns a self-GENERATE into a no-op (see
    // test_vm_exec's test_entity_rows).
    std::vector<std::uint8_t> cycle_code;
    generate(cycle_code, 1);
    cycle_code.push_back(0xFF);
    std::vector<Row> cycle_rows = {{0x1FF, 1, 0}};
    gm::Things cycle = load_synthetic(cycle_code, cycle_rows, single_context(true));
    CHECK(cycle.stats().reasons.count("recursion") >= 1u);
    CHECK_EQ(cycle.stats().executed, 0u);
}

void test_negatives() {
    // Truncated EVAL condition (no condition id).
    {
        std::vector<std::uint8_t> code = {0xC9};
        std::vector<Row> rows = {{0x3FF, 1, 0}};
        gm::Things things = load_synthetic(code, rows);
        CHECK(!things.ok());
        CHECK(things.stats().decode_error);
        CHECK(things.things().empty());
    }
    // Unknown opcode.
    {
        std::vector<std::uint8_t> code = {0xFE, 0xFF};
        std::vector<Row> rows = {{0x3FF, 1, 0}};
        gm::Things things = load_synthetic(code, rows);
        CHECK(!things.ok());
        CHECK(things.stats().decode_error);
    }
    // 70 nested IFs exceed kThingMaxDepth.
    {
        std::vector<std::uint8_t> code;
        std::function<void(int)> nest = [&](int depth) {
            if (depth == 0) {
                add_thing(code, 0, 1, 2, 3, 0, 0, 0);
                return;
            }
            code.push_back(0xC7);
            code.push_back(24);
            append_f64(code, 0.5);
            const std::size_t size_at = code.size();
            append_u32(code, 0);
            const std::size_t body_start = code.size();
            nest(depth - 1);
            put_u32(code, size_at, static_cast<std::uint32_t>(4 + code.size() - body_start));
        };
        nest(70);
        code.push_back(0xFF);
        std::vector<Row> rows = {{0x3FF, 1, 0}};
        gm::ThingsOptions options = single_context();
        options.execute = false;
        gm::Things things = load_synthetic(code, rows, options);
        CHECK(!things.ok());
        CHECK(things.stats().nesting_limit);
        CHECK_EQ(things.stats().recursive, 0u);
    }
    // Duplicate entity rows: top-level keeps both (historical behaviour), the
    // recursive and executed views deduplicate by site; runtime ids stay unique.
    {
        std::vector<Row> rows;
        const std::vector<std::uint8_t> code = synthetic_code(&rows);
        std::vector<Row> duplicate = {rows[0], rows[0]};
        gm::Things things = load_synthetic(code, duplicate);
        CHECK_EQ(things.stats().top_level, 4u);
        CHECK_EQ(things.stats().recursive, 2u);
        CHECK_EQ(things.stats().executed, 2u);
        CHECK(things.stats().static_site_duplicates >= 2u);
        CHECK_EQ(things.stats().duplicate_ids, 0u);
        std::set<int> ids;
        for (const gm::Thing& thing : things.things()) {
            CHECK(ids.insert(thing.id).second);
        }
        CHECK_EQ(things.children(10).size(), 2u);
    }
    // Missing SCRIPT record.
    {
        gm::SbtLevel level;
        gm::VmState vm;
        gm::Things things = gm::Things::load(level, vm);
        CHECK(!things.ok());
    }
}

void test_real_level(const fs::path& path, int top, int recursive, int executed,
                     int spawns, const char* name, const gm::ThingsOptions& options) {
    const gm::SbtLevel level = load_real(path);
    const gm::VmState vm = gm::vm_state_from_level(level);
    const gm::Things things = gm::Things::load(level, vm, options);
    if (!things.ok()) {
        std::fprintf(stderr, "things: %s: %s\n", name, things.error().c_str());
        ++g_failures;
        return;
    }
    const gm::ThingStats& stats = things.stats();
    if (stats.top_level != static_cast<std::size_t>(top) ||
        stats.recursive != static_cast<std::size_t>(recursive) ||
        stats.executed != static_cast<std::size_t>(executed) ||
        stats.spawns_executed != static_cast<std::size_t>(spawns)) {
        std::fprintf(stderr,
                     "things: %s counts top/rec/exec/spawn %zu/%zu/%zu/%zu, expected "
                     "%d/%d/%d/%d\n",
                     name, stats.top_level, stats.recursive, stats.executed,
                     stats.spawns_executed, top, recursive, executed, spawns);
        ++g_failures;
    }
    CHECK_EQ(stats.spawns_static, stats.spawns_executed);
    CHECK_EQ(stats.static_site_duplicates, 0u);
    CHECK_EQ(stats.duplicate_ids, 0u);
    CHECK_EQ(stats.executed, things.things().size());
    CHECK_EQ(stats.executed_top_level + stats.executed_nested, stats.executed);
    CHECK_EQ(stats.valid_model_refs, stats.thing_types);
    CHECK_EQ(stats.missing_model_refs, 0u);
    CHECK(stats.max_depth <= gm::kThingMaxDepth);
    for (const auto& [reason, count] : stats.reasons) {
        (void)count;
        if (reason != "end" && reason != "return" && reason != "program-exit" &&
            reason != "level-transition") {
            std::fprintf(stderr, "things: %s: non-terminal reason %s\n", name,
                         reason.c_str());
            ++g_failures;
        }
    }
}

void test_type_events() {
    if (!fs::exists(kGame / "level0.sbt")) {
        return;
    }
    const gm::SbtLevel level = load_real(kGame / "level0.sbt");
    const auto rocket = gm::thing_type_events(level, 31);
    CHECK(rocket.has_value());
    if (rocket) {
        CHECK(rocket->speed == 18.0); // +0x10, units per tick
        CHECK_EQ(rocket->expire_ticks, 728); // +0x1e8
        CHECK_EQ(rocket->collide_event, static_cast<std::uint32_t>(-30103));
        CHECK_EQ(rocket->expire_event, static_cast<std::uint32_t>(-30103));
        CHECK_EQ(rocket->create_event, static_cast<std::uint32_t>(-30104));
        CHECK_EQ(rocket->damaged_event, 0u);
        CHECK_NEAR(rocket->gravity, 0.0, 0.0);
        CHECK_NEAR(rocket->damping, 0.8, 0.0);
        CHECK_EQ(rocket->collision_shape, 2);
        CHECK_NEAR(rocket->radius, 10.0, 0.0); // params[4], unused for shape 2
        CHECK_NEAR(rocket->box[0], 4.0, 0.0);
        CHECK_NEAR(rocket->box[1], 10.0, 0.0);
        CHECK_NEAR(rocket->box[2], 4.0, 0.0);
        CHECK_EQ(rocket->proximity.size(), 0U);
    }
    const auto grenade = gm::thing_type_events(level, 28);
    CHECK(grenade.has_value());
    if (grenade) {
        CHECK_EQ(grenade->expire_ticks, 364);
        CHECK_EQ(grenade->collide_event, static_cast<std::uint32_t>(-30094));
        CHECK_EQ(grenade->expire_event, static_cast<std::uint32_t>(-30093));
        CHECK_EQ(grenade->create_event, 0u);
        CHECK_NEAR(grenade->gravity, 1.0, 0.0);
        CHECK_NEAR(grenade->damping, 0.8, 0.0);
        CHECK_EQ(grenade->collision_shape, 1);
        CHECK_NEAR(grenade->radius, 10.0, 0.0);
        CHECK_EQ(grenade->proximity.size(), 1U);
        if (grenade->proximity.size() == 1) {
            CHECK_EQ(grenade->proximity[0].event,
                     static_cast<std::uint32_t>(-30095));
            CHECK_EQ(grenade->proximity[0].high, 20);
        }
    }
    CHECK(!gm::thing_type_events(level, -1).has_value());
    CHECK(!gm::thing_type_events(level, 100000).has_value());
}

void test_real_dm1() {
    if (!fs::exists(kGame / "dm1.sbt")) {
        std::printf("things: game not installed, real levels skipped\n");
        return;
    }
    // The quick run uses one context; dm1/level0 have the same counts in the
    // eight-context matrix, which `--full` verifies for the whole corpus.
    const gm::ThingsOptions quick = single_context();
    test_real_level(kGame / "dm1.sbt", 38, 138, 141, 13, "dm1", quick);
    test_real_level(kGame / "level0.sbt", 29, 119, 121, 8, "level0", quick);

    const gm::SbtLevel level = load_real(kGame / "dm1.sbt");
    const gm::VmState vm = gm::vm_state_from_level(level);
    const gm::Things things = gm::Things::load(level, vm, quick);
    CHECK_EQ(things.stats().thing_types, 59u);
    CHECK_EQ(things.stats().valid_model_refs, 59u);
    CHECK_EQ(things.stats().null_type_static, 13u);
    CHECK_EQ(things.stats().null_type_things, 13u);
    CHECK_EQ(things.stats().executed_top_level, 41u);
    CHECK_EQ(things.stats().max_depth, 2u);

    // Golden first runtime instance (Python: dm1 executed[0]).
    const gm::Thing* first = things.by_id(0);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK_STR(first->kind, "ADD_THING_RELATIVE");
        CHECK_EQ(first->type, 0);
        CHECK_STR(first->type_name, "bgchunk1");
        CHECK_STR(first->model, "BIGCHUNK");
        CHECK_EQ(first->parent, -30070);
        CHECK_EQ(first->site, 11029u);
        CHECK_EQ(first->depth, 1);
        CHECK(!first->absolute());
        CHECK_EQ(first->angles.size(), 7u);
        // THING_TYPES fields copied into the runtime instance.
        CHECK_EQ(first->movement_type, 6);
        CHECK(first->search_radius == 6000.0);
        CHECK_EQ(first->target_zone_threshold, 200);
        CHECK_EQ(first->target_history_offset, 0);
        CHECK_EQ(first->max_health, 2000);
        CHECK_EQ(first->health, 2000);
        CHECK_EQ(first->hits_left, 0);
        CHECK_EQ(first->shields[0], 0);
        CHECK_EQ(first->timeout_event, 0u);
        CHECK_EQ(first->collide_event, 0u);
        CHECK_EQ(first->create_event, 0xffff8a8bu); // -30069
        CHECK_EQ(first->expire_event, 0xffff8a89u); // -30071
        CHECK_EQ(first->damaged_event, 0);
        CHECK_EQ(first->destroy_event, 0xffff8a89u);
    }
    // Golden last runtime instance (Python: dm1 executed[-1]).
    const gm::Thing* last = things.by_id(static_cast<int>(things.things().size()) - 1);
    CHECK(last != nullptr);
    if (last != nullptr) {
        CHECK_EQ(last->type, 58);
        CHECK_STR(last->type_name, "OILDRUM");
        CHECK_STR(last->model, "OILDRUM");
        CHECK(last->absolute());
        CHECK_EQ((*last->position)[0], 5688);
        CHECK_EQ((*last->position)[1], 216);
        CHECK_EQ((*last->position)[2], 264);
        CHECK_EQ(last->parent, -4);
        CHECK_EQ(last->site, 67357u);
    }
    CHECK(!things.children(-30070).empty());
}

struct LevelGolden {
    const char* name;
    int top;
    int recursive;
    int executed;
    int spawns;
};

constexpr LevelGolden kLevels[] = {
    {"dm1", 38, 138, 141, 13},
    {"dm2", 33, 123, 125, 9},
    {"dm3", 41, 131, 133, 9},
    {"dm4", 40, 130, 132, 10},
    {"level0", 29, 119, 121, 8},
    {"level00", 28, 118, 120, 8},
    {"level000", 28, 118, 120, 8},
    {"level0a", 28, 118, 120, 8},
    {"level0b", 27, 117, 119, 1},
    {"level1", 59, 155, 157, 16},
    {"level10", 64, 169, 171, 10},
    {"level11", 188, 292, 294, 9},
    {"level12", 141, 244, 248, 9},
    {"level13", 171, 267, 268, 9},
    {"level14", 149, 249, 253, 9},
    {"level15", 96, 190, 191, 9},
    {"level16", 239, 335, 337, 16},
    {"level17", 189, 299, 292, 10},
    {"level18", 278, 410, 397, 10},
    {"level19", 127, 237, 242, 16},
    {"level2", 55, 148, 152, 16},
    {"level20", 102, 197, 199, 11},
    {"level21", 159, 258, 260, 16},
    {"level22", 212, 320, 324, 9},
    {"level23", 236, 337, 339, 16},
    {"level24", 165, 265, 267, 16},
    {"level3", 139, 248, 247, 16},
    {"level4", 88, 181, 183, 10},
    {"level5", 109, 204, 206, 17},
    {"level6", 98, 195, 199, 10},
    {"level7", 134, 240, 244, 9},
    {"level8", 126, 226, 230, 9},
    {"level9", 192, 287, 289, 9},
    {"secrt1", 74, 175, 176, 12},
    {"secrt2", 90, 197, 193, 9},
    {"temple", 36, 126, 128, 9},
};

void test_corpus() {
    if (!fs::exists(kGame)) {
        std::printf("things: game not installed, corpus sweep skipped\n");
        return;
    }
    std::size_t top = 0;
    std::size_t recursive = 0;
    std::size_t executed = 0;
    std::size_t spawns = 0;
    std::size_t types = 0;
    std::size_t valid_models = 0;
    std::size_t missing_models = 0;
    std::size_t null_static = 0;
    std::size_t null_runtime = 0;
    std::size_t levels = 0;
    for (const LevelGolden& golden : kLevels) {
        // resolve by content: the shipped level1/level2 may be swapped
        const auto resolved = gmtest::resolve_game_file(
            kGame, std::string(golden.name) + ".sbt");
        if (!resolved) {
            continue;
        }
        const fs::path path = *resolved;
        ++levels;
        const gm::ThingsOptions matrix;
        test_real_level(path, golden.top, golden.recursive, golden.executed,
                        golden.spawns, golden.name, matrix);
        const gm::SbtLevel level = load_real(path);
        const gm::VmState vm = gm::vm_state_from_level(level);
        const gm::Things things = gm::Things::load(level, vm);
        top += things.stats().top_level;
        recursive += things.stats().recursive;
        executed += things.stats().executed;
        spawns += things.stats().spawns_executed;
        types += things.stats().thing_types;
        valid_models += things.stats().valid_model_refs;
        missing_models += things.stats().missing_model_refs;
        null_static += things.stats().null_type_static;
        null_runtime += things.stats().null_type_things;
    }
    CHECK_EQ(levels, 36u);
    CHECK_EQ(top, 4008u);
    CHECK_EQ(recursive, 7563u);
    CHECK_EQ(executed, 7617u);
    CHECK_EQ(spawns, 391u);
    CHECK_EQ(types, 2896u);
    CHECK_EQ(valid_models, 2896u);
    CHECK_EQ(missing_models, 0u);
    CHECK_EQ(null_static, 13u);
    CHECK_EQ(null_runtime, 13u);
    std::printf("things: corpus %zu/%zu/%zu, spawns %zu, types %zu\n", top, recursive,
                executed, spawns, types);
}

void test_expire_lifetime() {
    std::vector<std::uint8_t> code;
    add_thing(code, 0, 0, 0, 0, 0.0, 0.0, 0.0);
    code.push_back(0xFF);
    const std::vector<Row> rows{{0x3FF, 42, 0}};
    const std::vector<TypeSpec> types{{"EXPIRING", 0, 3, 900}};
    const gm::SbtLevel level = make_level(code, rows, types, {"M"});
    const gm::VmState vm = gm::vm_state_from_level(level);
    gm::ThingsOptions options;
    options.contexts = {{0, 0}};
    gm::Things things = gm::Things::load(level, vm, options);
    CHECK(things.ok());
    const gm::Thing* thing = things.things().empty() ? nullptr : &things.things()[0];
    CHECK(thing != nullptr);
    if (thing == nullptr) {
        return;
    }
    CHECK_EQ(thing->expire_ticks, 3);

    gm::EventQueue queue;
    things.tick(2, &queue);
    CHECK(!things.things()[0].expired);
    CHECK(queue.empty());

    things.tick(1, &queue);
    CHECK(things.things()[0].expired);
    CHECK(things.things()[0].alive); // +0x1e8 posts 0x4703a0, it does not kill
    const gm::EventParseResult parsed = queue.drain();
    CHECK_EQ(parsed.events.size(), 1U);
    if (parsed.events.size() == 1) {
        CHECK_EQ(parsed.events[0].id, 900);
    }

    // The expire event is posted exactly once.
    things.tick(1, &queue);
    CHECK(queue.empty());
}


void test_spawn_remove() {
    gm::Things things;
    gm::Thing prototype;
    prototype.id = -1;
    prototype.type = 7;
    prototype.position = std::array<int, 3>{1, 2, 3};
    prototype.create_event = 700;
    prototype.destroy_event = 701;

    gm::EventQueue queue;
    const std::size_t executed_before = things.stats().executed;
    const int id = things.spawn(prototype, &queue);
    CHECK_EQ(things.stats().executed, executed_before + 1);
    CHECK_EQ(id, 0);
    const gm::Thing* spawned = things.by_id(id);
    CHECK(spawned != nullptr);
    if (spawned != nullptr) {
        CHECK(spawned->alive);
        CHECK(spawned->executed);
        CHECK_EQ(spawned->id, id);
        CHECK_EQ(spawned->age, 0U);
        CHECK_EQ(spawned->expired, false);
    }
    const gm::EventParseResult created = queue.drain();
    CHECK_EQ(created.events.size(), 1U);
    if (created.events.size() == 1) {
        CHECK_EQ(created.events[0].id, 700);
    }

    // An explicit id that already exists is rejected; a free id is accepted.
    gm::Thing duplicate = prototype;
    duplicate.id = id;
    CHECK_EQ(things.spawn(duplicate), -1);
    gm::Thing explicit_id = prototype;
    explicit_id.id = 5;
    CHECK_EQ(things.spawn(explicit_id), 5);

    CHECK(things.remove(id, &queue));
    CHECK(things.by_id(id) != nullptr);
    CHECK(!things.by_id(id)->alive);
    const gm::EventParseResult destroyed = queue.drain();
    CHECK_EQ(destroyed.events.size(), 1U);
    if (destroyed.events.size() == 1) {
        CHECK_EQ(destroyed.events[0].id, 701);
    }
    CHECK(!things.remove(id, &queue)); // already dead
    CHECK(!things.remove(999, &queue));

    // A dead runtime id can be reused (projectile/thing respawn) without
    // growing the vector or duplicating by_id_.
    gm::Thing revived = prototype;
    revived.id = id;
    revived.create_event = 702;
    CHECK_EQ(things.spawn(revived, &queue), id);
    const gm::Thing* again = things.by_id(id);
    CHECK(again != nullptr);
    if (again != nullptr) {
        CHECK(again->alive);
        CHECK_EQ(again->type, 7);
    }
    const gm::EventParseResult revived_event = queue.drain();
    CHECK_EQ(revived_event.events.size(), 1U);
    if (revived_event.events.size() == 1) {
        CHECK_EQ(revived_event.events[0].id, 702);
    }

    // The automatic allocator also reuses a dead slot instead of growing.
    CHECK(things.remove(id, &queue));
    const std::size_t slots_before = things.things().size();
    gm::Thing auto_revived = prototype;
    auto_revived.create_event = 703;
    CHECK_EQ(things.spawn(auto_revived, &queue), id);
    CHECK_EQ(things.things().size(), slots_before);
}

// THING_TYPES +0x210/+0x214 -> instance +0x3ec/+0x3f0 (0x46dd71/0x46dd77):
// the mode-16 "every N ticks add M to the cooldown" pair.  They are plain
// type fields with no runtime consumer in `Things` yet; `thing_motion` reads
// them through `thing_motion_config`.
void test_timer_fields() {
    std::vector<Row> rows;
    std::vector<std::uint8_t> code;
    add_thing(code, 0, 0, 0, 0, 0, 0, 0);
    code.push_back(0xFF);
    rows.push_back({0x3FF, 7, 0});
    std::vector<TypeSpec> types = {{"TIMER", 0}};
    types[0].timer_period = 91;
    types[0].timer_step = 7;
    const gm::SbtLevel level = make_level(code, rows, types, {"MODEL"});
    const gm::VmState vm = gm::vm_state_from_level(level);
    const gm::Things things = gm::Things::load(level, vm, single_context());
    const gm::Thing* thing = things.by_id(0);
    CHECK(thing != nullptr);
    if (thing != nullptr) {
        CHECK_EQ(thing->timer_period, 91);
        CHECK_EQ(thing->timer_step, 7);
    }
    const std::optional<gm::ThingTypeEvents> events = gm::thing_type_events(level, 0);
    CHECK(events.has_value());
    if (events.has_value()) {
        CHECK_EQ(events->timer_period, 91);
        CHECK_EQ(events->timer_step, 7);
    }
    // Zero stays zero (a fresh type without the pair).
    const std::optional<gm::ThingTypeEvents> none =
        gm::thing_type_events(level, 5);
    CHECK(!none.has_value());
}

void test_motion_tick() {
    // Movement automata are opt-in (`Things::set_motion_enabled`); with them on,
    // `tick` steps every live instance through `world/thing_motion`.
    gm::Things things;
    CHECK(things.motion_state(0) == nullptr);

    gm::Thing spin;
    spin.id = -1;
    spin.type = 3;
    spin.movement_type = 1; // 0x470c70 case 1: angles += angle_rate * dt
    spin.angle_rate = {0.0, 0.0, 0.5};
    spin.position = std::array<int, 3>{10, 20, 30};
    CHECK_EQ(things.spawn(spin), 0);

    // Disabled: a tick only ages the instance.
    things.tick(1);
    CHECK(things.motion_state(0) == nullptr);
    CHECK_EQ(things.stats().motion_steps, std::size_t{0});

    things.set_motion_enabled(true);
    CHECK(things.motion_enabled());
    things.tick(1);
    CHECK_EQ(things.stats().motion_steps, std::size_t{1});
    const gm::ThingMotionState* state = things.motion_state(0);
    CHECK(state != nullptr);
    if (state != nullptr) {
        // `thing_motion_spawn` copies the three rates, the shared tick advances
        // the targets by rate * dt.
        CHECK(std::fabs(state->yaw_rate - 0.5) < 1e-15);
        CHECK(std::fabs(state->yaw_target - 0.5) < 1e-15);
        // The spawn view carries the script position.
        CHECK(std::fabs(state->position.x - 10.0) < 1e-12);
        CHECK(std::fabs(state->position.y - 20.0) < 1e-12);
        CHECK(std::fabs(state->position.z - 30.0) < 1e-12);
    }
    const gm::ThingMotionStep* last = things.motion_last(0);
    CHECK(last != nullptr);
    if (last != nullptr) {
        CHECK(last->supported);
    }
    // `Thing::position` stays the spawn view: the double state is authoritative.
    CHECK_EQ((*things.by_id(0)->position)[0], 10);

    // A walker (type 2) with a free world moves forward: yaw 0 means +y
    // (basis row 1, `forward = (sin yaw, cos yaw)`), so the body advances on y.
    gm::Thing walker;
    walker.id = -1;
    walker.type = 4;
    walker.movement_type = 2;
    walker.speed = 2.0;
    walker.radius = 1.0;
    walker.position = std::array<int, 3>{0, 0, 0};
    CHECK_EQ(things.spawn(walker), 1);
    const std::size_t moved_before = things.stats().motion_moved;
    things.tick(1);
    CHECK_EQ(things.stats().motion_moved, moved_before + 1);
    const gm::ThingMotionState* walked = things.motion_state(1);
    CHECK(walked != nullptr);
    if (walked != nullptr) {
        CHECK(walked->position.y > 0.0);
    }

    // Type 18 pushes away from the bodies installed with `set_push_targets`.
    gm::Thing pusher;
    pusher.id = -1;
    pusher.type = 5;
    pusher.movement_type = 18;
    pusher.speed = 1.0;
    pusher.box = {16.0, 10.0, 20.0};
    pusher.position = std::array<int, 3>{0, 0, 0};
    CHECK_EQ(things.spawn(pusher), 2);
    gm::ThingPushTarget body;
    body.position = {5.0, 0.0, 0.0};
    things.set_push_targets({body});
    things.tick(1);
    CHECK_EQ(things.stats().motion_pushed, std::size_t{1});
    const gm::ThingMotionState* pushed = things.motion_state(2);
    CHECK(pushed != nullptr);
    if (pushed != nullptr) {
        CHECK(pushed->position.x < 0.0);
    }

    // Disabling stops the stepping but keeps the states readable.
    things.set_motion_enabled(false);
    const std::size_t steps_before = things.stats().motion_steps;
    things.tick(1);
    CHECK_EQ(things.stats().motion_steps, steps_before);
    CHECK(things.motion_state(0) == nullptr); // motion off -> not exposed

    // Re-enabling continues from the stored state (no re-seed).
    things.set_motion_enabled(true);
    const double yaw_before = things.motion_state(0)->yaw_target;
    things.tick(1);
    CHECK(std::fabs(things.motion_state(0)->yaw_target - (yaw_before + 0.5)) < 1e-15);

    // A respawned id resets the motion state (fresh instance).
    gm::Thing fresh = spin;
    fresh.id = 0;
    CHECK(things.remove(0));
    CHECK_EQ(things.spawn(fresh), 0);
    CHECK(things.motion_state(0) == nullptr); // reset until the next tick
    things.tick(1);
    CHECK(things.motion_state(0) != nullptr);
    CHECK(std::fabs(things.motion_state(0)->yaw_target - 0.5) < 1e-15);

    // Out-of-range indices are ignored, including the target setter.
    things.set_motion_target(999, gm::ThingMotionTarget{});
    CHECK(things.motion_state(999) == nullptr);
    CHECK(things.motion_last(999) == nullptr);

    // Determinism: two containers with the same setup tick identically.
    gm::Things a;
    gm::Things b;
    for (gm::Things* list : {&a, &b}) {
        gm::Thing item;
        item.id = -1;
        item.movement_type = 14;
        item.speed = 1.5;
        item.radius = 1.0;
        item.position = std::array<int, 3>{0, 0, 0};
        list->spawn(item);
        gm::ThingMotionTarget target;
        target.valid = true;
        target.position = {100.0, 0.0, 0.0};
        target.yaw_error = 1.0;
        list->set_motion_target(0, target);
        list->set_motion_enabled(true);
        for (int tick = 0; tick < 20; ++tick) {
            list->tick(1);
        }
    }
    const gm::ThingMotionState* left = a.motion_state(0);
    const gm::ThingMotionState* right = b.motion_state(0);
    CHECK(left != nullptr && right != nullptr);
    if (left != nullptr && right != nullptr) {
        CHECK_EQ(left->position.x, right->position.x);
        CHECK_EQ(left->position.y, right->position.y);
        CHECK_EQ(left->yaw_target, right->yaw_target);
    }
}

void test_motion_aim() {
    // `aim_motion_at` fills the `0x473a00` errors in the body frame: the
    // local transform of `0x437909` (basis rows), the yaw of `0x41e2b8`
    // (forward = (sin yaw, cos yaw)) and the zero pitch error of this build.
    gm::Things things;
    gm::Thing body;
    body.id = -1;
    body.type = 6;
    body.movement_type = 0; // idle: no yaw integration, the frame is stable
    body.position = std::array<int, 3>{0, 0, 0};
    CHECK_EQ(things.spawn(body), 0);
    things.set_motion_enabled(true);

    // No state before the first step: the body angles are unknown.
    CHECK(!things.aim_motion_at(0, gm::Vec3d{100.0, 0.0, 0.0}));
    CHECK(things.motion_target(0) == nullptr);

    things.tick(1);
    CHECK(things.motion_state(0) != nullptr);

    constexpr double kHalfPi = 1.57079632679489661923;
    CHECK(things.aim_motion_at(0, gm::Vec3d{100.0, 0.0, 0.0}));
    const gm::ThingMotionTarget* aim = things.motion_target(0);
    CHECK(aim != nullptr);
    if (aim != nullptr) {
        CHECK(aim->valid);
        CHECK(aim->visible);
        CHECK_NEAR(aim->yaw_error, kHalfPi, 1e-12);
        CHECK_NEAR(aim->pitch_error, 0.0, 0.0);
        CHECK_NEAR(aim->vertical_error, 0.0, 0.0);
        CHECK_NEAR(aim->position.x, 100.0, 0.0);
    }

    // -X is -pi/2 in the body frame; the visibility flag travels along.
    CHECK(things.aim_motion_at(0, gm::Vec3d{-100.0, 0.0, 0.0}, false));
    aim = things.motion_target(0);
    CHECK(aim != nullptr && aim->valid);
    if (aim != nullptr) {
        CHECK_NEAR(aim->yaw_error, -kHalfPi, 1e-12);
        CHECK(!aim->visible);
    }

    // +Y is straight ahead at yaw 0; z is the vertical error.
    CHECK(things.aim_motion_at(0, gm::Vec3d{0.0, 100.0, 40.0}));
    aim = things.motion_target(0);
    CHECK(aim != nullptr && aim->valid);
    if (aim != nullptr) {
        CHECK_NEAR(aim->yaw_error, 0.0, 1e-12);
        CHECK_NEAR(aim->vertical_error, 40.0, 0.0);
    }

    // Non-finite points are rejected, the previous target stays.
    CHECK(!things.aim_motion_at(0, gm::Vec3d{std::nan(""), 0.0, 0.0}));
    CHECK(things.motion_target(0) != nullptr && things.motion_target(0)->valid);

    // `clear_motion_aim` drops the target without touching the state.
    things.clear_motion_aim(0);
    CHECK(things.motion_target(0) != nullptr);
    CHECK(!things.motion_target(0)->valid);

    // `index_of` is the stable slot lookup used by the AI bridge.
    CHECK(things.index_of(0) == std::optional<std::size_t>{0});
    CHECK(!things.index_of(42).has_value());

    // Out-of-range indices are ignored everywhere.
    CHECK(!things.aim_motion_at(1, gm::Vec3d{1.0, 0.0, 0.0}));
    CHECK(things.motion_target(1) == nullptr);

    // A turret (type 5) turns into the direction of the sign of `yaw_error`.
    gm::Things turret;
    gm::Thing gun;
    gun.id = -1;
    gun.type = 7;
    gun.movement_type = 5;
    gun.turn_accel = 0.5;
    gun.turn_max = 1.0;
    gun.position = std::array<int, 3>{0, 0, 0};
    CHECK_EQ(turret.spawn(gun), 0);
    turret.set_motion_enabled(true);
    turret.tick(1);
    // Put the body at a known yaw before aiming.
    turret.aim_motion_at(0, gm::Vec3d{100.0, 0.0, 0.0});
    const double error = turret.motion_target(0)->yaw_error;
    turret.tick(1);
    CHECK_EQ(turret.motion_last(0)->turn, error > 0.0 ? 3 : 2);
    CHECK(turret.motion_state(0)->yaw_target != 0.0);
}

} // namespace

int main(int argc, char** argv) {
    const bool full = argc > 1 && std::string(argv[1]) == "--full";
    test_synthetic();
    test_expire_lifetime();
    test_spawn_remove();
    test_motion_tick();
    test_timer_fields();
    test_motion_aim();
    test_generate_and_cycle();
    test_negatives();
    test_real_dm1();
    test_type_events();
    if (full) {
        test_corpus();
    } else {
        std::printf("things: quick run (synthetic + dm1/level0); use --full for the corpus\n");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "things: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("things: ok\n");
    return 0;
}
