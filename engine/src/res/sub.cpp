#include "res/sub.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

namespace gm {
namespace {

constexpr std::size_t kNameMax = 255; ///< `%s` into a 256-byte buffer, `0x422ff1`
constexpr std::size_t kKeyMax = 8;    ///< `char name[9]` + `strncpy(..., 9)`

std::string trim_cr(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

bool is_blank(std::string_view line) {
    return std::all_of(line.begin(), line.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
}

/// Strict decimal `int`: optional sign, at least one digit, no junk, no wrap.
bool parse_int(const std::string& text, int& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        i = 1;
    }
    if (i == text.size()) {
        return false;
    }
    long long value = 0;
    for (; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > std::numeric_limits<int>::max()) {
            return false;
        }
    }
    out = static_cast<int>(negative ? -value : value);
    return true;
}

std::vector<std::string> split_ws(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i])) != 0) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i])) == 0) {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return out;
}

void fail(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

} // namespace

std::string sub_key(const std::string& name) {
    std::string key = name.substr(0, std::min(name.size(), kKeyMax));
    for (char& c : key) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return key;
}

bool load_sub(const std::filesystem::path& file, SubKind kind, SubTable& out,
              std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    std::string ext = file.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext != ".sub") {
        fail(error, "not a .sub file: " + file.filename().string());
        return false;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        fail(error, "cannot open " + file.string());
        return false;
    }
    const std::string data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (data.find_first_not_of(" \t\r\n") == std::string::npos) {
        fail(error, "empty file: " + file.filename().string());
        return false;
    }

    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = data.find('\n', start);
        const std::string_view raw =
            nl == std::string::npos ? std::string_view(data).substr(start)
                                    : std::string_view(data).substr(start, nl - start);
        if (!is_blank(raw)) {
            lines.push_back(trim_cr(std::string(raw)));
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }

    SubTable table;
    table.kind = kind;
    table.source = file.string();
    std::vector<std::string> warnings;

    std::size_t line_no = 0; ///< 1-based index into the non-blank `lines`
    std::size_t first_record = 0;
    if (kind != SubKind::sounds) {
        int counter = 0;
        if (!parse_int(lines[0], counter)) {
            fail(error, "line 1: count is not a number: '" + lines[0] + "'");
            return false;
        }
        if (counter < 0) {
            fail(error, "line 1: negative count " + std::to_string(counter));
            return false;
        }
        table.declared = static_cast<std::size_t>(counter);
        first_record = 1;
        line_no = 1;
    }

    for (std::size_t i = first_record; i < lines.size(); ++i) {
        ++line_no;
        const std::string& line = lines[i];
        SubEntry entry;
        if (kind == SubKind::sounds) {
            const std::vector<std::string> fields = split_tab(line);
            if (fields.size() != 2) {
                fail(error, "line " + std::to_string(line_no) +
                                ": expected <from><TAB><to>, got " +
                                std::to_string(fields.size()) + " field(s)");
                return false;
            }
            entry.from = fields[0];
            entry.to = fields[1];
        } else {
            const std::vector<std::string> fields = split_ws(line);
            if (fields.size() != 3) {
                fail(error, "line " + std::to_string(line_no) +
                                ": expected <weight> <from> <to>, got " +
                                std::to_string(fields.size()) + " field(s)");
                return false;
            }
            if (!parse_int(fields[0], entry.weight)) {
                fail(error, "line " + std::to_string(line_no) +
                                ": weight is not a number: '" + fields[0] + "'");
                return false;
            }
            if (entry.weight < 0) {
                fail(error, "line " + std::to_string(line_no) +
                                ": negative weight " +
                                std::to_string(entry.weight));
                return false;
            }
            entry.from = fields[1];
            entry.to = fields[2];
        }
        for (const std::string* name : {&entry.from, &entry.to}) {
            if (name->empty()) {
                fail(error, "line " + std::to_string(line_no) + ": empty name");
                return false;
            }
            if (name->size() > kNameMax) {
                fail(error, "line " + std::to_string(line_no) + ": name longer than " +
                                std::to_string(kNameMax) + " bytes");
                return false;
            }
        }
        if (entry.from == entry.to) {
            warnings.push_back("row " + std::to_string(table.pairs.size() + 1) +
                               ": from == to ('" + entry.from + "') is a no-op");
        }
        table.pairs.push_back(std::move(entry));
    }

    if (kind != SubKind::sounds) {
        if (table.pairs.size() > table.declared) {
            warnings.push_back("declared " + std::to_string(table.declared) +
                               " but " + std::to_string(table.pairs.size()) +
                               " records; the extra rows are ignored (the loader "
                               "reads exactly count records)");
            table.pairs.resize(table.declared);
        } else if (table.pairs.size() < table.declared) {
            warnings.push_back("declared " + std::to_string(table.declared) + " but " +
                               std::to_string(table.pairs.size()) +
                               " records (the loader would abort the whole table)");
        }
    }

    out = std::move(table);
    if (error != nullptr && !warnings.empty()) {
        std::string text = "warning: ";
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            text += (i == 0 ? "" : "; ") + warnings[i];
        }
        *error = std::move(text);
    }
    return true;
}

bool has(const SubTable& table, const std::string& name) {
    const std::string key = sub_key(name);
    return std::any_of(table.pairs.begin(), table.pairs.end(),
                       [&key](const SubEntry& e) { return sub_key(e.to) == key; });
}

std::string resolve(const SubTable& table, const std::string& name) {
    const std::string key = sub_key(name);
    for (const SubEntry& entry : table.pairs) {
        if (sub_key(entry.to) == key) {
            return entry.from;
        }
    }
    return name;
}

void bind_indices(SubTable& table, const std::vector<std::string>& names) {
    std::vector<std::pair<std::string, int>> lookup;
    lookup.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        lookup.emplace_back(sub_key(names[i]), static_cast<int>(i));
    }
    const auto find_index = [&lookup](const std::string& name) {
        const std::string key = sub_key(name);
        for (const auto& [candidate, index] : lookup) {
            if (candidate == key) {
                return index;
            }
        }
        return -1;
    };

    table.bound = true;
    table.bound_size = names.size();
    table.from_index.assign(table.pairs.size(), -1);
    table.to_index.assign(table.pairs.size(), -1);
    for (std::size_t i = 0; i < table.pairs.size(); ++i) {
        table.from_index[i] = find_index(table.pairs[i].from);
        table.to_index[i] = find_index(table.pairs[i].to);
    }
}

bool has(const SubTable& table, int index) {
    if (!table.bound || index < 0 ||
        static_cast<std::size_t>(index) >= table.bound_size) {
        return false;
    }
    return std::any_of(table.to_index.begin(), table.to_index.end(),
                       [index](int slot) { return slot == index; });
}

int resolve(const SubTable& table, int index) {
    if (!table.bound || index < 0 ||
        static_cast<std::size_t>(index) >= table.bound_size) {
        return -1;
    }
    for (std::size_t i = 0; i < table.to_index.size(); ++i) {
        if (table.to_index[i] == index) {
            return table.from_index[i];
        }
    }
    return index;
}

bool applies(const SubEntry& entry, int ram_class) {
    return entry.weight < 0 || ram_class <= entry.weight;
}

} // namespace gm
