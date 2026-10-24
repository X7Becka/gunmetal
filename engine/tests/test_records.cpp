// Best-time screen model (`M8.4`, slice 2; docs/HI_SCORES.md).
//
// Pins `ui/records.h` against the shipped `GUNMETAL.HI` (read only: every write
// goes to a scratch copy under /var/tmp) and against the numbers of
// `docs/CAMPAIGN_FLOW.md` §6 and `docs/GAME_OVER.md` §5:
//
//   * the shipped file: 576 bytes, 9 rows, times
//     146,1570,18,109,18,7,11,40,6 and the names `TESTING RANGE`, `KELLER CORE`,
//     `TESTING RANGE`, `POWER STATION`, `NATAKA TOWER`, `NATAKA TOWER`,
//     `OIL RIG`, `OIL REFINERY`, `CADTLELTL`; two rows are duplicates;
//   * the minimum per name (`0x416850`/`0x45c8d6`): `TESTING RANGE` 18 (not
//     146), `KELLER CORE` 1570, `NATAKA TOWER` 7; 7 distinct names over 9 rows;
//   * `better()` is the strict comparison of the original: 17 < 18 and 17 <
//     1570 improve, 18 against 18 does not, a different case is the same name;
//   * `append_and_save()` grows a copy by exactly 0x40 to 640 bytes, leaves the
//     first 576 bytes byte-identical and appends `i32 5` + `TESTING RANGE`;
//     re-reading sees 10 rows and the new minimum 5;
//   * `format_time()` is `0x483f00`'s `%ld : %ld%ld`: 6 -> `0 : 06`, 18 ->
//     `0 : 18`, 47 -> `0 : 47`, 109 -> `1 : 49`, 1570 -> `26 : 10`, 0 ->
//     `UNKNOWN`;
//   * negatives: a missing file, 575 and 577 bytes, a negative stored time,
//     a 60-byte name for a write, an empty/NUL name, a negative `better()` and
//     an append onto a truncated file -- all refused with a text, with the loaded
//     rows and the file left untouched;
//   * a 60-byte name field without a NUL is truncated at 60 characters, and a
//     0-byte file is the empty table;
//   * two loads of the same bytes give the same rows, the same `best_by_name()`
//     order and the same texts.
//
// The numbers printed at the end are quoted in docs/HI_SCORES.md §2.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ui/records.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;
int checks = 0;
bool g_shipped_ok = false;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string in_quotes(std::string_view value) { return "\"" + std::string(value) + "\""; }

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_bytes(const fs::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

/// FNV-1a 64 of a file, the project's usual fingerprint.
std::uint64_t fnv1a64(std::string_view data) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// A scratch path with a per-process suffix (`std::filesystem` keeps this
/// stdlib-only).  `/var/tmp` is the project's scratch disk; `GM_RECORDS_TMPDIR`
/// overrides it and the system temp dir is the last resort.
fs::path temp_file(const std::string& leaf) {
    std::string base = "/var/tmp";
    if (const char* env = std::getenv("GM_RECORDS_TMPDIR"); env != nullptr && *env != '\0') {
        base = env;
    }
    std::error_code ec;
    if (!fs::is_directory(base, ec)) {
        base = fs::temp_directory_path(ec).string();
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&checks)));
    return fs::path(base) / ("gm_records" + std::string(suffix) + "_" + leaf);
}

/// The shipped `GUNMETAL.HI`; `GUNMETAL_DIR` overrides the install root and the
/// mirror under `~/.cache/gunmetal-re/game` is the second try.
fs::path shipped_hi() {
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("GUNMETAL_DIR"); env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    roots.emplace_back("/mnt/d/games/Gunmetal 3dfx");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(fs::path(home) / ".cache" / "gunmetal-re" / "game");
    }
    std::error_code ec;
    for (const fs::path& root : roots) {
        fs::path file = root / "GUNMETAL.HI";
        if (fs::is_regular_file(file, ec) && !ec) {
            return file;
        }
    }
    return {};
}

/// A 0x40 record: `i32` little-endian *seconds* and *name* NUL-padded to 60.
std::string make_record(int seconds, std::string_view name) {
    std::string record(static_cast<std::size_t>(gm::Records::kRecordBytes), '\0');
    record[0] = static_cast<char>(seconds & 0xFF);
    record[1] = static_cast<char>((seconds >> 8) & 0xFF);
    record[2] = static_cast<char>((seconds >> 16) & 0xFF);
    record[3] = static_cast<char>((seconds >> 24) & 0xFF);
    for (std::size_t i = 0; i < name.size() && i < 60; ++i) {
        record[4 + i] = name[i];
    }
    return record;
}

