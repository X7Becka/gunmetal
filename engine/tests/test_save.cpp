// Save container reader: parity with tools/re/savefile.py and
// tools/re/save_fields.py on the two shipped saves, plus synthetic negative
// cases.
//
// Golden values were produced by the Python tools on the installed files:
//
//   python3 tools/re/savefile.py SAVEGAME/saved0.sbt --json
//   python3 tools/re/save_fields.py --json
//
// The block chain, header fields, gauges and ammo below are pinned by both
// readers; docs/SAVE_CPP.md carries the same table.
#include "res/save.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        const double va = (a);                                                               \
        const double vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (eps))) {                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s=%.17g != %s=%.17g\n", __FILE__, __LINE__,   \
                         #a, va, #b, vb);                                                    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

struct GoldenBlock {
    std::uint8_t id;
    std::size_t offset;
    std::size_t size;
    std::uint32_t count;
};

// saved0 and saved1 have the same chain: same ids, offsets and sizes.  Only
// the contents differ (header F1, things slot 0, player fields, script bytes).
constexpr std::array<GoldenBlock, 15> kBlocks = {{
    {12, 0x0013c, 3901, 175},  // sound table
    {20, 0x01079, 1345, 67},   // SOB table
    {15, 0x015ba, 1669, 104},  // poly handles
    {17, 0x01c3f, 213, 26},    // sector handles
    {5, 0x01d14, 57, 1},       // named items
    {4, 0x01d4d, 53357, 57},   // thing types
    {11, 0x0edba, 76345, 10},  // script (10 runtime things)
    {2, 0x217f3, 4605, 25},    // doors
    {1, 0x229f0, 773, 24},     // switches
    {6, 0x22cf5, 181, 4},      // sprites
    {19, 0x22daa, 28801, 0},   // ITEM (raw)
    {8, 0x29e2b, 22529, 0},    // SECTOR_STUFF (raw)
    {7, 0x2f62c, 173, 6},      // sound sources
    {3, 0x2f6d9, 8181, 6},     // things (6/260 present)
    {9, 0x316ce, 40921, 1},    // player (1/8 present)
}};

constexpr std::array<std::uint32_t, gm::kSaveFieldCount> kFields = {
    1, 4288, 1, 0, 0, 0, 64, 0, 90000, 90000, 0, 0, 0, 0, 4, 2, 1, 1, 6, 6, 0, 0,
};

struct GoldenSave {
    const char* file;
    const char* title;
    std::uint32_t version;
    std::uint32_t level_clock;
    std::array<double, 3> position;
    std::array<double, 3> angles;
    std::int32_t history_index;
    std::int32_t weapon_slot;
    std::array<std::uint32_t, 2> ammo_first_two;
};

constexpr std::array<GoldenSave, 2> kSaves = {{
    {"saved0.sbt", "A SAVED GAME", 0x34, 4288,
     {{1116.173751898651, 798.2076214059642, 48.0}},
     {{0.0, 0.0, 5.0736149946795805}}, 194, 0, {{2000, 90}}},
    {"saved1.sbt", "2", 0x00, 8148,
     {{1156.355385156283, 797.2758300679517, 48.0}},
     {{0.0, 0.0, 4.747052494679579}}, 130, 1, {{1975, 87}}},
}};

std::vector<std::uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out;
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return out;
}

std::uint32_t get32(const std::vector<std::uint8_t>& v, std::size_t off) {
    return static_cast<std::uint32_t>(v[off]) |
           (static_cast<std::uint32_t>(v[off + 1]) << 8) |
           (static_cast<std::uint32_t>(v[off + 2]) << 16) |
           (static_cast<std::uint32_t>(v[off + 3]) << 24);
}

void put32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    v[off] = static_cast<std::uint8_t>(x & 0xff);
    v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xff);
    v[off + 2] = static_cast<std::uint8_t>((x >> 16) & 0xff);
    v[off + 3] = static_cast<std::uint8_t>((x >> 24) & 0xff);
}

void put_f64(std::vector<std::uint8_t>& v, std::size_t off, double x) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &x, sizeof(bits));
    for (std::size_t i = 0; i < 8; ++i) {
        v[off + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff);
    }
}

