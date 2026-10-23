#include "res/sound_catalog.h"

#include "res/bigfile.h"

namespace gm {
namespace {

/// `0x40ade0`: the last ':' or '\\' starts the name (`C:\Sounds\ALARMK`).
std::string_view basename(std::string_view name) {
    const auto cut = name.find_last_of(":\\");
    return cut == std::string_view::npos ? name : name.substr(cut + 1);
}

/// `0x480180` uppercases ASCII in place; everything else is left alone.
char upper(char value) {
    return (value >= 'a' && value <= 'z') ? static_cast<char>(value - 'a' + 'A') : value;
}

void load_archive(const std::filesystem::path& idx, const char* archive,
                  std::map<std::array<char, 8>, SoundRef>& out, std::string& error) {
    const auto file = BigFile::open(idx, error);
    if (!file) {
        return;
    }
    for (const auto& entry : file->entries()) {
        if (entry.name.empty() || entry.size == 0) {
            continue;
        }
        // The index name *is* the sample name (docs/SOUND_NAMES.md §1), so the
        // key is the name itself, padded like the engine's 8-byte slot.
        std::array<char, 8> key{};
        const std::size_t length = entry.name.size() < key.size() ? entry.name.size()
                                                                 : key.size();
        for (std::size_t i = 0; i < length; ++i) {
            key[i] = upper(entry.name[i]);
        }
        out.emplace(key, SoundRef{archive, entry.name, entry.offset, entry.size});
    }
}

} // namespace

std::array<char, 8> SoundCatalog::key_of(std::string_view name) {
    const std::string_view base = basename(name);
    std::array<char, 8> key{};
    const std::size_t length = base.size() < key.size() ? base.size() : key.size();
    for (std::size_t i = 0; i < length; ++i) {
        key[i] = upper(base[i]);
    }
    return key;
}

std::optional<SoundCatalog> SoundCatalog::open(const std::filesystem::path& game,
                                               std::string& error) {
    SoundCatalog catalog;
    load_archive(game / "sounds" / "SOU.IDX", "sounds", catalog.sounds_, error);
    if (catalog.sounds_.empty()) {
        if (error.empty()) {
            error = "no samples in " + (game / "sounds" / "SOU.IDX").string();
        }
        return std::nullopt;
    }
    // `MUSIC/SOU.IDX` is a fallback only: the original also runs without it.
    std::string music_error;
    load_archive(game / "MUSIC" / "SOU.IDX", "music", catalog.music_, music_error);
    return catalog;
}

std::optional<SoundRef> SoundCatalog::find(std::string_view name) const {
    const auto key = key_of(name);
    const auto hit = sounds_.find(key);
    if (hit != sounds_.end()) {
        return hit->second;
    }
    const auto music = music_.find(key); // `0x40ae3c`
    if (music != music_.end()) {
        return music->second;
    }
    return std::nullopt;
}

} // namespace gm
