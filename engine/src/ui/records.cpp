#include "ui/records.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <map>
#include <utility>

#include "game/campaign_state.h"

namespace gm {
namespace {

// The layout of this module and of the campaign codec must not drift apart:
// both describe the same `i32 seconds; char name[60]` record (§6).
static_assert(Records::kRecordBytes == kHiRecordSize, "GUNMETAL.HI record size");
static_assert(Records::kNameBytes == kHiNameSize, "GUNMETAL.HI name field");
static_assert(Records::kShippedRecords == kHiRecords, "GUNMETAL.HI record count");
static_assert(Records::kShippedBytes == kHiFileSize, "GUNMETAL.HI shipped size");

std::uint32_t read_u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// Folded name used by the sorting of `best_by_name()` only; the comparison
/// itself is `record_name_equal`.
std::string ascii_fold(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        c = ascii_upper(c);
    }
    return out;
}

/// `MODEL` refusals of a name that is going to be written: the 60-byte field
/// has to keep its terminator, so 59 bytes is the limit.
bool check_name(const std::string& name, std::string& error) {
    if (name.empty()) {
        error = "GUNMETAL.HI: empty name";
        return false;
    }
    if (name.find('\0') != std::string::npos) {
        error = "GUNMETAL.HI: name contains a NUL";
        return false;
    }
    if (name.size() > static_cast<std::size_t>(Records::kMaxNameBytes)) {
        error = "GUNMETAL.HI: name is " + std::to_string(name.size()) + " bytes, at most " +
                std::to_string(Records::kMaxNameBytes) + " fit the 60-byte field";
        return false;
    }
    return true;
}

bool check_seconds(int seconds, std::string& error) {
    if (seconds < 0) {
        error = "GUNMETAL.HI: negative time " + std::to_string(seconds);
        return false;
    }
    return true;
}

} // namespace

bool record_name_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_upper(a[i]) != ascii_upper(b[i])) {
            return false;
        }
    }
    return true;
}

Records::Records() = default;

bool Records::fail(std::string text, std::string* error) const {
    last_error_ = std::move(text);
    if (error != nullptr) {
        *error = last_error_;
    }
    return false;
}

bool Records::load(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        return fail("cannot stat " + file.string() + ": " + ec.message(), error);
    }
    const std::uintmax_t cap =
        static_cast<std::uintmax_t>(kMaxRecords) * static_cast<std::uintmax_t>(kRecordBytes);
    if (size > cap) {
        return fail(file.string() + " is " + std::to_string(size) + " bytes, larger than " +
                        std::to_string(cap) + " (" + std::to_string(kShippedBytes) +
                        " is the shipped table)",
                    error);
    }
    if (size % static_cast<std::uintmax_t>(kRecordBytes) != 0) {
        return fail(file.string() + " is " + std::to_string(size) +
                        " bytes, not a multiple of " + std::to_string(kRecordBytes) + " (" +
                        std::to_string(kShippedBytes) + " is the shipped table)",
                    error);
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return fail("cannot open " + file.string(), error);
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size), 0);
    if (size != 0 &&
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return fail("cannot read " + file.string(), error);
    }

    // Parse into a scratch table: a refusal leaves the loaded rows untouched.
    const int count = static_cast<int>(size / static_cast<std::size_t>(kRecordBytes));
    std::vector<RecordRow> parsed;
    parsed.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const unsigned char* rec = bytes.data() + static_cast<std::size_t>(i) * kRecordBytes;
        RecordRow row;
        row.seconds = static_cast<int>(read_u32(rec));
        std::string problem;
        if (!check_seconds(row.seconds, problem)) {
            return fail("record " + std::to_string(i) + ": " + problem, error);
        }
        const char* field = reinterpret_cast<const char*>(rec + 4);
        std::size_t length = 0;
        while (length < static_cast<std::size_t>(kNameBytes) && field[length] != '\0') {
            ++length;
        }
        // `MODEL`: a field without a terminator is cut at its 60 bytes instead
        // of reading into the next record.
        row.name.assign(field, length);
        parsed.push_back(std::move(row));
    }
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (record_name_equal(parsed[i].name, parsed[j].name)) {
                parsed[i].duplicate = true;
                break;
            }
        }
    }
    rows_ = std::move(parsed);
    loaded_ = true;
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::vector<RecordView> Records::best_by_name() const {
    std::vector<RecordView> views;
    std::map<std::string, std::size_t> index; // folded name -> views[] slot
    for (const RecordRow& row : rows_) {
        const std::string key = ascii_fold(row.name);
        const auto it = index.find(key);
        if (it == index.end()) {
            index.emplace(key, views.size());
            views.push_back(RecordView{row.name, row.seconds, 1});
            continue;
        }
        RecordView& view = views[it->second];
        // Strictly smaller: the first spelling of a name is kept, as `0x45c8d6`
        // only replaces the time.
        if (row.seconds < view.seconds) {
            view.seconds = row.seconds;
        }
        ++view.entries;
    }
    std::stable_sort(views.begin(), views.end(), [](const RecordView& a, const RecordView& b) {
        if (a.seconds != b.seconds) {
            return a.seconds < b.seconds;
        }
        const std::string fa = ascii_fold(a.name);
        const std::string fb = ascii_fold(b.name);
        return fa != fb ? fa < fb : a.name < b.name;
    });
    return views;
}

