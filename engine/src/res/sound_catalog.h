// Sample name resolution: `SOUND_SAMPLE_TABLE` name -> `sounds/SOU.IDX` entry.
//
// The original cannot fail here loudly: `0x40ade0` turns a name into an 8-byte
// uppercase key (`0x480180`, `0x40d1f0`), looks it up in the `sounds` archive
// and, on a miss, retries in `music` (`0x40ae3c`).  Playback then rejects a
// negative index / zero size (`0x40bec0`, `0x40bd80`, `0x40ace0`) — **silence,
// not a crash**.  A missing name must therefore be a normal `nullopt`, never an
// exception or an out-of-range read (docs/SOUND_NAMES.md).
//
// This is deliberately only the lookup: wiring a `WARN` log into the playback
// path belongs to the caller, once per (level, slot, name).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace gm {

struct SoundRef {
    std::string archive; ///< "sounds" or "music" (the fallback archive)
    std::string name;    ///< entry name in the index (up to 8 chars)
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

/// `sounds/SOU.IDX` + `sounds/SOU.DAT`, with `MUSIC/SOU.IDX` as the fallback.
class SoundCatalog {
public:
    static std::optional<SoundCatalog> open(const std::filesystem::path& game,
                                            std::string& error);

    /// `0x40ade0`: basename after the last '\\' or ':', 8-byte uppercase key,
    /// `sounds` first and `music` only on a miss.  `nullopt` = play nothing.
    std::optional<SoundRef> find(std::string_view name) const;

    /// `0x40d1f0`: first 8 bytes of the uppercased basename, NUL padded.
    static std::array<char, 8> key_of(std::string_view name);

    std::size_t size() const { return sounds_.size(); }
    bool has_music() const { return !music_.empty(); }

private:
    std::map<std::array<char, 8>, SoundRef> sounds_;
    std::map<std::array<char, 8>, SoundRef> music_;
};

} // namespace gm
