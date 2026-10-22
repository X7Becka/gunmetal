// SCRIPT-VM executor tests (docs/VM_CPP.md).
//
// Two layers:
//   * synthetic byte code with no game files: IF/ELSE (ELSE is a jump at the
//     end of the then-branch), conditions, GENERATE recursion / stack depth,
//     limits, unknown opcodes, truncated operands and OOB PCs;
//   * parity with gm/sbtvm_exec.py on the shipped .sbt corpus: per-level
//     instruction/path/scene counts, 36/36 main scripts reaching END, and (with
//     --full) the whole 15 061-block sweep.
//
// The goldens below were produced by the Python reference:
//   python3 tools/re/vm_exec.py --level <l> --script all --json
//   (one campaign context, difficulty 1, seed 0x12345678, follow=False)
// and, for the corpus totals, by summing the same run over all 36 levels.
#include "res/save.h"
#include "res/sbt.h"
#include "res/vm_exec.h"

#include "core/bytes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

using gm::SbtValue;

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

constexpr const char* kGameDir = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Python goldens
// ---------------------------------------------------------------------------

/// Category order matches VmScene::counts() (lights, locations, items,
/// named_items, attached, sounds, sprites, things, doors, switches, lifts,
/// spawns).
struct LevelGolden {
    const char* name;
    int blocks;
    int paths;
    int steps;
    std::array<int, 12> counts;
};

constexpr const char* kCountNames[12] = {
    "lights", "locations", "items", "named_items", "attached", "sounds",
    "sprites", "things", "doors", "switches", "lifts", "spawns",
};

constexpr LevelGolden kLevels[] = {
    {"dm1.sbt", 363, 479, 3225, {60, 8, 1, 125, 182, 21, 0, 61, 61, 129, 0, 13}},
    {"dm2.sbt", 315, 430, 2978, {121, 8, 0, 104, 138, 67, 0, 55, 32, 113, 0, 9}},
    {"dm3.sbt", 326, 441, 3092, {150, 8, 0, 90, 139, 20, 8, 63, 62, 119, 2, 9}},
    {"dm4.sbt", 329, 444, 2854, {58, 16, 0, 54, 137, 19, 0, 62, 33, 66, 0, 10}},
    {"level0.sbt", 350, 468, 2821, {29, 10, 2, 1, 135, 6, 4, 51, 25, 24, 0, 8}},
    {"level00.sbt", 327, 446, 2706, {29, 10, 1, 6, 141, 6, 4, 50, 24, 21, 0, 8}},
    {"level000.sbt", 327, 446, 2705, {29, 10, 1, 6, 141, 6, 4, 50, 24, 21, 0, 8}},
    {"level0a.sbt", 327, 446, 2704, {29, 10, 1, 6, 141, 6, 4, 50, 24, 21, 0, 8}},
    {"level0b.sbt", 297, 412, 2364, {13, 0, 0, 6, 137, 6, 0, 49, 0, 8, 0, 1}},
    {"level1.sbt", 438, 563, 3848, {90, 48, 10, 114, 175, 49, 0, 86, 33, 88, 1, 16}},
    {"level10.sbt", 425, 552, 3995, {204, 70, 0, 127, 156, 89, 15, 91, 55, 144, 0, 10}},
    {"level11.sbt", 451, 589, 4429, {167, 15, 0, 145, 174, 71, 53, 213, 73, 145, 4, 9}},
    {"level12.sbt", 492, 636, 4434, {141, 27, 2, 146, 172, 19, 1, 176, 69, 150, 4, 9}},
    {"level13.sbt", 488, 630, 4761, {229, 9, 6, 162, 206, 30, 85, 197, 84, 147, 5, 9}},
    {"level14.sbt", 435, 582, 3953, {110, 6, 1, 116, 187, 21, 10, 178, 34, 75, 1, 9}},
    {"level15.sbt", 415, 539, 4367, {347, 8, 21, 142, 161, 57, 10, 121, 48, 294, 0, 9}},
    {"level16.sbt", 419, 568, 4216, {208, 8, 5, 163, 169, 27, 21, 264, 38, 82, 2, 16}},
    {"level17.sbt", 524, 677, 4715, {140, 10, 2, 197, 191, 15, 2, 222, 65, 153, 6, 10}},
    {"level18.sbt", 515, 692, 5082, {191, 8, 0, 167, 173, 40, 53, 321, 100, 145, 7, 10}},
    {"level19.sbt", 490, 649, 4414, {130, 8, 1, 148, 249, 100, 0, 170, 37, 112, 0, 16}},
    {"level2.sbt", 360, 484, 3140, {48, 8, 0, 62, 143, 19, 0, 82, 50, 108, 0, 16}},
    {"level20.sbt", 417, 557, 3840, {104, 10, 1, 158, 148, 26, 11, 129, 21, 89, 1, 11}},
    {"level21.sbt", 457, 627, 4405, {180, 9, 2, 184, 182, 22, 30, 190, 58, 77, 2, 16}},
    {"level22.sbt", 507, 686, 5103, {255, 8, 4, 174, 209, 23, 30, 247, 56, 151, 3, 9}},
    {"level23.sbt", 560, 732, 5333, {5, 24, 3, 321, 188, 57, 15, 269, 99, 184, 3, 16}},
    {"level24.sbt", 459, 619, 4555, {258, 10, 9, 154, 225, 27, 46, 195, 43, 117, 1, 16}},
    {"level3.sbt", 440, 593, 4365, {294, 4, 0, 144, 158, 41, 54, 170, 42, 153, 1, 16}},
    {"level4.sbt", 390, 517, 3764, {99, 9, 4, 95, 191, 18, 22, 113, 48, 100, 0, 10}},
    {"level5.sbt", 418, 552, 3876, {90, 11, 4, 136, 167, 41, 13, 135, 87, 115, 0, 17}},
    {"level6.sbt", 444, 574, 3904, {73, 0, 13, 140, 193, 42, 0, 129, 62, 85, 1, 10}},
    {"level7.sbt", 425, 563, 3890, {127, 27, 0, 149, 160, 54, 29, 167, 26, 78, 0, 9}},
    {"level8.sbt", 512, 654, 4597, {147, 12, 1, 142, 190, 25, 14, 159, 64, 226, 1, 9}},
    {"level9.sbt", 487, 620, 4681, {0, 8, 3, 141, 220, 31, 13, 218, 101, 208, 0, 9}},
    {"secrt1.sbt", 392, 521, 3333, {97, 8, 0, 75, 147, 15, 0, 105, 48, 80, 0, 12}},
    {"secrt2.sbt", 416, 560, 3734, {145, 8, 2, 123, 156, 12, 0, 123, 40, 75, 0, 9}},
    {"temple.sbt", 324, 440, 2797, {73, 8, 0, 81, 140, 44, 0, 58, 13, 61, 0, 9}},
};

const LevelGolden* find_golden(std::string_view name) {
    for (const auto& golden : kLevels) {
        if (name == golden.name) {
            return &golden;
        }
    }
    return nullptr;
}

/// Main script (offset 0, follow_generate=true): steps and reason for all 36
/// levels.  Python: 36/36 reach END.
///
/// The steps are the Python golden except ``level20.sbt``: its startup script
/// GENERATEs -30065, a row with ``flags = 0`` (a ``DISABLED`` entity,
/// 0x4607c5).  The engine skips it (0x45fa74 tests the source bit) and so does
/// the C++ executor; the Python reference launches every id-matched row, which
/// costs the two instructions of that block (DEFINE_PLAYER_TYPES + END).  All
/// other levels are unchanged.
struct MainGolden {
    const char* name;
    int steps;
};

constexpr MainGolden kMain[] = {
    {"dm1.sbt", 22},    {"dm2.sbt", 16},     {"dm3.sbt", 22},     {"dm4.sbt", 22},
    {"level0.sbt", 24}, {"level00.sbt", 24}, {"level000.sbt", 23}, {"level0a.sbt", 22},
    {"level0b.sbt", 22}, {"level1.sbt", 28},  {"level10.sbt", 23},  {"level11.sbt", 23},
    {"level12.sbt", 23}, {"level13.sbt", 23}, {"level14.sbt", 23},  {"level15.sbt", 16},
    {"level16.sbt", 23}, {"level17.sbt", 23}, {"level18.sbt", 22},  {"level19.sbt", 22},
    {"level2.sbt", 24},  {"level20.sbt", 34}, {"level21.sbt", 22},  {"level22.sbt", 22},
    {"level23.sbt", 22}, {"level24.sbt", 22}, {"level3.sbt", 24},   {"level4.sbt", 24},
    {"level5.sbt", 24},  {"level6.sbt", 24},  {"level7.sbt", 24},   {"level8.sbt", 24},
    {"level9.sbt", 24},  {"secrt1.sbt", 24},  {"secrt2.sbt", 23},   {"temple.sbt", 22},
};

/// level0 SET_PLAYER_START rows (index, x, y, z) in execution order.
struct SpawnGolden {
    int index;
    int x;
    int y;
    int z;
};