std::vector<std::uint8_t> make_header() {
    std::vector<std::uint8_t> v(gm::kSaveHeaderSize, 0);
    std::memcpy(v.data(), "A SAVED GAME", 12);
    std::memcpy(v.data() + gm::kSaveLevelNameAt, "LEVEL0", 6);
    for (std::size_t i = 0; i < kFields.size(); ++i) {
        put32(v, gm::kSaveFieldsAt + 4 * i, kFields[i]);
    }
    std::memcpy(v.data() + gm::kSaveNameSlotAt, "K-9", 3);
    std::memcpy(v.data() + gm::kSaveNameSlotAt + 2 * gm::kSaveNameSlotLen, "coargus", 7);
    std::memcpy(v.data() + gm::kSaveNameSlotAt + 3 * gm::kSaveNameSlotLen, "none", 4);
    return v;
}

std::vector<std::uint8_t> make_player_struct() {
    std::vector<std::uint8_t> st(0x3510, 0);
    put_f64(st, 0x0000, 1.5);
    put_f64(st, 0x0008, -2.25);
    put_f64(st, 0x0010, 48.0);
    put_f64(st, 0x0018, 0.0);
    put_f64(st, 0x0020, 0.0);
    put_f64(st, 0x0028, 0.5);
    put32(st, 0x3178, 7496160);
    put32(st, 0x3204, 7);
    put32(st, 0x3210, 1);
    put32(st, 0x3214, 16);
    put32(st, 0x3244, 10000);
    put32(st, 0x3248, 5000);
    put32(st, 0x324c, 8000);
    for (std::size_t i = 0; i < 4; ++i) {
        put32(st, 0x3250 + 4 * i, 8000);
    }
    put32(st, 0x3294, 2);
    put32(st, 0x3298, 7);
    put32(st, 0x329c, 123);
    put32(st, 0x32a0, 45);
    return st;
}

// Block 9 payload: one present slot (optional attachments), then the two
// {0x50 header, 0x3510 struct} pairs.
std::vector<std::uint8_t> make_player_payload(std::vector<std::uint8_t> st,
                                              std::vector<std::uint8_t> attachments = {}) {
    std::vector<std::uint8_t> out;
    out.push_back(1);
    out.insert(out.end(), st.begin(), st.end());
    out.insert(out.end(), attachments.begin(), attachments.end());
    for (std::size_t i = 1; i < 8; ++i) {
        out.push_back(0);
    }
    std::vector<std::uint8_t> hdr(0x50, 0);
    std::memcpy(hdr.data(), "LEVEL0", 6);
    out.insert(out.end(), hdr.begin(), hdr.end());
    out.insert(out.end(), 0x3510, 0);
    std::fill(hdr.begin(), hdr.end(), 0);
    out.insert(out.end(), hdr.begin(), hdr.end());
    out.insert(out.end(), 0x3510, 0);
    return out;
}

void append_block(std::vector<std::uint8_t>& save, std::uint8_t id,
                  const std::vector<std::uint8_t>& payload) {
    save.push_back(id);
    save.insert(save.end(), payload.begin(), payload.end());
}

