// HUD films: .sof tile codec and .fdx indexes.
//
// The golden values come from gm/sof.py on the shipped install (the same bytes
// as docs/FILM.md): 48 files, 11637 tiles, FNV-1a64 of every decoded frame
// (files ordered case-insensitively) = 8e1f0f6d9e5c88bd.  All 11637 stored
// tiles must survive encode(decode(tile)) unchanged.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "res/sof.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::uint64_t fnv1a(std::uint64_t h, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    const fs::path dir = game / "Film";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::cout << "sof: game not installed, skipped\n";
        return 0;
    }

    std::vector<fs::path> films;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (lower(entry.path().extension().string()) == ".sof") {
            films.push_back(entry.path());
        }
    }
    std::sort(films.begin(), films.end(), [](const fs::path& a, const fs::path& b) {
        return lower(a.filename().string()) < lower(b.filename().string());
    });
    check(films.size() == 48, "48 shipped .sof files (found " +
                                  std::to_string(films.size()) + ")");

    std::uint64_t aggregate = 0xcbf29ce484222325ull;
    std::size_t tiles = 0;
    for (const auto& path : films) {
        const auto bytes = read_file(path);
        std::string error;
        const auto film = gm::parse_sof(bytes, error);
        check(film.has_value(), path.filename().string() + ": " + error);
        if (!film) {
            continue;
        }
        check(film->aligned(), path.filename().string() + ": tile-aligned");
        if (path.filename().string() == "ammo0.sof") {
            check(film->tiles() == 132, "ammo0.sof has 132 tiles");
        }
        if (lower(path.filename().string()) == "weapon8.sof") {
            check(film->tiles() == 137, "weapon8.sof has 137 tiles");
        }
        for (std::size_t i = 0; i < film->tiles(); ++i) {
            const auto stored = film->tile(i);
            check(stored.has_value(), "tile in range");
            if (!stored) {
                continue;
            }
            const auto pixels = gm::decode_sof_tile(*stored);
            const auto round = gm::encode_sof_tile(pixels);
            check(round.size() == stored->size() &&
                      std::equal(round.begin(), round.end(), stored->begin()),
                  path.filename().string() + " tile " + std::to_string(i) +
                      ": encode(decode) round-trip");
            aggregate = fnv1a(aggregate, pixels.data(), pixels.size());
            ++tiles;
        }
        check(film->to_bytes() == bytes, path.filename().string() + ": to_bytes");
    }
    check(tiles == 11637, "11637 decoded frames (found " + std::to_string(tiles) + ")");
    check(aggregate == 0x8e1f0f6d9e5c88bdull,
          "decoded-frame FNV-1a64 matches gm/sof.py");

    // .fdx: six indexes, counts (16, 1, 6, 1, 8, 24), every record count/fps.
    const std::vector<std::pair<std::string, std::size_t>> expected = {
        {"ammo.fdx", 16},   {"default.fdx", 1}, {"equip.fdx", 6},
        {"shield.fdx", 1},  {"tank.fdx", 8},    {"weapon.fdx", 24},
    };
    std::size_t fdx_files = 0;
    for (const auto& [name, count] : expected) {
        const fs::path path = dir / name;
        if (!fs::is_regular_file(path, ec)) {
            check(false, name + ": missing");
            continue;
        }
        ++fdx_files;
        const auto bytes = read_file(path);
        std::string error;
        const auto fdx = gm::parse_fdx(bytes, error);
        check(fdx.has_value(), name + ": " + error);
        if (!fdx) {
            continue;
        }
        check(fdx->entries.size() == count,
              name + ": " + std::to_string(fdx->entries.size()) + " entries, expected " +
                  std::to_string(count));
        for (const auto& entry : fdx->entries) {
            check(entry.count > 0 && entry.fps > 0, name + ": count/fps positive");
            check(entry.fps == 24, name + ": fps 24");
        }
        check(fdx->to_bytes() == bytes, name + ": to_bytes");
        const auto built = gm::build_fdx(fdx->entries);
        check(built == bytes, name + ": canonical build_fdx is byte-identical");
    }
    check(fdx_files == 6, "6 .fdx indexes");

    // Reject bad indexes like gm/sof.py: "0" fps and a non-numeric line.
    {
        const std::vector<std::uint8_t> bad = {'1', '2', ',', '0', '\r', '\n'};
        std::string error;
        check(!gm::parse_fdx(bad, error), "fdx rejects fps 0");
        const std::vector<std::uint8_t> junk = {'x', ',', '2', '\n'};
        error.clear();
        check(!gm::parse_fdx(junk, error), "fdx rejects non-numeric count");
    }

    if (failures == 0) {
        std::cout << "sof: 48 films, 11637 tiles, 6 fdx, FNV 8e1f0f6d9e5c88bd, ok\n";
    }
    return failures == 0 ? 0 : 1;
}