constexpr SpawnGolden kLevel0Spawns[] = {
    {0, 24, -208, 48}, {7, 328, 256, 48}, {1, -248, 256, 48}, {2, -152, 192, 48},
    {3, -56, 256, 48}, {4, 56, 176, 48},  {5, 136, 256, 48},  {6, 248, 176, 48},
};

/// Corpus totals (same context, follow=False), summed over all 36 levels.
constexpr int kCorpusBlocks = 15061;
constexpr int kCorpusPaths = 19988;
constexpr int kCorpusSteps = 138980;
/// Executed plain commands (78 421 = docs/VM_EXEC.md "исполнено ... инструкция")
/// and conditions; 156 / 42 distinct names.
constexpr int kCorpusCommands = 78421;
constexpr int kCorpusConditions = 19535;
constexpr int kCorpusDistinctCommands = 156;
constexpr int kCorpusDistinctConditions = 42;
constexpr std::array<int, 12> kCorpusCounts = {
    4470, 461, 100, 4304, 6121, 1172, 551, 5019, 1779, 3964, 45, 391,
};

// ---------------------------------------------------------------------------
// Synthetic byte-code encoder (the test counterpart of _enc_cmd/_enc_if)
// ---------------------------------------------------------------------------

using Bytes = std::vector<std::uint8_t>;

SbtValue u(std::uint32_t value) {
    return SbtValue::from_uint(value);
}

SbtValue d(double value) {
    return SbtValue::from_real(value);
}

void put_u32(Bytes& code, std::uint32_t value) {
    code.push_back(static_cast<std::uint8_t>(value & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void put_real(Bytes& code, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    for (int k = 0; k < 8; ++k) {
        code.push_back(static_cast<std::uint8_t>((bits >> (8 * k)) & 0xFF));
    }
}

void encode_args(Bytes& code, std::string_view sig, const std::vector<SbtValue>& args) {
    std::size_t index = 0;
    for (const char letter : sig) {
        CHECK(index < args.size());
        if (index >= args.size()) {
            return;
        }
        const SbtValue& value = args[index++];
        const int as_int = value.kind == SbtValue::Kind::Real
                               ? static_cast<int>(value.d)
                               : static_cast<std::int32_t>(value.u);
        switch (letter) {
        case 'A':
        case 'B':
        case 'D':
            put_real(code, value.kind == SbtValue::Kind::Real
                                ? value.d
                                : static_cast<double>(as_int));
            break;
        case 'Y':
            code.push_back(static_cast<std::uint8_t>(value.u & 0xFF));
            break;
        case 'S': {
            const std::string text =
                value.kind == SbtValue::Kind::String ? value.s : std::to_string(value.u);
            code.insert(code.end(), text.begin(), text.end());
            code.push_back(0);
            break;
        }
        default:
            put_u32(code, value.u);
            break;
        }
    }
}

void cmd(Bytes& code, std::string_view name, std::initializer_list<SbtValue> args) {
    const auto info = gm::vm_command_info(name);
    CHECK(info.has_value());
    if (!info) {
        return;
    }
    code.push_back(info->op);
    encode_args(code, info->sig, std::vector<SbtValue>(args));
}

void put_condition(Bytes& code, std::string_view name,
                   std::initializer_list<SbtValue> args) {
    const auto info = gm::vm_condition_info(name);
    CHECK(info.has_value());
    if (!info) {
        return;
    }
    code.push_back(info->op);
    encode_args(code, info->sig, std::vector<SbtValue>(args));
}

Bytes enc_if(std::string_view condition, std::initializer_list<SbtValue> args,
             const Bytes& then, const Bytes* otherwise = nullptr) {
    Bytes code;
    code.push_back(199);
    put_condition(code, condition, args);
    const std::size_t size = 4 + then.size() + (otherwise != nullptr ? 5 : 0);
    put_u32(code, static_cast<std::uint32_t>(size));
    code.insert(code.end(), then.begin(), then.end());
    if (otherwise != nullptr) {
        code.push_back(200);
        put_u32(code, static_cast<std::uint32_t>(4 + otherwise->size()));
        code.insert(code.end(), otherwise->begin(), otherwise->end());
    }
    return code;
}

struct SyntheticEntity {
    int id = 0;
    std::uint32_t offset = 0;
    std::uint32_t flags = 0x3FF; // the `VmEntity` default (data: 0/0x1FF/0x3FF)
};

/// Flags of the first row with *id* (0 when the state has no such row).
std::uint32_t flags_of(const gm::VmState& state, int id) {
    const gm::VmEntity* row = state.entity(id);
    return row != nullptr ? row->flags : 0u;
}

std::vector<gm::VmPathResult> run_bytes(const Bytes& code, gm::VmOptions options = {},
                                       std::vector<SyntheticEntity> entities = {},
                                       int difficulty = 1,
                                       std::optional<int> source_player = std::nullopt) {
    options.max_time = 0.0; // deterministic: no wall clock in tests
    gm::VmState state;
    state.code = code;
    state.difficulty = difficulty;
    state.rng = 1;
    state.source_player = source_player;
    state.players[0] = gm::VmPlayer{};
    for (const auto& row : entities) {
        gm::VmEntity entity;
        entity.id = row.id;
        entity.offset = row.offset;
        entity.flags = row.flags;
        state.entities.push_back(entity);
        state.order.push_back(row.id);
    }
    gm::VmExecutor executor(std::move(state), options);
    return executor.run(0);
}

// ---------------------------------------------------------------------------
// Synthetic tests
// ---------------------------------------------------------------------------

void test_synthetic() {
    Bytes location;
    cmd(location, "ADD_LOCATION", {u(1), u(10), u(20), u(30), d(0.0), d(0.0), d(0.0)});
    Bytes location2;
    cmd(location2, "ADD_LOCATION", {u(2), u(40), u(50), u(60), d(0.0), d(0.0), d(0.0)});

    // 1. IF/ELSE: GAME_DIFFICULTY_AT_LEAST 2.
    Bytes program = enc_if("GAME_DIFFICULTY_AT_LEAST", {u(2)}, location, &location2);
    program.push_back(0xFF);

    auto results = run_bytes(program, {}, {}, 1);
    CHECK_EQ(results.size(), std::size_t{1});
    CHECK(results[0].reason == gm::VmReason::End);
    CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{1});
    if (!results[0].state.scene.locations.empty()) {
        CHECK_EQ(results[0].state.scene.locations[0].thing_id, 2);
    }

    results = run_bytes(program, {}, {}, 2);
    CHECK_EQ(results.size(), std::size_t{1});
    CHECK(results[0].reason == gm::VmReason::End);
    if (!results[0].state.scene.locations.empty()) {
        CHECK_EQ(results[0].state.scene.locations[0].thing_id, 1);
    }

    // 2. Nested IF.
    const Bytes inner = enc_if("GAME_DIFFICULTY_AT_LEAST", {u(3)}, location);
    Bytes nested = enc_if("GAME_DIFFICULTY_AT_LEAST", {u(2)}, inner);
    nested.push_back(0xFF);
    results = run_bytes(nested, {}, {}, 1);
    CHECK(results[0].state.scene.locations.empty());
    results = run_bytes(nested, {}, {}, 3);
    CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{1});

    // 3. Comparison across commands: cash.
    Bytes cash;
    cmd(cash, "ADD_SOURCEPLAYER_CASH", {u(100)});
    const Bytes high = enc_if("SOURCEPLAYER_CASH_AT_LEAST", {u(50)}, location);
    const Bytes too_high = enc_if("SOURCEPLAYER_CASH_AT_LEAST", {u(250)}, location2);
    Bytes cash_program = cash;
    cash_program.insert(cash_program.end(), high.begin(), high.end());
    cash_program.insert(cash_program.end(), cash.begin(), cash.end());
    cash_program.insert(cash_program.end(), too_high.begin(), too_high.end());
    cash_program.push_back(0xFF);
    results = run_bytes(cash_program, {}, {}, 1, 0);
    CHECK(results[0].reason == gm::VmReason::End);
    CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{1});
    if (!results[0].state.scene.locations.empty()) {
        CHECK_EQ(results[0].state.scene.locations[0].thing_id, 1);
    }

    // 4. Unknown condition: stop vs both.
    Bytes unknown = enc_if("SOURCEPLAYER_UNDERWATER", {}, location, &location2);
    unknown.push_back(0xFF);
    gm::VmOptions stop;
    stop.on_unknown = gm::VmUnknownPolicy::Stop;
    results = run_bytes(unknown, stop);
    CHECK(results[0].reason == gm::VmReason::UnknownCondition);

    gm::VmOptions both;
    both.on_unknown = gm::VmUnknownPolicy::Both;
    results = run_bytes(unknown, both);
    CHECK_EQ(results.size(), std::size_t{2});
    for (const auto& result : results) {
        CHECK(result.reason == gm::VmReason::End);
    }

    // 5. Unknown opcode 0xfe -> clean stop, not a silent skip.
    const Bytes bad_op = {0xFE, 0xFF};
    results = run_bytes(bad_op);
    CHECK(results[0].reason == gm::VmReason::UnknownOpcode);
    CHECK(results[0].detail.find("0xfe") != std::string::npos);

    // 6. OOB PC: ELSE jumps far past the block.
    Bytes oob = {200};
    put_u32(oob, 0x1000);
    oob.push_back(0xFF);
    results = run_bytes(oob);
    CHECK(results[0].reason == gm::VmReason::BadPC);

    // 7. Bad operands: unterminated string / truncated dword.
    Bytes unterminated;
    cmd(unterminated, "LOG_MESSAGE", {SbtValue::from_string("x")});
    unterminated.pop_back(); // drop the NUL
    results = run_bytes(unterminated);
    CHECK(results[0].reason == gm::VmReason::BadOperand);

    Bytes truncated;
    cmd(truncated, "GENERATE", {u(1)});
    truncated.resize(truncated.size() - 1); // drop one byte of the id
    results = run_bytes(truncated);
    CHECK(results[0].reason == gm::VmReason::BadOperand);

    // 8. Recursion: GENERATE itself.  The row must not be a one-shot: 0x200
    // fires before the call (0x45fa9e), which turns the self-GENERATE into a
    // no-op — the separate case below proves exactly that.
    Bytes generate;
    cmd(generate, "GENERATE", {u(7)});
    generate.push_back(0xFF);
    results = run_bytes(generate, {}, {{7, 0, 0x1FF}});
    CHECK(results[0].reason == gm::VmReason::Recursion);

    // 9. Stack overflow: 40 entities, each GENERATEing the next.
    Bytes chain;
    std::vector<SyntheticEntity> rows;
    std::map<int, std::uint32_t> offsets;
    for (int k = 0; k < 40; ++k) {
        offsets[k] = static_cast<std::uint32_t>(chain.size());
        cmd(chain, "GENERATE", {u(static_cast<std::uint32_t>(101 + k))});
        chain.push_back(0xFF);
    }
    chain.push_back(0xFF);
    for (int k = 0; k < 40; ++k) {
        rows.push_back({100 + k, offsets[k]});
    }
    results = run_bytes(chain, {}, rows);
    CHECK(results[0].reason == gm::VmReason::StackOverflow);

    // 10. Step limit.
    Bytes many;
    for (int k = 0; k < 50; ++k) {
        cmd(many, "SLEEP", {u(1)});
    }
    many.push_back(0xFF);
    gm::VmOptions limited;
    limited.max_steps = 10;
    results = run_bytes(many, limited);
    CHECK(results[0].reason == gm::VmReason::StepLimit);

    // 11. SET_PLAYER_START operands.
    Bytes spawn;
    cmd(spawn, "SET_PLAYER_START", {u(0), u(1), u(2), u(3), d(0.0), d(0.0), d(0.0)});
    spawn.push_back(0xFF);
    results = run_bytes(spawn);
    CHECK_EQ(results[0].state.scene.spawns.size(), std::size_t{1});
    if (!results[0].state.scene.spawns.empty()) {
        const auto& row = results[0].state.scene.spawns[0];
        CHECK_EQ(row.index, 0);
        CHECK_EQ(row.pos[0], 1);
        CHECK_EQ(row.pos[1], 2);
        CHECK_EQ(row.pos[2], 3);
        CHECK_EQ(row.kind, std::string("campaign"));
    }

    // 12. ADD_THING_EX inside EVAL.
    Bytes thing;
    thing.push_back(0xC9);
    put_condition(thing, "ADD_THING_EX",
                  {u(0), u(0), u(0), u(4200), u(11), u(22), u(33), d(0.0), d(0.0), d(0.0),
                   u(0)});
    thing.push_back(0xFF);
    results = run_bytes(thing);
    CHECK_EQ(results[0].state.scene.things.size(), std::size_t{1});
    if (!results[0].state.scene.things.empty()) {
        const auto& entity = results[0].state.scene.things[0];
        CHECK_EQ(entity.type_index, 4200);
        CHECK(entity.pos.has_value());
        if (entity.pos) {
            CHECK_EQ((*entity.pos)[0], 11);
            CHECK_EQ((*entity.pos)[1], 22);
            CHECK_EQ((*entity.pos)[2], 33);
        }
    }

    // 13. Entity flags: DISABLE clears the low 9 bits.
    Bytes disable;
    cmd(disable, "DISABLE", {u(9)});
    disable.push_back(0xFF);
    results = run_bytes(disable, {}, {{9, 0}});
    CHECK_EQ(flags_of(results[0].state, 9) & 0x1FFu, 0u);

    // 13b. SP_ENABLE/SP_DISABLE: source player 3 sets bit 3; a non-player
    // source sets 0x100 (`0x457c80`, `docs/ENTITY_FIELDS.md`).
    Bytes sp_enable;
    cmd(sp_enable, "SOURCEPLAYER_ENABLE", {u(9)});
    sp_enable.push_back(0xFF);
    results = run_bytes(sp_enable, {}, {{9, 0, 0}}, 1, 3);
    CHECK_EQ(flags_of(results[0].state, 9), 1u << 3);
    results = run_bytes(sp_enable, {}, {{9, 0, 0}}, 1, std::nullopt);
    CHECK_EQ(flags_of(results[0].state, 9), 0x100u);

    Bytes sp_disable;
    cmd(sp_disable, "SOURCEPLAYER_DISABLE", {u(9)});
    sp_disable.push_back(0xFF);
    results = run_bytes(sp_disable, {}, {{9, 0, 0}}, 1, 3);
    CHECK_EQ(flags_of(results[0].state, 9) & (1u << 3), 0u);

    // 14. IF false with size 4 is an idle test (no else code).
    Bytes idle;
    idle.push_back(199);
    put_condition(idle, "GAME_DIFFICULTY_AT_LEAST", {u(9)});
    put_u32(idle, 4);
    idle.push_back(0xFF);
    results = run_bytes(idle, {}, {}, 1);
    CHECK(results[0].reason == gm::VmReason::End);
    CHECK_EQ(results[0].steps, 2);

    // 15. Decoder rejects an unknown opcode / truncated IF, and reports the PC.
    std::string error;
    const Bytes unknown_op = {0xFE};
    const auto unknown_insn = gm::vm_decode_at(unknown_op, 0, error);
    CHECK(unknown_insn.has_value());
    if (unknown_insn) {
        CHECK(unknown_insn->kind == gm::VmInsnKind::Unknown);
    }
    const Bytes short_if = {199, 6, 2, 0, 0};
    CHECK(!gm::vm_decode_at(short_if, 0, error).has_value());
    CHECK(!gm::vm_decode_at(unknown_op, 1, error).has_value());
}

