// Runtime sample cache (ROADMAP M7.4): `SOUND_SAMPLE_TABLE` name -> decoded
// PCM, with a reference count, an LRU eviction order and a byte budget.
//
// This is a **model** of the original cache, not a byte-for-byte port.  The
// addresses that name its pieces are documented in docs/SAMPLE_CACHE.md:
//
//   `0x83d960`  sample records (300 * 0x1c)
//   `0x4c9c90`  the level's 300 names * 9 bytes (SOUND_SAMPLE_TABLE, record 12)
//   `0x4cebd8`  per-slot reference counter (from the `.sbt` table)
//   `0x4cf1a8`  the second per-slot counter (duplicate names)
//   `0x4c8770`  the byte accounting variable (`+= 0x100 + size`, `-=` ...)
//
// What is taken from the original: the 300-slot name table, the rule that a
// zero `count` means "not loaded / absent", the `0x100`-byte per-entry
// overhead in the accounting, and the "name miss is silence, not a crash"
// contract (docs/SOUND_NAMES.md).  What is our model: the budget *limit*
// (the original only accumulates `0x4c8770` and never compares it in
// `0x40af30`/`0x40b400`/`0x40a9a0`), LRU order, and the handle/serial scheme.
//
// The cache is deliberately standalone: it does not touch the mixer, the
// platform audio device or the 3D queue yet.  `Handle::sound`-style access is
// provided through `sound()` so wiring it into `Mixer::play` is a local change.
#pragma once

#include "audio/mixer.h"    // Sound
#include "res/bigfile.h"    // BigFile
#include "res/sbt.h"        // SoundSampleTable
#include "res/sound_catalog.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace gm::audio {

/// Reference-counted, capacity-limited cache of decoded samples.
///
/// Keys are `SOUND_SAMPLE_TABLE` slots (0..299); `acquire` resolves a name to a
/// slot case-insensitively (like `0x40b9c0`), then either bumps the slot's
/// reference count or decodes it from `sounds/SOU.IDX` (falling back to
/// `MUSIC/SOU.IDX`, `0x40ae3c`) and inserts it.
///
/// Accounting: each entry costs `kEntryOverhead` (256 bytes, the `0x100` term of
/// `0x40af30`) plus `sizeof(std::int16_t) * samples` of canonical PCM.  When an
/// insert would exceed the budget, entries with `refcount == 0` are evicted in
/// LRU order (smallest `last_used` first); a pinned entry is never evicted.
/// If nothing is evictable the load fails with a `budget:` error.
///
/// Not thread safe: a cache belongs to one audio thread, like the original.
class SampleCache {
public:
    /// Per-entry accounting overhead (`0x40af30`: `+= 0x100 + size`).
    static constexpr std::size_t kEntryOverhead = 0x100;

    /// Default budget.  **Model choice, not an original constant**: M7.4 names
    /// the accounting variable `0x4c8770`, but the three sites that touch it
    /// only add/subtract/read it (checked against the listing), so there is no
    /// documented limit to copy.  16 MiB is half the shipped `sounds/SOU.DAT`
    /// (26,738,998 bytes), i.e. large enough for normal levels and small enough
    /// to make eviction observable.  See docs/SAMPLE_CACHE.md.
    static constexpr std::size_t kDefaultBudgetBytes = 16u * 1024u * 1024u;

    /// Opaque ownership token.  `serial` makes a stale handle harmless: a slot
    /// that was released, evicted and reloaded gets a new serial, so the old
    /// handle cannot release (or read) the new entry.
    struct Handle {
        std::uint32_t slot = 0;
        std::uint64_t serial = 0;

        bool valid() const { return serial != 0; }
        friend bool operator==(const Handle&, const Handle&) = default;
    };

    struct Stats {
        std::size_t entries = 0;    ///< entries currently resident
        std::size_t bytes = 0;      ///< their accounted size (overhead + PCM)
        std::uint64_t hits = 0;     ///< acquires served from the cache
        std::uint64_t misses = 0;   ///< acquires that had to resolve/load (or failed)
        std::uint64_t evictions = 0;///< entries dropped to stay inside the budget
        std::size_t peak_bytes = 0; ///< high-water mark of `bytes`
    };

    /// Read-only view of one resident entry (also returned by `find`).
    struct EntryInfo {
        std::uint32_t slot = 0;
        std::string name;
        std::uint32_t refcount = 0;
        std::size_t bytes = 0;
        std::uint64_t last_used = 0;
        const Sound* sound = nullptr;
    };

    /// Open the catalog and the two BIGFILE archives, then build an empty
    /// cache over `table`.  Fails only when `sounds/SOU.IDX` cannot be opened;
    /// `MUSIC/SOU.IDX` is an optional fallback (the original also runs without
    /// it).  `bytes_limit == 0` is allowed: every acquire then fails with
    /// `budget:` unless the sample itself is free (it never is).
    static std::optional<SampleCache> open(const std::filesystem::path& game,
                                           const SoundSampleTable& table,
                                           std::string& error,
                                           std::size_t bytes_limit = kDefaultBudgetBytes);

    /// Find `name` in `table` and take a reference.  Returns:
    ///  * a `Handle` with `refcount` bumped (a cache hit does not decode again);
    ///  * `nullopt` + `error` for an unknown name, `count == 0`, a missing
    ///    archive entry, a decode failure, or a full budget (`error` starts
    ///    with `budget:`).
    std::optional<Handle> acquire(std::string_view name, std::string* error = nullptr);

    /// Drop one reference.  Refuses (returns false + `error`) on a stale handle,
    /// an evicted entry or a second release of a zero count (`0x4cf1a8`-style
    /// rejection, not a crash).  A zero count leaves the entry cached and
    /// evictable.
    bool release(const Handle& handle, std::string* error = nullptr);

    /// Look up a resident entry by name **without loading or ref-counting**.
    /// Returns `nullopt` when the name is not in the table or not cached.
    std::optional<EntryInfo> find(std::string_view name) const;

    /// Decoded PCM of a live handle (nullptr for stale/released handles).
    const Sound* sound(const Handle& handle) const;

    Stats stats() const;
    std::size_t bytes_limit() const { return bytes_limit_; }

private:
    SampleCache(SoundCatalog catalog, SoundSampleTable table, std::size_t bytes_limit);

    struct Entry {
        Sound sound;
        std::string name;
        std::uint32_t refcount = 0;
        std::uint64_t serial = 0;
        std::uint64_t last_used = 0;
        std::size_t bytes = 0;
    };

    bool open_archives(const std::filesystem::path& game, std::string& error);
    std::optional<std::uint32_t> slot_of(std::string_view name) const;
    std::optional<Sound> decode_sample(std::string_view name, std::string& error) const;
    bool make_room(std::string_view name, std::size_t need, std::string& error);
    void touch(std::uint32_t slot);

    SoundCatalog catalog_;
    SoundSampleTable table_;
    std::size_t bytes_limit_ = 0;
    std::size_t bytes_ = 0;
    std::uint64_t clock_ = 0;
    std::uint64_t next_serial_ = 0;
    std::map<std::uint32_t, Entry> entries_;
    std::list<std::uint32_t> lru_; // front = most recently used
    std::optional<BigFile> sounds_;
    std::optional<BigFile> music_;
    Stats stats_{};
};

} // namespace gm::audio
