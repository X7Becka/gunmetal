#include "audio/sample_cache.h"

#include "res/sou1.h" // parse_sou1_header / decode_sou1 path for SOU0/SOU1
#include "res/sou2.h" // decode_sou2

#include <cstring>
#include <span>
#include <utility>

namespace gm::audio {
namespace {

constexpr std::string_view kMagicSou0 = "SOU0";
constexpr std::string_view kMagicSou1 = "SOU1";
constexpr std::string_view kMagicSou2 = "SOU2";

char ascii_upper(char value) {
    return (value >= 'a' && value <= 'z') ? static_cast<char>(value - 'a' + 'A') : value;
}

/// `0x40b9c0`: the tail after the last '\\' or ':', at most eight characters.
std::string_view name_tail(std::string_view name) {
    const auto cut = name.find_last_of(":\\");
    std::string_view base = cut == std::string_view::npos ? name : name.substr(cut + 1);
    if (base.size() > 8) {
        base = base.substr(0, 8);
    }
    return base;
}

/// `0x416850`-style comparison: ASCII case-insensitive, length sensitive.
bool name_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_upper(a[i]) != ascii_upper(b[i])) {
            return false;
        }
    }
    return true;
}

/// SOU0/SOU1/SOU2 -> the mixer's canonical 16-bit `Sound`.
std::optional<Sound> decode_blob(std::span<const std::uint8_t> blob, std::string& error) {
    if (blob.size() < 4) {
        error = "truncated container (no magic)";
        return std::nullopt;
    }
    const std::string_view magic(reinterpret_cast<const char*>(blob.data()), 4);
    if (magic == kMagicSou2) {
        auto clip = gm::decode_sou2(blob, error);
        if (!clip) {
            return std::nullopt;
        }
        return make_sound(*clip, error);
    }
    if (magic == kMagicSou1) {
        // Reuses the verified `audio::decode` path: source-rate S16, duplicates
        // dropped, `clamp(value >> 5)` (the mixer's canonical form).
        auto decoded = decode(blob, error);
        if (!decoded) {
            return std::nullopt;
        }
        return make_sound(*decoded, error);
    }
    if (magic == kMagicSou0) {
        auto header = gm::parse_sou1_header(blob, error);
        if (!header) {
            return std::nullopt;
        }
        if (blob.size() < 16 || blob.size() - 16 < header->payload_bytes) {
            error = "truncated SOU0 payload";
            return std::nullopt;
        }
        return make_sound_u8(blob.subspan(16, header->payload_bytes), header->sample_rate,
                             header->channels, error);
    }
    error = "unknown container magic";
    return std::nullopt;
}

void set_error(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

} // namespace

SampleCache::SampleCache(SoundCatalog catalog, SoundSampleTable table, std::size_t bytes_limit)
    : catalog_(std::move(catalog)), table_(std::move(table)), bytes_limit_(bytes_limit) {}

std::optional<SampleCache> SampleCache::open(const std::filesystem::path& game,
                                             const SoundSampleTable& table,
                                             std::string& error, std::size_t bytes_limit) {
    auto catalog = SoundCatalog::open(game, error);
    if (!catalog) {
        return std::nullopt;
    }
    SampleCache cache(std::move(*catalog), table, bytes_limit);
    if (!cache.open_archives(game, error)) {
        return std::nullopt;
    }
    return cache;
}

bool SampleCache::open_archives(const std::filesystem::path& game, std::string& error) {
    sounds_ = BigFile::open(game / "sounds" / "SOU.IDX", error);
    if (!sounds_) {
        if (error.empty()) {
            error = "cannot open " + (game / "sounds" / "SOU.IDX").string();
        }
        return false;
    }
    // Optional: `0x40ae3c` only runs the `music` lookup when `sounds` missed.
    std::string music_error;
    music_ = BigFile::open(game / "MUSIC" / "SOU.IDX", music_error);
    return true;
}

