#include "res/cache.h"

#include "core/path.h"

#include <utility>

namespace gm {
namespace {

// Archive names are compared case-insensitively; the engine stores 8-byte
// NUL-padded names and callers use both cases ("AFLM0001", "aflm0001").
std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::string make_key(const std::filesystem::path& idx_path, std::string_view name) {
    return idx_path.string() + '\n' + upper(name);
}

} // namespace

AssetCache::AssetCache(std::size_t budget_bytes) : budget_(budget_bytes) {}

BigFile* AssetCache::archive(const std::filesystem::path& idx_path, std::string& error) {
    const std::string key = idx_path.string();
    if (const auto it = archives_.find(key); it != archives_.end()) {
        return &it->second;
    }
    auto file = BigFile::open(idx_path, error);
    if (!file) {
        return nullptr;
    }
    return &archives_.emplace(key, std::move(*file)).first->second;
}

std::shared_ptr<const std::vector<std::uint8_t>> AssetCache::get(
    const std::filesystem::path& idx_path, const std::string& name, std::string& error) {
    const std::string key = make_key(idx_path, name);
    if (const auto it = items_.find(key); it != items_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        ++stats_.hits;
        stats_.entries = items_.size();
        stats_.bytes = bytes_;
        return it->second.data;
    }

    BigFile* file = archive(idx_path, error);
    if (file == nullptr) {
        return nullptr;
    }
    const BigEntry* entry = nullptr;
    for (const auto& candidate : file->entries()) {
        if (iequals_ascii(candidate.name, name)) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        error = "entry '" + name + "' not found in " + idx_path.string();
        return nullptr;
    }
    auto blob = file->read(*entry, error);
    if (!blob) {
        return nullptr;
    }
    auto data = std::make_shared<std::vector<std::uint8_t>>(std::move(*blob));
    bytes_ += data->size();
    ++stats_.misses;
    lru_.push_front(key);
    items_.emplace(key, Item{data, lru_.begin()});
    evict_locked();
    stats_.entries = items_.size();
    stats_.bytes = bytes_;
    return data;
}

void AssetCache::evict_locked() {
    while (bytes_ > budget_ && !lru_.empty()) {
        bool evicted = false;
        for (auto it = lru_.end(); it != lru_.begin();) {
            --it;
            const auto item = items_.find(*it);
            if (item == items_.end()) {
                it = lru_.erase(it);
                evicted = true;
                break;
            }
            if (item->second.data.use_count() > 1) {
                continue; // pinned by a caller
            }
            bytes_ -= item->second.data->size();
            items_.erase(item);
            it = lru_.erase(it);
            ++stats_.evictions;
            evicted = true;
            break;
        }
        if (!evicted) {
            break; // everything left is pinned
        }
    }
}

void AssetCache::clear() {
    // Pinned blobs survive in the callers' shared_ptrs; only the cache drops
    // its own references.
    items_.clear();
    lru_.clear();
    bytes_ = 0;
    stats_.bytes = 0;
    stats_.entries = 0;
}

CacheStats AssetCache::stats() const {
    CacheStats out = stats_;
    out.entries = items_.size();
    out.bytes = bytes_;
    return out;
}

} // namespace gm
