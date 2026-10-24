#include "ui/killed.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/path.h"

namespace gm {
namespace {

/// Bytes of the two spellings the data uses (`%S` in the file, `%s` after the
/// `0x485ad6` normalisation).
bool starts_placeholder(std::string_view text, std::size_t at) {
    return at + 1 < text.size() && text[at] == '%' &&
           (text[at + 1] == 'S' || text[at + 1] == 's');
}

std::size_t count_placeholders(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (starts_placeholder(text, i)) {
            ++count;
            ++i;
        }
    }
    return count;
}

/// A line of nothing but spaces/tabs (model: such a message prints nothing).
bool is_blank(std::string_view text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t') {
            return false;
        }
    }
    return true;
}

std::optional<std::string> read_file(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot stat " + path.string() + ": " + ec.message();
        return std::nullopt;
    }
    if (size == 0) {
        error = path.string() + ": empty file (0 bytes)";
        return std::nullopt;
    }
    if (size > static_cast<std::uintmax_t>(kKilledMaxBytes)) {
        error = path.string() + " is " + std::to_string(size) + " bytes, larger than " +
                std::to_string(kKilledMaxBytes) + " (the shipped files are 1967 and 2338)";
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = "cannot read " + path.string();
        return std::nullopt;
    }
    return bytes;
}

/// Split on CRLF or LF.  `\r\n` is one terminator and a trailing `\r` of a
/// CRLF line never reaches the message; a final terminator does not add an
/// empty line.
std::vector<std::string> split_lines(std::string_view bytes) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') {
            lines.push_back(std::move(current));
            current.clear();
            ++i;
            continue;
        }
        if (c == '\n') {
            lines.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        lines.push_back(std::move(current));
    }
    return lines;
}

/// The engine keeps a line only when `0 < strlen(line) < 0x20`, and `strlen`
/// stops at a NUL byte: a message is the bytes before it.
std::string_view message_of(std::string_view line) {
    const std::size_t nul = line.find('\0');
    return nul == std::string_view::npos ? line : line.substr(0, nul);
}

} // namespace

bool load_killed(const std::filesystem::path& game, const std::string& player_name,
                 KilledScreen& screen, std::string* error) {
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    std::optional<std::filesystem::path> path;
    std::string source;
    if (const auto primary = resolve_ci(game, kKilledPrimaryFile)) {
        path = *primary;
        source = kKilledPrimaryFile;
    } else if (const auto fallback = resolve_ci(game, kKilledFallbackFile)) {
        path = *fallback;
        source = kKilledFallbackFile;
    } else {
        return fail("killed: neither " + std::string(kKilledPrimaryFile) + " nor " +
                    std::string(kKilledFallbackFile) + " under " + game.string());
    }

    std::string read_error;
    const auto bytes = read_file(*path, read_error);
    if (!bytes) {
        return fail("killed: " + read_error);
    }

    std::vector<std::string> raw_lines;
    for (std::string& line : split_lines(*bytes)) {
        const std::string_view message = message_of(line);
        if (message.empty() || is_blank(message) ||
            message.size() > kKilledMaxLineChars) {
            continue;
        }
        if (raw_lines.size() == kKilledMaxMessages) {
            break;
        }
        raw_lines.emplace_back(message);
    }
    if (raw_lines.empty()) {
        return fail("killed: " + source + " has no message (every line is empty, blank or "
                    "longer than " + std::to_string(kKilledMaxLineChars) + " bytes)");
    }

    if (player_name.find('\0') != std::string::npos) {
        return fail("killed: the player name contains a NUL");
    }
    if (player_name.size() > kKilledMaxNameBytes) {
        return fail("killed: the player name is " + std::to_string(player_name.size()) +
                    " bytes, at most " + std::to_string(kKilledMaxNameBytes) +
                    " fit the 16-byte save slot");
    }

    std::vector<KilledLine> lines;
    lines.reserve(raw_lines.size());
    for (const std::string& raw : raw_lines) {
        if (player_name.empty() && count_placeholders(raw) != 0) {
            return fail("killed: " + source + " uses " + kKilledPlaceholder +
                        " but the player name is empty");
        }
        std::string text;
        text.reserve(raw.size() + player_name.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (starts_placeholder(raw, i)) {
                text += player_name;
                ++i;
                continue;
            }
            text.push_back(raw[i]);
        }
        lines.push_back(KilledLine{raw, std::move(text)});
    }

    // Commit only after every check passed: a refusal leaves the old screen.
    screen.lines = std::move(lines);
    screen.source = std::move(source);
    return true;
}

std::string pick(const KilledScreen& screen, std::uint32_t seed) {
    if (screen.lines.empty()) {
        return {};
    }
    const std::size_t index = static_cast<std::size_t>(seed) % screen.lines.size();
    return screen.lines[index].text;
}

std::size_t substitution_count(const KilledScreen& screen) {
    std::size_t count = 0;
    for (const KilledLine& line : screen.lines) {
        count += count_placeholders(line.raw);
    }
    return count;
}

} // namespace gm
