// Missing-sample policy (ROADMAP M7.6, slice 2): `SOUND_SAMPLE_TABLE` name ->
// which archive answered -> load through the sample cache -> quiet silence.
//
// The original never treats an absent sample as an error (docs/SOUND_NAMES.md):
// `0x40ade0` resolves the 8-byte uppercase key in `sounds`, retries in `music`
// (`0x40ae3c`) and returns -1 when both miss (`0x40af0c`); -2 means "no free
// record" (`0x40af61`) or a broken container header (`0x40afa6`).  Playback
// then rejects a negative handle (`0x40bec0`, `0x40bd80`, `0x40ace0`), so the
// result is **silence, not a crash**.  The original logs nothing for names that
// come from the level table, which is why the `Warn` line below (one per name)
// is our diagnostic addition, not a parity change.
//
// What this unit adds on top of `SoundCatalog` (lookup, M7.6) and
// `SampleCache` (decode + reference counting, M7.4):
//   * one place that separates "resolved in sounds" / "resolved in music" /
//     "missing" and keeps the -1/-2 codes from the doc;
//   * counters (`requested`, `resolved`, `fallback`, `missing`,
//     `silent_frames`) and a deduplicated, sorted list of absent names;
//   * a `play` that returns success for silence and increments `silent_frames`
//     instead of throwing or writing to stderr on every call.
//
// Model, not a port: `entry_id` is the 1-based position of the entry in its
// `SOU.IDX`, because the original's record number (0x83d960) is allocated in
// load order and is not observable from an index lookup; the engine's contract
// "slot index 0..299 != record number" (docs/SOUND_NAMES.md §3.4) still holds.
// The sample cache's budget is our model too, so "no free voice" (-2) is
// mapped to a cache/budget failure here, not to `0x83d960` exhaustion.
#pragma once

#include "audio/sample_cache.h"
#include "res/sbt.h"
#include "res/sound_catalog.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm::audio {

/// How a `SOUND_SAMPLE_TABLE` name resolved (`0x40ade0` -> `0x40af30`).
enum class SoundLookup {
    sounds,         ///< entry in `sounds/SOU.IDX` (direct hit)
    music_fallback, ///< entry only in `MUSIC/SOU.IDX` (`0x40ae3c`)
    missing,        ///< neither archive has it: play silence (`0x40af0c`)
};

/// Counters of one policy object.  `resolved + fallback + missing == requested`
/// holds after every public call; a name that resolves in an index but cannot
/// be loaded (budget, decode) counts as `missing`, because no audio comes out.
struct SoundPolicyStats {
    int requested = 0;     ///< `lookup`/`load`/`play` calls
    int resolved = 0;      ///< handed out by `sounds`
    int fallback = 0;      ///< handed out by the `music` fallback
    int missing = 0;       ///< no audio: absent, unreadable or over budget
    int silent_frames = 0; ///< `play` calls that ended in silence
};

/// Name resolution + loading + silence accounting for absent samples.
///
/// Lifecycle: `set_table` (the level's `SOUND_SAMPLE_TABLE`) and `set_root`
/// (the installation).  Until both succeed `ready()` is false and every request
/// is a quiet miss: `lookup` says `missing`, `load` returns `nullopt`, `play`
/// returns true and counts a silent frame — a missing installation must not
/// crash the caller either.  Not thread safe, like the cache it wraps.
class SoundPolicy {
public:
    /// `-1`: no entry in either index (`0x40af0c`), or nothing is ready.
    static constexpr int kCodeNotFound = -1;
    /// `-2`: entry exists but cannot become a voice (`0x40af61`, `0x40afa6`).
    static constexpr int kCodeUnplayable = -2;

    SoundPolicy() = default;

    /// The level's 300-slot table (`0x4c9c90`).  Must be set before `set_root`;
    /// a copy is kept, so the caller's table may go away.
    void set_table(const SoundSampleTable& table);