std::string joined_times(const std::vector<gm::RecordRow>& rows) {
    std::string out;
    for (const gm::RecordRow& row : rows) {
        if (!out.empty()) {
            out += ',';
        }
        out += std::to_string(row.seconds);
    }
    return out;
}

std::string joined_names(const std::vector<gm::RecordRow>& rows) {
    std::string out;
    for (const gm::RecordRow& row : rows) {
        if (!out.empty()) {
            out += '|';
        }
        out += row.name;
    }
    return out;
}

std::string joined_views(const std::vector<gm::RecordView>& views) {
    std::string out;
    for (const gm::RecordView& view : views) {
        if (!out.empty()) {
            out += ' ';
        }
        out += view.name + "=" + std::to_string(view.seconds);
    }
    return out;
}

// The module is loaded once per test function: a refusal must leave the rows
// of the successful load untouched.
struct Shipped {
    fs::path path;
    std::string bytes;
    gm::Records records;
    bool ok = false;
};

Shipped load_shipped() {
    Shipped shipped;
    shipped.path = shipped_hi();
    if (shipped.path.empty()) {
        return shipped;
    }
    shipped.bytes = read_bytes(shipped.path);
    std::string error;
    shipped.ok = shipped.records.load(shipped.path, &error);
    g_shipped_ok = g_shipped_ok || shipped.ok;
    if (!shipped.ok) {
        std::fprintf(stderr, "FAIL: the shipped GUNMETAL.HI does not load: %s\n", error.c_str());
        ++failures;
    }
    return shipped;
}

void test_shipped_file() {
    const Shipped shipped = load_shipped();
    if (shipped.path.empty()) {
        std::printf("records: shipped file not installed, skipping the file checks\n");
        return;
    }
    const std::vector<gm::RecordRow>& rows = shipped.records.rows();
    std::printf("records: file %s bytes=%d rows=%d fnv=0x%llx\n", shipped.path.c_str(),
                static_cast<int>(shipped.bytes.size()), static_cast<int>(rows.size()),
                static_cast<unsigned long long>(fnv1a64(shipped.bytes)));
    check(shipped.ok, "the shipped file loads");
    check(shipped.bytes.size() == static_cast<std::size_t>(gm::Records::kShippedBytes),
          "bytes: " + std::to_string(shipped.bytes.size()));
    check(gm::Records::kRecordBytes == 0x40, "record size 0x40");
    check(gm::Records::kNameBytes == 60, "name field 60");
    check(gm::Records::kShippedRecords == 9, "shipped records 9");
    check(gm::Records::kShippedBytes == 576, "shipped bytes 576");
    check(rows.size() == 9, "rows: " + std::to_string(rows.size()));

    std::printf("records: rows times=%s names=%s\n", joined_times(rows).c_str(),
                joined_names(rows).c_str());
    const int times[9] = {146, 1570, 18, 109, 18, 7, 11, 40, 6};
    const char* names[9] = {"TESTING RANGE", "KELLER CORE",    "TESTING RANGE", "POWER STATION",
                            "NATAKA TOWER",  "NATAKA TOWER",  "OIL RIG",       "OIL REFINERY",
                            "CADTLELTL"};
    for (int i = 0; i < 9 && i < static_cast<int>(rows.size()); ++i) {
        const std::string where = "row " + std::to_string(i);
        check(rows[static_cast<std::size_t>(i)].seconds == times[i],
              where + " seconds: " + std::to_string(rows[static_cast<std::size_t>(i)].seconds) +
                  " expected " + std::to_string(times[i]));
        check(rows[static_cast<std::size_t>(i)].name == names[i],
              where + " name: " + in_quotes(rows[static_cast<std::size_t>(i)].name));
    }

    int duplicates = 0;
    std::string duplicate_rows;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const bool expected = i == 2 || i == 5;
        check(rows[i].duplicate == expected,
              "row " + std::to_string(i) + " duplicate flag");
        if (rows[i].duplicate) {
            ++duplicates;
            duplicate_rows += (duplicate_rows.empty() ? "" : ",") + std::to_string(i);
        }
    }
    std::printf("records: duplicates=%d rows=%s\n", duplicates, duplicate_rows.c_str());
    check(duplicates == 2, "duplicate rows: " + std::to_string(duplicates));

    // The minimum per name and the derived list (`0x416850`, `0x45c8d6`).
    const std::vector<gm::RecordView> best = shipped.records.best_by_name();
    std::printf("records: best names=%d %s\n", static_cast<int>(best.size()),
                joined_views(best).c_str());
    check(best.size() == 7, "distinct names: " + std::to_string(best.size()));
    check(shipped.records.best_time("TESTING RANGE") == 18,
          "TESTING RANGE best: " + std::to_string(shipped.records.best_time("TESTING RANGE")));
    check(shipped.records.best_time("testing range") == 18, "the lookup folds case");
    check(shipped.records.best_time("KELLER CORE") == 1570,
          "KELLER CORE best: " + std::to_string(shipped.records.best_time("KELLER CORE")));
    check(shipped.records.best_time("NATAKA TOWER") == 7,
          "NATAKA TOWER best: " + std::to_string(shipped.records.best_time("NATAKA TOWER")));
    check(shipped.records.best_time("CADTLELTL") == 6, "CADTLELTL best 6");
    check(shipped.records.best_time("NO SUCH LEVEL") == -1, "a missing name is -1");

    int entries = 0;
    int view_dupes = 0;
    for (const gm::RecordView& view : best) {
        entries += view.entries;
        if (view.entries > 1) {
            ++view_dupes;
        }
    }
    check(entries == 9, "entries sum: " + std::to_string(entries));
    check(view_dupes == 2, "names with 2+ records: " + std::to_string(view_dupes));
    check(best.size() == 7 && best[0].name == "CADTLELTL" && best[0].seconds == 6,
          "the list starts with the fastest record");
    check(best.size() == 7 && best[6].name == "KELLER CORE" && best[6].seconds == 1570,
          "the list ends with the slowest record");
    for (std::size_t i = 1; i < best.size(); ++i) {
        check(best[i - 1].seconds <= best[i].seconds, "the list is sorted by seconds");
    }
}

