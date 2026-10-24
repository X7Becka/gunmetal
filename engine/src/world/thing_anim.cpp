#include "world/thing_anim.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <utility>
#include <vector>

namespace gm::thinganim {
namespace {

/// Cache key separator between the root and the lower-cased clip stem.
constexpr char kKeySep = '\x1f';

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

/// Extension check that ignores ASCII case (the shipped set has `DISRUPT.SOO`
/// next to `bamfin.soo`).
bool has_soo_extension(const std::string& name) {
    if (name.size() < 4) {
        return false;
    }
    return ascii_lower(std::string_view(name).substr(name.size() - 4)) == ".soo";
}

/// A model name must stay one path component inside `Sobanim/`.
bool safe_model_name(const std::string& name) {
    return name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.find("..") == std::string::npos && name.find(':') == std::string::npos;
}

struct ClipEntry {
    bool ok = false;
    std::string stem; ///< as spelled on disk, e.g. `Cultist`
    Soo soo;
    std::string error;
};

/// root -> (lower-case stem -> file name).  One directory scan per root; the
/// shipped `Sobanim/` holds 40 files, so this is 40 `stat` calls once.
std::map<std::string, std::map<std::string, std::string>>& directory_index() {
    static std::map<std::string, std::map<std::string, std::string>> index;
    return index;
}

/// (root, lower-case stem) -> parsed clip or parse error.
std::map<std::string, ClipEntry>& clip_cache() {
    static std::map<std::string, ClipEntry> cache;
    return cache;
}

std::string cache_key(const std::string& root, const std::string& lower_stem) {
    return root + kKeySep + lower_stem;
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
               std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot stat '" + path.string() + "': " + ec.message();
        return false;
    }
    if (size > kMaxClipBytes) {
        error = "'" + path.string() + "' is " + std::to_string(size) +
                " bytes, over the " + std::to_string(kMaxClipBytes) + "-byte limit";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path.string() + "'";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "read error in '" + path.string() + "'";
        return false;
    }
    return true;
}

/// Build (once) the `Sobanim/*.soo` index of *root*.
const std::map<std::string, std::string>& index_for(const std::filesystem::path& root) {
    const std::string key = root.string();
    auto found = directory_index().find(key);
    if (found != directory_index().end()) {
        return found->second;
    }
    std::map<std::string, std::string>& index = directory_index()[key];
    const std::filesystem::path dir = root / "Sobanim";
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (!ec) {
        for (const auto& entry : it) {
            std::error_code ec2;
            if (!entry.is_regular_file(ec2)) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (!has_soo_extension(name)) {
                continue;
            }
            const std::string stem = name.substr(0, name.size() - 4);
            // Duplicate lower-case stems cannot exist on the Windows target;
            // keep the first one so the scan stays deterministic.
            index.emplace(ascii_lower(stem), name);
        }
    }
    return index;
}

/// Resolve `stem` inside the index; false when the file is not shipped.
bool find_clip_file(const std::filesystem::path& root, const std::string& stem,
                    std::string& file_name) {
    const auto& index = index_for(root);
    const auto it = index.find(ascii_lower(stem));
    if (it == index.end()) {
        return false;
    }
    file_name = it->second;
    return true;
}

/// Parse (or take from the cache) `Sobanim/<stem>.soo`.
const ClipEntry* load_clip(const std::filesystem::path& root, const std::string& stem,
                           const std::string& file_name, std::string& key) {
    key = cache_key(root.string(), ascii_lower(stem));
    auto found = clip_cache().find(key);
    if (found != clip_cache().end()) {
        return &found->second;
    }
    ClipEntry entry;
    entry.stem = file_name.substr(0, file_name.size() - 4);
    std::vector<std::uint8_t> bytes;
    std::string read_error;
    if (!read_file(root / "Sobanim" / file_name, bytes, read_error)) {
        entry.error = std::move(read_error);
    } else {
        std::string parse_error;
        auto soo = parse_soo(bytes, parse_error);
        if (!soo) {
            entry.error = std::move(parse_error);
        } else {
            entry.ok = true;
            entry.soo = std::move(*soo);
        }
    }
    auto inserted = clip_cache().emplace(std::move(key), std::move(entry));
    key = inserted.first->first;
    return &inserted.first->second;
}

/// The engine's effective frame count: `min(count, frames)` (`0x42b5f1`,
/// `0x42b637`); 0 when `count` is not positive.
std::size_t effective_frames(const Soo& soo) {
    if (soo.declared <= 0) {
        return 0;
    }
    return std::min<std::size_t>(static_cast<std::size_t>(soo.declared), soo.frames.size());
}

/// One fully resolved binding plus the numbers `collect` needs for the
/// `animated`/`still` split.
struct Resolved {
    ThingAnimBinding binding;
    bool moving = false;
};

std::optional<Resolved> resolve(const Thing& thing, AiState state, int tick,
                                const ThingAnimConfig& config, std::string& error) {
    auto fail = [&error](std::string message) -> std::optional<Resolved> {
        error = "thing_anim: " + std::move(message);
        return std::nullopt;
    };

    if (tick < 0) {
        return fail("negative tick " + std::to_string(tick) +
                    " (the engine clamps to 0 at 0x433e84, the caller may not)");
    }
    const std::optional<ThingClip> clip = clip_for_state(state);
    if (!clip) {
        return fail("unknown AI state " + std::to_string(static_cast<int>(state)));
    }

    const std::string name = !thing.model.empty() ? thing.model : thing.type_name;
    if (name.empty()) {
        return fail("thing " + std::to_string(thing.id) +
                    " has no model and no type name (type " + std::to_string(thing.type) + ")");
    }
    if (!safe_model_name(name)) {
        return fail("model name '" + name + "' is not a plain Sobanim/ stem");
    }

    const std::filesystem::path root = resolve_anim_root(config);
    if (root.empty()) {
        return fail("no installation with a Sobanim/ directory (set $GUNMETAL_DIR or "
                    "ThingAnimConfig::root)");
    }
    if (index_for(root).empty()) {
        return fail("no .soo clips under '" + (root / "Sobanim").string() + "'");
    }

    // Probe the state-specific variant first, then the model's own clip.  The
    // shipped corpus has no variant files at all (0 of 40), so every state
    // falls through to the base clip (docs/THING_ANIM_BIND.md §2).
    std::string file_name;
    std::string stem = name + std::string(clip_suffix(*clip));
    if (!find_clip_file(root, stem, file_name)) {
        stem = name;
        if (!find_clip_file(root, stem, file_name)) {
            return fail("no clip for model '" + name + "' (probed '" + name +
                        std::string(clip_suffix(*clip)) + ".soo' and '" + name + ".soo')");
        }
    }

    std::string key;
    const ClipEntry* entry = load_clip(root, stem, file_name, key);
    if (entry == nullptr || !entry->ok) {
        return fail("clip '" + stem + "': " + (entry == nullptr ? "unavailable" : entry->error));
    }

    // `mode` is the file's own word (0 hold / 1 loop); the only place this
    // module overrides it is the "a dead body must not loop" model.
    bool held = false;
    const Soo* soo = &entry->soo;
    Soo held_copy;
    if (config.hold_on_death && *clip == ThingClip::death && entry->soo.mode == 1) {
        held_copy = entry->soo;
        held_copy.mode = 0;
        soo = &held_copy;
        held = true;
    }

    std::string pose_error;
    const auto pose = soo_pose_at_tick(*soo, tick, 0, pose_error);
    if (!pose) {
        return fail("clip '" + entry->stem + "': " + pose_error);
    }

    Resolved resolved;
    resolved.binding.clip = *clip;
    resolved.binding.clip_name = entry->stem;
    resolved.binding.frame = pose->index;
    resolved.binding.tick = pose->phase;
    resolved.binding.looping = soo->mode == 1 && !held;

    const int total = pose->total;
    const std::size_t frames = effective_frames(*soo);
    resolved.moving =
        frames > 1 && total > 1 && (resolved.binding.looping || pose->phase < total - 1);
    return resolved;
}

} // namespace

