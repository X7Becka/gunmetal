// VM music and overlay text state (`M5.2.5`).
//
// `LOAD_MUSIC`/`PLAY_MUSIC`/`STOP_MUSIC`/`UNLOAD_MUSIC` and the two
// memorise/replay commands used to fall through to the generic event log: the
// trace was right, the state was not.  The executor now keeps the bank table
// and the playing bank so the runtime can hand them to the mixer, and the
// screen-text commands keep their `(slot, text)` lines in script order.
//
// The event is still logged exactly once per command, so the C++/Python trace
// comparison (`vm_trace`) does not move.
#include "res/vm_exec.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

std::optional<gm::VmPathResult> run(const Bytes& code) {
    gm::VmState state;
    state.code = code;
    state.rng = 1;
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

void test_banks_and_playback() {
    Bytes code;
    cmd(code, "LOAD_MUSIC", {u(3), s("L00D")});
    cmd(code, "LOAD_MUSIC", {u(4), s("L00E")});
    cmd(code, "PLAY_MUSIC", {u(3)});
    cmd(code, "STOP_MUSIC", {});
    code.push_back(0xFF); // END

    const auto path = run(code);
    CHECK(path.has_value());
    if (!path) {
        return;
    }
    const auto& state = path->state;
    CHECK(state.music_banks.size() == 2);
    CHECK_EQ(state.music_banks.at(3), std::string("L00D"));
    CHECK_EQ(state.music_banks.at(4), std::string("L00E"));
    CHECK(state.music_bank.has_value());
    CHECK_EQ(*state.music_bank, 3);
    CHECK(!state.music_playing); // STOP_MUSIC (83) came last

    // The generic event log is unchanged: one entry per command, so vm_trace
    // keeps comparing the same stream as gm/sbtvm_exec.py.
    int loads = 0;
    int plays = 0;
    int stops = 0;
    for (const auto& event : state.events) {
        loads += event.name == "LOAD_MUSIC" ? 1 : 0;
        plays += event.name == "PLAY_MUSIC" ? 1 : 0;
        stops += event.name == "STOP_MUSIC" ? 1 : 0;
    }
    CHECK_EQ(loads, 2);
    CHECK_EQ(plays, 1);
    CHECK_EQ(stops, 1);
}

void test_unload_and_memorize() {
    Bytes code;
    cmd(code, "LOAD_MUSIC", {u(0), s("A")});
    cmd(code, "PLAY_MUSIC", {u(0)});
    cmd(code, "MEMORIZE_CURRENT_MUSIC_BANK", {});
    cmd(code, "STOP_MUSIC", {});
    cmd(code, "PLAY_MEMORIZED_MUSIC_BANK", {});
    code.push_back(0xFF);

    const auto path = run(code);
    CHECK(path.has_value());
    if (!path) {
        return;
    }
    const auto& state = path->state;
    CHECK(state.music_memorized.has_value());
    CHECK_EQ(*state.music_memorized, 0);
    CHECK(state.music_playing); // replayed after the stop
    CHECK_EQ(*state.music_bank, 0);

    Bytes unload;
    cmd(unload, "LOAD_MUSIC", {u(7), s("B")});
    cmd(unload, "PLAY_MUSIC", {u(7)});
    cmd(unload, "UNLOAD_MUSIC", {u(7)});
    unload.push_back(0xFF);
    const auto after = run(unload);
    CHECK(after.has_value());
    if (after) {
        CHECK(after->state.music_banks.empty());
        CHECK(!after->state.music_playing); // unloading the playing bank stops it
    }
}

void test_screen_text() {
    Bytes code;
    cmd(code, "ADD_SCREEN_TEXT", {u(1), s("HELLO")});
    cmd(code, "ADD_SOURCESCREEN_TEXT", {u(2), s("THIS DOOR CANNOT BE OPENED")});
    code.push_back(0xFF);

    const auto path = run(code);
    CHECK(path.has_value());
    if (!path) {
        return;
    }
    const auto& text = path->state.screen_text;
    CHECK_EQ(text.size(), std::size_t{2});
    if (text.size() == 2) {
        CHECK_EQ(text[0].first, 1);
        CHECK_EQ(text[0].second, std::string("HELLO"));
        CHECK_EQ(text[1].first, 2);
        CHECK_EQ(text[1].second, std::string("THIS DOOR CANNOT BE OPENED"));
    }
}

} // namespace

int main() {
    test_banks_and_playback();
    test_unload_and_memorize();
    test_screen_text();
    if (g_failures != 0) {
        std::fprintf(stderr, "vm_music: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vm_music: ok\n");
    return 0;
}
