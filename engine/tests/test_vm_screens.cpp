// Screen requests from the level script (`M8.2`/`M9.5` seam).
//
// `SHOW_LEVEL_END_SCREEN` (91), the shop screens (189..193), `SHOW_BRIEFING_SCREEN`
// (194) and `SHOW_HELP_SCREEN` (195) used to fall through to the event log
// only.  The executor now records the request (name, operands, site) so the
// frontend can act on it, and still logs the one event the trace had.
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

void cmd(Bytes& code, std::string_view name) {
    const auto info = gm::vm_command_info(name);
    CHECK(info.has_value());
    if (info) {
        code.push_back(info->op);
    }
}

std::optional<gm::VmPathResult> run(const Bytes& code) {
    gm::VmState state;
    state.code = code;
    state.rng = 1;
    state.players[0] = gm::VmPlayer{};
    gm::VmOptions options;
    options.max_time = 0.0;
    gm::VmExecutor executor(std::move(state), options);
    const auto paths = executor.run(0);
    if (paths.empty()) {
        return std::nullopt;
    }
    return paths.front();
}

void test_requests_in_order() {
    Bytes code;
    cmd(code, "SHOW_LEVEL_END_SCREEN");
    cmd(code, "SHOW_BRIEFING_SCREEN");
    cmd(code, "SHOW_HELP_SCREEN");
    cmd(code, "SHOW_AMMO_BUY_SCREEN");
    code.push_back(0xFF); // END

    const auto path = run(code);
    CHECK(path.has_value());
    if (!path) {
        return;
    }
    const auto& screens = path->state.screens;
    CHECK(screens.size() == 4);
    if (screens.size() == 4) {
        CHECK(screens[0].name == "SHOW_LEVEL_END_SCREEN");
        CHECK(screens[1].name == "SHOW_BRIEFING_SCREEN");
        CHECK(screens[2].name == "SHOW_HELP_SCREEN");
        CHECK(screens[3].name == "SHOW_AMMO_BUY_SCREEN");
        // Sites are instruction offsets, strictly increasing in script order.
        CHECK(screens[0].site < screens[1].site);
        CHECK(screens[1].site < screens[2].site);
        CHECK(screens[2].site < screens[3].site);
    }

    // The generic event stream is unchanged (one entry per request).
    int events = 0;
    for (const auto& event : path->state.events) {
        if (event.name.rfind("SHOW_", 0) == 0 || event.name == "REFRESH_SCREEN") {
            ++events;
        }
    }
    CHECK(events == 4);
}

void test_refresh_is_recorded() {
    Bytes code;
    cmd(code, "REFRESH_SCREEN");
    code.push_back(0xFF);
    const auto path = run(code);
    CHECK(path.has_value());
    if (path) {
        CHECK(path->state.screens.size() == 1);
        if (!path->state.screens.empty()) {
            CHECK(path->state.screens[0].name == "REFRESH_SCREEN");
            CHECK(path->state.screens[0].args.empty());
        }
    }
}

} // namespace

int main() {
    test_requests_in_order();
    test_refresh_is_recorded();
    if (g_failures != 0) {
        std::fprintf(stderr, "vm_screens: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vm_screens: ok\n");
    return 0;
}