void test_better() {
    const Shipped shipped = load_shipped();
    if (shipped.path.empty()) {
        return;
    }
    const gm::Records& records = shipped.records;
    std::string error;

    // Strictly smaller wins (`0x45c8d6`): 17 < 18, 18 is not < 18.
    check(records.better(17, "TESTING RANGE", &error),
          "17 improves TESTING RANGE 18: " + in_quotes(error));
    check(error.empty(), "an improvement has no error text");
    check(!records.better(18, "TESTING RANGE", &error),
          "18 does not improve 18 (strict comparison)");
    check(error.empty(), "a non-improvement is not an error: " + in_quotes(error));
    check(!records.better(19, "TESTING RANGE", &error), "19 does not improve 18");
    check(!records.better(1570, "KELLER CORE", &error), "1570 does not improve 1570");
    check(records.better(1569, "KELLER CORE", &error), "1569 improves 1570");
    check(!records.better(18, "testing range", &error),
          "a different case is the same name: 18 does not improve 18");
    check(records.better(17, "Testing Range", &error),
          "a different case still improves: 17 < 18");
    check(records.better(100, "NEW LEVEL", &error),
          "an unknown name accepts any time: " + in_quotes(error));
    check(!records.better(7, "NATAKA TOWER", &error), "7 does not improve 7");
    check(records.better(6, "NATAKA TOWER", &error), "6 improves NATAKA TOWER 7");
    std::printf("records: better 17<18=1 18<18=0 19<18=0 1569<1570=1 18@lowercase=0 "
                "17@MixedCase=1 new_name=1 6<7=1\n");

    // The decision never writes: the shipped file is a data file of the player
    // and the module only compares in memory.
    const std::string after = read_bytes(shipped.path);
    check(after == shipped.bytes, "better() leaves the shipped file untouched");
    check(shipped.records.rows().size() == 9, "better() leaves the rows untouched");
}