const char* clip_name(ThingClip clip) {
    switch (clip) {
    case ThingClip::none: return "none";
    case ThingClip::idle: return "idle";
    case ThingClip::walk: return "walk";
    case ThingClip::attack: return "attack";
    case ThingClip::death: return "death";
    }
    return "none";
}

std::string_view clip_suffix(ThingClip clip) {
    switch (clip) {
    case ThingClip::idle: return "_idle";
    case ThingClip::walk: return "_walk";
    case ThingClip::attack: return "_atk";
    case ThingClip::death: return "_dead";
    case ThingClip::none: break;
    }
    return {};
}

std::optional<ThingClip> clip_for_state(AiState state) {
    switch (state) {
    case AiState::idle: return ThingClip::idle;
    case AiState::patrol: return ThingClip::walk;
    case AiState::alert: return ThingClip::walk;
    case AiState::attack: return ThingClip::attack;
    case AiState::dead: return ThingClip::death;
    }
    return std::nullopt;
}

std::filesystem::path resolve_anim_root(const ThingAnimConfig& config) {
    if (!config.root.empty()) {
        return config.root;
    }
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        candidates.emplace_back(env);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    candidates.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate / "Sobanim", ec)) {
            return candidate;
        }
    }
    return {};
}

void clear_clip_cache() {
    clip_cache().clear();
    directory_index().clear();
}

std::optional<ThingAnimBinding> bind_thing_anim(const Thing& thing, AiState state, int tick,
                                                std::string* error) {
    return bind_thing_anim(thing, state, tick, ThingAnimConfig{}, error);
}

std::optional<ThingAnimBinding> bind_thing_anim(const Thing& thing, AiState state, int tick,
                                                const ThingAnimConfig& config,
                                                std::string* error) {
    std::string local_error;
    const auto resolved = resolve(thing, state, tick, config, local_error);
    if (error != nullptr) {
        *error = resolved ? std::string() : local_error;
    }
    if (!resolved) {
        return std::nullopt;
    }
    return resolved->binding;
}

ThingAnimStats collect(std::span<const Thing> things, std::span<const AiState> states, int tick,
                       std::string* first_error) {
    return collect(things, states, tick, ThingAnimConfig{}, first_error);
}

ThingAnimStats collect(std::span<const Thing> things, std::span<const AiState> states, int tick,
                       const ThingAnimConfig& config, std::string* first_error) {
    ThingAnimStats stats;
    if (first_error != nullptr) {
        first_error->clear();
    }
    for (std::size_t i = 0; i < things.size(); ++i) {
        std::string error;
        if (i >= states.size()) {
            ++stats.unbound;
            error = "thing_anim: no AI state for thing " + std::to_string(things[i].id);
        } else {
            const auto resolved = resolve(things[i], states[i], tick, config, error);
            if (resolved) {
                ++stats.bound;
                if (resolved->moving) {
                    ++stats.animated;
                } else {
                    ++stats.still;
                }
                continue;
            }
            ++stats.unbound;
        }
        if (first_error != nullptr && first_error->empty()) {
            *first_error = error;
        }
    }
    return stats;
}

} // namespace gm::thinganim
