// Cross-language parser parity (M3.2.7): C++ readers vs Python's fixtures.
//
// `tools/re/export_parity.py` parses 36 levels, the texture archive and a slice
// of the model corpus with `gm/*` and writes the expected numbers; this test
// re-parses the same bytes with `res/*` and compares every `key=value`.  The
// point is that Python is not needed at test time: the fixture file is the
// contract, so a drift on either side becomes a concrete mismatch.
//
// The corpus is read-only.
//
// Usage: gm_parity_tests <fixtures.txt>   (game root: $GUNMETAL_DIR or default)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/hash.h"
#include "res/bigfile.h"
#include "res/sbc.h"
#include "res/sbm.h"
#include "res/sbt.h"
#include "res/sbw.h"
#include "res/sml.h"
#include "res/sob.h"
#include "res/sou.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
long g_checks = 0;
long g_values = 0;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

std::string hex64(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%016llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

void report_mismatch(const std::string& kind, const std::string& name,
              const std::string& key, const std::string& expected,
              const std::string& got) {
    std::printf("MISMATCH %s %s %s: expected %s, got %s\n", kind.c_str(),
                name.c_str(), key.c_str(), expected.c_str(), got.c_str());
    ++g_failures;
}

void verify(const std::string& kind, const std::string& name,
            const std::map<std::string, std::string>& expected,
            const std::map<std::string, std::string>& actual) {
    ++g_checks;
    for (const auto& [key, value] : expected) {
        ++g_values;
        const auto it = actual.find(key);
        if (it == actual.end()) {
            report_mismatch(kind, name, key, value, "<absent>");
        } else if (it->second != value) {
            report_mismatch(kind, name, key, value, it->second);
        }
    }
    for (const auto& [key, value] : actual) {
        if (expected.find(key) == expected.end()) {
            report_mismatch(kind, name, key, "<absent>", value);
        }
    }
}

/// Texture archives are opened once; `PICS/SBM.IDX#7` names entry 7 by index.
const std::vector<std::vector<std::uint8_t>>& archive_blobs(
    const fs::path& game, const std::string& index_path, std::string& error) {
    static std::map<std::string, std::vector<std::vector<std::uint8_t>>> cache;
    static std::map<std::string, std::string> errors;
    const std::string key = (game / index_path).string();
    const auto hit = cache.find(key);
    if (hit != cache.end()) {
        return hit->second;
    }
    auto& blobs = cache[key];
    const auto archive = gm::BigFile::open(game / index_path, error);
    if (!archive) {
        errors[key] = error;
        return blobs;
    }
    for (const auto& entry : archive->entries()) {
        std::string read_error;
        if (auto data = archive->read(entry, read_error)) {
            blobs.push_back(std::move(*data));
        } else {
            blobs.emplace_back();
        }
    }
    return blobs;
}

void check_sbw(const fs::path& game, const std::string& name,
               const std::map<std::string, std::string>& expected) {
    const auto bytes = read_file(game / name);
    std::string error;
    const auto level = gm::parse_sbw(bytes, error);
    if (!level) {
        report_mismatch("sbw", name, "parse", "ok", error);
        return;
    }
    verify("sbw", name, expected,
           {{"tex", std::to_string(level->header.textures.size())},
            {"effects", std::to_string(level->header.effects.size())},
            {"sectors", std::to_string(level->header.sectors.size())},
            {"objects", std::to_string(level->header.object_count)},
            {"verts", std::to_string(level->total_vertices)},
            {"polys", std::to_string(level->total_polygons)}});
}

void check_sbt(const fs::path& game, const std::string& name,
               const std::map<std::string, std::string>& expected) {
    const auto bytes = read_file(game / name);
    std::string error;
    const auto level = gm::parse_sbt(bytes, error);
    if (!level) {
        report_mismatch("sbt", name, "parse", "ok", error);
        return;
    }
    std::map<std::string, std::string> actual;
    actual["records"] = std::to_string(level->records.size());
    std::map<int, long> per_type;
    for (const auto& record : level->records) {
        ++per_type[record.type];
    }
    for (const auto& [type, count] : per_type) {
        actual["t" + std::to_string(type)] = std::to_string(count);
    }
    long named = 0;
    if (level->sound_samples) {
        for (const auto& sample_name : level->sound_samples->names) {
            if (!sample_name.empty()) {
                ++named;
            }
        }
    }
    actual["names"] = std::to_string(named);
    verify("sbt", name, expected, actual);
}

void check_sbc(const fs::path& game, const std::string& name,
               const std::map<std::string, std::string>& expected) {
    const auto bytes = read_file(game / name);
    std::string error;
    const auto blob = gm::parse_sbc(bytes, error);
    if (!blob) {
        report_mismatch("sbc", name, "parse", "ok", error);
        return;
    }
    std::vector<std::uint8_t> pixels;
    for (const auto& record : blob->records) {
        pixels.insert(pixels.end(), record.pixels.begin(), record.pixels.end());
    }
    verify("sbc", name, expected,
           {{"magic", blob->magic},
            {"records", std::to_string(blob->records.size())},
            {"pix", hex64(gm::fnv1a64(pixels))}});
}

void check_sou(const fs::path& game, const std::string& name,
               const std::map<std::string, std::string>& expected) {
    const auto hash = name.rfind('#');
    if (hash == std::string::npos) {
        report_mismatch("sou", name, "name", "path#index", "no index");
        return;
    }
    const std::string index_path = name.substr(0, hash);
    const long index = std::strtol(name.c_str() + hash + 1, nullptr, 10);
    std::string error;
    const auto& blobs = archive_blobs(game, index_path, error);
    if (static_cast<long>(blobs.size()) <= index) {
        report_mismatch("sou", name, "entry", "<present>",
                        error.empty() ? "index out of range" : error);
        return;
    }
    const auto sound = gm::parse_sou(blobs[static_cast<std::size_t>(index)], error);
    if (!sound) {
        report_mismatch("sou", name, "parse", "ok", error);
        return;
    }
    verify("sou", name, expected,
           {{"magic", sound->magic},
            {"rate", std::to_string(sound->rate)},
            {"flags", std::to_string(sound->flags)},
            {"len", std::to_string(sound->payload.size())}});
}

void check_sml(const fs::path& game, const std::string& name,
               const std::map<std::string, std::string>& expected) {
    const auto bytes = read_file(game / name);
    std::string error;
    const auto document = gm::parse_sml(bytes, error);
    if (!document) {
        report_mismatch("sml", name, "parse", "ok", error);
        return;
    }
    const auto* text = reinterpret_cast<const std::uint8_t*>(document->text.data());
    verify("sml", name, expected,
           {{"chars", std::to_string(document->text.size())},
            {"tags", std::to_string(document->tags.size())},
            {"pix", hex64(gm::fnv1a64(
                        std::span<const std::uint8_t>(text, document->text.size())))}});
}

void check_sbm(const fs::path& game, const std::string& name,
               const std::map<std::string, std::string>& expected) {
    const auto hash = name.rfind('#');
    if (hash == std::string::npos) {
        report_mismatch("sbm", name, "name", "path#index", "no index");
        return;
    }
    const std::string index_path = name.substr(0, hash);
    const long index = std::strtol(name.c_str() + hash + 1, nullptr, 10);
    std::string error;
    const auto& blobs = archive_blobs(game, index_path, error);
    if (static_cast<long>(blobs.size()) <= index) {
        report_mismatch("sbm", name, "entry", "<present>",
                 error.empty() ? "index out of range" : error);
        return;
    }
    const auto image = gm::parse_sbm(blobs[static_cast<std::size_t>(index)], error);
    if (!image) {
        report_mismatch("sbm", name, "parse", "ok", error);
        return;
    }
    const std::uint64_t hash_value = image->mips.empty()
                                         ? 0
                                         : gm::fnv1a64(image->mips.front().pixels);
    verify("sbm", name, expected,
           {{"w", std::to_string(image->width)},
            {"h", std::to_string(image->height)},
            {"levels", std::to_string(image->levels)},
            {"flags", std::to_string(image->flags)},
            {"pix", hex64(hash_value)}});
}

void check_sob(const fs::path& game, const std::string& name,
               const std::map<std::string, std::string>& expected) {
    const auto bytes = read_file(game / name);
    std::string error;
    const auto model = gm::parse_sob(bytes, error);
    if (!model) {
        report_mismatch("sob", name, "parse", "ok", error);
        return;
    }
    verify("sob", name, expected,
           {{"verts", std::to_string(model->vertices.size())},
            {"faces", std::to_string(model->faces.size())}});
}

} // namespace

