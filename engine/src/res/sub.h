#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace gm {

/// Which sprite family a `.sub` table substitutes.  `textures`/`sprites` are
/// the two counted files the engine loads (`kind` 0 and 1 of `0x422f75`),
/// `sounds` is the tab-separated `sounds/sounds.sub` (`docs/SUB.md` §1/§5).
enum class SubKind { textures, sprites, sounds };

/// One line of a `.sub` table.
///
/// The shipped format stores **names**, not indices: `textures.sub` and
/// `sprites.sub` are `<weight> <replacement> <replaced>` per line after a
/// count line, `sounds.sub` is `<replacement><TAB><replaced>` with neither a
/// count nor a weight (`docs/SUB.md` §2/§5).  The names are kept verbatim
/// (latin-1 bytes, 255 max).
struct SubEntry {
    /// RAM class threshold of `0x42302c`: the row is used only when
    /// `ram_class <= weight`.  `-1` when the file has no weight (`sounds`).
    int weight = -1;
    /// Name of the picture that is drawn (the file's *left* field).
    std::string from;
    /// Name of the sprite/sound that is replaced (the file's *right* field).
    std::string to;
};

/// A parsed `.sub` file plus an optional index view over a name list.
struct SubTable {
    SubKind kind = SubKind::textures;
    /// The count line of a counted file (`119` / `64`); `0` when the file has
    /// no count (`sounds`) or the counter could not be read.
    std::size_t declared = 0;
    /// Every row in file order, duplicates kept.
    std::vector<SubEntry> pairs;
    /// Path the table was read from (as passed to `load_sub`).
    std::string source;

    // --- index view, filled by `bind_indices()` ---------------------------
    /// True after `bind_indices()`.
    bool bound = false;
    /// Size of the name list the table was bound to.
    std::size_t bound_size = 0;
    /// `pairs[i].from` in the bound name list, `-1` when the name is absent.
    std::vector<int> from_index;
    /// `pairs[i].to` in the bound name list, `-1` when the name is absent.
    std::vector<int> to_index;
};

/// Read a `.sub` file.
///
/// Both layouts are accepted; the layout is chosen by `kind` (a counted file
/// read as `sounds` is refused and vice versa).
///
/// Contract:
/// * `false` — the table could not be read: not a `.sub` path, unreadable or
///   empty file, a non-numeric/negative counter or weight, a malformed line,
///   an empty or over-long name.  `*error` (when non-null) holds a text and
///   `out` is left untouched.
/// * `true` with a non-empty `*error` — loaded, but the file disagrees with
///   itself: the counter differs from the number of records, or a row is a
///   no-op (`from == to`).  The text starts with `warning:`.  `pairs` holds
///   the data: when there are more records than the counter, only the first
///   `declared` are kept (the loader's `fscanf` loop reads exactly `count`
///   records, `0x422fe1`); when there are fewer, all are kept and the table
///   is simply shorter than declared.
/// * `true` with an empty `*error` — clean file.
///
/// Duplicate names are **kept**: the engine registers the first row for a
/// replaced name and silently drops the rest (`0x42307a`), which `resolve()`
/// reproduces.
bool load_sub(const std::filesystem::path& file, SubKind kind, SubTable& out,
              std::string* error);

/// Comparison key of the engine's 9-byte name slots: the first 8 bytes,
/// ASCII-uppercased (`strncpy(..., 9)` at `0x423244`, `docs/SUB.md` §4).
/// The `.sub` files spell names in lowercase while the `.sbw` tables mix
/// upper and lower case, so the fold is what makes the tables line up.
std::string sub_key(const std::string& name);

/// `true` when `name` is replaced by some row.
bool has(const SubTable& table, const std::string& name);

/// The picture drawn for `name`; `name` itself when there is no row
/// (the engine keeps the original image).  The first row for a name wins.
std::string resolve(const SubTable& table, const std::string& name);

/// Resolve every row name against `names` (e.g. `SbwHeader::textures`), so
/// the hot path is index-only.  Unknown names map to `-1`; the first
/// occurrence of a duplicate name wins.  `names` may be empty.
void bind_indices(SubTable& table, const std::vector<std::string>& names);

/// `true` when index `index` of the bound name list is replaced.
bool has(const SubTable& table, int index);

/// Replacement index of `index`, or `index` itself when there is no row.
/// `-1` when the table is not bound, `index` is outside the bound list, or
/// the replacement name does not occur in the list.
int resolve(const SubTable& table, int index);

/// The loader's own filter: a row applies only on a machine whose RAM class
/// (MB above ~4 MiB, `0x404630`) is `<= weight`; rows without a weight
/// (`sounds`) always apply.  See `docs/SUB.md` §3.
bool applies(const SubEntry& entry, int ram_class);

} // namespace gm
