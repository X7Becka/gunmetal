// `MUSIC/*.sog` song playlists (ROADMAP M7.5, slice 2): the text format
// resolved against the *second* BIGFILE archive `MUSIC/SOU.IDX`/`SOU.DAT`.
//
// A `.sog` file is nothing but a list of sample names, one per line
// (`docs/FORMATS.md` §9, `gm/sog.py`):
//
//     line 1..N:  sample name (matches a `MUSIC/SOU.IDX` entry name)
//
// The samples are **not** in `sounds/SOU.DAT`; they are the 3092 `SOU2` blobs
// of the music archive (docs §9).  The original walks the list and reports
// `Unable to load sample '%s' for song 'music\%s.sog'` for a miss, so a miss is
// data, not a parse error.  This unit models the whole chain in two steps,
// because the archive is opened once for all 651 songs:
//
//   * `load_playlist()`     — read/parse the text (needs no archive);
//   * `resolve_playlist()`  — name -> `MUSIC/SOU.IDX` entry (index + size);
//   * `playlist_files()`    — the 651 `*.sog`/`*.SOG` files of an installation.
//
// Nothing here plays, decodes or mixes: a `Playlist` is data.  Playback is
// still the CLI's `--music` path (`audio/music.h` is the track model) and the
// two are deliberately not wired together yet (docs/PLAYLIST.md §4).
//
// Model decisions (docs/PLAYLIST.md §2), none of them recovered behaviour:
//
//   * **case-insensitive names.**  Playlists write `Dm1B01`/`winner`, the
//     archive stores `DM1B01`/`WINNER`; the engine compares ASCII-uppercased
//     keys (`0x40ae0c`, docs/SOUND_NAMES.md §3.1).  1054 of the 5211 shipped
//     name occurrences are mixed case — a case-sensitive resolver would find
//     only 4038 instead of 5088.
//   * **duplicates are kept** in file order.  The shipped corpus repeats
//     samples on purpose (998 extra occurrences, e.g. `L26I.sog` is a loop of
//     4-sample blocks), so de-duplicating would change the song.
//   * **first archive entry wins** for a folded name (the archive has no
//     duplicate names today, 3092/3092 unique).
//   * a name longer than the 8-byte BIGFILE field loads but can never
//     resolve (`found == false`): the miss is the loader's error, not ours.
//   * an **empty** playlist is refused, not returned empty: a song with no
//     samples is a broken file, and `gm::parse_sog` (the CLI model) refuses it
//     the same way.
//   * a line longer than `kMaxNameBytes` (255) is refused as untrusted data
//     before anything is allocated per name.
//
// Text is bytes (latin-1): only ASCII `A..Z` is folded, so every shipped
// playlist (5211 names, ASCII) is reproduced without a locale.

#pragma once

#include "res/bigfile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gm::audio {

/// Longest accepted line/sample name, bytes.  A `.sog` line is a sample name;
/// 255 is the trust-boundary guard (`res/sub.cpp` uses the same limit for its
/// 256-byte name buffer), not a format constant of the archive.
inline constexpr std::size_t kMaxNameBytes = 255;

/// The BIGFILE `char name[8]` field (`docs/FORMATS.md` §1): a name longer than
/// this can never be an archive key, so such an entry is always a miss.
inline constexpr std::size_t kArchiveNameBytes = 8;

/// One line of a playlist and, after `resolve_playlist()`, the archive entry
/// it names.
struct PlaylistEntry {
    /// The name as written in the file, whitespace-trimmed, case kept.
    std::string name;
    /// 0-based position in `BigFile::entries()`, `-1` when unresolved.
    int index_in_music = -1;
    /// Size of that entry in `MUSIC/SOU.DAT` bytes, `0` when unresolved.
    int bytes = 0;
    /// True when the folded name was found in the archive.
    bool found = false;
};

/// A parsed `.sog` file plus its resolution against `MUSIC/SOU.IDX`.
struct Playlist {
    /// Every non-blank line in file order, duplicates included.
    std::vector<PlaylistEntry> entries;
    /// The file it was read from (as passed to `load_playlist`).
    std::filesystem::path path;
    /// True after a successful `resolve_playlist()`.
    bool resolved = false;

    /// Entries whose sample exists in the archive.
    int found_count() const;
    /// Entries that are silent in the original (`Unable to load sample ...`).
    int missing_count() const;
    /// Extra occurrences beyond the first of each folded name: a 4-name block
    /// repeated 3 times is 8 duplicates.  Never negative.
    int duplicate_count() const;
    /// Σ `bytes` over the found entries; repetitions counted per occurrence,
    /// so on the shipped corpus it exceeds the 75 MB archive.
    std::int64_t found_bytes() const;
    /// Distinct missing names, ASCII-uppercased and sorted (the engine's key
    /// form), one per absent sample regardless of how often it is listed.
    std::vector<std::string> missing_names() const;
};

/// Read one `.sog` playlist.
///
/// Returns `false` and fills `*error` (when not null) — `out` untouched — for:
/// a path whose extension is not `.sog` (any case), an unreadable file, a file
/// with no sample name at all (empty, whitespace or blank lines only), and a
/// name longer than `kMaxNameBytes`.  On `true`, `*error` is cleared and
/// `entries` holds every non-blank line, trimmed of surrounding whitespace and
/// a trailing `\r`; `resolved` is false (nothing is looked up yet).
///
/// The line model is the one of `gm::parse_sog` (`res/text.h`), which the CLI
/// `--music` path already uses, so both agree on all 651 shipped files.
bool load_playlist(const std::filesystem::path& file, Playlist& out,
                   std::string* error);

/// Fill `index_in_music`/`bytes`/`found` of every entry from `music`.
///
/// The lookup key is the ASCII-lowercased name; the first matching archive
/// entry wins.  Idempotent.  Returns `false` and fills `*error` when the
/// archive has no entries at all (a broken installation is not "everything
/// missing" — callers see the difference).  A name that is absent is **not**
/// an error: `found` stays false, `index_in_music` stays `-1`, which is the
/// original's `Unable to load sample` case.
bool resolve_playlist(Playlist& playlist, const BigFile& music,
                      std::string* error = nullptr);

/// Every playlist of an installation, sorted by file name (byte order).
///
/// The songs live in `<game>/MUSIC/*.sog`; if that directory is absent the
/// path itself is scanned, so a bare songs directory works too.  The extension
/// test is case-insensitive: the shipped set is 598 `.sog` + 53 `.SOG` = 651,
/// and a case-sensitive glob silently loses the upper-case batch.
/// An absent directory is an empty list, not an error.
std::vector<std::filesystem::path> playlist_files(const std::filesystem::path& game);

} // namespace gm::audio
