// SOUND_SAMPLE_TABLE (record 12, docs/SOUND_FIELDS.md): 300 counts + 300
// nine-byte names.
//
// The table is the level's sound menu: every `ADD_SOUND_SOURCE`, door/lift
// `sound` field and save record references a slot by number, so a wrong
// stride or a swapped name region would silently mislabel sounds.  The test
// pins the layout on the shipped levels and checks the decoded names against
// `gm/sbt.py::SampleTable` (first slots are the UI sounds).
#include "core/bytes.h"
#include "game_path.h"
#include "res/sbt.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va = (a);                                                         \
        const auto vb = (b);                                                         \
        if (!(va == vb)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a,   \
                         #b);                                                            \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

constexpr const char* kGameDir = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

void test_level(const char* name) {
    const auto resolved = gmtest::resolve_game_file(kGameDir, name);
    CHECK(resolved.has_value());
    if (!resolved) {
        return;
    }
    const auto bytes = read_file(*resolved);
    std::string error;
    auto level = gm::parse_sbt(bytes, error);
    CHECK(level.has_value());
    if (!level) {
        std::fprintf(stderr, "  %s: %s\n", name, error.c_str());
        return;
    }
    CHECK(level->sound_samples.has_value());
    if (!level->sound_samples) {
        return;
    }
    const gm::SoundSampleTable& table = *level->sound_samples;
    CHECK_EQ(table.names.size(), gm::SoundSampleTable::kSlots);
    // The first slots are the UI sounds in every shipped level.
    CHECK_EQ(table.names[0], std::string("updown"));
    CHECK_EQ(table.names[1], std::string("select"));
    CHECK_EQ(table.names[2], std::string("vleft"));
    CHECK_EQ(table.names[5], std::string("menuon"));
    CHECK_EQ(table.counts[0], 1u);
    CHECK_EQ(table.counts[2], 3u);
    // Names are at most eight characters plus the NUL of the nine-byte slot.
    std::size_t empty = 0;
    std::set<std::string> distinct;
    for (const std::string& slot : table.names) {
        CHECK(slot.size() < gm::SoundSampleTable::kNameWidth);
        if (slot.empty()) {
            ++empty;
        } else {
            distinct.insert(slot);
        }
    }
    // The installation renames/swaps some files, so the *content* decides:
    // every shipped level has both named and unnamed slots, and the named
    // ones dominate.  (level0: 137 names, dm1: 135 as read here.)
    CHECK(empty > 0 && empty < 200);
    CHECK(distinct.size() > 100);
    // Every slot has a positive "count" except the unnamed ones.
    for (std::size_t i = 0; i < table.names.size(); ++i) {
        if (!table.names[i].empty()) {
            CHECK(table.counts[i] > 0);
        }
    }
    std::printf("samples %s: %zu names, %zu empty, %zu distinct\n", name,
                table.names.size() - empty, empty, distinct.size());
}

} // namespace

int main() {
    test_level("level0.sbt");
    test_level("dm1.sbt");

    // A truncated record must fail cleanly, not read past the buffer.
    std::string error;
    const std::vector<std::uint8_t> garbage(64, 0);
    auto bad = gm::parse_sbt(garbage, error);
    CHECK(!bad.has_value());

    if (g_failures != 0) {
        std::fprintf(stderr, "sample_table: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sample_table: ok\n");
    return 0;
}