void test_installed() {
    if (!fs::exists(kGame / "SAVEGAME" / "saved0.sbt") ||
        !fs::exists(kGame / "SAVEGAME" / "saved1.sbt")) {
        std::printf("save: shipped saves not installed, skipping golden parity\n");
        return;
    }

    for (const GoldenSave& golden : kSaves) {
        const fs::path path = kGame / "SAVEGAME" / golden.file;
        const std::vector<std::uint8_t> bytes = read_all(path);
        CHECK(!bytes.empty());
        if (bytes.empty()) {
            continue;
        }
        CHECK_EQ(bytes.size(), std::size_t(243368));

        std::string error;
        const auto save = gm::parse_save(bytes, error);
        if (!save) {
            std::fprintf(stderr, "FAIL %s: %s\n", golden.file, error.c_str());
            ++g_failures;
            continue;
        }
        CHECK(error.empty());
        CHECK_EQ(save->size, bytes.size());
        CHECK_EQ(save->header.title, std::string(golden.title));
        CHECK_EQ(save->header.version, golden.version);
        CHECK_EQ(save->header.reserved, std::uint32_t(0));
        CHECK_EQ(save->header.level_name, std::string("LEVEL0"));
        for (std::size_t i = 0; i < kFields.size(); ++i) {
            if (i == 1) {
                continue; // F1 is the level clock and differs between saves
            }
            CHECK_EQ(save->header.fields[i], kFields[i]);
        }
        CHECK_EQ(save->header.fields[1], golden.level_clock);
        CHECK_EQ(save->header.name_slots[0], std::string("K-9"));
        CHECK_EQ(save->header.name_slots[1], std::string(""));
        CHECK_EQ(save->header.name_slots[2], std::string("coargus"));
        CHECK_EQ(save->header.name_slots[3], std::string("none"));

        CHECK_EQ(save->blocks.size(), kBlocks.size());
        for (std::size_t i = 0; i < kBlocks.size() && i < save->blocks.size(); ++i) {
            const gm::SaveBlock& b = save->blocks[i];
            CHECK_EQ(static_cast<int>(b.id), static_cast<int>(kBlocks[i].id));
            CHECK_EQ(b.offset, kBlocks[i].offset);
            CHECK_EQ(b.size, kBlocks[i].size);
            CHECK_EQ(b.count, kBlocks[i].count);
            const gm::SaveBlock* found = save->find_block(kBlocks[i].id);
            CHECK(found != nullptr);
            if (found) {
                CHECK_EQ(found->offset, kBlocks[i].offset);
                CHECK_EQ(found->size, kBlocks[i].size);
            }
        }

        CHECK(save->player.has_value());
        if (!save->player) {
            continue;
        }
        const gm::SavePlayer& p = *save->player;
        CHECK(p.has_slot);
        CHECK_EQ(p.block_offset, std::size_t(0x316ce));
        CHECK_EQ(p.slot_offsets[0], std::size_t(0x316d0));
        CHECK_EQ(p.present[0], true);
        for (std::size_t i = 1; i < 8; ++i) {
            CHECK_EQ(p.present[i], false);
            CHECK_EQ(p.attachment_counts[i], std::uint32_t(0));
        }
        CHECK_EQ(p.attachment_counts[0], std::uint32_t(0));
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK_NEAR(p.position[i], golden.position[i], 1e-12);
            CHECK_NEAR(p.angles[i], golden.angles[i], 1e-12);
        }
        CHECK_EQ(p.history_index, golden.history_index);
        CHECK_EQ(p.exists, std::uint32_t(7496160));
        CHECK_EQ(p.status_present, std::int32_t(1));
        CHECK_EQ(p.object_id, std::int32_t(16));
        CHECK_EQ(p.armor_max, std::int32_t(10000));
        CHECK_EQ(p.armor, std::int32_t(10000));
        CHECK_EQ(p.shield_max, std::int32_t(8000));
        for (std::int32_t v : p.shields) {
            CHECK_EQ(v, std::int32_t(8000));
        }
        CHECK_EQ(p.weapon_slot, golden.weapon_slot);
        CHECK_EQ(p.weapon_mask, std::uint32_t(3));
        CHECK_EQ(p.ammo[0], golden.ammo_first_two[0]);
        CHECK_EQ(p.ammo[1], golden.ammo_first_two[1]);
        for (std::size_t i = 2; i < p.ammo.size(); ++i) {
            CHECK_EQ(p.ammo[i], std::uint32_t(0));
        }
        CHECK_NEAR(p.armor_points(), 100.0, 1e-12);
        CHECK_NEAR(p.armor_max_points(), 100.0, 1e-12);
        CHECK_NEAR(p.shield_max_points(), 80.0, 1e-12);
        CHECK_NEAR(p.shield_points(3), 80.0, 1e-12);
        CHECK_EQ(p.pair_headers[0], std::string("LEVEL0"));
        CHECK_EQ(p.pair_headers[1], std::string(""));

        // savefile.py's anchor heuristic: level0.sbt, 33 anchors, state
        // boundary 0x1f384 (inside the SCRIPT record; the chain is parsed from
        // +0x13c instead).
        std::string level_error;
        const auto match = gm::find_save_level(bytes, kGame, level_error);
        CHECK(match.has_value());
        if (match) {
            CHECK_EQ(match->level_file, std::string("level0.sbt"));
            CHECK_EQ(match->hits, std::size_t(33));
            CHECK_EQ(match->last_anchor, std::size_t(0x1f384));
        } else {
            std::fprintf(stderr, "FAIL %s: level match: %s\n", golden.file,
                         level_error.c_str());
            ++g_failures;
        }
    }
}

