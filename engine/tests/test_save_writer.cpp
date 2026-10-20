// Save writer: byte-exact round-trip of the shipped saves, typed player edits,
// 1/8 present slots and synthetic negatives.
//
// Golden hashes are FNV-1a 64 of the installed files:
//
//   saved0.sbt  243368 bytes  0xb994929515f53af5
//   saved1.sbt  243368 bytes  0x0d038197e152cd65
//
// The API and the raw/typed strategy are documented in docs/SAVE_WRITER.md.
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
#include <utility>
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
constexpr std::size_t kShippedSize = 243368;

constexpr std::array<std::uint32_t, gm::kSaveFieldCount> kFields = {
    1, 4288, 1, 0, 0, 0, 64, 0, 90000, 90000, 0, 0, 0, 0, 4, 2, 1, 1, 6, 6, 0, 0,
};

struct GoldenSave {
    const char* file;
    std::uint64_t fnv;
};

constexpr std::array<GoldenSave, 2> kSaves = {{
    {"saved0.sbt", 0xb994929515f53af5ULL},
    {"saved1.sbt", 0x0d038197e152cd65ULL},
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

std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/// Byte runs where a and b differ, capped at *limit*.
std::vector<std::pair<std::size_t, std::size_t>>
diff_runs(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
          std::size_t limit = 16) {
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    const std::size_t n = std::min(a.size(), b.size());
    bool inside = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            if (!inside) {
                start = i;
                inside = true;
            }
        } else if (inside) {
            runs.emplace_back(start, i);
            inside = false;
            if (runs.size() >= limit) {
                return runs;
            }
        }
    }
    if (inside) {
        runs.emplace_back(start, n);
    }
    if (a.size() != b.size()) {
        runs.emplace_back(std::min(a.size(), b.size()), std::max(a.size(), b.size()));
    }
    return runs;
}

/// True when no byte outside [lo, hi) differs and the sizes match.
bool same_outside(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                  std::size_t lo, std::size_t hi) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i >= lo && i < hi) {
            continue;
        }
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> make_header() {
    std::vector<std::uint8_t> v(gm::kSaveHeaderSize, 0);
    std::memcpy(v.data(), "A SAVED GAME", 12);
    std::memcpy(v.data() + 0x0c, "4", 1); // version word 0x34, as in saved0
    std::memcpy(v.data() + gm::kSaveLevelNameAt, "LEVEL0", 6);
    for (std::size_t i = 0; i < kFields.size(); ++i) {
        put32(v, gm::kSaveFieldsAt + 4 * i, kFields[i]);
    }
    std::memcpy(v.data() + gm::kSaveNameSlotAt, "K-9", 3);
    std::memcpy(v.data() + gm::kSaveNameSlotAt + 2 * gm::kSaveNameSlotLen, "coargus", 7);
    std::memcpy(v.data() + gm::kSaveNameSlotAt + 3 * gm::kSaveNameSlotLen, "none", 4);
    return v;
}

std::vector<std::uint8_t> make_player_struct(int seed) {
    std::vector<std::uint8_t> st(0x3510, 0);
    put_f64(st, 0x0000, 1.5 + seed);
    put_f64(st, 0x0008, -2.25);
    put_f64(st, 0x0010, 48.0);
    put_f64(st, 0x0018, 0.0);
    put_f64(st, 0x0020, 0.0);
    put_f64(st, 0x0028, 0.5);
    put32(st, 0x3178, 7496160);
    put32(st, 0x3204, static_cast<std::uint32_t>((7 + seed) % 240));
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
    put32(st, 0x329c, static_cast<std::uint32_t>(100 + seed));
    put32(st, 0x32a0, 20);
    return st;
}

/// Block 9 payload from up to eight slot structs (empty = absent) plus their
/// opaque attachment bytes, followed by the two {0x50 header, 0x3510} pairs.
std::vector<std::uint8_t>
make_player_payload(const std::array<std::vector<std::uint8_t>, 8>& structs,
                    const std::array<std::vector<std::uint8_t>, 8>& attachments = {}) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(structs[i].empty() ? 0 : 1);
        if (structs[i].empty()) {
            continue;
        }
        out.insert(out.end(), structs[i].begin(), structs[i].end());
        out.insert(out.end(), attachments[i].begin(), attachments[i].end());
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

