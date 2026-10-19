#include "core/path.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <vector>

namespace gm {
namespace {

char ascii_lower(char c) {
    const auto u = static_cast<unsigned char>(c);
    return static_cast<char>(std::tolower(u));
}

std::vector<std::string> split_components(std::string_view relative) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : relative) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

std::optional<std::string> find_ci(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return std::nullopt;
    }
    for (const auto& entry : it) {
        const std::string filename = entry.path().filename().string();
        if (iequals_ascii(filename, name)) {
            return filename;
        }
    }
    return std::nullopt;
}

} // namespace

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> resolve_ci(const std::filesystem::path& root,
                                                std::string_view relative) {
    std::filesystem::path current = root;
    for (std::string& part : split_components(relative)) {
        if (part == ".") {
            continue;
        }
        if (part == "..") {
            current = current.parent_path();
            continue;
        }
        std::error_code ec;
        const std::filesystem::path exact = current / part;
        if (std::filesystem::exists(exact, ec)) {
            current = exact;
            continue;
        }
        const auto matched = find_ci(current, part);
        if (!matched) {
            return std::nullopt;
        }
        current /= *matched;
    }
    return current;
}

} // namespace gm