void test_append() {
    const Shipped shipped = load_shipped();
    if (shipped.path.empty()) {
        return;
    }
    std::error_code ec;
    const fs::path copy = temp_file("appended.hi");
    check(write_bytes(copy, shipped.bytes), "the scratch copy is written");

    gm::Records records;
    std::string error;
    check(records.load(copy, &error), "the scratch copy loads: " + in_quotes(error));
    check(records.append_and_save(copy, 5, "TESTING RANGE", &error),
          "append 5 TESTING RANGE: " + in_quotes(error));
    check(error.empty(), "a clean append has no error text");

    const std::string grown = read_bytes(copy);
    const std::intmax_t size = static_cast<std::intmax_t>(fs::file_size(copy, ec));
    check(size == 640, "the copy is 640 bytes: " + std::to_string(size));
    check(grown.size() == 640, "the grown file is 640 bytes");
    check(grown.size() >= shipped.bytes.size() &&
              grown.substr(0, shipped.bytes.size()) == shipped.bytes,
          "the first 576 bytes are byte-identical");
    check(grown.size() == shipped.bytes.size() + 64, "the file grew by exactly 0x40");

    // Byte-by-byte the new record: i32 5 little-endian, then the name, then zero
    // padding to 60 bytes (`"ab"`, one 0x40 record).
    const std::string expected_tail = make_record(5, "TESTING RANGE");
    bool tail_ok = grown.size() == 640;
    std::string dump;
    for (std::size_t i = 0; tail_ok && i < expected_tail.size(); ++i) {
        const char actual = grown[576 + i];
        tail_ok = actual == expected_tail[i];
        if (i < 16) {
            char byte[8];
            std::snprintf(byte, sizeof(byte), "%02x ", static_cast<unsigned char>(actual));
            dump += byte;
        }
    }
    std::printf("records: append tail bytes %s\n", dump.c_str());
    check(tail_ok, "the appended record is i32 5 + the 60-byte name field");

    gm::Records reread;
    check(reread.load(copy, &error), "the grown file re-reads: " + in_quotes(error));
    std::printf("records: append bytes=%d->%d head_same=%d tail=%d,%s reread=%d "
                "best_TESTING RANGE=%d\n",
                static_cast<int>(shipped.bytes.size()), static_cast<int>(size),
                grown.substr(0, shipped.bytes.size()) == shipped.bytes ? 1 : 0,
                reread.rows().empty() ? -1 : reread.rows().back().seconds,
                reread.rows().empty() ? "" : reread.rows().back().name.c_str(),
                static_cast<int>(reread.rows().size()), reread.best_time("TESTING RANGE"));
    check(reread.rows().size() == 10, "10 rows after the append: " +
                                          std::to_string(reread.rows().size()));
    const gm::RecordRow& last = reread.rows()[9];
    check(last.seconds == 5, "the last row is 5: " + std::to_string(last.seconds));
    check(last.name == "TESTING RANGE", "the last row name: " + in_quotes(last.name));
    check(last.duplicate, "the appended TESTING RANGE is a duplicate");
    check(reread.rows()[0].seconds == 146 && reread.rows()[2].seconds == 18,
          "the earlier rows keep their times");
    check(reread.best_time("TESTING RANGE") == 5,
          "the new minimum is 5: " + std::to_string(reread.best_time("TESTING RANGE")));
    const std::vector<gm::RecordView> best = reread.best_by_name();
    check(best.size() == 7, "still 7 distinct names");
    if (!best.empty()) {
        check(best[0].name == "TESTING RANGE" && best[0].seconds == 5 && best[0].entries == 3,
              "TESTING RANGE 5 with 3 entries is now the fastest");
    }
    check(reread.format_time(reread.best_time("TESTING RANGE")) == "0 : 05",
          "the screen text of the new minimum");

    // `"ab"` creates a missing file: a fresh path becomes a 1-record table.
    const fs::path fresh = temp_file("fresh.hi");
    fs::remove(fresh, ec);
    gm::Records created;
    check(created.append_and_save(fresh, 42, "OIL RIG", &error),
          "append to a missing path: " + in_quotes(error));
    check(fs::file_size(fresh, ec) == 64, "the created file is 64 bytes");
    check(created.load(fresh, &error) && created.rows().size() == 1,
          "the created file reads as 1 row");
    check(created.rows()[0].seconds == 42 && created.rows()[0].name == "OIL RIG" &&
              !created.rows()[0].duplicate,
          "the created row is 42 OIL RIG, not a duplicate");

    fs::remove(copy, ec);
    fs::remove(fresh, ec);
}

void test_format_time() {
    gm::Records records;
    const int values[5] = {6, 18, 47, 109, 1570};
    const char* texts[5] = {"0 : 06", "0 : 18", "0 : 47", "1 : 49", "26 : 10"};
    std::string printed;
    for (int i = 0; i < 5; ++i) {
        const std::string text = records.format_time(values[i]);
        printed += " " + std::to_string(values[i]) + "=" + text;
        check(text == texts[i], "format_time(" + std::to_string(values[i]) + ") = " + text +
                                    " expected " + texts[i]);
    }
    std::printf("records: format time%s 0=UNKNOWN\n", printed.c_str());
    check(records.format_time(0) == "UNKNOWN", "0 is UNKNOWN (`0x4bcbd4`)");
    check(records.format_time(-1) == "UNKNOWN", "a negative value is UNKNOWN too (model)");
    check(records.format_time(59) == "0 : 59", "59 is 0 : 59");
    check(records.format_time(60) == "1 : 00", "60 is 1 : 00");
    check(records.format_time(61) == "1 : 01", "61 is 1 : 01");
    check(records.format_time(600) == "10 : 00", "600 is 10 : 00");
    check(records.format_time(3599) == "59 : 59", "3599 is 59 : 59");
    check(records.format_time(3600) == "60 : 00", "3600 is 60 : 00");
}

