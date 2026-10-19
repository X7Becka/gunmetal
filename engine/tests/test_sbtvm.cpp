// SBT SCRIPT decoder tests: decode every shipped .sbt level, check that the
// stream ends exactly at code.size(), and compare the canonical text form
// (render) against goldens produced by gm/sbtvm.py over the same files.
#include "core/bytes.h"
#include "core/hash.h"
#include "res/sbt.h"
#include "res/sbtvm.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

/// Locate the SCRIPT (type 11) record's code block inside its raw payload.
std::optional<std::span<const std::uint8_t>> script_code(const gm::SbtRecord& record,
                                                        std::uint32_t& declared) {
    const auto& p = record.payload;
    std::size_t offset = 0;
    auto take = [&](std::uint32_t& value) {
        if (offset + 4 > p.size()) {
            return false;
        }
        value = gm::rd32(p.data() + offset);
        offset += 4;
        return true;
    };
    auto skip = [&](std::uint64_t count) {
        if (count > p.size() - offset) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
        return true;
    };

    std::uint32_t things = 0;
    std::uint32_t handles = 0;
    if (!take(things) || !skip(static_cast<std::uint64_t>(things) * 40)) {
        return std::nullopt;
    }
    if (!take(handles) || !skip(static_cast<std::uint64_t>(handles) * 12)) {
        return std::nullopt;
    }
    if (!take(declared) || declared > p.size() - offset) {
        return std::nullopt;
    }
    return std::span<const std::uint8_t>(p.data() + offset, declared);
}

struct Golden {
    const char* name;
    std::size_t insns;
    std::size_t code_size;
    std::uint64_t fnv;
};

// Produced by gm/sbtvm.py: len(decode(code)) and FNV-1a 64 of render(code)
// encoded as latin1.  Sorted by file name.
constexpr Golden kGolden[] = {
    {"dm1.sbt", 2317, 83990, 0x08a7c7d6c7ade279ull},
    {"dm2.sbt", 2074, 77721, 0x84cb397f2c4ff22dull},
    {"dm3.sbt", 2198, 85128, 0xd8289553779769f7ull},
    {"dm4.sbt", 1935, 69940, 0x0b1be44c39f7e913ull},
    {"level0.sbt", 1895, 62497, 0xcf1935d2906393a8ull},
    {"level00.sbt", 1768, 62289, 0x5105bf889cb13a28ull},
    {"level000.sbt", 1767, 61523, 0x0ec9992926f5ba00ull},
    {"level0a.sbt", 1766, 59416, 0xa6f178665b6b6263ull},
    {"level0b.sbt", 1459, 50351, 0x3890f61c7c70075full},
    {"level1.sbt", 2878, 93118, 0x2aa71f11f6af8a64ull},
    {"level10.sbt", 3022, 106252, 0x95b8bc7abe24b2e9ull},
    {"level11.sbt", 3382, 119044, 0xdd4fc0e835bd8c66ull},
    {"level12.sbt", 3332, 114382, 0xa3e61cae1725023aull},
    {"level13.sbt", 3687, 132131, 0xca1c16a2abc668d7ull},
    {"level14.sbt", 2825, 98488, 0x3050a5996aed1bc7ull},
    {"level15.sbt", 3379, 120297, 0x883a80201c1db245ull},
    {"level16.sbt", 3057, 112177, 0xaba00f30127aa7bfull},
    {"level17.sbt", 3531, 121558, 0x1c1264af7a6ffe82ull},
    {"level18.sbt", 3677, 134686, 0xc8d4c56ed2ecab7bull},
    {"level19.sbt", 3161, 110452, 0x0be80d9edf4933c1ull},
    {"level2.sbt", 2177, 78774, 0x7e6d2adb76563c93ull},
    {"level20.sbt", 2746, 90571, 0xa9e450c7e056abf2ull},
    {"level21.sbt", 3068, 113269, 0x7d12b81e73368393ull},
    {"level22.sbt", 3703, 128133, 0x3689ba911502f1afull},
    {"level23.sbt", 3981, 132508, 0xea8b70b6d44b0f84ull},
    {"level24.sbt", 3294, 120924, 0x9878fe57c7237147ull},
    {"level3.sbt", 3085, 117121, 0xcc6988e0a3c54135ull},
    {"level4.sbt", 2780, 96928, 0x0882c60ef495e20bull},
    {"level5.sbt", 2853, 106221, 0x31088e350f267c83ull},
    {"level6.sbt", 2869, 100177, 0x96440fdf35350099ull},
    {"level7.sbt", 2817, 99859, 0xdeffd1f17d3617bfull},
    {"level8.sbt", 3496, 115234, 0x0724722555493a2bull},
    {"level9.sbt", 3684, 121716, 0x5981aeae75f23223ull},
    {"secrt1.sbt", 2330, 86089, 0x83bfe5591f3d638cull},
    {"secrt2.sbt", 2606, 92334, 0xb9f4c1356acfd4f1ull},
    {"temple.sbt", 1888, 66725, 0xac681cf5bd408523ull},
};

