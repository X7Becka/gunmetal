// SECTOR_STUFF (.sbt record / save block 8) reader: 256 x 0x58 slots,
// runtime array 0x800b60 (+0x50 = sector flags, 0x800bb0 for slot 0).
// Layout: docs/RECORDS.md §2, docs/WORLD_FIELDS.md; oracle gm/sbt.py
// ``SectorSlot`` (decode/encode) and tools/re/records.py.
//
// Synthetic boundaries and truncation are always checked.  With the install
// present the whole corpus is checked: the 36 level ``*.sbt`` must parse and
// carry no SECTOR_STUFF (Python: 36/36), and the block-8 payload of both
// saves must decode field-for-field to the Python oracle's table below.
// Goldens were produced on the installed files with a throwaway script that
// runs, per file, gm.sbt.parse_file() (levels) or savefile.inspect() +
// gm.sbt.SectorSlot.decode() (saves), printing every non-empty slot and the
// FNV-1a 64 of the 0x5800 payload; it also asserted encode(decode(x)) == x.
#include "core/hash.h"
#include "core/path.h"
#include "res/save.h"
#include "res/sbt.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

void put64(std::uint8_t* p, std::uint64_t v) {
    put32(p, static_cast<std::uint32_t>(v));
    put32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

std::uint64_t bits(double v) {
    std::uint64_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

/// Minimal level: 84-byte header ("BINARY SAVE", version 0) + records.
std::vector<std::uint8_t> sbt_with(const std::vector<std::uint8_t>& records) {
    std::vector<std::uint8_t> out(84, 0);
    std::memcpy(out.data(), "BINARY SAVE", 11);
    out.insert(out.end(), records.begin(), records.end());
    return out;
}

/// One slot with every field distinct, signed and padding bytes non-zero.
void fill_slot(std::uint8_t* p, std::uint32_t seed) {
    put64(p + 0x00, bits(1.5 + seed));
    put64(p + 0x08, bits(-2.25));
    put64(p + 0x10, bits(0.125));
    put64(p + 0x18, bits(10.0));
    put64(p + 0x20, 0x7ff8dead0000beefull); // NaN payload must survive
    put64(p + 0x28, bits(-0.0));
    put64(p + 0x30, bits(0.05));
    put32(p + 0x38, static_cast<std::uint32_t>(-30233));
    put32(p + 0x3c, 302);
    p[0x40] = 0x34;
    p[0x41] = 0xff;
    p[0x42] = 0xaa;
    p[0x43] = 0xbb; // damage_a = 0xbbaaff34
    p[0x44] = 0x01;
    p[0x45] = 0x80;
    p[0x46] = 0xcc;
    p[0x47] = 0xdd; // damage_b = 0xddcc8001
    put32(p + 0x48, 130);
    put32(p + 0x4c, static_cast<std::uint32_t>(-1));
    put32(p + 0x50, 0x1f + seed);
    put32(p + 0x54, 0xfeedface);
}

void check_filled(const gm::SbtSectorSlot& s, std::uint32_t seed) {
    CHECK_EQ(bits(s.move[0]), bits(1.5 + seed));
    CHECK_EQ(s.move[1], -2.25);
    CHECK_EQ(s.move[2], 0.125);
    CHECK_EQ(s.extra[0], 10.0);
    CHECK_EQ(bits(s.extra[1]), 0x7ff8dead0000beefull);
    CHECK_EQ(bits(s.extra[2]), bits(-0.0));
    CHECK_EQ(s.sound_param, 0.05);
    CHECK_EQ(s.event_a, -30233);
    CHECK_EQ(s.event_b, 302);
    CHECK_EQ(s.damage_a, 0xbbaaff34u);
    CHECK_EQ(s.damage_b, static_cast<std::int32_t>(0xddcc8001u));
    CHECK_EQ(s.sound, 130);
    CHECK_EQ(s.sound_count, -1);
    CHECK_EQ(s.flags, 0x1fu + seed);
    CHECK_EQ(s.unused, 0xfeedfaceu);
    CHECK(!s.empty());
}

void test_synthetic_payload() {
    std::string error;
    // Exact size only: 0, one short, one long.
    for (const std::size_t size : {std::size_t(0), gm::kSbtSectorStuffSize - 1,
                                   gm::kSbtSectorStuffSize + 1, gm::kSbtSectorSlotSize}) {
        std::vector<std::uint8_t> payload(size, 0);
        std::vector<gm::SbtSectorSlot> out(3);
        error.clear();
        CHECK(!gm::parse_sector_stuff(payload, out, error));
        CHECK(error.find("SECTOR_STUFF payload is") != std::string::npos);
        CHECK_EQ(out.size(), std::size_t(3)); // untouched on error
    }
    CHECK_EQ(gm::kSbtSectorStuffSize, std::size_t(0x5800));

    // All-zero block: 256 empty slots, re-encodes to itself.
    std::vector<std::uint8_t> zero(gm::kSbtSectorStuffSize, 0);
    std::vector<gm::SbtSectorSlot> slots;
    CHECK(gm::parse_sector_stuff(zero, slots, error));
    CHECK_EQ(slots.size(), std::size_t(256));
    bool all_empty = true;
    for (const auto& s : slots) {
        all_empty = all_empty && s.empty() && s.flags == 0 && s.sound == 0;
    }
    CHECK(all_empty);
    CHECK(gm::encode_sector_stuff(slots) == zero);

    // First and last slot filled: stride 0x58, flags of slot 0 at payload
    // +0x50 (runtime 0x800bb0), slot 255 ends exactly at 0x5800.
    std::vector<std::uint8_t> payload(gm::kSbtSectorStuffSize, 0);
    fill_slot(payload.data(), 0);
    fill_slot(payload.data() + 255 * gm::kSbtSectorSlotSize, 7);
    CHECK_EQ(payload[0x50], std::uint8_t(0x1f));
    CHECK(gm::parse_sector_stuff(payload, slots, error));
    check_filled(slots[0], 0);
    check_filled(slots[255], 7);
    CHECK(slots[1].empty());
    CHECK(slots[254].empty());
    CHECK(std::memcmp(slots[255].raw.data(), payload.data() + 255 * 0x58, 0x58) == 0);
    // Typed fields alone (raw ignored) rebuild the block byte-for-byte,
    // including the NaN payload and both 16-bit gaps.
    for (auto& s : slots) {
        s.raw.fill(0);
    }
    CHECK(gm::encode_sector_stuff(slots) == payload);
    slots.pop_back();
    CHECK(gm::encode_sector_stuff(slots).empty());
}

void test_synthetic_sbt() {
    std::string error;
    std::vector<std::uint8_t> rec(1 + gm::kSbtSectorStuffSize, 0);
    rec[0] = 8;
    fill_slot(rec.data() + 1 + 34 * gm::kSbtSectorSlotSize, 3);
    std::vector<std::uint8_t> body = rec;
    body.push_back(255);
    const auto file = sbt_with(body);

    auto level = gm::parse_sbt(file, error);
    CHECK(level.has_value());
    if (level) {
        CHECK_EQ(level->records.size(), std::size_t(2));
        CHECK_EQ(level->records[0].type, std::uint8_t(8));
        CHECK_EQ(level->records[0].size, std::size_t(1 + 0x5800));
        CHECK_EQ(level->records[0].payload.size(), std::size_t(0x5800));
        CHECK_EQ(level->sector_stuff.size(), std::size_t(256));
        if (level->sector_stuff.size() == 256) {
            check_filled(level->sector_stuff[34], 3);
            CHECK(level->sector_stuff[33].empty());
        }
    }

    // Truncation at every interesting cut: right after the type byte, one
    // slot in, one byte short, and the terminator missing.
    for (const std::size_t cut : {std::size_t(1), std::size_t(2), std::size_t(1 + 0x58),
                                  std::size_t(0x5800), std::size_t(0x5801)}) {
        std::vector<std::uint8_t> t(rec.begin(), rec.begin() + static_cast<std::ptrdiff_t>(cut));
        error.clear();
        CHECK(!gm::parse_sbt(sbt_with(t), error).has_value());
        if (cut <= 0x5800) {
            CHECK(error.find("bad SECTOR_STUFF record") != std::string::npos);
            CHECK(error.find("truncated") != std::string::npos);
        } else {
            CHECK(error.find("END_OF_FILE") != std::string::npos);
        }
    }

    // No record -> empty view; a second record overwrites the first.
    {
        auto none = gm::parse_sbt(sbt_with({255}), error);
        CHECK(none.has_value() && none->sector_stuff.empty());
        std::vector<std::uint8_t> two = rec;
        std::vector<std::uint8_t> second(1 + gm::kSbtSectorStuffSize, 0);
        second[0] = 8;
        put32(second.data() + 1 + 0x50, 0x10); // slot 0 flags
        two.insert(two.end(), second.begin(), second.end());
        two.push_back(255);
        auto both = gm::parse_sbt(sbt_with(two), error);
        CHECK(both.has_value());
        if (both && both->sector_stuff.size() == 256) {
            CHECK_EQ(both->records.size(), std::size_t(3));
            CHECK(both->sector_stuff[34].empty());
            CHECK_EQ(both->sector_stuff[0].flags, 0x10u);
        }
    }
}

struct GoldenSlot {
    int index;
    double sound_param;
    std::int32_t event_a, event_b, sound, sound_count;
};

// gm/sbt.py SectorSlot.decode of saved0/saved1 block 8 (identical in both).
// Every other field of these slots is zero (move, extra, damage, flags,
// unused, gaps) and the remaining 243 slots are all-zero.
constexpr std::array<GoldenSlot, 13> kSaveSlots = {{
    {5, 0.0, 606, 0, 0, 0},
    {9, 0.03, 40, 0, 130, 10},
    {11, 0.0, 20, 0, 0, 0},
    {13, 0.0, 601, 0, 0, 0},
    {14, 0.0, 70, 0, 0, 0},
    {19, 0.0, 604, 0, 0, 0},
    {34, 0.05, -30233, -30234, 129, 29},
    {35, 0.0, 607, 0, 0, 0},
    {48, 0.0, 604, 0, 131, 10},
    {50, 0.0, 605, 0, 0, 0},
    {51, 0.0, 0, 302, 0, 0},
    {54, 0.05, 0, 0, 129, 25},
    {57, 0.03, 40, 0, 130, 10},
}};
constexpr std::uint64_t kSavePayloadFnv = 0x19fa6a27ba4b0c33ull;
constexpr std::size_t kSaveBlockOffset = 0x29e2b; // id byte; payload at +1

void check_save(const fs::path& path) {
    std::string error;
    const auto bytes = read_file(path);
    auto save = gm::parse_save(bytes, error);
    CHECK(save.has_value());
    if (!save) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), error.c_str());
        return;
    }
    const gm::SaveBlock* block = nullptr;
    for (const auto& b : save->blocks) {
        if (b.id == 8) {
            block = &b;
        }
    }
    CHECK(block != nullptr);
    if (!block) {
        return;
    }
    CHECK_EQ(block->offset, kSaveBlockOffset);
    CHECK_EQ(block->size, std::size_t(1 + 0x5800));
    CHECK_EQ(gm::fnv1a64(block->raw), kSavePayloadFnv);

    std::vector<gm::SbtSectorSlot> slots;
    CHECK(gm::parse_sector_stuff(block->raw, slots, error));
    if (slots.size() != 256) {
        return;
    }
    std::size_t matched = 0;
    std::size_t non_empty = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& s = slots[i];
        non_empty += s.empty() ? 0 : 1;
        const GoldenSlot* g = nullptr;
        for (const auto& row : kSaveSlots) {
            if (static_cast<std::size_t>(row.index) == i) {
                g = &row;
            }
        }
        if (!g) {
            CHECK(s.empty());
            continue;
        }
        const bool ok = s.sound_param == g->sound_param && s.event_a == g->event_a &&
                        s.event_b == g->event_b && s.sound == g->sound &&
                        s.sound_count == g->sound_count && s.move == std::array<double, 3>{} &&
                        s.extra == std::array<double, 3>{} && s.damage_a == 0 &&
                        s.damage_b == 0 && s.flags == 0 && s.unused == 0;
        CHECK(ok);
        matched += ok ? 1 : 0;
    }
    CHECK_EQ(non_empty, kSaveSlots.size());
    CHECK_EQ(matched, kSaveSlots.size());
    CHECK(gm::encode_sector_stuff(slots) == block->raw);
    std::printf("sector_stuff: %s block 8 %zu/%zu slots match oracle\n",
                path.filename().string().c_str(), matched, kSaveSlots.size());
}

