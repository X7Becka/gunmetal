// Content-addressed game files for the C++ tests.
//
// The golden tables name levels the way docs/manifests/gunmetal-3dfx-136.sha256
// does.  An installation may have those files renamed or swapped (this one
// ships level1/level2 swapped), and a name-based lookup would then compare the
// wrong bytes.  kGameContent stores the FNV-1a64 of the *canonical* content;
// these helpers find the file that actually holds it.
//
// Regenerate the table with: python3 tools/re/game_content.py --write
#pragma once

#include "core/hash.h"
#include "game_content.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gmtest {

namespace fs = std::filesystem;

inline std::vector<std::uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

inline std::optional<std::uint64_t> canonical_fnv(const std::string& name) {
    for (const GameContent& row : kGameContent) {
        if (name == row.name) {
            return row.fnv;
        }
    }
    return std::nullopt;
}

inline const char* canonical_name(std::uint64_t fnv) {
    for (const GameContent& row : kGameContent) {
        if (fnv == row.fnv) {
            return row.name;
        }
    }
    return nullptr;
}

inline std::optional<std::uint64_t> file_fnv(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    const auto bytes = read_all(path);
    if (bytes.empty()) {
        return std::nullopt;
    }
    return gm::fnv1a64(std::span<const std::uint8_t>(bytes));
}

/// Path in *dir* whose content is the canonical *name*, or nullopt.
///
/// The fast path checks `dir/name`; on a mismatch it scans the same extension
/// (36 files, a few MB) and compares FNV-1a64 of the bytes.
inline std::optional<fs::path> resolve_game_file(const fs::path& dir,
                                                 const std::string& name) {
    const auto want = canonical_fnv(name);
    if (!want) {
        std::error_code ec;
        const fs::path direct = dir / name;
        return fs::is_regular_file(direct, ec) ? std::optional<fs::path>(direct)
                                               : std::nullopt;
    }
    if (const auto got = file_fnv(dir / name); got && *got == *want) {
        return dir / name;
    }
    std::error_code ec;
    const std::string ext = fs::path(name).extension().string();
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() != ext) {
            continue;
        }
        if (const auto got = file_fnv(entry.path()); got && *got == *want) {
            return entry.path();
        }
    }
    return std::nullopt;
}

} // namespace gmtest
