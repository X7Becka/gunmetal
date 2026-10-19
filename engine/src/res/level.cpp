#include "res/level.h"

#include "core/path.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace gm {
namespace {

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

} // namespace

std::optional<LevelBundle> load_level(const std::filesystem::path& dir,
                                      std::string_view name, std::string& error) {
    LevelBundle bundle;
    bundle.name = std::string(name);

    const std::string base(name);
    const auto world_path = resolve_ci(dir, base + ".sbw");
    if (!world_path) {
        error = "level '" + base + "': .sbw not found under " + dir.string();
        return std::nullopt;
    }
    const auto world_bytes = read_file(*world_path);
    if (!world_bytes) {
        error = "cannot read " + world_path->string();
        return std::nullopt;
    }
    bundle.world = parse_sbw(*world_bytes, error);
    if (!bundle.world) {
        error = world_path->string() + ": " + error;
        return std::nullopt;
    }

    if (const auto path = resolve_ci(dir, base + ".sbt")) {
        if (const auto bytes = read_file(*path)) {
            bundle.entities = parse_sbt(*bytes, error);
            if (!bundle.entities) {
                error = path->string() + ": " + error;
                return std::nullopt;
            }
        }
    }
    if (const auto path = resolve_ci(dir, base + ".sbc")) {
        if (const auto bytes = read_file(*path)) {
            bundle.lightmaps = parse_sbc(*bytes, error);
            if (!bundle.lightmaps) {
                error = path->string() + ": " + error;
                return std::nullopt;
            }
        }
    }
    return bundle;
}

} // namespace gm
