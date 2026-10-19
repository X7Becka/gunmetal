#include "res/sob.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace gm {
namespace {

constexpr int kTexSlots = 8;
constexpr int kPlaneNumbers = 15;

std::vector<std::string> tokenize(std::span<const std::uint8_t> data) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < data.size()) {
        while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < data.size() && !std::isspace(static_cast<unsigned char>(data[i]))) {
            ++i;
        }
        if (i > start) {
            out.emplace_back(reinterpret_cast<const char*>(data.data() + start), i - start);
        }
    }
    return out;
}

bool to_int(const std::string& token, int& out) {
    char* end = nullptr;
    const long long value = std::strtoll(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != 0) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool to_float(const std::string& token, float& out) {
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || *end != 0) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

struct Walker {
    const std::vector<std::string>& tokens;
    std::unordered_map<std::size_t, long long> memo; // -2 in progress, -1 fail
    std::unordered_map<std::size_t, int> extra;

    long long walk(std::size_t p) {
        if (const auto it = memo.find(p); it != memo.end()) {
            return it->second == -2 ? -1 : it->second;
        }
        memo[p] = -2;
        if (p == tokens.size()) {
            memo[p] = static_cast<long long>(p);
            return static_cast<long long>(p);
        }

        std::size_t q = p + kTexSlots;
        int flags = 0;
        int nv = 0;
        bool ok = q + 2 <= tokens.size() && to_int(tokens[q], flags) &&
                  to_int(tokens[q + 1], nv) && nv >= 3 && nv <= 32;
        if (ok) {
            q += 2;
            for (int k = 0; k < nv; ++k) {
                int index = 0;
                if (!to_int(tokens[q + k], index) || index < 0) {
                    ok = false;
                    break;
                }
            }
            q += static_cast<std::size_t>(nv);
        }
        if (ok) {
            for (int k = 0; k < kPlaneNumbers; ++k) {
                float value = 0.0f;
                if (q + k >= tokens.size() || !to_float(tokens[q + k], value)) {
                    ok = false;
                    break;
                }
            }
            q += kPlaneNumbers;
        }
        if (ok) {
            // No sentinel.
            const long long r0 = walk(q);
            if (r0 >= 0) {
                memo[p] = r0;
                extra[p] = 0;
                return r0;
            }
            // Optional lone -1 sentinel.
            if (q < tokens.size() && tokens[q] == "-1") {
                const long long r1 = walk(q + 1);
                if (r1 >= 0) {
                    memo[p] = r1;
                    extra[p] = 1;
                    return r1;
                }
            }
        }
        memo[p] = -1;
        return -1;
    }
};

} // namespace

std::optional<Sob> parse_sob(std::span<const std::uint8_t> data, std::string& error) {
    const auto tokens = tokenize(data);
    if (tokens.size() < 2) {
        error = "not a .sob: fewer than two numbers";
        return std::nullopt;
    }
    int nverts = 0;
    int nfaces = 0;
    if (!to_int(tokens[0], nverts) || !to_int(tokens[1], nfaces) || nverts < 0 || nfaces < 0) {
        error = "not a .sob: bad vertex/face counts";
        return std::nullopt;
    }
    std::size_t pos = 2;
    if (pos + static_cast<std::size_t>(nverts) * 3 > tokens.size()) {
        error = "truncated vertex list";
        return std::nullopt;
    }
    Sob sob;
    sob.vertices.reserve(nverts);
    for (int i = 0; i < nverts; ++i) {
        Vec3f v;
        if (!to_float(tokens[pos], v.x) || !to_float(tokens[pos + 1], v.y) ||
            !to_float(tokens[pos + 2], v.z)) {
            error = "bad vertex " + std::to_string(i);
            return std::nullopt;
        }
        pos += 3;
        sob.vertices.push_back(v);
    }

    Walker walker{tokens, {}, {}};
    const long long end = walker.walk(pos);
    if (end < 0) {
        error = "no valid face stream from token " + std::to_string(pos);
        return std::nullopt;
    }

    std::size_t p = pos;
    sob.faces.reserve(nfaces);
    while (p < static_cast<std::size_t>(end)) {
        SobFace face;
        for (int t = 0; t < kTexSlots; ++t) {
            face.textures[t] = tokens[p + t];
        }
        std::size_t q = p + kTexSlots;
        to_int(tokens[q], face.flags);
        int nv = 0;
        to_int(tokens[q + 1], nv);
        q += 2;
        face.indices.resize(nv);
        for (int k = 0; k < nv; ++k) {
            to_int(tokens[q + k], face.indices[k]);
        }
        q += static_cast<std::size_t>(nv);
        if (walker.extra[p]) {
            q += 1;
        }
        float plane[kPlaneNumbers];
        for (int k = 0; k < kPlaneNumbers; ++k) {
            to_float(tokens[q + k], plane[k]);
        }
        face.origin = {plane[0], plane[1], plane[2]};
        face.u = {plane[3], plane[4], plane[5]};
        face.v = {plane[6], plane[7], plane[8]};
        face.normal = {plane[9], plane[10], plane[11]};
        face.spare = {plane[12], plane[13], plane[14]};
        p = q + kPlaneNumbers;
        sob.faces.push_back(std::move(face));
    }
    if (p != static_cast<std::size_t>(end)) {
        error = "parser stopped before the end of the stream";
        return std::nullopt;
    }
    if (p != tokens.size()) {
        error = std::to_string(tokens.size() - p) + " trailing tokens";
        return std::nullopt;
    }
    return sob;
}

} // namespace gm