constexpr std::size_t kTotalInsns = 100487;
constexpr std::uint64_t kAggregateFnv = 0xe0d46e033fa0b415ull;

void test_real_levels() {
    const fs::path dir = "/mnt/d/games/Gunmetal 3dfx";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("sbtvm: game directory not found, skipping golden files\n");
        return;
    }

    std::size_t total_insns = 0;
    std::vector<std::string> rendered;
    rendered.reserve(std::size(kGolden));

    std::string error;
    for (const Golden& golden : kGolden) {
        const fs::path path = dir / golden.name;
        if (!fs::is_regular_file(path, ec)) {
            std::fprintf(stderr, "FAIL missing %s\n", path.string().c_str());
            ++g_failures;
            continue;
        }
        const auto bytes = read_file(path);
        const auto level = gm::parse_sbt(bytes, error);
        CHECK(level.has_value());
        if (!level) {
            std::fprintf(stderr, "  %s: %s\n", golden.name, error.c_str());
            continue;
        }

        const gm::SbtRecord* script = nullptr;
        for (const gm::SbtRecord& record : level->records) {
            if (record.type == 11) {
                script = &record;
                break;
            }
        }
        CHECK(script != nullptr);
        if (script == nullptr) {
            continue;
        }

        std::uint32_t declared = 0;
        const auto code = script_code(*script, declared);
        CHECK(code.has_value());
        if (!code) {
            std::fprintf(stderr, "  %s: bad SCRIPT payload\n", golden.name);
            continue;
        }
        CHECK_EQ(code->size(), golden.code_size);
        CHECK_EQ(declared, level->script.code_size);

        const auto decoded = gm::decode_sbt_script(*code, error);
        CHECK(decoded.has_value());
        if (!decoded) {
            std::fprintf(stderr, "FAIL %s: %s\n", golden.name, error.c_str());
            ++g_failures;
            continue;
        }
        CHECK_EQ(decoded->end, code->size());
        CHECK_EQ(decoded->insns.size(), golden.insns);

        const std::string text = gm::render_sbt_script(*decoded);
        const std::uint64_t hash = gm::fnv1a64(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                          text.size()));
        if (hash != golden.fnv) {
            std::fprintf(stderr, "FAIL %s: render fnv %016llx != %016llx\n", golden.name,
                         static_cast<unsigned long long>(hash),
                         static_cast<unsigned long long>(golden.fnv));
            ++g_failures;
        }

        total_insns += decoded->insns.size();
        rendered.push_back(text);
    }

    CHECK_EQ(total_insns, kTotalInsns);
    std::string all;
    for (std::size_t k = 0; k < rendered.size(); ++k) {
        if (k != 0) {
            all += '\n';
        }
        all += rendered[k];
    }
    const std::uint64_t aggregate =
        gm::fnv1a64(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(all.data()), all.size()));
    CHECK_EQ(aggregate, kAggregateFnv);

    std::printf("sbtvm: %zu files, %zu instructions, aggregate fnv %016llx\n",
                std::size(kGolden), total_insns, static_cast<unsigned long long>(aggregate));
}

void test_synthetic() {
    std::string error;

    const std::uint8_t end_only[] = {0xFF};
    const auto decoded = gm::decode_sbt_script(end_only, error);
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK_EQ(decoded->end, std::size_t{1});
        CHECK_EQ(decoded->insns.size(), std::size_t{1});
        CHECK_EQ(gm::render_sbt_script(*decoded), std::string("---"));
    }

    const auto empty = gm::decode_sbt_script(std::span<const std::uint8_t>(), error);
    CHECK(empty.has_value());
    if (empty) {
        CHECK_EQ(empty->end, std::size_t{0});
        CHECK(empty->insns.empty());
        CHECK_EQ(gm::render_sbt_script(*empty), std::string());
    }

    const std::uint8_t unknown[] = {0xFE};
    CHECK(!gm::decode_sbt_script(unknown, error).has_value());

    const std::uint8_t truncated[] = {0x12, 'h', 'i'};
    CHECK(!gm::decode_sbt_script(truncated, error).has_value());
}

} // namespace

int main() {
    test_synthetic();
    test_real_levels();
    if (g_failures != 0) {
        std::fprintf(stderr, "sbtvm: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sbtvm: ok\n");
    return 0;
}
