#include "res/text.h"

#include <cctype>

namespace gm {
namespace {

std::vector<std::string> split_ws(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i > start) {
            out.emplace_back(text.substr(start, i - start));
        }
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string line(text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

bool is_name_char(char c, bool first) {
    const auto u = static_cast<unsigned char>(c);
    if (first) {
        return std::isalpha(u) != 0;
    }
    return std::isalnum(u) != 0;
}

} // namespace

std::optional<Sog> parse_sog(std::span<const std::uint8_t> data, std::string& error) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    Sog sog;
    for (auto& line : split_lines(text)) {
        std::size_t b = 0;
        std::size_t e = line.size();
        while (b < e && std::isspace(static_cast<unsigned char>(line[b]))) {
            ++b;
        }
        while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1]))) {
            --e;
        }
        if (e > b) {
            sog.names.emplace_back(line.substr(b, e - b));
        }
    }
    if (sog.names.empty()) {
        error = ".sog has no sample names";
        return std::nullopt;
    }
    return sog;
}

std::optional<Soe> parse_soe_text(std::span<const std::uint8_t> data, std::string& error) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    Soe soe;
    soe.tokens = split_ws(text);
    if (soe.tokens.empty()) {
        error = ".soe is empty";
        return std::nullopt;
    }
    return soe;
}

std::vector<std::uint8_t> sml_encode(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::uint8_t b = data[i];
        const int shift = static_cast<int>(i % 10);
        if (b >= 'A' && b <= 'Z') {
            out[i] = static_cast<std::uint8_t>('A' + (b - 'A' + shift) % 26);
        } else if (b >= 'a' && b <= 'z') {
            out[i] = static_cast<std::uint8_t>('a' + (b - 'a' + shift) % 26);
        } else {
            out[i] = b;
        }
    }
    return out;
}

std::vector<std::uint8_t> sml_decode(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::uint8_t b = data[i];
        const int shift = static_cast<int>(i % 10);
        if (b >= 'A' && b <= 'Z') {
            out[i] = static_cast<std::uint8_t>('A' + ((b - 'A' - shift) % 26 + 26) % 26);
        } else if (b >= 'a' && b <= 'z') {
            out[i] = static_cast<std::uint8_t>('a' + ((b - 'a' - shift) % 26 + 26) % 26);
        } else {
            out[i] = b;
        }
    }
    return out;
}

std::vector<SmlTag> sml_tags(std::string_view text) {
    std::vector<SmlTag> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t lt = text.find('<', i);
        if (lt == std::string_view::npos) {
            break;
        }
        std::size_t p = lt + 1;
        SmlTag tag;
        tag.offset = lt;
        tag.closing = p < text.size() && text[p] == '/';
        if (tag.closing) {
            ++p;
        }
        if (p >= text.size() || !is_name_char(text[p], true)) {
            i = lt + 1;
            continue;
        }
        const std::size_t name_start = p;
        ++p;
        while (p < text.size() && is_name_char(text[p], false)) {
            ++p;
        }
        tag.name.assign(text.substr(name_start, p - name_start));
        const std::size_t gt = text.find('>', p);
        if (gt == std::string_view::npos) {
            break;
        }
        // Attributes may contain <> in theory, but shipped markup does not;
        // mirror the Python regex: stop at the first '>'.
        std::string_view body = text.substr(p, gt - p);
        std::size_t end = body.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(body[end - 1]))) {
            --end;
        }
        tag.self_closing = end > 0 && body[end - 1] == '/';
        tag.raw.assign(text.substr(lt, gt - lt + 1));
        out.push_back(std::move(tag));
        i = gt + 1;
    }
    return out;
}

std::string sml_plain(std::string_view text) {
    std::string out;
    std::size_t last = 0;
    for (const auto& tag : sml_tags(text)) {
        out.append(text.substr(last, tag.offset - last));
        last = tag.offset + tag.raw.size();
        if (tag.closing) {
            continue;
        }
        std::string low = tag.name;
        for (char& c : low) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (low == "r" || low == "p") {
            out.push_back('\n');
        } else if (low == "c") {
            out.append("  ");
        } else if (tag.name == "k" || tag.name == "K") {
            out.append("Soldier");
        } else if (tag.name == "s" || tag.name == "S") {
            out.push_back(' ');
        } else if (tag.raw.find(' ') != std::string::npos) {
            out.append(tag.raw);
        }
    }
    out.append(text.substr(last));

    // Trim every line and drop empty ones.
    std::string result;
    std::size_t start = 0;
    while (start <= out.size()) {
        std::size_t end = out.find('\n', start);
        if (end == std::string::npos) {
            end = out.size();
        }
        std::size_t b = start;
        std::size_t e = end;
        while (b < e && std::isspace(static_cast<unsigned char>(out[b]))) {
            ++b;
        }
        while (e > b && std::isspace(static_cast<unsigned char>(out[e - 1]))) {
            --e;
        }
        if (e > b) {
            if (!result.empty()) {
                result.push_back('\n');
            }
            result.append(out.substr(b, e - b));
        }
        if (end == out.size()) {
            break;
        }
        start = end + 1;
    }
    return result;
}

} // namespace gm