void test_negatives() {
    const Shipped shipped = load_shipped();
    std::string error;

    // Missing file.
    gm::Records empty;
    fs::path missing = temp_file("does_not_exist.hi");
    std::error_code ec;
    fs::remove(missing, ec);
    error = "sentinel";
    check(!empty.load(missing, &error), "a missing file is refused");
    check(!error.empty() && error != "sentinel", "the refusal has a text: " + in_quotes(error));
    check(empty.rows().empty() && !empty.loaded(), "a refused load leaves no rows");
    check(empty.last_error() == error, "last_error() carries the same text");

    if (shipped.path.empty()) {
        std::printf("records: shipped file not installed, the size negatives are synthetic\n");
    }

    // 575 and 577 bytes.
    const std::string table = shipped.bytes.empty() ? std::string(576, '\0') : shipped.bytes;
    const fs::path short_file = temp_file("short.hi");
    const fs::path long_file = temp_file("long.hi");
    check(write_bytes(short_file, std::string_view(table).substr(0, 575)),
          "the 575-byte file is written");
    check(write_bytes(long_file, table + std::string(1, '\0')), "the 577-byte file is written");
    gm::Records bad;
    error = "sentinel";
    check(!bad.load(short_file, &error), "575 bytes are refused");
    check(contains(error, "not a multiple of 64"), "575-byte text: " + in_quotes(error));
    error.clear();
    check(!bad.load(long_file, &error), "577 bytes are refused");
    check(contains(error, "not a multiple of 64"), "577-byte text: " + in_quotes(error));
    std::printf("records: negatives size 575=%s 577=%s\n", "refused", "refused");

    // A negative time in a record is corrupt (the clock is ticks / 91).
    const fs::path negative_file = temp_file("negative.hi");
    check(write_bytes(negative_file, make_record(-1, "TESTING RANGE")),
          "the negative-time file is written");
    check(!bad.load(negative_file, &error), "a negative stored time is refused");
    check(contains(error, "negative time"), "negative-time text: " + in_quotes(error));

    // A 60-byte name field without a NUL: truncated, not refused (`MODEL`).
    std::string wide = make_record(12, "");
    for (std::size_t i = 4; i < 64; ++i) {
        wide[i] = 'A';
    }
    const fs::path wide_file = temp_file("wide.hi");
    check(write_bytes(wide_file, wide), "the 60-character name file is written");
    gm::Records truncated;
    check(truncated.load(wide_file, &error), "a 60-byte name without NUL still loads");
    check(truncated.rows().size() == 1 && truncated.rows()[0].name.size() == 60,
          "the name is cut at 60 bytes: " + std::to_string(truncated.rows()[0].name.size()));
    check(truncated.rows()[0].name == std::string(60, 'A'), "the 60 characters survive");

    // Writing a 60-byte name would lose the terminator: refused.
    const fs::path append_file = temp_file("append_negatives.hi");
    check(write_bytes(append_file, table), "the append scratch file is written");
    const std::string before = read_bytes(append_file);
    gm::Records writer;
    error.clear();
    check(!writer.append_and_save(append_file, 10, std::string(60, 'B'), &error),
          "a 60-byte name is refused on write");
    check(contains(error, "at most 59"), "60-byte name text: " + in_quotes(error));
    error.clear();
    check(!writer.append_and_save(append_file, 10, "", &error), "an empty name is refused");
    check(contains(error, "empty name"), "empty-name text: " + in_quotes(error));
    error.clear();
    check(!writer.append_and_save(append_file, 10, std::string("A\0B", 3), &error),
          "a name with a NUL is refused");
    check(contains(error, "NUL"), "NUL text: " + in_quotes(error));
    error.clear();
    check(!writer.append_and_save(append_file, -5, "TESTING RANGE", &error),
          "a negative time is refused on write");
    check(contains(error, "negative time"), "negative write text: " + in_quotes(error));
    check(read_bytes(append_file) == before, "the refused writes leave the file untouched");

    // Appending onto a truncated table would ruin it: refused.
    error.clear();
    check(!writer.append_and_save(short_file, 10, "TESTING RANGE", &error),
          "appending to a 575-byte file is refused");
    check(contains(error, "refusing to append"), "truncated append text: " + in_quotes(error));

    // `better()` refusals.
    check(!writer.better(-1, "TESTING RANGE", &error), "a negative better() is refused");
    check(contains(error, "negative time"), "negative better() text: " + in_quotes(error));
    error.clear();
    check(!writer.better(10, "", &error), "an empty name in better() is refused");
    check(contains(error, "empty name"), "empty better() text: " + in_quotes(error));
    error.clear();
    check(!writer.better(10, std::string(60, 'C'), &error),
          "a 60-byte name in better() is refused");
    check(contains(error, "at most 59"), "60-byte better() text: " + in_quotes(error));

    // A 0-byte file is the empty table, not a refusal.
    const fs::path empty_file = temp_file("zero.hi");
    check(write_bytes(empty_file, ""), "the 0-byte file is written");
    gm::Records zero;
    error = "sentinel";
    check(zero.load(empty_file, &error), "a 0-byte file is the empty table");
    check(error.empty(), "the empty table has no error text");
    check(zero.rows().empty(), "the empty table has no rows");
    check(zero.best_by_name().empty(), "the empty table has no best list");
    check(zero.best_time("TESTING RANGE") == -1, "the empty table has no minimum");
    check(zero.better(1, "TESTING RANGE", &error), "any time improves an empty table");

    // A refusal keeps the rows of the last good load.
    if (!shipped.path.empty()) {
        gm::Records kept;
        check(kept.load(shipped.path, &error), "the shipped file loads again");
        check(!kept.load(short_file, &error), "the truncated file is still refused");
        check(kept.rows().size() == 9, "the refused load keeps the 9 rows: " +
                                           std::to_string(kept.rows().size()));
        check(kept.rows()[0].seconds == 146, "the kept row 0 is still 146");
        check(kept.loaded(), "the refused load keeps `loaded()`");
    }

    fs::remove(short_file, ec);
    fs::remove(long_file, ec);
    fs::remove(negative_file, ec);
    fs::remove(wide_file, ec);
    fs::remove(append_file, ec);
    fs::remove(empty_file, ec);
}

