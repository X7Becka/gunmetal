#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "res/bigfile.h"

namespace gm {

struct CacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t bytes = 0;
    std::size_t entries = 0;
};

/// Ref-counted LRU cache over ``BIGFILE`` archives, the C++ counterpart of the
/// engine's asset cache around ``0x5310c0``.
///
/// A blob handed out is a ``shared_ptr``; entries still referenced by a caller
/// are *pinned* and are never evicted, so a texture or level held by the
/// renderer stays alive even when the byte budget is exceeded.
class AssetCache {
public:
    explicit AssetCache(std::size_t budget_bytes = 64u * 1024u * 1024u);

    void set_budget(std::size_t budget_bytes) { budget_ = budget_bytes; }
    std::size_t budget() const { return budget_; }

    /// Load *name* (the 8-character virtual name) from the archive at
    /// *idx_path*.  The archive is opened once and kept open.
    std::shared_ptr<const std::vector<std::uint8_t>> get(
        const std::filesystem::path& idx_path, const std::string& name, std::string& error);

    void clear();
    CacheStats stats() const;

private:
    struct Item {
        std::shared_ptr<std::vector<std::uint8_t>> data;
        std::list<std::string>::iterator lru;
    };

    void evict_locked();
    BigFile* archive(const std::filesystem::path& idx_path, std::string& error);

    std::size_t budget_ = 0;
    std::uint64_t bytes_ = 0;
    std::map<std::string, Item> items_;
    std::list<std::string> lru_; // front = most recently used
    std::map<std::string, BigFile> archives_;
    CacheStats stats_;
};

} // namespace gm