// ---------------------------------------------------------------------------
// Real levels
// ---------------------------------------------------------------------------

struct BlockRun {
    std::size_t blocks = 0;
    std::vector<gm::VmPathResult> results;
    gm::VmScene merged;
    std::map<std::uint8_t, int> commands;
    std::map<std::string, int> conditions;
    int steps = 0;
};

std::vector<std::pair<std::optional<int>, std::size_t>> block_offsets(const gm::SbtLevel& level) {
    std::vector<std::pair<std::optional<int>, std::size_t>> out;
    std::set<std::size_t> seen;
    const std::size_t n = level.script.handles;
    if (n == 0 || level.script_handles_data.size() < n * 12) {
        return out;
    }
    const auto* data = level.script_handles_data.data();
    for (std::size_t i = 0; i < n; ++i) {
        const int id = static_cast<std::int32_t>(gm::rd32(data + (n + i) * 4));
        const std::size_t offset = gm::rd32(data + (2 * n + i) * 4);
        if (offset >= level.script_code.size() || !seen.insert(offset).second) {
            continue;
        }
        out.emplace_back(id, offset);
    }
    return out;
}

BlockRun run_blocks(const gm::SbtLevel& level, bool follow_generate, bool collect_counts) {
    gm::VmOptions options;
    options.follow_generate = follow_generate;
    options.on_unknown = gm::VmUnknownPolicy::Both;
    options.max_time = 0.0;

    gm::VmState state = gm::vm_state_from_level(level);
    const auto blocks = block_offsets(level);
    gm::VmExecutor executor(std::move(state), options);

    BlockRun run;
    run.blocks = blocks.size();
    for (const auto& [entity, offset] : blocks) {
        auto paths = executor.run(offset, entity);
        for (const auto& path : paths) {
            run.steps += path.steps;
            run.results.push_back(path);
        }
    }
    run.merged = gm::vm_merge_scenes(run.results);
    if (collect_counts) {
        run.commands = executor.command_counts();
        run.conditions = executor.condition_counts();
    }
    return run;
}

bool load_level(const fs::path& path, gm::SbtLevel& level) {
    std::string error;
    const auto bytes = read_file(path);
    auto parsed = gm::parse_sbt(bytes, error);
    if (!parsed) {
        std::fprintf(stderr, "FAIL %s: %s\n", path.string().c_str(), error.c_str());
        ++g_failures;
        return false;
    }
    level = std::move(*parsed);
    return true;
}