int main(int argc, char** argv) {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    if (!fs::is_directory(game)) {
        std::printf("parity: game not installed at %s, skipped\n", game.c_str());
        return 0;
    }
    if (argc < 2) {
        std::printf("parity: no fixture file given, skipped\n");
        return 0;
    }
    std::ifstream fixture(argv[1]);
    if (!fixture) {
        std::printf("parity: %s not found, skipped\n", argv[1]);
        return 0;
    }

    std::string line;
    long unknown = 0;
    while (std::getline(fixture, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> tokens;
        std::size_t start = 0;
        while (start < line.size()) {
            const std::size_t space = line.find(' ', start);
            tokens.push_back(line.substr(start, space - start));
            if (space == std::string::npos) {
                break;
            }
            start = space + 1;
        }
        if (tokens.size() < 3) {
            std::printf("parity: short fixture line: %s\n", line.c_str());
            ++g_failures;
            continue;
        }
        const std::string kind = tokens[0];
        const std::string name = tokens[1];
        std::map<std::string, std::string> expected;
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const std::size_t eq = tokens[i].find('=');
            if (eq == std::string::npos) {
                std::printf("parity: bad token %s in %s\n", tokens[i].c_str(),
                            name.c_str());
                ++g_failures;
                continue;
            }
            expected[tokens[i].substr(0, eq)] = tokens[i].substr(eq + 1);
        }
        if (kind == "sbw") {
            check_sbw(game, name, expected);
        } else if (kind == "sbt") {
            check_sbt(game, name, expected);
        } else if (kind == "sbc") {
            check_sbc(game, name, expected);
        } else if (kind == "sbm") {
            check_sbm(game, name, expected);
        } else if (kind == "sou") {
            check_sou(game, name, expected);
        } else if (kind == "sml") {
            check_sml(game, name, expected);
        } else if (kind == "sob") {
            check_sob(game, name, expected);
        } else {
            std::printf("parity: unknown kind %s\n", kind.c_str());
            ++unknown;
            ++g_failures;
        }
    }

    std::printf("parity: %ld checks, %ld values, %d failed\n", g_checks, g_values,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
