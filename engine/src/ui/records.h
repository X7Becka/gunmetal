#pragma once

// Headless model of the best-time screen (`M8.4`, slice 2): the record table of
// `GUNMETAL.HI` that the level-end screen prints as `YOUR BEST:`.
//
// `game/campaign_state.h` already reads and writes the file as campaign state
// (`load_hi`/`save_hi`).  This module is the screen side of the same data: a
// row per record with the derived `duplicate` flag, the minimum per level name,
// the "is this an improvement" decision, the append-and-save step and the
// proven `M : SS` time text.  It owns no pixels, no font and no screen state.
//
// Proven behaviour (`docs/CAMPAIGN_FLOW.md` §6, `docs/GAME_OVER.md` §5):
//
//   * the file is `<game>/GUNMETAL.HI`, 576 bytes = 9 records of 0x40:
//     `i32 seconds; char name[60]`.  The shipped data is 146 `TESTING RANGE`,
//     1570 `KELLER CORE`, 18 `TESTING RANGE`, 109 `POWER STATION`, 18 and 7
//     `NATAKA TOWER`, 11 `OIL RIG`, 40 `OIL REFINERY`, 6 `CADTLELTL` (§6);
//   * it is opened through `0x493630` (path string `0x4be60c`) with mode `"rb"`
//     (`0x4a6388`) and `"ab"` (`0x4b8690`); one record is read with
//     `fread(buf, 0x40, 1, f)` (`0x40dc70`);
//   * the name is compared case-insensitively (`0x416850`) and the **minimum**
//     per name is kept (`0x45c8d6`): `TESTING RANGE` is 18, not 146;
//   * a better time is appended (`0x45c904`..`0x45c94c`), nothing is deleted, so
//     duplicate names are legal and the file grows by one 0x40 record;
//   * the level-end screen (`0x45c057` -> sub-mode 5, single-player layout
//     `0x483a70`) prints `TIME:` (`0x4bcb9c`), `BEST TIME:` (`0x4bcb90`, the par
//     `0x80d88c`) and `YOUR BEST:` (`0x4bcb84`); the last one is the value
//     `0x45c880` returned (`0x83d2c0`), i.e. the minimum after the insert;
//   * the time text is `0x483f00`: `"%ld : %ld%ld"` (`0x4bcbdc`) - minutes,
//     then tens and units of the seconds - and `UNKNOWN` (`0x4bcbd4`, stored
//     right before the format string) for zero.
//
// Model, not a port -- every such place is marked `MODEL`:
//
//   * **there is no list of all records in the original.**  `YOUR BEST:` is a
//     single value for the level being finished, looking its name up in the
//     file.  `best_by_name()` and its ordering (seconds ascending, then name) are
//     ours; the screen would still show one row per level name.
//   * `duplicate` is derived, not stored: true when the same name (ASCII
//     case-insensitive) already appeared in an **earlier** record.  The original
//     never looks at the flag; the file keeps every row.
//   * a negative time is refused as corrupt: the game's clock is
//     `ticks / 91` of an unsigned counter, so it cannot produce one.
//   * a name field of all 60 bytes without a NUL is **truncated** to 60
//     characters.  The original would read the C string past the field into the
//     next record (undefined for the last one); stopping at the field is ours.
//   * a file with more than 9 records is accepted on read (the game appends
//     without any limit), but a 64 KiB / 1024-record cap keeps a wrong path from
//     being slurped.  The shipped file has 9.
//   * the name of a new record is limited to 59 bytes so that the 60-byte field
//     keeps its terminator; `campaign_state` allows 60 bytes, the screen's
//     C-string path does not.
//   * `append_and_save()` writes to the path it is given and nowhere else: it
//     does not resolve `<game>/GUNMETAL.HI` itself, so a test or a caller can
//     point it at a scratch copy.  Opening `"ab"` creates a missing file, and a
//     file whose size is not a multiple of 0x40 is refused instead of ruining it.
//   * `format_time()` of a negative value is `UNKNOWN` (the zero branch); the
//     original has no negative time to print.
//
// Not wired into the frontend, the level-end screen, `GameRuntime` or the CLI:
// nothing here draws `YOUR BEST:`/`BEST TIME:`, and no game file is written
// unless the caller passes its path.  See `docs/HI_SCORES.md`.
//
// Invariants (checked by `engine/tests/test_records.cpp`)
// -------------------------------------------------------
//   * `rows().size() * kRecordBytes` is the size of the loaded file; a refused
//     `load()` leaves the previous rows untouched.
//   * `best_by_name()` has one entry per distinct name; `entries` sum to
//     `rows().size()`; entries are ordered by seconds, then by name.
//   * `better(s, n)` is `s < minimum(n)` for a known name and `true` for an
//     unknown one; it never writes anything.
//   * `append_and_save()` grows the file by exactly `kRecordBytes` and leaves
//     every earlier byte as it was.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