void check_level(const fs::path& path, const LevelGolden& golden) {
    gm::SbtLevel level;
    if (!load_level(path, level)) {
        return;
    }
    const BlockRun run = run_blocks(level, false, false);

    if (run.blocks != static_cast<std::size_t>(golden.blocks)) {
        std::fprintf(stderr, "FAIL %s: blocks %zu != %d\n", golden.name, run.blocks,
                     golden.blocks);
        ++g_failures;
    }
    if (run.results.size() != static_cast<std::size_t>(golden.paths)) {
        std::fprintf(stderr, "FAIL %s: paths %zu != %d\n", golden.name, run.results.size(),
                     golden.paths);
        ++g_failures;
    }
    if (run.steps != golden.steps) {
        std::fprintf(stderr, "FAIL %s: steps %d != %d\n", golden.name, run.steps,
                     golden.steps);
        ++g_failures;
    }
    const auto counts = run.merged.counts();
    for (std::size_t k = 0; k < counts.size(); ++k) {
        const auto actual = counts.at(kCountNames[k]);
        if (actual != static_cast<std::size_t>(golden.counts[k])) {
            std::fprintf(stderr, "FAIL %s: %s %zu != %d\n", golden.name, kCountNames[k],
                         actual, golden.counts[k]);
            ++g_failures;
        }
    }
    for (const auto& result : run.results) {
        if (!gm::vm_reason_ok(result.reason)) {
            std::fprintf(stderr, "FAIL %s: path ended with %s at 0x%zx (%s)\n",
                         golden.name, std::string(gm::vm_reason_name(result.reason)).c_str(),
                         result.pc, result.detail.c_str());
            ++g_failures;
        }
    }

    if (std::string_view(golden.name) == "level0.sbt") {
        CHECK_EQ(run.merged.spawns.size(), std::size(kLevel0Spawns));
        for (std::size_t k = 0; k < run.merged.spawns.size() && k < std::size(kLevel0Spawns);
             ++k) {
            const auto& spawn = run.merged.spawns[k];
            const auto& want = kLevel0Spawns[k];
            if (spawn.index != want.index || spawn.pos[0] != want.x ||
                spawn.pos[1] != want.y || spawn.pos[2] != want.z) {
                std::fprintf(stderr,
                             "FAIL level0 spawn %zu: (%d,%d,%d,%d) != (%d,%d,%d,%d)\n", k,
                             spawn.index, spawn.pos[0], spawn.pos[1], spawn.pos[2], want.index,
                             want.x, want.y, want.z);
                ++g_failures;
            }
        }
    }
}

void test_main_scripts() {
    std::size_t ended = 0;
    for (const MainGolden& golden : kMain) {
        const fs::path path = fs::path(kGameDir) / golden.name;
        gm::SbtLevel level;
        if (!load_level(path, level)) {
            continue;
        }
        gm::VmOptions options;
        options.max_time = 0.0;
        options.follow_generate = true;
        gm::VmExecutor executor(gm::vm_state_from_level(level), options);
        const auto results = executor.run(0);
        CHECK(!results.empty());
        if (results.empty()) {
            continue;
        }
        if (results[0].reason == gm::VmReason::End) {
            ++ended;
        } else {
            std::fprintf(stderr, "FAIL %s main: %s\n", golden.name,
                         std::string(gm::vm_reason_name(results[0].reason)).c_str());
            ++g_failures;
        }
        if (results[0].steps != golden.steps) {
            std::fprintf(stderr, "FAIL %s main: steps %d != %d\n", golden.name,
                         results[0].steps, golden.steps);
            ++g_failures;
        }
    }
    CHECK_EQ(ended, std::size(kMain));
    std::printf("vm: main scripts %zu/%zu to END\n", ended, std::size(kMain));
}

void test_corpus() {
    int blocks = 0;
    int paths = 0;
    int steps = 0;
    int commands = 0;
    int conditions = 0;
    std::set<std::uint8_t> command_ops;
    std::map<std::string, int> condition_names;
    std::array<int, 12> counts{};

    for (const LevelGolden& golden : kLevels) {
        const fs::path path = fs::path(kGameDir) / golden.name;
        gm::SbtLevel level;
        if (!load_level(path, level)) {
            continue;
        }
        const BlockRun run = run_blocks(level, false, true);
        blocks += static_cast<int>(run.blocks);
        paths += static_cast<int>(run.results.size());
        steps += run.steps;
        for (const auto& [op, count] : run.commands) {
            commands += count;
            command_ops.insert(op);
        }
        for (const auto& [name, count] : run.conditions) {
            conditions += count;
            condition_names[name] += count;
        }
        const auto level_counts = run.merged.counts();
        for (std::size_t k = 0; k < counts.size(); ++k) {
            counts[k] += static_cast<int>(level_counts.at(kCountNames[k]));
        }
    }

    CHECK_EQ(blocks, kCorpusBlocks);
    CHECK_EQ(paths, kCorpusPaths);
    CHECK_EQ(steps, kCorpusSteps);
    CHECK_EQ(conditions, kCorpusConditions);
    CHECK_EQ(static_cast<int>(condition_names.size()), kCorpusDistinctConditions);
    CHECK_EQ(static_cast<int>(command_ops.size()), kCorpusDistinctCommands);
    for (std::size_t k = 0; k < counts.size(); ++k) {
        if (counts[k] != kCorpusCounts[k]) {
            std::fprintf(stderr, "FAIL corpus %s %d != %d\n", kCountNames[k], counts[k],
                         kCorpusCounts[k]);
            ++g_failures;
        }
    }
    CHECK_EQ(commands, kCorpusCommands);
    std::printf("vm: corpus %d blocks, %d paths, %d instructions, %d commands, %d conditions\n",
                blocks, paths, steps, commands, conditions);
}

// ---------------------------------------------------------------------------
// SECTOR_STUFF writes (docs/RECORDS.md §2, docs/VM_EXEC.md)
// ---------------------------------------------------------------------------

