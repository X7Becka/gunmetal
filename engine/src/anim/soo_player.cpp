#include "anim/soo_player.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

#include "core/path.h"
#include "res/anim.h"
#include "res/sob.h"

namespace gm {
namespace {

std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

bool has_extension(const std::filesystem::path& path) {
    return !path.extension().empty();
}

std::optional<std::filesystem::path> resolve_soo(const std::filesystem::path& game,
                                                 const std::filesystem::path& path) {
    std::error_code ec;
    if (path.is_absolute()) {
        if (std::filesystem::is_regular_file(path, ec)) {
            return path;
        }
        std::filesystem::path with_ext = path;
        if (!has_extension(path)) {
            with_ext += ".soo";
            if (std::filesystem::is_regular_file(with_ext, ec)) {
                return with_ext;
            }
        }
        // Case-insensitive lookup of the final component (Windows installs).
        if (const auto hit = resolve_ci(path.parent_path(), path.filename().string())) {
            if (std::filesystem::is_regular_file(*hit, ec)) {
                return hit;
            }
        }
        return std::nullopt;
    }
    std::vector<std::filesystem::path> candidates{game / path, game / "Sobanim" / path};
    if (!has_extension(path)) {
        candidates.push_back(game / (path.string() + ".soo"));
        candidates.push_back(game / "Sobanim" / (path.string() + ".soo"));
    }
    for (const auto& candidate : candidates) {
        if (const auto hit = resolve_ci(candidate.parent_path(), candidate.filename().string())) {
            if (std::filesystem::is_regular_file(*hit, ec)) {
                return hit;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                                   std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "read error: " + path.string();
        return std::nullopt;
    }
    return bytes;
}

} // namespace

SooPlayer::SooPlayer(std::filesystem::path game_dir) : game_dir_(std::move(game_dir)) {}

bool SooPlayer::load(const std::filesystem::path& soo_path, std::string& error) {
    error.clear();
    if (soo_path.empty()) {
        error = "empty .soo path";
        return false;
    }
    const auto path = resolve_soo(game_dir_, soo_path);
    if (!path) {
        error = "animation not found: " + soo_path.string();
        return false;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        return false;
    }
    if (bytes->empty()) {
        error = "empty .soo: " + path->string();
        return false;
    }
    auto parsed = parse_soo(*bytes, error);
    if (!parsed) {
        return false;
    }

    // Engine loader 0x42b4e1: mode must be one it knows, count 1..32, and the
    // file has to list at least that many frames.
    if (parsed->mode != 0 && parsed->mode != 1) {
        error = "unsupported .soo mode " + std::to_string(parsed->mode) +
                " (engine knows 0/1)";
        return false;
    }
    if (parsed->declared <= 0 || parsed->declared > 0x20) {
        error = "engine would refuse this .soo (count " +
                std::to_string(parsed->declared) + ", allowed 1..32)";
        return false;
    }
    const auto count = static_cast<std::size_t>(parsed->declared);
    if (parsed->frames.size() < count) {
        error = "declared " + std::to_string(parsed->declared) + " frames but only " +
                std::to_string(parsed->frames.size()) + " listed";
        return false;
    }

    int total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        total += parsed->frames[i].duration;
    }
    if (total <= 0) {
        error = "animation duration is not positive";
        return false;
    }

    // Build the new state first: a failed load leaves the previous animation
    // usable.  Models that parsed successfully stay in the cache.
    std::vector<ModelPtr> frames;
    frames.reserve(count);
    std::size_t base_vertices = 0;
    std::size_t base_faces = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& name = parsed->frames[i].name;
        ++stats_.frame_loads;
        auto model = load_model(name, error);
        if (!model) {
            error = "frame '" + name + "': " + error;
            return false;
        }
        if (i == 0) {
            base_vertices = (*model)->vertices.size();
            base_faces = (*model)->faces.size();
        } else if ((*model)->vertices.size() != base_vertices ||
                   (*model)->faces.size() != base_faces) {
            error = "animation poses have different topology at frame '" + name + "': " +
                    std::to_string(base_vertices) + "/" +
                    std::to_string((*model)->vertices.size()) + " v, " +
                    std::to_string(base_faces) + "/" +
                    std::to_string((*model)->faces.size()) + " f";
            return false;
        }
        frames.push_back(std::move(*model));
    }

    soo_ = std::move(*parsed);
    frames_ = std::move(frames);
    total_ticks_ = total;
    loaded_ = true;
    ++stats_.soo_loads;
    return true;
}

std::optional<SooPlayer::ModelPtr> SooPlayer::load_model(const std::string& name,
                                                         std::string& error) {
    const std::string key = ascii_lower(name);
    if (const auto it = cache_.find(key); it != cache_.end()) {
        ++stats_.cache_hits;
        return it->second;
    }
    const auto path = resolve_ci(game_dir_ / "SOBS", name + ".sob");
    if (!path) {
        error = "model not found: SOBS/" + name + ".sob";
        return std::nullopt;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        return std::nullopt;
    }
    auto model = parse_sob(*bytes, error);
    if (!model) {
        error = path->string() + ": " + error;
        return std::nullopt;
    }
    auto ptr = std::make_shared<const Sob>(std::move(*model));
    cache_.emplace(key, ptr);
    ++stats_.cache_misses;
    return ptr;
}

std::optional<SooPlayerPose> SooPlayer::pose_at(int ms, std::string& error) const {
    error.clear();
    if (!loaded_) {
        error = "no animation loaded";
        return std::nullopt;
    }
    const auto pose = soo_pose_at(soo_, ms, error);
    if (!pose) {
        return std::nullopt;
    }
    const auto a = static_cast<std::size_t>(pose->index);
    const auto b = static_cast<std::size_t>(pose->next_index);
    if (a >= frames_.size() || b >= frames_.size()) {
        error = "internal error: pose index out of range";
        return std::nullopt;
    }
    auto morphed = morph_sob(*frames_[a], *frames_[b], pose->alpha, error);
    if (!morphed) {
        return std::nullopt;
    }

    SooPlayerPose out;
    out.frame_a = pose->index;
    out.frame_b = pose->next_index;
    out.name_a = pose->name;
    out.name_b = pose->next_name;
    out.alpha = pose->alpha;
    out.tick = pose->phase;
    out.elapsed = pose->elapsed;
    out.total_ticks = pose->total;
    out.mesh = std::move(*morphed);

    // The engine/Python morph the plane vectors linearly; a runtime consumer
    // wants unit normals, so renormalise the interpolated face normals.  Zero
    // normals stay zero instead of becoming NaN.
    out.normals.resize(out.mesh.faces.size());
    for (std::size_t i = 0; i < out.mesh.faces.size(); ++i) {
        Vec3f& n = out.mesh.faces[i].normal;
        const double length = std::sqrt(static_cast<double>(n.x) * n.x +
                                        static_cast<double>(n.y) * n.y +
                                        static_cast<double>(n.z) * n.z);
        if (length > 0.0) {
            const double inv = 1.0 / length;
            n = {static_cast<float>(n.x * inv), static_cast<float>(n.y * inv),
                 static_cast<float>(n.z * inv)};
        }
        out.normals[i] = n;
    }
    return out;
}

void SooPlayer::set_loop(bool loop) {
    if (loaded_) {
        soo_.mode = loop ? 1 : 0;
    }
}

bool SooPlayer::loop() const {
    return loaded_ && soo_.mode == 1;
}

SooPlayerStats SooPlayer::stats() const {
    SooPlayerStats out = stats_;
    out.cached_models = cache_.size();
    return out;
}

} // namespace gm
