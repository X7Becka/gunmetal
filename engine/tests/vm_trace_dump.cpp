// vm_trace_dump — the VM instruction trace in a language-neutral form.
//
// `tools/re/vm_trace_check.py` runs this binary and the Python reference
// (`gm/sbtvm_exec.py`) on the same level and diffs the two dumps, closing
// M5.3.2 (a trace must not depend on the implementation language).  Numbers
// are printed without locale or float formatting: reals as the raw IEEE-754
// bits, strings as hex bytes, so both sides compare exact values.
//
// Usage:
//   vm_trace_dump <level.sbt> [--block N]... [--all] [--limit N]
//
// Output per block:
//   block <index> eid=<id> off=<hex> paths=<n>
//   path <j> reason=<name> steps=<n> pc=<hex>
//   ev <pc> <depth> <name> <nargs> [<arg>...]
// where an arg is `i:<decimal>` (u32), `d:<16 hex>` (double bits) or
// `s:<hex>` (byte string).
#include "core/bytes.h"
#include "res/sbtvm.h"
#include "res/vm_exec.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

std::string hex_bytes(const std::string& text) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char c : text) {
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0xf]);
    }
    return out;
}

std::string format_arg(const gm::SbtValue& value) {
    switch (value.kind) {
    case gm::SbtValue::Kind::UInt:
        return "i:" + std::to_string(value.u);
    case gm::SbtValue::Kind::Real: {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value.d), "double is 8 bytes");
        std::memcpy(&bits, &value.d, sizeof(bits));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "d:%016" PRIx64, bits);
        return buf;
    }
    case gm::SbtValue::Kind::String:
        return "s:" + hex_bytes(value.s);
    }
    return "?";
}

// Entity-table offsets, in handle order, first occurrence wins (the same walk
// as engine/tests/test_vm_exec.cpp and tools/re/vm_exec.py).
std::vector<std::pair<int, std::size_t>> block_offsets(const gm::SbtLevel& level) {
    std::vector<std::pair<int, std::size_t>> out;
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

void dump_block(std::size_t index, int entity, std::size_t offset,
                const std::vector<gm::VmPathResult>& paths) {
    std::printf("block %zu eid=%d off=0x%x paths=%zu\n", index, entity,
                static_cast<unsigned>(offset), paths.size());
    for (std::size_t j = 0; j < paths.size(); ++j) {
        const gm::VmPathResult& path = paths[j];
        std::printf("path %zu reason=%s steps=%d pc=0x%x\n", j,
                    std::string(gm::vm_reason_name(path.reason)).c_str(),
                    path.steps, static_cast<unsigned>(path.pc));
        for (const gm::VmTraceEvent& event : path.state.trace) {
            std::printf("ev 0x%x %d %s %zu", static_cast<unsigned>(event.pc),
                        event.depth, event.name.c_str(), event.args.size());
            for (const gm::SbtValue& arg : event.args) {
                std::printf(" %s", format_arg(arg).c_str());
            }
            std::printf("\n");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <level.sbt> [--block N]... [--all] [--limit N]\n",
                     argv[0]);
        return 2;
    }
    const std::string level_path = argv[1];
    std::vector<int> wanted;
    bool all = false;
    long limit = 100000;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--all") {
            all = true;
        } else if (arg == "--block" && i + 1 < argc) {
            wanted.push_back(std::atoi(argv[++i]));
        } else if (arg == "--limit" && i + 1 < argc) {
            limit = std::atol(argv[++i]);
        } else {
            std::fprintf(stderr, "vm_trace_dump: bad option %s\n", arg.c_str());
            return 2;
        }
    }

    const std::vector<std::uint8_t> bytes = read_all(level_path);
    if (bytes.empty()) {
        std::fprintf(stderr, "vm_trace_dump: cannot read %s\n", level_path.c_str());
        return 1;
    }
    std::string error;
    auto parsed = gm::parse_sbt(bytes, error);
    if (!parsed) {
        std::fprintf(stderr, "vm_trace_dump: %s: %s\n", level_path.c_str(),
                     error.c_str());
        return 1;
    }
    const gm::SbtLevel& level = *parsed;

    const auto blocks = block_offsets(level);
    std::printf("# level %s blocks=%zu\n", level_path.c_str(), blocks.size());

    gm::VmOptions options;
    options.max_time = 0.0;
    options.trace = true;
    options.trace_limit = static_cast<std::size_t>(limit);
    gm::VmExecutor executor(gm::vm_state_from_level(level), options);

    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (!all && !wanted.empty() &&
            std::find(wanted.begin(), wanted.end(), static_cast<int>(index)) ==
                wanted.end()) {
            continue;
        }
        const auto& [entity, offset] = blocks[index];
        dump_block(index, entity, offset, executor.run(offset, entity));
    }
    return 0;
}