std::uint64_t dbits(double value) {
    std::uint64_t out = 0;
    static_assert(sizeof(out) == sizeof(value));
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

/// Run *code* over a caller-prepared state (the sector view has to survive
/// into the executor, so run_bytes() cannot be used).
std::vector<gm::VmPathResult> run_state(gm::VmState state, const Bytes& code) {
    gm::VmOptions options;
    options.max_time = 0.0;
    state.code = code;
    state.difficulty = 1;
    state.rng = 1;
    state.players[0] = gm::VmPlayer{};
    gm::VmExecutor executor(std::move(state), options);
    return executor.run(0);
}

/// Same, with explicit options and a start PC (woken sleeping scripts resume
/// at a saved offset, docs/ENTITY_FIELDS.md §8).
std::vector<gm::VmPathResult> run_from(gm::VmState state, const Bytes& code,
                                       std::size_t start, gm::VmOptions options = {}) {
    options.max_time = 0.0;
    state.code = code;
    state.difficulty = 1;
    state.rng = 1;
    if (state.players.empty()) {
        state.players[0] = gm::VmPlayer{};
    }
    gm::VmExecutor executor(std::move(state), options);
    return executor.run(start);
}

bool same_slot(const gm::VmSectorSlot& a, const gm::VmSectorSlot& b) {
    return dbits(a.move[0]) == dbits(b.move[0]) && dbits(a.move[1]) == dbits(b.move[1]) &&
           dbits(a.move[2]) == dbits(b.move[2]) &&
           dbits(a.extra[0]) == dbits(b.extra[0]) &&
           dbits(a.extra[1]) == dbits(b.extra[1]) &&
           dbits(a.extra[2]) == dbits(b.extra[2]) &&
           dbits(a.sound_param) == dbits(b.sound_param) && a.event_a == b.event_a &&
           a.event_b == b.event_b && a.damage_a == b.damage_a &&
           a.damage_b == b.damage_b && a.sound == b.sound &&
           a.sound_count == b.sound_count && a.flags == b.flags;
}

void test_sector_source_and_handles() {
    // SET_SOURCESECTOR_* (cids 0x1b..0x1f) address SECTOR_STUFF through the
    // implicit source sector (`edi` = the third argument of 0x457390), and
    // SET_HANDLESECTOR_* (0x28..0x2d) through the handle table
    // 0x5b1df8/0x617fb8 (count 0x5e3de0, at most 0x100).
    struct SigCase {
        const char* name;
        std::size_t bytes; // operand bytes, opcode excluded
    };
    for (const SigCase& item : {SigCase{"SET_SOURCESECTOR_MOVEMENT", 0x24},
                                SigCase{"SET_SOURCESECTOR_BRIGHTNESS", 0x4},
                                SigCase{"SET_SOURCESECTOR_FLAGS", 0x4},
                                SigCase{"SET_SOURCESECTOR_TYPE", 0x4},
                                SigCase{"SET_SOURCESECTOR_DAMAGE", 0x4},
                                SigCase{"SET_HANDLESECTOR_MOVEMENT", 0x28},
                                SigCase{"SET_HANDLESECTOR_BRIGHTNESS", 0x8},
                                SigCase{"SET_HANDLESECTOR_TYPE", 0x8},
                                SigCase{"SET_HANDLESECTOR_DAMAGE", 0x8},
                                SigCase{"SET_HANDLESECTOR_FLAGS", 0x8},
                                SigCase{"SET_HANDLESECTOR_SOUND", 0x14}}) {
        const auto info = gm::vm_command_info(item.name);
        CHECK(info.has_value());
        if (info) {
            Bytes code;
            code.push_back(info->op);
            for (std::size_t k = 0; k < item.bytes; ++k) {
                code.push_back(0);
            }
            std::string error;
            const auto insn = gm::vm_decode_at(code, 0, error);
            CHECK(insn.has_value());
            if (insn) {
                CHECK_EQ(insn->next, 1 + item.bytes);
            }
        }
    }

    // 1. The source sector replaces the sector operand: 0x457fc6 never reads
    // one, it writes 0x800b60 + edi*0x58.
    {
        gm::VmState state;
        state.source_sector = 7;
        Bytes code;
        cmd(code, "SET_SOURCESECTOR_MOVEMENT",
            {u(7), u(-3), u(2), d(1.5), d(-0.0), d(4096.5)});
        code.push_back(0xFF);
        const auto results = run_state(state, code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            const gm::VmSectorSlot& slot = results[0].state.sectors[7];
            CHECK_EQ(slot.move[0], 7.0);
            CHECK_EQ(slot.move[1], -3.0);
            CHECK_EQ(slot.move[2], 2.0);
            CHECK_EQ(dbits(slot.extra[0]), dbits(1.5));
            CHECK_EQ(dbits(slot.extra[1]), dbits(-0.0));
            CHECK_EQ(dbits(slot.extra[2]), dbits(4096.5));
            for (std::size_t i = 0; i < results[0].state.sectors.size(); ++i) {
                if (i != 7) {
                    CHECK(same_slot(results[0].state.sectors[i], gm::VmSectorSlot{}));
                }
            }
            CHECK(results[0].state.source_sector == 7); // context is preserved
        }
    }

    // 2. No source sector (-1) is the original's `edi < 0` error path.
    {
        Bytes code;
        cmd(code, "SET_SOURCESECTOR_FLAGS", {u(0x1234)});
        code.push_back(0xFF);
        const auto results = run_state(gm::VmState{}, code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            CHECK_EQ(results[0].state.sectors[0].flags, 0u);
        }
    }

    // 3. FLAGS and DAMAGE write the same fields as their SET_SECTOR twins.
    {
        gm::VmState state;
        state.source_sector = 5;
        Bytes code;
        cmd(code, "SET_SOURCESECTOR_FLAGS", {u(0x8001)});
        cmd(code, "SET_SOURCESECTOR_DAMAGE", {u(-2)}); // 0xfffffffe
        code.push_back(0xFF);
        const auto results = run_state(state, code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            const gm::VmSectorSlot& slot = results[0].state.sectors[5];
            CHECK_EQ(slot.flags, 0x8001u);
            CHECK_EQ(slot.damage_a, 0xfffeu);
            CHECK_EQ(slot.damage_b, -1);
        }
    }

    // 4. BRIGHTNESS/TYPE do not touch the array (they write the .sbw sector
    // record through 0x42d8a5/0x42d9d5).
    {
        gm::VmState state;
        state.source_sector = 9;
        Bytes code;
        cmd(code, "SET_SOURCESECTOR_BRIGHTNESS", {u(200)});
        cmd(code, "SET_SOURCESECTOR_TYPE", {u(3)});
        code.push_back(0xFF);
        const auto results = run_state(state, code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            CHECK(same_slot(results[0].state.sectors[9], gm::VmSectorSlot{}));
        }
    }

    // 5. Handle table: first match wins, unknown handles are a no-op.
    {
        gm::VmState state;
        state.sector_handles = {{42, 9}, {43, 200}, {42, 11}};
        CHECK(state.sector_of_handle(42).value_or(-1) == 9); // 0x4582b3 scans in order
        CHECK(state.sector_of_handle(43).value_or(-1) == 200);
        CHECK(!state.sector_of_handle(44).has_value());

        Bytes code;
        cmd(code, "SET_HANDLESECTOR_MOVEMENT",
            {u(42), u(1), u(2), u(3), d(0.5), d(0.25), d(-4.0)});
        cmd(code, "SET_HANDLESECTOR_FLAGS", {u(42), u(0xabc)});
        cmd(code, "SET_HANDLESECTOR_DAMAGE", {u(43), u(0x0003fffe)});
        cmd(code, "SET_HANDLESECTOR_SOUND", {u(43), u(130), u(10), d(0.03)});
        cmd(code, "SET_HANDLESECTOR_FLAGS", {u(99), u(0xffff)}); // unknown
        code.push_back(0xFF);
        const auto results = run_state(state, code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            const gm::VmSectorSlot& nine = results[0].state.sectors[9];
            CHECK_EQ(nine.move[0], 1.0);
            CHECK_EQ(nine.move[1], 2.0);
            CHECK_EQ(nine.move[2], 3.0);
            CHECK_EQ(dbits(nine.extra[0]), dbits(0.5));
            CHECK_EQ(dbits(nine.extra[1]), dbits(0.25));
            CHECK_EQ(dbits(nine.extra[2]), dbits(-4.0));
            CHECK_EQ(nine.flags, 0xabcu);
            const gm::VmSectorSlot& two_hundred = results[0].state.sectors[200];
            CHECK_EQ(two_hundred.damage_a, 0xfffeu);
            CHECK_EQ(two_hundred.damage_b, 3);
            CHECK_EQ(two_hundred.sound, 130);
            CHECK_EQ(two_hundred.sound_count, 10);
            CHECK_EQ(dbits(two_hundred.sound_param), dbits(0.03));
            // The duplicate handle's second row is never used.
            CHECK(same_slot(results[0].state.sectors[11], gm::VmSectorSlot{}));
        }
    }

    // 6. Handle parser: `SECTOR_HANDLE: <ld> <ld>` (0x4b9888), per line, up to
    //    0x100 entries; other record types and bad lines are skipped.
    {
        gm::VmState state;
        const std::string text =
            "POLY_HANDLE: 1 2 3\n"
            "SECTOR_HANDLE: 700 12\n"
            "SECTOR_HANDLE: -5 13\n"
            "SECTOR_HANDLE: garbage\n"
            "OBJECT_HANDLE: 9 9\n"
            "SECTOR_HANDLE: 701 14";
        CHECK_EQ(gm::vm_load_sector_handles(state, text), std::size_t{3});
        CHECK_EQ(state.sector_handles.size(), std::size_t{3});
        CHECK(state.sector_of_handle(700).value_or(-1) == 12);
        CHECK(state.sector_of_handle(-5).value_or(-1) == 13);
        CHECK(state.sector_of_handle(701).value_or(-1) == 14);

        gm::VmState many;
        std::string bulk;
        for (int i = 0; i < 300; ++i) {
            bulk += "SECTOR_HANDLE: " + std::to_string(i) + " " + std::to_string(i + 1) + "\n";
        }
        CHECK_EQ(gm::vm_load_sector_handles(many, bulk), std::size_t{0x100});
        CHECK_EQ(many.sector_handles.size(), std::size_t{0x100});
    }
}

void test_sector_stuff() {
    // Operand signatures and sizes: the handlers consume exactly these bytes
    // (0x457815 advances 0x28, 0x457961/0x457929 0x8, 0x4579ab 0xc,
    // 0x4579ee 0x14 after the opcode).
    struct SigCase {
        const char* name;
        std::size_t bytes; // operand bytes, opcode excluded
    };
    for (const SigCase& item : {SigCase{"SET_SECTOR_MOVEMENT", 0x28},
                                SigCase{"SET_SECTOR_DAMAGE", 0x8},
                                SigCase{"SET_SECTOR_SOUND", 0x14},
                                SigCase{"SET_SECTOR_EVENTS", 0xc},
                                SigCase{"SET_SECTOR_FLAGS", 0x8}}) {
        const auto info = gm::vm_command_info(item.name);
        CHECK(info.has_value());
        if (info) {
            Bytes code;
            code.push_back(info->op);
            for (std::size_t k = 0; k < item.bytes; ++k) {
                code.push_back(0);
            }
            std::string error;
            const auto insn = gm::vm_decode_at(code, 0, error);
            CHECK(insn.has_value());
            if (insn) {
                CHECK_EQ(insn->next, 1 + item.bytes);
            }
        }
    }

    // 1. MOVEMENT: three integer operands sign-extended into +0x00/+0x08/+0x10
    // and three raw doubles into +0x18/+0x20/+0x28 (0x457834..0x4578ac).
    {
        Bytes code;
        cmd(code, "SET_SECTOR_MOVEMENT",
            {u(34), u(7), u(-3), u(2), d(1.5), d(-0.0), d(4096.5)});
        code.push_back(0xFF);
        const auto results = run_bytes(code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            const gm::VmSectorSlot& slot = results[0].state.sectors[34];
            CHECK_EQ(slot.move[0], 7.0);
            CHECK_EQ(slot.move[1], -3.0);
            CHECK_EQ(slot.move[2], 2.0);
            CHECK_EQ(dbits(slot.extra[0]), dbits(1.5));
            CHECK_EQ(dbits(slot.extra[1]), dbits(-0.0));
            CHECK_EQ(dbits(slot.extra[2]), dbits(4096.5));
            // Nothing else was touched, and no other slot has data.
            for (std::size_t i = 0; i < results[0].state.sectors.size(); ++i) {
                if (i != 34) {
                    CHECK(same_slot(results[0].state.sectors[i], gm::VmSectorSlot{}));
                }
            }
            CHECK_EQ(slot.event_a, 0);
            CHECK_EQ(slot.flags, 0u);
        }
    }

    // 2. DAMAGE/SOUND/EVENTS/FLAGS on one slot, then a second DAMAGE that
    // overwrites both halves (arithmetic shift: 0xfffffffe -> 0xfffe/0xffff).
    {
        Bytes code;
        cmd(code, "SET_SECTOR_DAMAGE", {u(5), u(0x12345678)});
        cmd(code, "SET_SECTOR_SOUND", {u(5), u(130), u(10), d(0.03)});
        cmd(code, "SET_SECTOR_EVENTS", {u(5), u(606), u(-30234)});
        cmd(code, "SET_SECTOR_FLAGS", {u(5), u(0x10)});
        code.push_back(0xFF);
        auto results = run_bytes(code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            const gm::VmSectorSlot& slot = results[0].state.sectors[5];
            CHECK_EQ(slot.damage_a, 0x5678u);
            CHECK_EQ(slot.damage_b, 0x1234);
            CHECK_EQ(slot.sound, 130);
            CHECK_EQ(slot.sound_count, 10);
            CHECK_EQ(dbits(slot.sound_param), dbits(0.03));
            CHECK_EQ(slot.event_a, 606);
            CHECK_EQ(slot.event_b, -30234);
            CHECK_EQ(slot.flags, 0x10u);
        }

        Bytes second;
        cmd(second, "SET_SECTOR_DAMAGE", {u(5), u(0xfffffffe)});
        second.push_back(0xFF);
        results = run_bytes(second);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            // 0xfffffffe: +0x40 = 0xfffe (upper half zero), +0x44 = -1.
            CHECK_EQ(results[0].state.sectors[5].damage_a, 0xfffeu);
            CHECK_EQ(results[0].state.sectors[5].damage_b, -1);
        }
    }

    // 3. Out-of-range indices: the command is still consumed (an event is
    // recorded) but no slot is written, and the run reaches END.
    {
        Bytes code;
        cmd(code, "SET_SECTOR_FLAGS", {u(256), u(0x1f)});
        cmd(code, "SET_SECTOR_DAMAGE", {u(-1), u(1)});
        cmd(code, "SET_SECTOR_SOUND", {u(1000), u(1), u(1), d(0.5)});
        cmd(code, "SET_SECTOR_EVENTS", {u(300), u(1), u(2)});
        cmd(code, "SET_SECTOR_MOVEMENT",
            {u(-5), u(1), u(2), u(3), d(1.0), d(1.0), d(1.0)});
        code.push_back(0xFF);
        const auto results = run_bytes(code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            CHECK(results[0].reason == gm::VmReason::End);
            CHECK_EQ(results[0].state.events.size(), std::size_t{5});
            for (const gm::VmSectorSlot& slot : results[0].state.sectors) {
                CHECK(same_slot(slot, gm::VmSectorSlot{}));
            }
        }
    }

    // 4. SET_SECTOR_BRIGHTNESS/TYPE write the .sbw sector record, not this
    // array (0x4578f8 stores sector+0x16, 0x42d9d5 stores sector+0x14=value*6).
    {
        Bytes code;
        cmd(code, "SET_SECTOR_BRIGHTNESS", {u(9), u(200)});
        cmd(code, "SET_SECTOR_TYPE", {u(9), u(3)});
        code.push_back(0xFF);
        const auto results = run_bytes(code);
        CHECK_EQ(results.size(), std::size_t{1});
        if (results.size() == 1) {
            CHECK_EQ(results[0].state.events.size(), std::size_t{2});
            for (const gm::VmSectorSlot& slot : results[0].state.sectors) {
                CHECK(same_slot(slot, gm::VmSectorSlot{}));
            }
        }
    }

    // 5. Seed from a parsed save block 8 and then write through the VM: the
    // loaded fields survive, only the written ones change.  Needs the install.
    if (fs::exists(fs::path(kGameDir) / "SAVEGAME" / "saved0.sbt")) {
        std::ifstream in(fs::path(kGameDir) / "SAVEGAME" / "saved0.sbt",
                         std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
        std::string error;
        const auto save = gm::parse_save(bytes, error);
        CHECK(save.has_value());
        if (save && save->sector_stuff.size() == gm::kSbtSectorSlots) {
            gm::VmState state;
            gm::vm_load_sector_stuff(state, save->sector_stuff);
            CHECK_EQ(state.sectors[9].sound, 130);
            CHECK_EQ(state.sectors[9].sound_count, 10);
            CHECK_EQ(dbits(state.sectors[9].sound_param), dbits(0.03));
            CHECK_EQ(state.sectors[9].event_a, 40);
            CHECK_EQ(state.sectors[34].event_b, -30234);
            CHECK_EQ(state.sectors[34].damage_a, 0u);
            CHECK_EQ(state.sectors[34].damage_b, 0);
            CHECK(same_slot(state.sectors[10], gm::VmSectorSlot{}));

            Bytes code;
            cmd(code, "SET_SECTOR_FLAGS", {u(9), u(0x18)});
            cmd(code, "SET_SECTOR_SOUND", {u(9), u(-1), u(0), d(0.0)});
            cmd(code, "SET_SECTOR_DAMAGE", {u(34), u(0x00010002)});
            code.push_back(0xFF);
            const auto results = run_state(std::move(state), code);
            CHECK_EQ(results.size(), std::size_t{1});
            if (results.size() == 1) {
                const gm::VmSectorSlot& nine = results[0].state.sectors[9];
                CHECK_EQ(nine.flags, 0x18u);
                CHECK_EQ(nine.sound, -1);
                CHECK_EQ(nine.sound_count, 0);
                CHECK_EQ(dbits(nine.sound_param), dbits(0.0));
                CHECK_EQ(nine.event_a, 40); // untouched by the writes above
                const gm::VmSectorSlot& thirtyfour = results[0].state.sectors[34];
                CHECK_EQ(thirtyfour.damage_a, 0x0002u);
                CHECK_EQ(thirtyfour.damage_b, 0x0001);
                CHECK_EQ(thirtyfour.event_a, -30233);
                CHECK_EQ(thirtyfour.sound, 129);
            }

            // An empty vector is the cleared array (loader 0x46b1b0).
            gm::VmState cleared;
            gm::vm_load_sector_stuff(cleared, save->sector_stuff);
            gm::vm_load_sector_stuff(cleared, {});
            for (const gm::VmSectorSlot& slot : cleared.sectors) {
                CHECK(same_slot(slot, gm::VmSectorSlot{}));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Entity rows: one-shot (0x200), flag-driven row selection and duplicate ids
// (docs/ENTITY_FIELDS.md §4, §5, §12)
// ---------------------------------------------------------------------------

Bytes location_code(int thing_id) {
    Bytes code;
    cmd(code, "ADD_LOCATION", {u(static_cast<std::uint32_t>(thing_id)), u(1), u(2), u(3), d(0.0),
                               d(0.0), d(0.0)});
    code.push_back(0xFF);
    return code;
}

void test_entity_rows() {
    // 1. One-shot: 0x45fa95 `TEST AH,0x2` -> 0x45fa9e `AND EAX,0xfffffe00`
    // *before* the call, so the second GENERATE finds flags == 0x200 and the
    // row is not eligible any more (0x45fa74 tests the source bit).
    {
        Bytes sub = location_code(1);
        Bytes code;
        cmd(code, "GENERATE", {u(5)});
        cmd(code, "GENERATE", {u(5)});
        code.push_back(0xFF);
        const auto offset = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), sub.begin(), sub.end());

        auto results = run_bytes(code, {}, {{5, offset, 0x3FF}});
        CHECK_EQ(results.size(), std::size_t{1});
        CHECK(results[0].reason == gm::VmReason::End);
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{1});
        CHECK_EQ(flags_of(results[0].state, 5), 0x200u); // 0x1ff cleared, 0x200 kept

        // ENABLE adds 0x1ff back (0x457c80) and re-arms the one-shot.
        Bytes again;
        cmd(again, "GENERATE", {u(5)});
        cmd(again, "ENABLE", {u(5)});
        cmd(again, "GENERATE", {u(5)});
        again.push_back(0xFF);
        const auto again_offset = static_cast<std::uint32_t>(again.size());
        again.insert(again.end(), sub.begin(), sub.end());
        results = run_bytes(again, {}, {{5, again_offset, 0x3FF}});
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{2});
        CHECK_EQ(flags_of(results[0].state, 5), 0x200u);

        // DISABLE clears 0x1ff but not 0x200 (0x457c49), so the row stays out.
        Bytes disabled;
        cmd(disabled, "DISABLE", {u(5)});
        disabled.push_back(0xFF);
        results = run_bytes(disabled, {}, {{5, 0, 0x3FF}});
        CHECK_EQ(flags_of(results[0].state, 5), 0x200u);
        CHECK_EQ(results[0].state.entity(5)->one_shot(), true);
    }

    // 2. The reset precedes the call: a one-shot row that GENERATEs itself does
    // not re-enter (the engine would loop; the model reports no recursion).
    {
        Bytes code;
        cmd(code, "GENERATE", {u(7)});
        code.push_back(0xFF);
        const auto results = run_bytes(code, {}, {{7, 0, 0x3FF}});
        CHECK(results[0].reason == gm::VmReason::End);
        CHECK_EQ(results[0].steps, 4);
        CHECK_EQ(flags_of(results[0].state, 7), 0x200u);
    }

    // 3. Row selection by the source bit (0x45fa8a `SHL EDX,CL` for a player,
    // 0x45facf `TEST AH,0x1` for anything else).
    {
        Bytes sub = location_code(2);
        Bytes code;
        cmd(code, "GENERATE", {u(9)});
        code.push_back(0xFF);
        const auto offset = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), sub.begin(), sub.end());

        const std::vector<SyntheticEntity> rows = {{9, offset, 1u << 3}};
        CHECK_EQ(run_bytes(code, {}, rows, 1, 3)[0].state.scene.locations.size(),
                 std::size_t{1}); // player 3 owns bit 3
        CHECK_EQ(run_bytes(code, {}, rows, 1, 0)[0].state.scene.locations.size(),
                 std::size_t{0}); // player 0 does not
        CHECK_EQ(run_bytes(code, {}, rows, 1, std::nullopt)[0]
                     .state.scene.locations.size(),
                 std::size_t{1}); // no source: the union of all sources
    }

    // 4. Duplicate ids: 0x45fa58 walks the whole column and runs *every*
    // matching row, in table order (249 such rows in the shipped levels).
    {
        const Bytes first = location_code(1);
        const Bytes second = location_code(2);
        Bytes code;
        cmd(code, "GENERATE", {u(7)});
        code.push_back(0xFF);
        const auto off1 = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), first.begin(), first.end());
        const auto off2 = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), second.begin(), second.end());

        auto results = run_bytes(code, {}, {{7, off1, 0x1FF}, {7, off2, 0x1FF}});
        CHECK_EQ(results.size(), std::size_t{1});
        CHECK(results[0].reason == gm::VmReason::End);
        CHECK_EQ(results[0].state.entity_count(7), std::size_t{2});
        CHECK_EQ(results[0].state.entities.size(), std::size_t{2});
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{2});
        if (results[0].state.scene.locations.size() == 2) {
            CHECK_EQ(results[0].state.scene.locations[0].thing_id, 1); // table order
            CHECK_EQ(results[0].state.scene.locations[1].thing_id, 2);
        }

        // Each duplicate has its own flags: a disabled or already-fired row is
        // skipped while the others still run.
        results = run_bytes(code, {}, {{7, off1, 0x1FF}, {7, off2, 0x000}});
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{1});
        results = run_bytes(code, {}, {{7, off1, 0x000}, {7, off2, 0x200}});
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{0});

        // Both one-shot rows fire once and each clears its own 0x1ff.
        results = run_bytes(code, {}, {{7, off1, 0x3FF}, {7, off2, 0x3FF}});
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{2});
        CHECK_EQ(results[0].state.entities[0].flags, 0x200u);
        CHECK_EQ(results[0].state.entities[1].flags, 0x200u);
    }

    // 5. The rows of one id run one after another in the same state: row A's
    // ENABLE is visible to row B's GENERATE (0x45fb1c loops back to 0x45fa58
    // after the previous sub-script returned).
    {
        Bytes enable8;
        cmd(enable8, "ENABLE", {u(8)});
        enable8.push_back(0xFF);
        Bytes generate8;
        cmd(generate8, "GENERATE", {u(8)});
        generate8.push_back(0xFF);
        Bytes row8 = location_code(3);

        Bytes code;
        cmd(code, "GENERATE", {u(7)});
        code.push_back(0xFF);
        const auto offA = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), enable8.begin(), enable8.end());
        const auto offB = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), generate8.begin(), generate8.end());
        const auto off8 = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), row8.begin(), row8.end());

        const auto results =
            run_bytes(code, {}, {{7, offA, 0x1FF}, {7, offB, 0x1FF}, {8, off8, 0x000}});
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{1});
        if (!results[0].state.scene.locations.empty()) {
            CHECK_EQ(results[0].state.scene.locations[0].thing_id, 3);
        }
        CHECK_EQ(flags_of(results[0].state, 8), 0x1FFu);
    }

    // 6. Real data: vm_state_from_level keeps every row.  dm1 has 363 rows and
    // 8 duplicate ids (docs/ENTITY_FIELDS.md §10).
    if (fs::exists(fs::path(kGameDir) / "dm1.sbt")) {
        gm::SbtLevel level;
        if (load_level(fs::path(kGameDir) / "dm1.sbt", level)) {
            const gm::VmState state = gm::vm_state_from_level(level);
            CHECK_EQ(state.entities.size(), level.script.handles);
            std::set<int> ids;
            for (const gm::VmEntity& row : state.entities) {
                ids.insert(row.id);
            }
            CHECK_EQ(state.entities.size() - ids.size(), std::size_t{8}); // 363 - 355
            CHECK(std::any_of(ids.begin(), ids.end(), [&state](int id) {
                return state.entity_count(id) > 1;
            }));
        }
    }
}