void test_determinism() {
    const Shipped first = load_shipped();
    if (first.path.empty()) {
        return;
    }
    gm::Records second;
    std::string error;
    check(second.load(first.path, &error), "the second load succeeds");
    check(second.rows().size() == first.records.rows().size(), "the same row count");

    bool same_rows = second.rows().size() == first.records.rows().size();
    for (std::size_t i = 0; same_rows && i < second.rows().size(); ++i) {
        const gm::RecordRow& a = first.records.rows()[i];
        const gm::RecordRow& b = second.rows()[i];
        same_rows = a.seconds == b.seconds && a.name == b.name && a.duplicate == b.duplicate;
    }
    check(same_rows, "the same rows, seconds, names and flags");

    const std::vector<gm::RecordView> a = first.records.best_by_name();
    const std::vector<gm::RecordView> b = second.best_by_name();
    bool same_best = a.size() == b.size();
    for (std::size_t i = 0; same_best && i < a.size(); ++i) {
        same_best = a[i].name == b[i].name && a[i].seconds == b[i].seconds &&
                    a[i].entries == b[i].entries;
    }
    check(same_best, "the same best_by_name() order and numbers");
    check(joined_views(a) == joined_views(b), "the same printed list");
    check(first.records.format_time(first.records.best_time("TESTING RANGE")) ==
              second.format_time(second.best_time("TESTING RANGE")),
          "the same time text");
    std::printf("records: deterministic rows=%d best=%d text=%s\n",
                static_cast<int>(second.rows().size()), static_cast<int>(a.size()),
                second.format_time(second.best_time("TESTING RANGE")).c_str());
}

} // namespace

int main() {
    test_shipped_file();
    test_better();
    test_append();
    test_format_time();
    test_negatives();
    test_determinism();

    if (failures == 0) {
        if (g_shipped_ok) {
            std::printf("records: %d checks, ok (rows=9 best=7 duplicates=2)\n", checks);
        } else {
            std::printf("records: %d checks, ok (shipped file not installed)\n", checks);
        }
    }
    return failures == 0 ? 0 : 1;
}