/// One `GUNMETAL.HI` record in file order (`CAMPAIGN_FLOW.md` §6).
struct RecordRow {
    int seconds = 0;    ///< `i32`, the level completion time in seconds
    std::string name;   ///< `SET_LEVEL_NAME` text, e.g. `KELLER CORE`
    bool duplicate = false; ///< the same name appeared in an earlier row
};

/// One level name as the screen would show it: the best time and how many
/// records the name owns.
struct RecordView {
    std::string name;
    int seconds = 0; ///< minimum over the records of this name (`0x45c8d6`)
    int entries = 0; ///< records carrying this name
};

/// `GUNMETAL.HI` as the best-time screen sees it.
class Records {
public:
    /// `i32 seconds; char name[60]` (`CAMPAIGN_FLOW.md` §6).
    static constexpr int kRecordBytes = 0x40;
    static constexpr int kNameBytes = 60;
    /// Records of the shipped file: 576 bytes = 9 records (§6, data).
    static constexpr int kShippedRecords = 9;
    static constexpr int kShippedBytes = kRecordBytes * kShippedRecords;
    /// Longest accepted name: the field must keep its terminator.
    static constexpr int kMaxNameBytes = kNameBytes - 1;
    /// `MODEL`: read guard, a wrong path is not a 64 KiB record table.
    static constexpr int kMaxRecords = 1024;

    /// Empty table, no file read.
    Records();

    // --- reading -----------------------------------------------------------

    /// Read *file* (no writing).  Accepted: 0 bytes (an empty table) and any
    /// number of whole 0x40 records up to `kMaxRecords`.  Refused with a text
    /// for a missing/unreadable file, a size that is not a multiple of 0x40, a
    /// negative time and a size over the guard; a refusal keeps the previous
    /// rows.  *error*, when given, also receives the text, `last_error()` always
    /// does.
    bool load(const std::filesystem::path& file, std::string* error = nullptr);

    /// The records in file order (9 for the shipped file).
    const std::vector<RecordRow>& rows() const { return rows_; }

    /// True when the last `load()` succeeded and `rows()` matches a file.
    bool loaded() const { return loaded_; }

    /// Distinct names with their minimum time (`0x416850`/`0x45c8d6`),
    /// ordered by seconds ascending, then by name case-insensitively.  `MODEL`:
    /// the original has no such list (see the header).
    std::vector<RecordView> best_by_name() const;

    /// Best time of *name* (case-insensitive), or -1 when absent.
    int best_time(std::string_view name) const;

    // --- the level-end decision --------------------------------------------

    /// True when *seconds* is strictly better (smaller) than the stored minimum
    /// of *name*, or when the name has no record at all.  This is the
    /// `0x45c8d6` comparison only: nothing is written and the caller decides
    /// (the original then appends and returns the new value, §6).  Refused with
    /// a text -- and false -- for a negative time or an unusable name.
    bool better(int seconds, const std::string& name, std::string* error = nullptr) const;

    /// Append one record to *file* and nothing else: `i32` little-endian,
    /// then the name NUL-padded to 60 bytes (mode `"ab"`, `0x4b8690`).  The
    /// file grows by exactly `kRecordBytes`; a missing file is created (that is
    /// what `"ab"` does).  Does **not** consult `better()`: the caller decides.
    /// Does not touch `rows()` -- reload the file to see the new row.  Refused
    /// with a text for a negative time, an empty/60-byte/NUL-carrying name or an
    /// existing file whose size is not a multiple of 0x40.
    bool append_and_save(const std::filesystem::path& file, int seconds, const std::string& name,
                         std::string* error = nullptr);

    // --- text --------------------------------------------------------------

    /// The proven time text of `0x483f00`: minutes, then the two digits of the
    /// seconds, in the original's spacing -- `"%d : %d%d"` (`0x4bcbdc`), e.g.
    /// 6 -> `0 : 06`, 109 -> `1 : 49`, 1570 -> `26 : 10`; 0 -> `UNKNOWN`
    /// (`0x4bcbd4`).  A negative value is `UNKNOWN` too (`MODEL`).
    std::string format_time(int seconds) const;

    /// Reason of the last refusal (empty when the last call was clean).
    const std::string& last_error() const { return last_error_; }
    void clear_error() { last_error_.clear(); }

private:
    /// Store *text* as the last error and return false.
    bool fail(std::string text, std::string* error) const;

    std::vector<RecordRow> rows_;
    bool loaded_ = false;
    mutable std::string last_error_;
};

/// The name comparison of `0x416850`: ASCII case-insensitive.  Exposed because
/// `duplicate` and the minimum per name both rest on it.
bool record_name_equal(std::string_view a, std::string_view b) noexcept;

} // namespace gm
