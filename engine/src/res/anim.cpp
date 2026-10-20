#include "res/anim.h"

#include <cctype>
#include <sstream>
#include <string_view>

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

bool to_int(const std::string& token, int& out) {
    try {
        std::size_t used = 0;
        const long value = std::stol(token, &used);
        if (used != token.size()) {
            return false;
        }
        out = static_cast<int>(value);
        return true;
    } catch (...) {
        return false;
    }
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

} // namespace

std::optional<Soo> parse_soo(std::span<const std::uint8_t> data, std::string& error) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    const auto tokens = split_ws(text);
    if (tokens.size() < 2) {
        error = "not a .soo: fewer than two numbers";
        return std::nullopt;
    }
    Soo soo;
    if (!to_int(tokens[0], soo.mode) || !to_int(tokens[1], soo.declared)) {
        error = "not a .soo: header is not two integers";
        return std::nullopt;
    }
    if ((tokens.size() - 2) % 2 != 0) {
        error = "not a .soo: odd number of frame tokens";
        return std::nullopt;
    }
    for (std::size_t i = 2; i + 1 < tokens.size(); i += 2) {
        SooFrame frame;
        frame.name = tokens[i];
        if (!to_int(tokens[i + 1], frame.duration)) {
            error = "not a .soo: bad duration for '" + frame.name + "'";
            return std::nullopt;
        }
        soo.frames.push_back(std::move(frame));
    }
    return soo;
}

std::optional<SooPose> soo_pose_at(const Soo& soo, int ms, std::string& error) {
    const std::size_t count = soo.declared > 0
                                  ? std::min<std::size_t>(static_cast<std::size_t>(soo.declared),
                                                          soo.frames.size())
                                  : 0;
    if (count == 0) {
        error = "animation has no frames";
        return std::nullopt;
    }
    if (soo.mode != 0 && soo.mode != 1) {
        error = "unsupported .soo mode " + std::to_string(soo.mode);
        return std::nullopt;
    }
    int total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        total += soo.frames[i].duration;
    }
    if (total <= 0) {
        error = "animation duration is not positive";
        return std::nullopt;
    }
    int phase = std::max(0, ms);
    if (soo.mode == 0) {
        if (phase >= total) {
            phase = total - 1;
        }
    } else {
        phase %= total;
    }
    int acc = 0;
    std::size_t index = count - 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (phase < acc + soo.frames[i].duration) {
            index = i;
            break;
        }
        acc += soo.frames[i].duration;
    }
    const std::size_t next = index + 1 < count ? index + 1 : (soo.mode == 1 ? 0 : index);
    SooPose pose;
    pose.index = static_cast<int>(index);
    pose.next_index = static_cast<int>(next);
    pose.elapsed = phase - acc;
    pose.phase = phase;
    pose.total = total;
    pose.name = soo.frames[index].name;
    pose.next_name = soo.frames[next].name;
    pose.alpha = soo.frames[index].duration > 0
                     ? static_cast<double>(pose.elapsed) / soo.frames[index].duration
                     : 1.0;
    return pose;
}

std::optional<Soa> parse_soa(std::span<const std::uint8_t> data, std::string& error) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    auto lines = split_lines(text);
    if (lines.empty()) {
        error = "empty .soa";
        return std::nullopt;
    }
    const auto header = split_ws(lines[0]);
    if (header.size() < 4) {
        error = ".soa header must be: mode period frames loop";
        return std::nullopt;
    }
    Soa soa;
    if (!to_int(header[0], soa.mode) || !to_int(header[1], soa.period) ||
        !to_int(header[2], soa.declared) || !to_int(header[3], soa.loop)) {
        error = ".soa header is not four integers";
        return std::nullopt;
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto trimmed = split_ws(lines[i]);
        if (!trimmed.empty()) {
            soa.names.push_back(trimmed.front());
        }
    }
    return soa;
}

} // namespace gm
