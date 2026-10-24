#include "audio/sound_policy.h"

#include "core/log.h"
#include "res/bigfile.h"

#include <cstdint>
#include <utility>

namespace gm::audio {
namespace {

/// The SOU0/SOU1/SOU2 container header (`docs/FORMATS.md` §5).  An index entry
/// smaller than this cannot be a sample: `0x40afa6` treats such a read as a
/// broken header and returns -2, never a crash.
constexpr std::uint32_t kContainerHeaderBytes = 16;

/// `0x40ade0`: the tail after the last ':' or '\\'.
std::string_view basename(std::string_view name) {
    const auto cut = name.find_last_of(":\\");
    return cut == std::string_view::npos ? name : name.substr(cut + 1);
}

std::string trimmed(const std::array<char, 8>& key) {
    std::size_t length = 0;
    while (length < key.size() && key[length] != '\0') {
        ++length;
    }
    return std::string(key.data(), length);
}

void set_error(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

void set_code(int* entry_id, int code) {
    if (entry_id != nullptr) {
        *entry_id = code;
    }
}

/// Record ordinal of every named entry: 1-based position in the index.  This is
/// our stand-in for the original's record number (`0x83d960` issues numbers in
/// load order), see the header comment.
bool load_ids(const std::filesystem::path& index_path,
              std::map<std::array<char, 8>, int>& out) {
    std::string error;
    const auto file = BigFile::open(index_path, error);
    if (!file) {
        return false;
    }
    int position = 0;
    for (const auto& entry : file->entries()) {
        ++position;
        if (entry.name.empty()) {
            continue;
        }
        out.emplace(SoundCatalog::key_of(entry.name), position);
    }
    return true;
}

} // namespace

void SoundPolicy::set_table(const SoundSampleTable& table) { table_ = table; }

void SoundPolicy::set_budget(std::size_t bytes) { budget_ = bytes; }

bool SoundPolicy::set_root(const std::filesystem::path& game, std::string* error) {
    // Start from a clean, not-ready state: a failed re-root must not keep the
    // previous installation alive.
    cache_.reset();
    catalog_.reset();
    sounds_ids_.clear();
    music_ids_.clear();

    std::string local;
    auto catalog = SoundCatalog::open(game, local);
    if (!catalog) {
        set_error(error, local);
        return false;
    }
    const std::filesystem::path sounds_index = game / "sounds" / "SOU.IDX";
    if (!load_ids(sounds_index, sounds_ids_)) {
        set_error(error, "cannot read the entry order of " + sounds_index.string());
        return false;
    }
    // `MUSIC/SOU.IDX` is optional (`0x40ae3c` only runs on a `sounds` miss).
    load_ids(game / "MUSIC" / "SOU.IDX", music_ids_);

    if (!table_) {
        set_error(error,
                  "SOUND_SAMPLE_TABLE is not set: call set_table() before set_root()");
        return false;
    }
    auto cache = SampleCache::open(game, *table_, local, budget_);
    if (!cache) {
        set_error(error, local);
        return false;
    }
    catalog_ = std::move(*catalog);
    cache_.emplace(std::move(*cache));
    return true;
}

bool SoundPolicy::ready() const { return catalog_.has_value() && cache_.has_value(); }

SoundPolicy::Resolved SoundPolicy::resolve(std::string_view name) const {
    if (!ready()) {
        return Resolved{SoundLookup::missing, kCodeNotFound};
    }
    const auto ref = catalog_->find(name);
    if (!ref) {
        return Resolved{SoundLookup::missing, kCodeNotFound}; // 0x40af0c -> -1
    }
    if (ref->size < kContainerHeaderBytes) {
        // `0x40afa6`: the record exists but the header read cannot succeed.
        return Resolved{SoundLookup::missing, kCodeUnplayable};
    }
    const auto& ids = ref->archive == "music" ? music_ids_ : sounds_ids_;
    const auto found = ids.find(SoundCatalog::key_of(ref->name));
    const int entry_id = found == ids.end() ? kCodeNotFound : found->second;
    const SoundLookup kind = ref->archive == "music" ? SoundLookup::music_fallback
                                                    : SoundLookup::sounds;
    return Resolved{kind, entry_id};
}

void SoundPolicy::account(SoundLookup kind) const {
    ++stats_.requested;
    switch (kind) {
    case SoundLookup::sounds:
        ++stats_.resolved;
        break;
    case SoundLookup::music_fallback:
        ++stats_.fallback;
        break;
    case SoundLookup::missing:
        ++stats_.missing;
        break;
    }
}

SoundLookup SoundPolicy::lookup(std::string_view name, int* entry_id) const {
    const Resolved resolved = resolve(name);
    account(resolved.kind);
    set_code(entry_id, resolved.entry_id);
    return resolved.kind;
}

std::optional<SampleCache::Handle> SoundPolicy::load(std::string_view name,
                                                     std::string* error,
                                                     int* entry_id) {
    const Resolved resolved = resolve(name);
    if (resolved.kind == SoundLookup::missing) {
        account(SoundLookup::missing);
        note_missing(name);
        set_code(entry_id, resolved.entry_id);
        set_error(error, "sample \"" + std::string(basename(name)) +
                             "\" is absent: playing silence (" +
                             (resolved.entry_id == kCodeUnplayable
                                  ? "unreadable index entry, -2"
                                  : "no entry in sounds/SOU.IDX or MUSIC/SOU.IDX, -1") +
                             ")");
        return std::nullopt;
    }

    std::string local;
    auto handle = cache_->acquire(name, &local);
    if (!handle) {
        // Resolved in an index but not playable here (budget, decode, or the
        // name is not a slot of this level's table).  `0x40af61`/`0x40afa6`
        // also end in -2 and silence.
        account(SoundLookup::missing);
        note_missing(name);
        set_code(entry_id, kCodeUnplayable);
        set_error(error, local);
        return std::nullopt;
    }
    account(resolved.kind);
    set_code(entry_id, resolved.entry_id);
    return handle;
}

bool SoundPolicy::play(std::string_view name, std::string* error) {
    const Resolved resolved = resolve(name);
    if (resolved.kind == SoundLookup::missing) {
        account(SoundLookup::missing);
        note_missing(name);
        ++stats_.silent_frames;
        set_error(error, "sample \"" + std::string(basename(name)) +
                             "\" is absent: silence, not a failure");
        return true;
    }

    std::string local;
    auto handle = cache_->acquire(name, &local);
    if (!handle) {
        account(SoundLookup::missing);
        note_missing(name);
        ++stats_.silent_frames;
        set_error(error, local);
        return true;
    }
    account(resolved.kind);
    // One-shot model: the voice would be copied into the mixer here, so the
    // cache reference is returned immediately instead of leaking it.
    cache_->release(*handle);
    return true;
}

void SoundPolicy::note_missing(std::string_view name) {
    const auto key = SoundCatalog::key_of(name);
    if (!missing_.emplace(key, trimmed(key)).second) {
        return; // already logged: no stderr flood on repeated calls
    }
    const std::string_view base = basename(name);
    logf(LogLevel::Warn, "sound_policy", "missing sample \"", base,
         "\": no playable entry; playing silence (0x40af0c)");
}

SoundPolicyStats SoundPolicy::stats() const { return stats_; }

std::vector<std::string> SoundPolicy::missing_names() const {
    std::vector<std::string> out;
    out.reserve(missing_.size());
    for (const auto& [key, name] : missing_) {
        out.push_back(name);
    }
    return out; // map order = 8-byte key order = ascending
}

std::size_t SoundPolicy::warned_count() const { return missing_.size(); }

const Sound* SoundPolicy::sound(const SampleCache::Handle& handle) const {
    return cache_ ? cache_->sound(handle) : nullptr;
}

bool SoundPolicy::release(const SampleCache::Handle& handle, std::string* error) {
    if (!cache_) {
        set_error(error, "release: the sample cache is not open");
        return false;
    }
    return cache_->release(handle, error);
}

const SampleCache* SoundPolicy::cache() const { return cache_ ? &*cache_ : nullptr; }

} // namespace gm::audio