void test_synthetic() {
    const std::vector<std::uint8_t> attachment(16, 0xaa);
    std::vector<std::uint8_t> st = make_player_struct();
    put32(st, 0x317c, 7); // one non-zero attachment id -> one 16-byte record

    std::vector<std::uint8_t> save = make_header();
    append_block(save, 19, std::vector<std::uint8_t>(0x7080, 0));
    append_block(save, 9, make_player_payload(st, attachment));
    // things: 260 x {flag; 0x528 if present}, slots 1 and 3 present
    std::vector<std::uint8_t> things;
    for (std::size_t i = 0; i < 260; ++i) {
        const bool present = (i == 1 || i == 3);
        things.push_back(present ? 1 : 0);
        if (present) {
            things.insert(things.end(), 0x528, static_cast<std::uint8_t>(i + 1));
        }
    }
    append_block(save, 3, things);
    save.push_back(0xff);

    std::string error;
    const auto parsed = gm::parse_save(save, error);
    CHECK(parsed.has_value());
    if (!parsed) {
        std::fprintf(stderr, "FAIL synthetic: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(parsed->header.title, std::string("A SAVED GAME"));
    CHECK_EQ(parsed->blocks.size(), std::size_t(3));
    const gm::SaveBlock* block = parsed->find_block(9);
    CHECK(block != nullptr);
    if (block) {
        CHECK_EQ(block->offset, gm::kSaveHeaderSize + 1 + 0x7080);
        CHECK_EQ(block->count, std::uint32_t(1));
        CHECK_EQ(block->size, std::size_t(1 + 8 + 0x3510 + 16 + 2 * (0x50 + 0x3510)));
    }
    const gm::SaveBlock* th = parsed->find_block(3);
    CHECK(th != nullptr);
    if (th) {
        CHECK_EQ(th->count, std::uint32_t(2));
        CHECK_EQ(th->size, std::size_t(1 + 260 + 2 * 0x528));
    }
    CHECK(parsed->player.has_value());
    if (parsed->player) {
        const gm::SavePlayer& p = *parsed->player;
        CHECK(p.has_slot);
        CHECK_EQ(p.attachment_counts[0], std::uint32_t(1));
        CHECK_NEAR(p.position[0], 1.5, 0.0);
        CHECK_NEAR(p.position[1], -2.25, 0.0);
        CHECK_NEAR(p.position[2], 48.0, 0.0);
        CHECK_NEAR(p.angles[2], 0.5, 0.0);
        CHECK_EQ(p.history_index, std::int32_t(7));
        CHECK_EQ(p.armor, std::int32_t(5000));
        CHECK_EQ(p.shield_max, std::int32_t(8000));
        CHECK_EQ(p.weapon_slot, std::int32_t(2));
        CHECK_EQ(p.weapon_mask, std::uint32_t(7));
        CHECK_EQ(p.ammo[0], std::uint32_t(123));
        CHECK_EQ(p.ammo[1], std::uint32_t(45));
    }

    // find_save_level on a directory without level scripts reports cleanly.
    std::string level_error;
    const auto none = gm::find_save_level(save, fs::temp_directory_path() / "gm_save_no_levels",
                                          level_error);
    CHECK(!none.has_value());
    CHECK(!level_error.empty());
}

bool rejected(std::vector<std::uint8_t> bytes, const char* needle) {
    std::string error;
    const auto parsed = gm::parse_save(bytes, error);
    if (parsed) {
        std::fprintf(stderr, "FAIL: corrupt save was accepted (expected %s)\n", needle);
        return false;
    }
    if (error.find(needle) == std::string::npos) {
        std::fprintf(stderr, "FAIL: error %s does not contain %s\n", error.c_str(), needle);
        return false;
    }
    return true;
}

void test_negative() {
    std::string error;
    const std::vector<std::uint8_t> header = make_header();
    const std::vector<std::uint8_t> player = make_player_payload(make_player_struct());

    // Short files.
    CHECK(!gm::parse_save(std::vector<std::uint8_t>{}, error).has_value());
    CHECK(error.find("too short") != std::string::npos);
    CHECK(!gm::parse_save(std::vector<std::uint8_t>(0x100, 0), error).has_value());
    CHECK(error.find("too short") != std::string::npos);

    // Header only: no terminator.
    CHECK(rejected(header, "no 0xff terminator"));
    // Trailing data after EOF.
    {
        std::vector<std::uint8_t> save = header;
        save.push_back(0xff);
        save.push_back(0x00);
        CHECK(rejected(save, "trailing bytes"));
    }
    // Unknown/oversized block id.
    {
        std::vector<std::uint8_t> save = header;
        save.push_back(200);
        CHECK(rejected(save, "unknown record type 200"));
    }
    // Truncated player: flag byte only.
    {
        std::vector<std::uint8_t> save = header;
        save.push_back(9);
        save.push_back(1);
        CHECK(rejected(save, "truncated player struct"));
    }
    // Truncated player: cut 100 bytes out of a valid block.
    {
        std::vector<std::uint8_t> save = header;
        std::vector<std::uint8_t> cut = player;
        cut.resize(cut.size() - 100);
        append_block(save, 9, cut);
        save.push_back(0xff);
        CHECK(rejected(save, "truncated player sub struct"));
    }
    // Presence flag must be 0/1 (player and things).
    {
        std::vector<std::uint8_t> save = header;
        save.push_back(9);
        save.push_back(2);
        CHECK(rejected(save, "presence flag 2 is not 0/1"));
    }
    {
        std::vector<std::uint8_t> save = header;
        std::vector<std::uint8_t> things(260, 0);
        things[17] = 2;
        append_block(save, 3, things);
        save.push_back(0xff);
        CHECK(rejected(save, "presence flag 2 is not 0/1"));
    }
    // Huge counts.
    {
        std::vector<std::uint8_t> save = header;
        save.push_back(1);
        save.insert(save.end(), {0xff, 0xff, 0xff, 0xff});
        CHECK(rejected(save, "count 4294967295 out of range [0, 512]"));
    }
    {
        std::vector<std::uint8_t> save = header;
        save.push_back(2);
        std::vector<std::uint8_t> n(4, 0);
        put32(n, 0, 101);
        save.insert(save.end(), n.begin(), n.end());
        CHECK(rejected(save, "count 101 out of range [0, 100]"));
    }
    // Duplicate block.
    {
        std::vector<std::uint8_t> save = header;
        append_block(save, 19, std::vector<std::uint8_t>(0x7080, 0));
        append_block(save, 19, std::vector<std::uint8_t>(0x7080, 0));
        save.push_back(0xff);
        CHECK(rejected(save, "duplicate block 19"));
    }
    // Non-printable title / empty level name.
    {
        std::vector<std::uint8_t> save = header;
        save[0] = 0x01;
        save.push_back(0xff);
        CHECK(rejected(save, "not printable ASCII"));
    }
    {
        std::vector<std::uint8_t> save = header;
        std::memset(save.data() + gm::kSaveLevelNameAt, 0, 0x10);
        save.push_back(0xff);
        CHECK(rejected(save, "empty level name"));
    }
    // Player field bounds.
    {
        std::vector<std::uint8_t> bad = make_player_struct();
        put32(bad, 0x3244, 0);
        std::vector<std::uint8_t> save = header;
        append_block(save, 9, make_player_payload(bad));
        save.push_back(0xff);
        CHECK(rejected(save, "armor_max 0 out of range"));
    }
    {
        std::vector<std::uint8_t> bad = make_player_struct();
        put32(bad, 0x3248, 20000);
        std::vector<std::uint8_t> save = header;
        append_block(save, 9, make_player_payload(bad));
        save.push_back(0xff);
        CHECK(rejected(save, "armor 20000 > max 10000"));
    }
    {
        std::vector<std::uint8_t> bad = make_player_struct();
        put32(bad, 0x3250 + 8, 9000);
        std::vector<std::uint8_t> save = header;
        append_block(save, 9, make_player_payload(bad));
        save.push_back(0xff);
        CHECK(rejected(save, "shield 2 value 9000 outside [0, 8000]"));
    }
    {
        std::vector<std::uint8_t> bad = make_player_struct();
        put32(bad, 0x3294, 12);
        std::vector<std::uint8_t> save = header;
        append_block(save, 9, make_player_payload(bad));
        save.push_back(0xff);
        CHECK(rejected(save, "weapon slot 12 outside 0..11"));
    }
    // Truncated script (sanity check that block 11 sizes are honoured).
    {
        std::vector<std::uint8_t> save = header;
        save.push_back(11);
        for (int i = 0; i < 4; ++i) {
            save.push_back(0); // things = 0
        }
        for (int i = 0; i < 4; ++i) {
            save.push_back(0); // named = 0
        }
        save.insert(save.end(), {0x10, 0, 0, 0}); // code length 16
        save.insert(save.end(), 4, 0);            // only 4 bytes present
        save.push_back(0xff);
        CHECK(rejected(save, "truncated script code"));
    }
}

} // namespace

int main() {
    test_installed();
    test_synthetic();
    test_negative();
    if (g_failures != 0) {
        std::fprintf(stderr, "save: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("save: ok\n");
    return 0;
}