void test_corpus() {
    if (!fs::is_directory(kGame)) {
        std::printf("sector_stuff: install not found, corpus checks skipped\n");
        return;
    }
    // Levels: Python gm/sbt.py parses all 36 and finds no type-8 record.
    std::size_t levels = 0;
    std::size_t ok = 0;
    for (const auto& entry : fs::directory_iterator(kGame)) {
        if (!entry.is_regular_file() ||
            !gm::iequals_ascii(entry.path().extension().string(), ".sbt")) {
            continue;
        }
        ++levels;
        std::string error;
        auto level = gm::parse_sbt(read_file(entry.path()), error);
        bool has8 = false;
        if (level) {
            for (const auto& r : level->records) {
                has8 = has8 || r.type == 8;
            }
        }
        const bool good = level && !has8 && level->sector_stuff.empty();
        if (!good) {
            std::fprintf(stderr, "level %s: %s\n", entry.path().filename().string().c_str(),
                         error.c_str());
        }
        ok += good ? 1 : 0;
    }
    CHECK_EQ(levels, std::size_t(36));
    CHECK_EQ(ok, levels);
    std::printf("sector_stuff: levels %zu/%zu parse, none carries SECTOR_STUFF\n", ok, levels);

    std::size_t saves = 0;
    for (const char* name : {"saved0.sbt", "saved1.sbt"}) {
        const fs::path p = kGame / "SAVEGAME" / name;
        if (fs::is_regular_file(p)) {
            ++saves;
            check_save(p);
        }
    }
    CHECK_EQ(saves, std::size_t(2));
}

} // namespace

int main() {
    test_synthetic_payload();
    test_synthetic_sbt();
    test_corpus();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d sector_stuff check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("sector_stuff tests passed\n");
    return 0;
}
