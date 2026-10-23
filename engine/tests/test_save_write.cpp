// Engine save writer (M8.7/M9.4, docs/SAVE_WRITE_CPP.md).
//
// 1. Both shipped saves: parse_save -> write_save -> bytes, byte for byte
//    identical, with the size/SHA-256/FNV-1a 64 printed before and after.
// 2. Opaque header bytes (0x10..0x50, never read back) survive write_save()
//    when the original file is passed and are canonicalised to zero without it.
// 3. Title / gauge / ammo edits change only the bytes of those fields and the
//    edited values survive a re-parse.
// 4. Negatives: an empty model, a block offset/size/raw outside the file, a
//    duplicate/0xff id, block 9 without a player model, an unrepresentable
//    field and a mismatched original are refused with a text and a code.
//
// Golden hashes are of the installed files:
//
//   saved0.sbt  243368 bytes  sha256 e39c0c35…b623ff  0xb994929515f53af5
//   saved1.sbt  243368 bytes  sha256 93f4edd3…34bd13  0x0d038197e152cd65
//
// `--emit DIR` writes the C++ bytes of both saves into DIR so the Python
// writer can be diffed against them (see the doc for the cmp command).
#include "res/save.h"
#include "res/save_write.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";
constexpr std::size_t kShippedSize = 243368;
constexpr std::size_t kShippedBlocks = 15;

struct GoldenSave {
    const char* file;
    const char* sha256;
    std::uint64_t fnv;
};

constexpr std::array<GoldenSave, 2> kSaves = {{
    {"saved0.sbt", "e39c0c35c44af6b985cb272f52b1236404d4316b266dffc7ccf0548503b623ff",
     0xb994929515f53af5ULL},
    {"saved1.sbt", "93f4edd3fad07b31f47f392acf0c409f4e9217efcc521052ca8e74966834bd13",
     0x0d038197e152cd65ULL},
}};

// --- helpers -------------------------------------------------------------

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

bool write_all(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string fnv_hex(std::uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(value));
    return std::string(buf);
}

// --- SHA-256 (FIPS 180-4), only to print the numbers in the doc/test -------

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