    /// Byte budget for the sample cache (default `SampleCache::kDefaultBudgetBytes`).
    /// Applies on the next `set_root`.
    void set_budget(std::size_t bytes);

    /// Open `sounds/SOU.IDX` (+ optional `MUSIC/SOU.IDX`) and the cache.
    /// Returns false with `*error` set when the installation, the index or the
    /// table is unusable; on failure the object stays not-ready, never half-open.
    bool set_root(const std::filesystem::path& game, std::string* error = nullptr);

    /// True when both the catalog and the cache are open.
    bool ready() const;

    /// `0x40ade0`: `sounds` first, `music` on a miss (`0x40ae3c`).
    /// `entry_id` receives the 1-based index position on a hit, `-1` when the
    /// name is absent from both indexes, `-2` when the index entry is too small
    /// to hold a container header (an index-only check; nothing is decoded).
    /// Counts `requested` and one of `resolved`/`fallback`/`missing`.  A miss
    /// here is not logged: diagnostics are recorded on `note_missing`/`load`/
    /// `play`, so a sweep over all 300 slots does not spam `missing_names`.
    SoundLookup lookup(std::string_view name, int* entry_id = nullptr) const;

    /// Resolve and decode through `SampleCache`.  `nullopt` + `*error` for a
    /// miss (`entry_id` -1), an unloadable entry (budget/decode: -2) or a
    /// not-ready policy.  On success the caller owns one reference and must
    /// `release` it (or rely on the cache's eviction, like `SampleCache`).
    std::optional<SampleCache::Handle> load(std::string_view name,
                                            std::string* error = nullptr,
                                            int* entry_id = nullptr);

    /// `play`-like call: start one voice, or play silence if the name is
    /// missing/unloadable.  **Silence is success**: the return value is true in
    /// both cases (the original's callers ignore -1/0 as well) and the silence
    /// is counted in `silent_frames`; nothing is written to stderr on repeat
    /// calls (the `Warn` line is emitted once per distinct name).  The loaded
    /// reference is released immediately: wiring this into `Mixer::play` is
    /// future work.
    bool play(std::string_view name, std::string* error = nullptr);

    /// Record one absent name: `Warn` once, keep the deduplicated list.  The
    /// cache and the counters are not touched, so a caller that wants its own
    /// diagnostics (a script name, a door slot) can report a miss itself.
    void note_missing(std::string_view name);

    SoundPolicyStats stats() const;

    /// Absent names as 8-byte game keys (uppercased, basename after the last
    /// `\\`/`:`, truncated to 8 bytes), unique and sorted ascending.  This is
    /// the same key `0x40d1f0` builds, so `nosuchsample` and `NOSUCHSAMPLE`
    /// collapse into one entry.
    std::vector<std::string> missing_names() const;

    /// Number of `Warn` lines emitted (= distinct absent keys) — the testable
    /// form of "logged once per name".
    std::size_t warned_count() const;

    /// Decoded PCM of a live handle, forwarded to the cache.
    const Sound* sound(const SampleCache::Handle& handle) const;
    bool release(const SampleCache::Handle& handle, std::string* error = nullptr);

    /// Read-only cache view (stats, `find`), nullptr when not ready.
    const SampleCache* cache() const;

private:
    struct Resolved {
        SoundLookup kind = SoundLookup::missing;
        int entry_id = kCodeNotFound;
    };

    Resolved resolve(std::string_view name) const;
    void account(SoundLookup kind) const;

    std::optional<SoundSampleTable> table_;
    std::optional<SoundCatalog> catalog_;
    std::optional<SampleCache> cache_;
    std::map<std::array<char, 8>, int> sounds_ids_;
    std::map<std::array<char, 8>, int> music_ids_;
    mutable SoundPolicyStats stats_{};
    std::map<std::array<char, 8>, std::string> missing_;
    std::size_t budget_ = SampleCache::kDefaultBudgetBytes;
};

} // namespace gm::audio