std::vector<std::uint8_t> make_things() {
    std::vector<std::uint8_t> things;
    for (std::size_t i = 0; i < 260; ++i) {
        const bool present = (i == 1 || i == 3);
        things.push_back(present ? 1 : 0);
        if (present) {
            things.insert(things.end(), 0x528, static_cast<std::uint8_t>(i + 1));
        }
    }
    return things;
}

void append_block(std::vector<std::uint8_t>& save, std::uint8_t id,
                  const std::vector<std::uint8_t>& payload) {
    save.push_back(id);
    save.insert(save.end(), payload.begin(), payload.end());
}

/// Minimal valid save used by the parse negatives: header, ITEM, player and
/// things.  Blocks 19/9/3 are enough to exercise truncation and sizing.
std::vector<std::uint8_t> make_synthetic_bytes() {
    std::array<std::vector<std::uint8_t>, 8> structs{};
    structs[0] = make_player_struct(0);
    structs[5] = make_player_struct(5);
    put32(structs[5], 0x317c, 7); // one attachment id -> one 16-byte record
    std::array<std::vector<std::uint8_t>, 8> attachments{};
    attachments[5] = std::vector<std::uint8_t>(16, 0xaa);

    std::vector<std::uint8_t> save = make_header();
    append_block(save, 19, std::vector<std::uint8_t>(0x7080, 0));
    append_block(save, 9, make_player_payload(structs, attachments));
    append_block(save, 3, make_things());
    save.push_back(0xff);
    return save;
}

const gm::Save* parsed_or_fail(const std::vector<std::uint8_t>& bytes, const char* label) {
    static gm::Save save;
    std::string error;
    auto parsed = gm::parse_save(bytes, error);
    if (!parsed) {
        std::fprintf(stderr, "FAIL %s: %s\n", label, error.c_str());
        ++g_failures;
        return nullptr;
    }
    save = std::move(*parsed);
    return &save;
}

void test_installed_roundtrip() {
    if (!fs::exists(kGame / "SAVEGAME" / "saved0.sbt") ||
        !fs::exists(kGame / "SAVEGAME" / "saved1.sbt")) {
        std::printf("save_writer: shipped saves not installed, skipping golden round-trip\n");
        return;
    }

    for (const GoldenSave& golden : kSaves) {
        const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / golden.file);
        CHECK(!bytes.empty());
        if (bytes.empty()) {
            continue;
        }
        CHECK_EQ(bytes.size(), kShippedSize);
        CHECK_EQ(fnv1a(bytes), golden.fnv);

        std::string error;
        const auto save = gm::parse_save(bytes, error);
        if (!save) {
            std::fprintf(stderr, "FAIL %s: %s\n", golden.file, error.c_str());
            ++g_failures;
            continue;
        }

        // Every parsed block keeps its payload verbatim.
        std::size_t sum = gm::kSaveHeaderSize + 1; // header + EOF byte
        for (const gm::SaveBlock& block : save->blocks) {
            CHECK_EQ(block.raw.size(), block.size - 1);
            sum += block.size;
        }
        CHECK_EQ(sum, bytes.size());

        std::string write_error;
        const auto rebuilt = save->to_bytes(write_error);
        if (!rebuilt) {
            std::fprintf(stderr, "FAIL %s: to_bytes: %s\n", golden.file,
                         write_error.c_str());
            ++g_failures;
            continue;
        }
        CHECK_EQ(rebuilt->size(), bytes.size());
        CHECK_EQ(fnv1a(*rebuilt), golden.fnv);
        CHECK(*rebuilt == bytes);

        // The same must hold through rebuild(verify=true), which also refreshes
        // offsets from the emitted bytes.
        gm::Save copy = *save;
        std::string rebuild_error;
        const auto rebuilt2 = gm::rebuild_save(copy, true, rebuild_error);
        if (!rebuilt2) {
            std::fprintf(stderr, "FAIL %s: rebuild: %s\n", golden.file,
                         rebuild_error.c_str());
            ++g_failures;
            continue;
        }
        CHECK(*rebuilt2 == bytes);
        CHECK_EQ(copy.size, bytes.size());
        CHECK_EQ(copy.blocks.size(), save->blocks.size());
    }
}