// ---------------------------------------------------------------------------
// Sleeping-script table (0x5e3df0..0x5b29c8, docs/ENTITY_FIELDS.md §8)
// ---------------------------------------------------------------------------

void test_sleep_table() {
    // SLEEP 5; SLEEP 5; ADD_LOCATION (SLEEP is 1 + 4 bytes, so the second one
    // starts at pc 5 and the location body at pc 10).
    const Bytes program = [] {
        Bytes code;
        cmd(code, "SLEEP", {u(5)});
        cmd(code, "SLEEP", {u(5)});
        cmd(code, "ADD_LOCATION", {u(1), u(1), u(2), u(3), d(0.0), d(0.0), d(0.0)});
        code.push_back(0xFF);
        return code;
    }();

    // 1. Default (exploration) model: SLEEP is `clock += operand`, inline.
    {
        const auto results = run_bytes(program);
        CHECK_EQ(results.size(), std::size_t{1});
        CHECK_EQ(results[0].state.clock, 10);
        CHECK(!results[0].state.sleeping[0].active());
        CHECK_EQ(results[0].state.scene.locations.size(), std::size_t{1});
    }

    // 2. Engine model (sleep_queue): the rest of the script is suspended, the
    // slot keeps the continuation PC and the delay (0x45c49e/0x45c46c).
    gm::VmOptions options;
    options.sleep_queue = true;
    auto results = run_bytes(program, options);
    CHECK_EQ(results.size(), std::size_t{1});
    CHECK(results[0].reason == gm::VmReason::End);
    CHECK_EQ(results[0].state.clock, 0);
    CHECK(results[0].state.scene.locations.empty());
    const gm::VmSleepSlot& slot = results[0].state.sleeping[0];
    CHECK(slot.active());
    CHECK_EQ(slot.ticks, 5);
    CHECK_EQ(slot.offset, 5); // pc right after SLEEP
    CHECK_EQ(slot.source, 0xFFFFu); // no source player -> "no source" handle

    // 3. Ticker 0x457230: subtract, wake only when the timer runs out, and
    // report the overshoot as the carry (0x457356 passes -remaining).
    gm::VmState state = results[0].state;
    auto wakes = gm::vm_tick_sleepers(state, 3);
    CHECK(wakes.empty());
    CHECK_EQ(state.sleeping[0].ticks, 2);
    CHECK_EQ(state.clock, 3);
    wakes = gm::vm_tick_sleepers(state, 4);
    CHECK_EQ(wakes.size(), std::size_t{1});
    if (wakes.size() == 1) {
        CHECK_EQ(wakes[0].offset, 5u);
        CHECK_EQ(wakes[0].carry, 2);
    }
    CHECK(!state.sleeping[0].active()); // freed: offset = -1
    CHECK_EQ(state.sleeping[0].ticks, -2); // only the offset array is cleared

    // 4. Resume: the saved offset continues with the carry applied to the next
    // SLEEP (`delay = operand - carry`, 0x45c43b) — the second SLEEP asks for
    // 5 but 2 ticks were already spent past the deadline.
    if (wakes.size() == 1) {
        state.sleep_carry = wakes[0].carry;
        const auto resumed = run_from(state, program, wakes[0].offset, options);
        CHECK_EQ(resumed.size(), std::size_t{1});
        CHECK_EQ(resumed[0].state.scene.locations.size(), std::size_t{0});
        CHECK(resumed[0].state.sleeping[0].active());
        CHECK_EQ(resumed[0].state.sleeping[0].ticks, 3); // 5 - carry 2
        CHECK_EQ(resumed[0].state.sleeping[0].offset, 10);

        // And the last wake reaches the body: the script finally runs.
        gm::VmState after = resumed[0].state;
        const auto second = gm::vm_tick_sleepers(after, 3);
        CHECK_EQ(second.size(), std::size_t{1});
        if (second.size() == 1) {
            after.sleep_carry = second[0].carry;
            const auto done = run_from(after, program, second[0].offset, options);
            CHECK_EQ(done[0].state.scene.locations.size(), std::size_t{1});
        }
    }

    // 5. A SLEEP inside a generated sub-script suspends only that script: the
    // caller continues (the engine's nested 0x457390 returns, 0x45c4ba).
    {
        Bytes sub;
        cmd(sub, "SLEEP", {u(4)});
        cmd(sub, "ADD_LOCATION", {u(1), u(1), u(2), u(3), d(0.0), d(0.0), d(0.0)});
        sub.push_back(0xFF);
        Bytes code;
        cmd(code, "GENERATE", {u(5)});
        cmd(code, "ADD_LOCATION", {u(2), u(4), u(5), u(6), d(0.0), d(0.0), d(0.0)});
        code.push_back(0xFF);
        const auto offset = static_cast<std::uint32_t>(code.size());
        code.insert(code.end(), sub.begin(), sub.end());

        const auto nested = run_bytes(code, options, {{5, offset, 0x1FF}});
        CHECK_EQ(nested.size(), std::size_t{1});
        CHECK(nested[0].reason == gm::VmReason::End);
        CHECK_EQ(nested[0].state.scene.locations.size(), std::size_t{1});
        if (!nested[0].state.scene.locations.empty()) {
            CHECK_EQ(nested[0].state.scene.locations[0].thing_id, 2); // the caller
        }
        CHECK(nested[0].state.sleeping[0].active());
        CHECK_EQ(nested[0].state.sleeping[0].ticks, 4);
        CHECK_EQ(nested[0].state.sleeping[0].offset, offset + 5); // sub-script body
    }

    // 6. Slot exhaustion: "Maximum sleeping scripts (256 reached)" (0x4b8134)
    // drops the rest of the script (the engine returns from 0x457390).
    {
        gm::VmState full;
        for (gm::VmSleepSlot& s : full.sleeping) {
            s.offset = 0;
            s.ticks = 100;
        }
        const auto capped = run_from(std::move(full), program, 0, options);
        CHECK_EQ(capped.size(), std::size_t{1});
        CHECK(capped[0].reason == gm::VmReason::End);
        CHECK(capped[0].detail.find("maximum sleeping scripts") != std::string::npos);
        CHECK(capped[0].state.scene.locations.empty());

        // Inside a sub-script the caller continues: 0x457390 returns, exactly
        // like a successful SLEEP, and only the callee's remainder is lost.
        gm::VmState full2;
        for (gm::VmSleepSlot& s : full2.sleeping) {
            s.offset = 0;
            s.ticks = 100;
        }
        Bytes sub;
        cmd(sub, "SLEEP", {u(4)});
        cmd(sub, "ADD_LOCATION", {u(1), u(1), u(2), u(3), d(0.0), d(0.0), d(0.0)});
        sub.push_back(0xFF);
        Bytes caller;
        cmd(caller, "GENERATE", {u(5)});
        cmd(caller, "ADD_LOCATION", {u(2), u(4), u(5), u(6), d(0.0), d(0.0), d(0.0)});
        caller.push_back(0xFF);
        const auto sub_offset = static_cast<std::uint32_t>(caller.size());
        caller.insert(caller.end(), sub.begin(), sub.end());
        full2.entities.push_back(gm::VmEntity{5, 0x1FF, sub_offset});
        const auto nested = run_from(std::move(full2), caller, 0, options);
        CHECK_EQ(nested.size(), std::size_t{1});
        CHECK(nested[0].reason == gm::VmReason::End);
        CHECK_EQ(nested[0].state.scene.locations.size(), std::size_t{1});
        if (!nested[0].state.scene.locations.empty()) {
            CHECK_EQ(nested[0].state.scene.locations[0].thing_id, 2); // the caller
        }
    }

    // 7. Loader layout (0x462a80): nine 256-word arrays, ticks first, offset
    // second, then the six parameters and the source handle.
    {
        Bytes tables(gm::kVmSleepSlots * 4 * 9, 0);
        auto put = [&tables](std::size_t table, std::size_t slot, std::uint32_t value) {
            const std::size_t at = (table * gm::kVmSleepSlots + slot) * 4;
            tables[at] = static_cast<std::uint8_t>(value & 0xFF);
            tables[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
            tables[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
            tables[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
        };
        for (std::size_t i = 0; i < gm::kVmSleepSlots; ++i) {
            put(0, i, static_cast<std::uint32_t>(100 + i)); // 0x5e4278 ticks
            put(1, i, static_cast<std::uint32_t>(1000 + i)); // 0x5e3df0 offset
            for (std::size_t k = 0; k < 6; ++k) {
                put(2 + k, i, static_cast<std::uint32_t>(10 * k + i));
            }
            put(8, i, 7); // 0x5b29c8 source handle
        }
        gm::VmState loaded;
        gm::vm_load_sleeping_scripts(loaded, tables);
        CHECK_EQ(loaded.sleeping[3].ticks, 103);
        CHECK_EQ(loaded.sleeping[3].offset, 1003);
        for (std::size_t k = 0; k < 6; ++k) {
            CHECK_EQ(loaded.sleeping[3].params[k], static_cast<std::uint32_t>(10 * k + 3));
        }
        CHECK_EQ(loaded.sleeping[3].source, 7u);
        CHECK_EQ(loaded.sleeping[255].offset, 1255);

        // All 36 shipped levels carry flag = 0; a wrong size clears the table.
        gm::vm_load_sleeping_scripts(loaded, std::span<const std::uint8_t>(tables).first(16));
        CHECK(!loaded.sleeping[3].active());
        CHECK_EQ(loaded.sleeping[3].ticks, 0);
    }

    // 8. STOP_SOURCEPLAYER_SLEEPING_SCRIPTS (cid 198, 0x45b93e) frees every
    // slot whose saved source handle matches, and only those.
    {
        gm::VmState state2;
        state2.sleeping[0] = gm::VmSleepSlot{5, 3, {}, 3};
        state2.sleeping[1] = gm::VmSleepSlot{9, 4, {}, 5};
        state2.sleeping[2] = gm::VmSleepSlot{11, 5, {}, 3};
        CHECK_EQ(gm::vm_stop_sleeping(state2, 3), std::size_t{2});
        CHECK(!state2.sleeping[0].active());
        CHECK(state2.sleeping[1].active());
        CHECK(!state2.sleeping[2].active());
        CHECK_EQ(gm::vm_stop_sleeping(state2, 0xFFFF), std::size_t{0});

        // Through the command: the handler uses the source player's handle.
        gm::VmState state3;
        state3.sleeping[0] = gm::VmSleepSlot{5, 3, {}, 4};
        state3.source_player = 4;
        Bytes code;
        cmd(code, "STOP_SOURCEPLAYER_SLEEPING_SCRIPTS", {});
        code.push_back(0xFF);
        const auto stopped = run_from(state3, code, 0);
        CHECK(!stopped[0].state.sleeping[0].active());
    }

    // 9. Wake order is slot order (0x457230 walks 0..255); the table of a
    // shipped level is empty (flag = 0 in all 36 files).
    {
        gm::VmState state4;
        state4.sleeping[0] = gm::VmSleepSlot{5, 1, {}, 1};
        state4.sleeping[2] = gm::VmSleepSlot{9, 1, {}, 2};
        const auto order = gm::vm_tick_sleepers(state4, 1);
        CHECK_EQ(order.size(), std::size_t{2});
        if (order.size() == 2) {
            CHECK_EQ(order[0].offset, 5u);
            CHECK_EQ(order[1].offset, 9u);
            CHECK_EQ(order[0].carry, 0);
        }
        if (fs::exists(fs::path(kGameDir) / "dm1.sbt")) {
            gm::SbtLevel level;
            if (load_level(fs::path(kGameDir) / "dm1.sbt", level)) {
                const gm::VmState level_state = gm::vm_state_from_level(level);
                for (const gm::VmSleepSlot& s : level_state.sleeping) {
                    CHECK(!s.active());
                }
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    bool full = false;
    for (int k = 1; k < argc; ++k) {
        if (std::string_view(argv[k]) == "--full") {
            full = true;
        }
    }

    test_synthetic();
    test_sector_stuff();
    test_sector_source_and_handles();
    test_entity_rows();
    test_sleep_table();

    std::error_code ec;
    if (!fs::is_directory(kGameDir, ec)) {
        std::printf("vm: game directory not found, skipping level goldens\n");
        if (g_failures != 0) {
            std::fprintf(stderr, "vm: %d failure(s)\n", g_failures);
            return 1;
        }
        std::printf("vm: ok (synthetic only)\n");
        return 0;
    }

    test_main_scripts();

    if (full) {
        for (const LevelGolden& golden : kLevels) {
            check_level(fs::path(kGameDir) / golden.name, golden);
        }
        test_corpus();
    } else {
        for (const char* name : {"level0.sbt", "dm1.sbt", "level1.sbt"}) {
            const LevelGolden* golden = find_golden(name);
            CHECK(golden != nullptr);
            if (golden != nullptr) {
                check_level(fs::path(kGameDir) / name, *golden);
            }
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "vm: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vm: ok\n");
    return 0;
}