int Records::best_time(std::string_view name) const {
    int best = -1;
    for (const RecordRow& row : rows_) {
        if (!record_name_equal(row.name, name)) {
            continue;
        }
        if (best < 0 || row.seconds < best) {
            best = row.seconds;
        }
    }
    return best;
}

bool Records::better(int seconds, const std::string& name, std::string* error) const {
    std::string problem;
    if (!check_name(name, problem) || !check_seconds(seconds, problem)) {
        return fail(problem, error);
    }
    const int best = best_time(name);
    if (best < 0) {
        last_error_.clear();
        if (error != nullptr) {
            error->clear();
        }
        return true; // no record for this name yet: anything is an improvement
    }
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return seconds < best;
}

bool Records::append_and_save(const std::filesystem::path& file, int seconds,
                              const std::string& name, std::string* error) {
    std::string problem;
    if (!check_name(name, problem) || !check_seconds(seconds, problem)) {
        return fail(problem, error);
    }

    // Do not append to a table that is already not a record table.
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec) {
        return fail("cannot stat " + file.string() + ": " + ec.message(), error);
    }
    if (exists) {
        const std::uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec) {
            return fail("cannot stat " + file.string() + ": " + ec.message(), error);
        }
        if (size % static_cast<std::uintmax_t>(kRecordBytes) != 0) {
            return fail(file.string() + " is " + std::to_string(size) +
                            " bytes, not a multiple of " + std::to_string(kRecordBytes) +
                            ": refusing to append",
                        error);
        }
        if (size >= static_cast<std::uintmax_t>(kMaxRecords) * kRecordBytes) {
            return fail(file.string() + " already holds the read guard of " +
                            std::to_string(kMaxRecords) + " records: refusing to append", error);
        }
    }

    // `"ab"` (`0x4b8690`): the append itself, after the reads of `0x40dc70`.
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        return fail("cannot open " + file.string() + " for append", error);
    }
    std::array<char, kRecordBytes> record{};
    record[0] = static_cast<char>(seconds & 0xFF);
    record[1] = static_cast<char>((seconds >> 8) & 0xFF);
    record[2] = static_cast<char>((seconds >> 16) & 0xFF);
    record[3] = static_cast<char>((seconds >> 24) & 0xFF);
    for (std::size_t i = 0; i < name.size(); ++i) {
        record[4 + i] = name[i];
    }
    out.write(record.data(), static_cast<std::streamsize>(record.size()));
    out.flush();
    if (!out) {
        return fail("cannot write " + file.string(), error);
    }
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::string Records::format_time(int seconds) const {
    // `0x483f00`: `"%ld : %ld%ld"` (`0x4bcbdc`), `UNKNOWN` (`0x4bcbd4`) at zero.
    if (seconds <= 0) {
        return "UNKNOWN";
    }
    const int minutes = seconds / 60;
    const int rest = seconds % 60;
    char text[32];
    std::snprintf(text, sizeof(text), "%d : %d%d", minutes, rest / 10, rest % 10);
    return text;
}

} // namespace gm