std::uint32_t rotr32(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_hex(std::span<const std::uint8_t> data) {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> msg(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff));
    }
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t at = chunk + 4 * i;
            w[i] = (static_cast<std::uint32_t>(msg[at]) << 24) |
                   (static_cast<std::uint32_t>(msg[at + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[at + 2]) << 8) |
                   static_cast<std::uint32_t>(msg[at + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    char buf[65];
    for (std::size_t i = 0; i < h.size(); ++i) {
        std::snprintf(buf + 8 * i, 9, "%08x", h[i]);
    }
    return std::string(buf, 64);
}

/// Half-open [begin, end) ranges where the two files differ.  Sizes must match.
std::vector<std::pair<std::size_t, std::size_t>> diff_runs(
    const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    const std::size_t size = std::min(a.size(), b.size());
    std::size_t i = 0;
    while (i < size) {
        if (a[i] == b[i]) {
            ++i;
            continue;
        }
        const std::size_t begin = i;
        while (i < size && a[i] != b[i]) {
            ++i;
        }
        runs.emplace_back(begin, i);
    }
    if (a.size() != b.size()) {
        runs.emplace_back(size, std::max(a.size(), b.size()));
    }
    return runs;
}

bool inside(const std::pair<std::size_t, std::size_t>& run,
            const std::pair<std::size_t, std::size_t>& allowed) {
    return run.first >= allowed.first && run.second <= allowed.second;
}

void print_runs(const std::vector<std::pair<std::size_t, std::size_t>>& runs) {
    for (const auto& run : runs) {
        std::fprintf(stderr, "  diff 0x%zx..0x%zx\n", run.first, run.second);
    }
}

/// Byte-exact round-trip of one installed save through write_save().
void test_roundtrip(const GoldenSave& golden) {
    const int before = g_failures;
    const fs::path path = kGame / "SAVEGAME" / golden.file;
    const std::vector<std::uint8_t> shipped = read_all(path);
    CHECK(!shipped.empty());
    if (shipped.empty()) {
        return;
    }
    CHECK_EQ(shipped.size(), kShippedSize);
    CHECK_EQ(fnv1a(shipped), golden.fnv);
    CHECK_EQ(sha256_hex(shipped), std::string(golden.sha256));

    std::string error;
    const auto save = gm::parse_save(shipped, error);
    if (!save) {
        std::fprintf(stderr, "FAIL %s: parse_save: %s\n", golden.file, error.c_str());
        ++g_failures;
        return;
    }
    CHECK_EQ(save->size, kShippedSize);
    CHECK_EQ(save->blocks.size(), kShippedBlocks);

    gm::SaveWriteCode code = gm::SaveWriteCode::no_round_trip;
    std::vector<std::uint8_t> written;
    const bool ok = gm::write_save(*save, written, &error, &code, shipped);
    if (!ok) {
        std::fprintf(stderr, "FAIL %s: write_save: %s (%s)\n", golden.file, error.c_str(),
                     gm::save_write_code_name(code));
        ++g_failures;
        return;
    }
    CHECK(code == gm::SaveWriteCode::ok);
    CHECK(error.empty());
    CHECK_EQ(written.size(), shipped.size());
    CHECK_EQ(fnv1a(written), golden.fnv);
    CHECK_EQ(sha256_hex(written), std::string(golden.sha256));
    CHECK(written == shipped);

    // Without the original the opaque header bytes are canonicalised; both
    // shipped saves keep zero there, so the result must be identical too.
    std::vector<std::uint8_t> canonical;
    CHECK(gm::write_save(*save, canonical, &error));
    CHECK(canonical == shipped);

    // The re-parse guarantee also means the model is unchanged.
    const auto again = gm::parse_save(written, error);
    CHECK(again.has_value());
    if (again) {
        CHECK_EQ(again->blocks.size(), save->blocks.size());
        CHECK_EQ(again->header.level_name, save->header.level_name);
        CHECK(again->player.has_value());
    }

    if (g_failures == before) {
        std::printf("save_write: %s %zu bytes sha256 %s fnv %s -> identical (%zu blocks)\n",
                    golden.file, written.size(), sha256_hex(written).c_str(),
                    fnv_hex(fnv1a(written)).c_str(), save->blocks.size());
    }
}

// --- opaque header -------------------------------------------------------

void test_opaque_header() {
    const fs::path path = kGame / "SAVEGAME" / "saved0.sbt";
    std::vector<std::uint8_t> shipped = read_all(path);
    if (shipped.empty()) {
        return;
    }
    std::string error;
    const auto save = gm::parse_save(shipped, error);
    CHECK(save.has_value());
    if (!save) {
        return;
    }
    CHECK(std::all_of(shipped.begin() + gm::kSaveOpaqueHeaderAt,
                      shipped.begin() + gm::kSaveOpaqueHeaderAt + gm::kSaveOpaqueHeaderLen,
                      [](std::uint8_t b) { return b == 0; }));

    // Fill 0x10..0x50 with a pattern the parser never reads.
    std::vector<std::uint8_t> patched = shipped;
    for (std::size_t i = 0; i < gm::kSaveOpaqueHeaderLen; ++i) {
        patched[gm::kSaveOpaqueHeaderAt + i] = static_cast<std::uint8_t>(0xa0 + i);
    }
    const auto reparsed = gm::parse_save(patched, error);
    CHECK(reparsed.has_value());
    if (!reparsed) {
        std::fprintf(stderr, "FAIL opaque header: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    CHECK(reparsed->header.title == save->header.title);
    CHECK(reparsed->header.level_name == save->header.level_name);

    // Passing the original keeps those bytes; it is a real byte-exact
    // round-trip on a file the canonical writer would normalise.
    std::vector<std::uint8_t> kept;
    gm::SaveWriteCode code = gm::SaveWriteCode::ok;
    CHECK(gm::write_save(*reparsed, kept, &error, &code, patched));
    CHECK(kept == patched);
    CHECK_EQ(kept.size(), patched.size());

    // Without it they are zero, which is the documented canonical form.
    std::vector<std::uint8_t> canonical;
    CHECK(gm::write_save(*reparsed, canonical, &error, &code));
    CHECK(canonical == shipped);
    CHECK(std::all_of(canonical.begin() + gm::kSaveOpaqueHeaderAt,
                      canonical.begin() + gm::kSaveOpaqueHeaderAt + gm::kSaveOpaqueHeaderLen,
                      [](std::uint8_t b) { return b == 0; }));

    // A wrong-sized original is refused.
    std::vector<std::uint8_t> longer = patched;
    longer.push_back(0);
    std::vector<std::uint8_t> out(7, 0xcd);
    CHECK(!gm::write_save(*reparsed, out, &error, &code, longer));
    CHECK(out.empty());
    CHECK(code == gm::SaveWriteCode::block_bounds);
}

// --- edits ---------------------------------------------------------------

/// The vitals edit shared by the test and by `--emit`, so the Python writer
/// can be diffed on exactly the same input.
struct VitalsEdit {
    std::int32_t armor = 0;
    std::int32_t shield = 0;
    std::uint32_t ammo = 0;
};

VitalsEdit apply_vitals_edit(gm::SaveGame& game) {
    VitalsEdit edit;
    if (!game.player) {
        return edit;
    }
    edit.armor = game.player->armor_max / 2;
    edit.shield = game.player->shield_max / 4;
    edit.ammo = game.player->ammo[3] + 7;
    game.player->armor = edit.armor;
    game.player->shields[2] = edit.shield;
    game.player->ammo[3] = edit.ammo;
    return edit;
}

void test_edits() {
    const int before = g_failures;
    const fs::path path = kGame / "SAVEGAME" / "saved0.sbt";
    const std::vector<std::uint8_t> shipped = read_all(path);
    if (shipped.empty()) {
        return;
    }
    std::string error;
    auto save = gm::parse_save(shipped, error);
    CHECK(save.has_value());
    if (!save || !save->player) {
        return;
    }
    const std::size_t slot = save->player->slot_offsets[0];
    CHECK(slot > 0);

    // 1. Title only: bytes 0..12 are the title field.
    gm::SaveGame edited = *save;
    edited.header.title = "GM WRITE";
    std::vector<std::uint8_t> out;
    CHECK(gm::write_save(edited, out, &error));
    CHECK_EQ(out.size(), shipped.size());
    const auto title_runs = diff_runs(shipped, out);
    CHECK(!title_runs.empty());
    bool title_ok = true;
    for (const auto& run : title_runs) {
        if (!inside(run, {0, 12})) {
            title_ok = false;
        }
    }
    if (!title_ok) {
        print_runs(title_runs);
    }
    CHECK(title_ok);
    const auto retitled = gm::parse_save(out, error);
    CHECK(retitled.has_value());
    if (retitled) {
        CHECK_EQ(retitled->header.title, std::string("GM WRITE"));
        CHECK_EQ(retitled->header.level_name, save->header.level_name);
        CHECK_EQ(retitled->header.fields, save->header.fields);
        CHECK_EQ(retitled->blocks.size(), save->blocks.size());
        CHECK(retitled->player.has_value());
        if (retitled->player) {
            CHECK_EQ(retitled->player->armor, save->player->armor);
            CHECK_EQ(retitled->player->ammo, save->player->ammo);
        }
    }

    // 2. Vitals and ammo: armor, shields[2], ammo[3].  Only the bytes of
    //    those three fields may move.
    gm::SaveGame vitals = *save;
    const VitalsEdit edit = apply_vitals_edit(vitals);
    const std::int32_t new_armor = edit.armor;
    const std::int32_t new_shield = edit.shield;
    const std::uint32_t new_ammo = edit.ammo;

    std::vector<std::uint8_t> patched;
    gm::SaveWriteCode code = gm::SaveWriteCode::ok;
    CHECK(gm::write_save(vitals, patched, &error, &code, shipped));
    CHECK(code == gm::SaveWriteCode::ok);
    CHECK_EQ(patched.size(), shipped.size());
    const std::array<std::pair<std::size_t, std::size_t>, 3> allowed = {{
        {slot + 0x3248, slot + 0x3248 + 4},                       // armor
        {slot + 0x3250 + 2 * 4, slot + 0x3250 + 2 * 4 + 4},       // shields[2]
        {slot + 0x329c + 3 * 4, slot + 0x329c + 3 * 4 + 4},       // ammo[3]
    }};
    const auto runs = diff_runs(shipped, patched);
    CHECK(!runs.empty());
    bool only_targets = true;
    for (const auto& run : runs) {
        bool hit = false;
        for (const auto& range : allowed) {
            if (inside(run, range)) {
                hit = true;
            }
        }
        if (!hit) {
            only_targets = false;
        }
    }
    if (!only_targets) {
        print_runs(runs);
    }
    CHECK(only_targets);

    const auto reread = gm::parse_save(patched, error);
    CHECK(reread.has_value());
    if (reread && reread->player) {
        CHECK_EQ(reread->player->armor, new_armor);
        CHECK_EQ(reread->player->shields[2], new_shield);
        CHECK_EQ(reread->player->ammo[3], new_ammo);
        CHECK_EQ(reread->player->shield_max, save->player->shield_max);
        CHECK_EQ(reread->player->ammo[0], save->player->ammo[0]);
        CHECK_EQ(reread->player->position, save->player->position);
        CHECK_EQ(reread->header.title, save->header.title);
        // Blocks other than 9 are untouched byte for byte.
        for (const gm::SaveBlock& block : save->blocks) {
            if (block.id == 9) {
                continue;
            }
            const gm::SaveBlock* other = reread->find_block(block.id);
            CHECK(other != nullptr);
            if (other) {
                CHECK(other->raw == block.raw);
            }
        }
    }
    if (g_failures == before) {
        std::printf("save_write: edits ok (title %zu run(s), vitals %zu run(s), size %zu)\n",
                    title_runs.size(), runs.size(), patched.size());
    }
}

// --- negatives -----------------------------------------------------------

bool refused(const gm::SaveGame& game, gm::SaveWriteCode expect, const char* needle,
             std::span<const std::uint8_t> original = {}) {
    ++g_checks;
    std::string error;
    std::vector<std::uint8_t> out(13, 0xcd);
    gm::SaveWriteCode code = gm::SaveWriteCode::ok;
    const bool ok = gm::write_save(game, out, &error, &code, original);
    if (ok) {
        std::fprintf(stderr, "FAIL negative: accepted (expected %s)\n",
                     gm::save_write_code_name(expect));
        ++g_failures;
        return false;
    }
    if (code != expect) {
        std::fprintf(stderr, "FAIL negative: code %s, expected %s (%s)\n",
                     gm::save_write_code_name(code), gm::save_write_code_name(expect),
                     error.c_str());
        ++g_failures;
        return false;
    }
    if (!out.empty()) {
        std::fprintf(stderr, "FAIL negative: out is not empty after a failure\n");
        ++g_failures;
        return false;
    }
    if (error.rfind("write_save: ", 0) != 0) {
        std::fprintf(stderr, "FAIL negative: error is not prefixed: %s\n", error.c_str());
        ++g_failures;
        return false;
    }
    if (needle && error.find(needle) == std::string::npos) {
        std::fprintf(stderr, "FAIL negative: '%s' not in '%s'\n", needle, error.c_str());
        ++g_failures;
        return false;
    }
    return true;
}

void test_synthetic_negatives() {
    // No blocks at all: an empty model is not a save.
    {
        gm::SaveGame empty;
        refused(empty, gm::SaveWriteCode::no_blocks, "no blocks");
    }
    // No size: a hand-built model that never came from parse_save.
    {
        gm::SaveGame nostamp;
        nostamp.header.title = "SYNTHETIC";
        nostamp.header.level_name = "LEVEL0";
        nostamp.blocks.push_back(gm::SaveBlock{});
        refused(nostamp, gm::SaveWriteCode::block_bounds, "no size");
    }
    // A block whose raw payload does not fill its record is refused even
    // without an installation to read.
    {
        gm::SaveGame badraw;
        badraw.header.title = "SYNTHETIC";
        badraw.header.level_name = "LEVEL0";
        badraw.size = gm::kSaveHeaderSize + 3;
        gm::SaveBlock block;
        block.id = 5;
        block.offset = gm::kSaveHeaderSize;
        block.size = 2;
        badraw.blocks.push_back(block);
        refused(badraw, gm::SaveWriteCode::block_bounds, "raw payload");
    }
}

void test_negatives() {
    const std::vector<std::uint8_t> shipped =
        read_all(kGame / "SAVEGAME" / "saved0.sbt");
    if (shipped.empty()) {
        return;
    }
    std::string error;
    const auto parsed = gm::parse_save(shipped, error);
    CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }

    // Block offset outside the file.
    {
        gm::SaveGame moved = *parsed;
        moved.blocks[2].offset += 0x100;
        refused(moved, gm::SaveWriteCode::block_bounds, "expected");
    }
    // Block size running past the end of the file.
    {
        gm::SaveGame huge = *parsed;
        huge.blocks[3].size = huge.size;
        refused(huge, gm::SaveWriteCode::block_bounds, "past the");
    }
    // raw and size disagree (truncated payload).
    {
        gm::SaveGame shortraw = *parsed;
        shortraw.blocks[4].raw.pop_back();
        refused(shortraw, gm::SaveWriteCode::block_bounds, "raw payload");
    }
    // Duplicate id.
    {
        gm::SaveGame dup = *parsed;
        dup.blocks[5].id = dup.blocks[4].id;
        refused(dup, gm::SaveWriteCode::block_layout, "duplicate");
    }
    // 0xff is the terminator, not a record.
    {
        gm::SaveGame eof = *parsed;
        eof.blocks[5].id = gm::kSaveBlockEof;
        refused(eof, gm::SaveWriteCode::block_layout, "0xff");
    }
    // Block 9 without a player model.
    {
        gm::SaveGame noplayer = *parsed;
        noplayer.player.reset();
        refused(noplayer, gm::SaveWriteCode::missing_player, "no player state");
    }
    // Player model without block 9.
    {
        gm::SaveGame noblock = *parsed;
        const std::size_t last_size = noblock.blocks.back().size;
        CHECK_EQ(noblock.blocks.back().id, 9);
        noblock.blocks.pop_back();
        noblock.size -= last_size;
        CHECK(noblock.find_block(9) == nullptr);
        refused(noblock, gm::SaveWriteCode::missing_player, "no block 9");
    }
    // The model's size does not match the chain (missing terminator byte).
    {
        gm::SaveGame size = *parsed;
        size.size += 1;
        refused(size, gm::SaveWriteCode::block_bounds, "terminator");
    }
    // Unrepresentable title (the field is 12 bytes).
    {
        gm::SaveGame title = *parsed;
        title.header.title = std::string(13, 'A');
        refused(title, gm::SaveWriteCode::not_representable, "maximum is 12");
    }
    // Unrepresentable gauges (armor above armor_max).
    {
        gm::SaveGame gauges = *parsed;
        gauges.player->armor = gauges.player->armor_max + 1;
        refused(gauges, gm::SaveWriteCode::not_representable, "armor");
    }
    // A non-finite position is refused.
    {
        gm::SaveGame nan = *parsed;
        nan.player->position[1] = std::numeric_limits<double>::quiet_NaN();
        refused(nan, gm::SaveWriteCode::not_representable, "position");
    }
    // An original of the wrong size.
    {
        std::vector<std::uint8_t> longer = shipped;
        longer.push_back(0);
        refused(*parsed, gm::SaveWriteCode::block_bounds, "original is", longer);
    }
    // Success path still works on the same model afterwards.
    {
        std::vector<std::uint8_t> out;
        CHECK(gm::write_save(*parsed, out, &error));
        CHECK(out == shipped);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--emit") {
        const fs::path dir = argv[2];
        int failed = 0;
        for (const GoldenSave& golden : kSaves) {
            const std::vector<std::uint8_t> shipped =
                read_all(kGame / "SAVEGAME" / golden.file);
            std::string error;
            const auto save = gm::parse_save(shipped, error);
            std::vector<std::uint8_t> out;
            if (!save || !gm::write_save(*save, out, &error)) {
                std::fprintf(stderr, "emit %s: %s\n", golden.file, error.c_str());
                ++failed;
                continue;
            }
            const fs::path dst = dir / golden.file;
            if (!write_all(dst, out)) {
                std::fprintf(stderr, "emit %s: cannot write %s\n", golden.file,
                             dst.string().c_str());
                ++failed;
                continue;
            }
            std::printf("emit %s %zu bytes sha256 %s\n", dst.string().c_str(), out.size(),
                        sha256_hex(out).c_str());
        }

        // Same armor/shields[2]/ammo[3] edit as test_edits(), written as
        // `saved0-edit.sbt` so the Python writer can be diffed on it too.
        {
            const std::vector<std::uint8_t> shipped =
                read_all(kGame / "SAVEGAME" / "saved0.sbt");
            std::string error;
            const auto save = gm::parse_save(shipped, error);
            gm::SaveGame edited = save ? *save : gm::SaveGame{};
            const VitalsEdit edit = apply_vitals_edit(edited);
            std::vector<std::uint8_t> out;
            if (!save || !gm::write_save(edited, out, &error)) {
                std::fprintf(stderr, "emit saved0-edit.sbt: %s\n", error.c_str());
                ++failed;
            } else {
                const fs::path dst = dir / "saved0-edit.sbt";
                if (!write_all(dst, out)) {
                    std::fprintf(stderr, "emit: cannot write %s\n", dst.string().c_str());
                    ++failed;
                } else {
                    std::printf("emit %s %zu bytes sha256 %s (armor %d, shield %d, ammo %u)\n",
                                dst.string().c_str(), out.size(), sha256_hex(out).c_str(),
                                edit.armor, edit.shield, edit.ammo);
                }
            }
        }
        return failed == 0 ? 0 : 1;
    }

    test_synthetic_negatives();
    if (!fs::exists(kGame / "SAVEGAME" / "saved0.sbt")) {
        std::printf("save_write: shipped saves not installed, synthetic cases only\n");
    } else {
        for (const GoldenSave& golden : kSaves) {
            test_roundtrip(golden);
        }
        test_opaque_header();
        test_edits();
        test_negatives();
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "save_write: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("save_write: %d checks, ok\n", g_checks);
    return 0;
}