void test_installed_edits() {
    if (!fs::exists(kGame / "SAVEGAME" / "saved0.sbt")) {
        return;
    }
    const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / "saved0.sbt");
    if (bytes.empty()) {
        return;
    }
    std::string error;
    auto parsed = gm::parse_save(bytes, error);
    if (!parsed) {
        std::fprintf(stderr, "FAIL saved0: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    CHECK(parsed->player.has_value());
    if (!parsed->player) {
        return;
    }

    // ammo[0] lives in the first present 0x3510 struct at +0x329c.
    {
        gm::Save edited = *parsed;
        const std::uint32_t before = edited.player->ammo[0];
        edited.player->ammo[0] = before + 7;
        const std::size_t off = edited.player->slot_offsets[0] + 0x329c;

        std::string write_error;
        const auto out = edited.to_bytes(write_error);
        CHECK(out.has_value());
        if (out) {
            CHECK_EQ(out->size(), bytes.size());
            CHECK(same_outside(bytes, *out, off, off + 4));
            CHECK_EQ(get32(*out, off), before + 7);

            std::string reparse_error;
            const auto check = gm::parse_save(*out, reparse_error);
            CHECK(check.has_value());
            if (check) {
                CHECK_EQ(check->player->ammo[0], before + 7);
                CHECK_EQ(check->player->ammo[1], parsed->player->ammo[1]);
                CHECK_EQ(check->player->armor, parsed->player->armor);
            }
        }
    }

    // armor at +0x3248.
    {
        gm::Save edited = *parsed;
        const std::int32_t before = edited.player->armor;
        edited.player->armor = before - 100;
        const std::size_t off = edited.player->slot_offsets[0] + 0x3248;

        std::string write_error;
        const auto out = edited.to_bytes(write_error);
        CHECK(out.has_value());
        if (out) {
            CHECK(same_outside(bytes, *out, off, off + 4));
            CHECK_EQ(get32(*out, off), static_cast<std::uint32_t>(before - 100));

            std::string reparse_error;
            const auto check = gm::parse_save(*out, reparse_error);
            CHECK(check.has_value());
            if (check) {
                CHECK_EQ(check->player->armor, before - 100);
            }
        }
    }

    // position.x is a little-endian f64 at struct+0x0000.
    {
        gm::Save edited = *parsed;
        const double before = edited.player->position[0];
        edited.player->position[0] = before + 1.25;
        const std::size_t off = edited.player->slot_offsets[0] + 0x0000;

        std::string write_error;
        const auto out = edited.to_bytes(write_error);
        CHECK(out.has_value());
        if (out) {
            CHECK(same_outside(bytes, *out, off, off + 8));

            std::string reparse_error;
            const auto check = gm::parse_save(*out, reparse_error);
            CHECK(check.has_value());
            if (check) {
                CHECK_NEAR(check->player->position[0], before + 1.25, 0.0);
                CHECK_NEAR(check->player->position[1], parsed->player->position[1], 0.0);
            }
        }
    }

    // rebuild(verify=true) keeps the edit and refreshes the raw payload.
    {
        gm::Save edited = *parsed;
        const std::uint32_t before = edited.player->ammo[1];
        edited.player->ammo[1] = before + 3;
        std::string rebuild_error;
        const auto out = gm::rebuild_save(edited, true, rebuild_error);
        CHECK(out.has_value());
        if (out) {
            const std::size_t off = edited.player->slot_offsets[0] + 0x32a0;
            CHECK(same_outside(bytes, *out, off, off + 4));
            CHECK_EQ(edited.player->ammo[1], before + 3);
            const gm::SaveBlock* block = edited.find_block(9);
            CHECK(block != nullptr);
            if (block) {
                CHECK_EQ(block->raw.size(), block->size - 1);
            }
            std::string again_error;
            const auto again = edited.to_bytes(again_error);
            CHECK(again.has_value());
            if (again) {
                CHECK(*again == *out);
            }
        }
    }
}

void test_eight_present_slots() {
    std::array<std::vector<std::uint8_t>, 8> structs{};
    for (std::size_t i = 0; i < structs.size(); ++i) {
        structs[i] = make_player_struct(static_cast<int>(i));
    }
    put32(structs[5], 0x317c, 7); // opaque attachment on slot 5
    std::array<std::vector<std::uint8_t>, 8> attachments{};
    attachments[5].assign(16, 0x5a);

    std::vector<std::uint8_t> bytes = make_header();
    append_block(bytes, 9, make_player_payload(structs, attachments));
    bytes.push_back(0xff);

    std::string error;
    auto parsed = gm::parse_save(bytes, error);
    CHECK(parsed.has_value());
    if (!parsed) {
        std::fprintf(stderr, "FAIL eight slots: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(parsed->blocks.size(), std::size_t(1));
    CHECK_EQ(parsed->blocks[0].count, std::uint32_t(8));
    CHECK_EQ(parsed->blocks[0].size,
             std::size_t(1 + 8 + 8 * 0x3510 + 16 + 2 * (0x50 + 0x3510)));
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK_EQ(parsed->player->present[i], true);
        CHECK_EQ(parsed->player->attachment_counts[i], std::uint32_t(i == 5 ? 1 : 0));
    }

    std::string write_error;
    const auto rebuilt = parsed->to_bytes(write_error);
    CHECK(rebuilt.has_value());
    if (rebuilt) {
        CHECK(*rebuilt == bytes);
    }

    // Only the typed first slot is patched; the other seven structs are raw.
    gm::Save edited = *parsed;
    edited.player->ammo[5] = 777;
    const std::size_t off = edited.player->slot_offsets[0] + 0x329c + 5 * 4;
    std::string edit_error;
    const auto out = edited.to_bytes(edit_error);
    CHECK(out.has_value());
    if (out) {
        CHECK(same_outside(bytes, *out, off, off + 4));
        std::string reparse_error;
        const auto check = gm::parse_save(*out, reparse_error);
        CHECK(check.has_value());
        if (check) {
            CHECK_EQ(check->player->ammo[5], std::uint32_t(777));
            CHECK_EQ(check->player->attachment_counts[5], std::uint32_t(1));
            CHECK_EQ(check->player->slot_raw[5], parsed->player->slot_raw[5]);
            CHECK_EQ(check->player->attachment_raw[5], parsed->player->attachment_raw[5]);
        }
    }
}

void test_writer_from_scratch() {
    gm::Save save;
    save.header.title = "A SAVED GAME";
    save.header.version = 0x34;
    save.header.level_name = "LEVEL0";
    save.header.fields = kFields;
    save.header.name_slots = {"K-9", "", "coargus", "none"};

    gm::SaveBlock item;
    item.id = 19;
    item.raw.assign(0x7080, 0);
    save.blocks.push_back(item);

    gm::SaveBlock things;
    things.id = 3;
    things.raw = make_things();
    save.blocks.push_back(things);

    std::array<std::vector<std::uint8_t>, 8> structs{};
    structs[0] = make_player_struct(0);
    gm::SaveBlock player;
    player.id = 9;
    player.raw = make_player_payload(structs);
    save.blocks.push_back(player);

    std::string error;
    const auto bytes = save.to_bytes(error);
    if (!bytes) {
        std::fprintf(stderr, "FAIL writer scratch: %s\n", error.c_str());
        ++g_failures;
        return;
    }

    const std::size_t item_at = gm::kSaveHeaderSize;
    const std::size_t things_at = item_at + 1 + 0x7080;
    const std::size_t player_at = things_at + 1 + things.raw.size();
    CHECK_EQ(bytes->size(), player_at + 1 + player.raw.size() + 1);

    std::string parse_error;
    const auto parsed = gm::parse_save(*bytes, parse_error);
    CHECK(parsed.has_value());
    if (!parsed) {
        std::fprintf(stderr, "FAIL writer scratch parse: %s\n", parse_error.c_str());
        return;
    }
    CHECK_EQ(parsed->blocks.size(), std::size_t(3));
    const gm::SaveBlock* parsed_item = parsed->find_block(19);
    const gm::SaveBlock* parsed_player = parsed->find_block(9);
    CHECK(parsed_item != nullptr);
    CHECK(parsed_player != nullptr);
    if (parsed_item) {
        CHECK_EQ(parsed_item->offset, item_at);
    }
    if (parsed_player) {
        CHECK_EQ(parsed_player->offset, player_at);
    }
    CHECK(parsed->player.has_value());
    if (parsed->player) {
        CHECK_EQ(parsed->player->attachment_counts[0], std::uint32_t(0));
        CHECK_EQ(parsed->player->ammo[0], std::uint32_t(100));
    }

    std::string again_error;
    const auto again = parsed->to_bytes(again_error);
    CHECK(again.has_value());
    if (again) {
        CHECK(*again == *bytes);
    }

    gm::Save copy = *parsed;
    std::string rebuild_error;
    const auto rebuilt = gm::rebuild_save(copy, true, rebuild_error);
    CHECK(rebuilt.has_value());
    if (rebuilt) {
        CHECK(*rebuilt == *bytes);
    }
}

void test_parse_negatives() {
    const std::vector<std::uint8_t> valid = make_synthetic_bytes();
    const gm::Save* parsed = parsed_or_fail(valid, "negatives baseline");
    if (!parsed) {
        return;
    }
    const gm::SaveBlock* player = parsed->find_block(9);
    CHECK(player != nullptr);
    if (!player) {
        return;
    }

    auto rejected = [](std::vector<std::uint8_t> bytes, const char* needle) {
        std::string error;
        const auto save = gm::parse_save(bytes, error);
        if (save) {
            std::fprintf(stderr, "FAIL: corrupt save accepted (expected %s)\n", needle);
            ++g_failures;
            return;
        }
        if (error.find(needle) == std::string::npos) {
            std::fprintf(stderr, "FAIL: error \"%s\" does not contain \"%s\"\n",
                         error.c_str(), needle);
            ++g_failures;
        }
    };

    // Truncated block: flag byte + four struct bytes, then a valid terminator.
    {
        std::vector<std::uint8_t> cut(valid.begin(),
                                      valid.begin() + static_cast<std::ptrdiff_t>(
                                                           player->offset + 1 + 4));
        cut.push_back(0xff);
        rejected(std::move(cut), "truncated player struct");
    }
    // Wrong count for a count-prefixed block.
    {
        std::vector<std::uint8_t> save = make_header();
        save.push_back(2);
        const std::size_t at = save.size();
        save.resize(at + 4, 0);
        put32(save, at, 101);
        save.push_back(0xff);
        rejected(std::move(save), "count 101 out of range [0, 100]");
    }
    // Oversized count (0xffffffff).
    {
        std::vector<std::uint8_t> save = make_header();
        save.push_back(1);
        const std::size_t at = save.size();
        save.resize(at + 4, 0xff);
        save.push_back(0xff);
        rejected(std::move(save), "count 4294967295 out of range [0, 512]");
    }
    // Missing terminator.
    {
        std::vector<std::uint8_t> save = valid;
        save.pop_back();
        rejected(std::move(save), "no 0xff terminator");
    }
    // Trailing data after the terminator.
    {
        std::vector<std::uint8_t> save = valid;
        save.push_back(0);
        rejected(std::move(save), "trailing bytes");
    }
}

void test_writer_negatives() {
    const std::vector<std::uint8_t> valid = make_synthetic_bytes();
    std::string error;
    auto parsed = gm::parse_save(valid, error);
    CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }

    auto expect_error = [](const gm::Save& save, const char* needle) {
        std::string write_error;
        const auto bytes = save.to_bytes(write_error);
        if (bytes) {
            std::fprintf(stderr, "FAIL: writer accepted invalid save (expected %s)\n",
                         needle);
            ++g_failures;
            return;
        }
        if (write_error.find(needle) == std::string::npos) {
            std::fprintf(stderr, "FAIL: writer error \"%s\" does not contain \"%s\"\n",
                         write_error.c_str(), needle);
            ++g_failures;
        }
    };

    {
        gm::Save save = *parsed;
        save.header.title.clear();
        expect_error(save, "empty save title");
    }
    {
        gm::Save save = *parsed;
        save.header.title = "1234567890123"; // 13 bytes
        expect_error(save, "maximum is 12");
    }
    {
        gm::Save save = *parsed;
        save.header.level_name.clear();
        expect_error(save, "empty level name");
    }
    {
        gm::Save save = *parsed;
        save.player->armor = save.player->armor_max + 1;
        expect_error(save, "armor");
    }
    {
        gm::Save save = *parsed;
        save.blocks[0].raw.clear();
        expect_error(save, "has no raw payload");
    }
    {
        gm::Save save = *parsed;
        save.blocks.push_back(save.blocks[0]);
        expect_error(save, "duplicate block");
    }
    {
        gm::Save save = *parsed;
        for (std::size_t i = 0; i < save.blocks.size(); ++i) {
            if (save.blocks[i].id == 9) {
                save.blocks.erase(save.blocks.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        expect_error(save, "no block 9");
    }
    {
        gm::Save save = *parsed;
        save.blocks[0].id = gm::kSaveBlockEof;
        expect_error(save, "reserved for the terminator");
    }
    {
        gm::Save save = *parsed;
        save.blocks[0].id = 200;
        expect_error(save, "unknown block id 200");
    }
}

} // namespace

int main() {
    test_installed_roundtrip();
    test_installed_edits();
    test_eight_present_slots();
    test_writer_from_scratch();
    test_parse_negatives();
    test_writer_negatives();
    if (g_failures != 0) {
        std::fprintf(stderr, "save_writer: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("save_writer: ok\n");
    return 0;
}
