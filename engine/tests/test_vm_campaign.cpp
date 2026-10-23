// Campaign counter in the VM (`M9.1`, docs/CAMPAIGN_FLOW.md §4).
//
// The original keeps the mission number in `ds:0x80f9cc`: a new game sets 1,
// `NEXT_LEVEL` with no operand builds `LEVEL<n>` from it and *then* increments
// (0x457b17..0x457b2f), and `NEXT_LEVEL_NUMBER n` (0x45ea26) compares it.
// None of the 36 shipped scripts uses `NEXT_LEVEL_NUMBER` (measured: 0 sites),
// so the behaviour is pinned with synthetic byte code.
#include "res/vm_exec.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using gm::SbtValue;
using Bytes = std::vector<std::uint8_t>;

int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va = (a);                                                         \
        const auto vb = (b);                                                         \
        if (!(va == vb)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a,   \
                         #b);                                                            \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

SbtValue u(std::uint32_t value) { return SbtValue::from_uint(value); }

SbtValue s(const char* text) { return SbtValue::from_string(text); }

void put_u32(Bytes& code, std::uint32_t value) {
    code.push_back(static_cast<std::uint8_t>(value & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void encode_args(Bytes& code, std::string_view sig, const std::vector<SbtValue>& args) {
    std::size_t index = 0;
    for (const char letter : sig) {
        if (index >= args.size()) {
            return;
        }
        const SbtValue& value = args[index++];
        if (letter == 'S') {
            const std::string text =
                value.kind == SbtValue::Kind::String ? value.s : std::to_string(value.u);
            code.insert(code.end(), text.begin(), text.end());
            code.push_back(0);
        } else {
            put_u32(code, value.u);
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
             const Bytes& then) {
    Bytes code;
    code.push_back(199); // IF
    put_condition(code, condition, args);
    put_u32(code, static_cast<std::uint32_t>(4 + then.size()));
    code.insert(code.end(), then.begin(), then.end());
    return code;
}

std::optional<gm::VmPathResult> run(const Bytes& code, int mission_counter) {
    gm::VmState state;
    state.code = code;
    state.rng = 1;
    state.mission_counter = mission_counter;
    state.players[0] = gm::VmPlayer{};
    gm::VmOptions options;
    options.max_time = 0.0; // deterministic: no wall clock in tests
    gm::VmExecutor executor(std::move(state), options);
    const auto paths = executor.run(0);
    if (paths.empty()) {
        return std::nullopt;
    }
    return paths.front();
}

void test_next_level_steps_the_counter() {
    Bytes code;
    cmd(code, "NEXT_LEVEL", {});
    const auto path = run(code, 1);
    CHECK(path.has_value());
    if (!path) {
        return;
    }
    // 0x457b11: build "LEVEL<n>" from the counter, then ++.
    CHECK(path->reason == gm::VmReason::LevelTransition);
    CHECK_EQ(path->detail, std::string("NEXT_LEVEL LEVEL1"));
    CHECK_EQ(path->state.mission_counter, 2);

    // The next step reads the counter back: 2 -> "LEVEL2" -> 3.
    const auto again = run(code, 2);
    CHECK(again.has_value());
    if (again) {
        CHECK_EQ(again->detail, std::string("NEXT_LEVEL LEVEL2"));
        CHECK_EQ(again->state.mission_counter, 3);
    }
}

void test_next_level_number_compares_the_counter() {
    Bytes then;
    cmd(then, "ADD_SCREEN_TEXT", {u(1), s("REACHED")});
    const Bytes code = enc_if("NEXT_LEVEL_NUMBER", {u(2)}, then);

    const auto hit = run(code, 2);
    CHECK(hit.has_value());
    if (hit) {
        CHECK_EQ(hit->state.screen_text.size(), std::size_t{1});
        if (!hit->state.screen_text.empty()) {
            CHECK_EQ(hit->state.screen_text[0].second, std::string("REACHED"));
        }
    }
    const auto miss = run(code, 1); // counter 1 != 2
    CHECK(miss.has_value());
    if (miss) {
        CHECK(miss->state.screen_text.empty());
    }
}

void test_new_game_resets_to_one() {
    // A fresh state starts the campaign at mission 1 (0x442a0e).
    Bytes then;
    cmd(then, "ADD_SCREEN_TEXT", {u(1), s("ONE")});
    const Bytes code = enc_if("NEXT_LEVEL_NUMBER", {u(1)}, then);
    const auto path = run(code, 1);
    CHECK(path.has_value());
    if (path) {
        CHECK_EQ(path->state.screen_text.size(), std::size_t{1});
    }
}

} // namespace

int main() {
    test_next_level_steps_the_counter();
    test_next_level_number_compares_the_counter();
    test_new_game_resets_to_one();
    if (g_failures != 0) {
        std::fprintf(stderr, "vm_campaign: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vm_campaign: ok\n");
    return 0;
}
