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
    {"level2.sbt", 24},  {"level20.sbt", 36}, {"level21.sbt", 22},  {"level22.sbt", 22},
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
};

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
        state.entities[row.id] = entity;
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

    // 8. Recursion: GENERATE itself.
    Bytes generate;
    cmd(generate, "GENERATE", {u(7)});
    generate.push_back(0xFF);
    results = run_bytes(generate, {}, {{7, 0}});
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
    CHECK_EQ(results[0].state.entities.at(9).flags & 0x1FFu, 0u);

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

} // namespace

int main(int argc, char** argv) {
    bool full = false;
    for (int k = 1; k < argc; ++k) {
        if (std::string_view(argv[k]) == "--full") {
            full = true;
        }
    }

    test_synthetic();

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