std::optional<std::uint32_t> SampleCache::slot_of(std::string_view name) const {
    const std::string_view tail = name_tail(name);
    if (tail.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < table_.names.size(); ++i) {
        if (!table_.names[i].empty() && name_equal(table_.names[i], tail)) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return std::nullopt;
}

std::optional<Sound> SampleCache::decode_sample(std::string_view name,
                                                std::string& error) const {
    const auto ref = catalog_.find(name);
    if (!ref) {
        error = "no entry in sounds/SOU.IDX and no music fallback";
        return std::nullopt;
    }
    const BigFile* archive = nullptr;
    if (ref->archive == "music") {
        if (music_) {
            archive = &*music_;
        } else {
            error = "MUSIC/SOU.IDX is not available";
            return std::nullopt;
        }
    } else {
        if (sounds_) {
            archive = &*sounds_;
        } else {
            error = "sounds/SOU.IDX is not available";
            return std::nullopt;
        }
    }

    const BigEntry* entry = nullptr;
    for (const auto& candidate : archive->entries()) {
        if (candidate.name == ref->name) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr || entry->size == 0) {
        error = "empty or missing BIGFILE entry " + ref->name;
        return std::nullopt;
    }

    std::string read_error;
    auto blob = archive->read(*entry, read_error);
    if (!blob) {
        error = "BIGFILE read failed: " + read_error;
        return std::nullopt;
    }
    std::string decode_error;
    auto sound = decode_blob(*blob, decode_error);
    if (!sound) {
        error = decode_error;
        return std::nullopt;
    }
    return sound;
}

void SampleCache::touch(std::uint32_t slot) {
    lru_.remove(slot);
    lru_.push_front(slot);
}

bool SampleCache::make_room(std::string_view name, std::size_t need, std::string& error) {
    if (need > bytes_limit_) {
        error = "budget: sample \"" + std::string(name) + "\" needs " + std::to_string(need) +
                " bytes, limit is " + std::to_string(bytes_limit_);
        return false;
    }
    while (bytes_ + need > bytes_limit_) {
        // Oldest evictable entry first: scan the LRU tail while skipping
        // entries that still have references.
        auto victim = lru_.end();
        for (auto it = lru_.end(); it != lru_.begin();) {
            --it;
            const auto found = entries_.find(*it);
            if (found != entries_.end() && found->second.refcount == 0) {
                victim = it;
                break;
            }
        }
        if (victim == lru_.end()) {
            error = "budget: sample \"" + std::string(name) + "\" needs " +
                    std::to_string(need) + " bytes, cache holds " + std::to_string(bytes_) +
                    " of " + std::to_string(bytes_limit_) + " and every entry is referenced";
            return false;
        }
        const auto found = entries_.find(*victim);
        if (found != entries_.end()) {
            bytes_ -= found->second.bytes;
            entries_.erase(found);
        }
        lru_.erase(victim);
        ++stats_.evictions;
    }
    return true;
}

std::optional<SampleCache::Handle> SampleCache::acquire(std::string_view name,
                                                        std::string* error) {
    const auto slot = slot_of(name);
    if (!slot) {
        ++stats_.misses;
        set_error(error, "sample \"" + std::string(name) +
                             "\" is not in SOUND_SAMPLE_TABLE (0x4c9c90)");
        return std::nullopt;
    }
    if (table_.counts[*slot] == 0) {
        ++stats_.misses;
        set_error(error, "sample \"" + table_.names[*slot] + "\" (slot " +
                             std::to_string(*slot) + ") has count 0: absent in this level");
        return std::nullopt;
    }

    const auto cached = entries_.find(*slot);
    if (cached != entries_.end()) {
        ++stats_.hits;
        ++cached->second.refcount;
        cached->second.last_used = ++clock_;
        touch(*slot);
        return Handle{*slot, cached->second.serial};
    }

    ++stats_.misses;
    std::string decode_error;
    auto sound = decode_sample(table_.names[*slot], decode_error);
    if (!sound) {
        set_error(error, "sample \"" + table_.names[*slot] + "\": " + decode_error);
        return std::nullopt;
    }

    const std::size_t cost = kEntryOverhead + sound->samples.size() * sizeof(std::int16_t);
    std::string budget_error;
    if (!make_room(table_.names[*slot], cost, budget_error)) {
        set_error(error, budget_error);
        return std::nullopt;
    }

    Entry entry;
    entry.sound = std::move(*sound);
    entry.name = table_.names[*slot];
    entry.refcount = 1;
    entry.serial = ++next_serial_;
    entry.last_used = ++clock_;
    entry.bytes = cost;
    const Handle handle{*slot, entry.serial};
    bytes_ += cost;
    if (bytes_ > stats_.peak_bytes) {
        stats_.peak_bytes = bytes_;
    }
    entries_.emplace(*slot, std::move(entry));
    lru_.push_front(*slot);
    return handle;
}

bool SampleCache::release(const Handle& handle, std::string* error) {
    const auto found = entries_.find(handle.slot);
    if (found == entries_.end()) {
        set_error(error, "release: slot " + std::to_string(handle.slot) +
                             " is not cached (already evicted)");
        return false;
    }
    if (found->second.serial != handle.serial) {
        set_error(error, "release: stale handle for slot " + std::to_string(handle.slot));
        return false;
    }
    if (found->second.refcount == 0) {
        set_error(error, "release: slot " + std::to_string(handle.slot) +
                             " has refcount 0 (double release)");
        return false;
    }
    --found->second.refcount;
    return true;
}

std::optional<SampleCache::EntryInfo> SampleCache::find(std::string_view name) const {
    const auto slot = slot_of(name);
    if (!slot) {
        return std::nullopt;
    }
    const auto found = entries_.find(*slot);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    const Entry& entry = found->second;
    return EntryInfo{*slot, entry.name, entry.refcount, entry.bytes, entry.last_used,
                     &entry.sound};
}

const Sound* SampleCache::sound(const Handle& handle) const {
    const auto found = entries_.find(handle.slot);
    if (found == entries_.end() || found->second.serial != handle.serial ||
        found->second.refcount == 0) {
        return nullptr;
    }
    return &found->second.sound;
}

SampleCache::Stats SampleCache::stats() const {
    Stats out = stats_;
    out.entries = entries_.size();
    out.bytes = bytes_;
    return out;
}

} // namespace gm::audio
