// Typed save blocks 1 (switches), 2 (doors), 3 (things), 6 (sprites) and
// 7 (sound sources): byte-exact round-trip of the shipped saves, one-field
// edits and synthetic negatives.
//
// Goldens are FNV-1a 64 of the installed files:
//
//   saved0.sbt  243368 bytes  0xb994929515f53af5
//   saved1.sbt  243368 bytes  0x0d038197e152cd65
//
// The layouts are documented in docs/SAVE_BLOCKS.md (sources: SAVE_FIELDS.md
// and tools/re/save_fields.py).
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
    return runs;
}

bool expect_parse_error(const std::vector<std::uint8_t>& bytes, const char* needle) {
    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    if (save) {
        std::fprintf(stderr, "FAIL: expected parse error containing \"%s\"\n", needle);
        return false;
    }
    if (error.find(needle) == std::string::npos) {
        std::fprintf(stderr, "FAIL: error \"%s\" does not contain \"%s\"\n", error.c_str(),
                     needle);
        return false;
    }
    return true;
}

std::size_t block_payload(const gm::Save& save, std::uint8_t id) {
    const gm::SaveBlock* block = save.find_block(id);
    if (!block) {
        std::fprintf(stderr, "FAIL: block %u is missing\n", static_cast<unsigned>(id));
        return 0;
    }
    return block->offset + 1;
}

void check_switches(const gm::Save& save) {
    CHECK_EQ(save.switches.size(), 24u);
    const gm::SaveBlock* block = save.find_block(1);
    CHECK(block != nullptr);
    if (!block) {
        return;
    }
    CHECK_EQ(block->count, 24u);
    CHECK_EQ(block->size, 1 + 4 + 24 * gm::kSaveSwitchRecordSize);

    const gm::SaveSwitch& s0 = save.switches[0];
    CHECK_EQ(s0.kind, 0);
    CHECK_EQ(s0.id, -30240);
    CHECK_EQ(s0.radius, 160);
    CHECK_EQ(s0.pos[0], -600);
    CHECK_EQ(s0.pos[1], 928);
    CHECK_EQ(s0.pos[2], -88);
    CHECK_EQ(s0.aux, 0);
    CHECK_EQ(s0.sound, -1);

    const gm::SaveSwitch& s3 = save.switches[3];
    CHECK_EQ(s3.kind, 0);
    CHECK_EQ(s3.id, 11);
    CHECK_EQ(s3.radius, 96);
    CHECK_EQ(s3.pos[0], -968);
    CHECK_EQ(s3.pos[1], 928);
    CHECK_EQ(s3.pos[2], -96);
    CHECK_EQ(s3.aux, 38);
    CHECK_EQ(s3.sound, 127);

    const gm::SaveSwitch& s5 = save.switches[5];
    CHECK_EQ(s5.kind, 0x101);
    CHECK_EQ(s5.id, 9);
    CHECK_EQ(s5.radius, 100);
    CHECK_EQ(s5.pos[0], 1);
    CHECK_EQ(s5.pos[1], 0);
    CHECK_EQ(s5.pos[2], 4);

    const gm::SaveSwitch& s23 = save.switches[23];
    CHECK_EQ(s23.kind, 0x104);
    CHECK_EQ(s23.id, -30221);
    CHECK_EQ(s23.radius, 0);
    CHECK_EQ(s23.pos[0], 56);
    CHECK_EQ(s23.pos[1], 1);
    CHECK_EQ(s23.pos[2], 0);
}

void check_doors(const gm::Save& save) {
    CHECK_EQ(save.doors.size(), 25u);
    const gm::SaveBlock* block = save.find_block(2);
    CHECK(block != nullptr);
    if (!block) {
        return;
    }
    CHECK_EQ(block->count, 25u);
    CHECK_EQ(block->size, 1 + 4 + 25 * gm::kSaveDoorRecordSize);

    const gm::SaveDoor& d0 = save.doors[0];
    CHECK_NEAR(d0.box[0], 664.0, 0.0);
    CHECK_NEAR(d0.box[1], 928.0, 0.0);
    CHECK_NEAR(d0.box[2], -4.0, 0.0);
    CHECK_NEAR(d0.box[5], 172.0, 0.0);
    CHECK_EQ(d0.box[8], 0.0);
    CHECK_EQ(d0.delta[0], 0.0);
    CHECK_NEAR(d0.constant, 0.03, 1e-15);
    CHECK_EQ(d0.flags, 3);
    CHECK_EQ(d0.ids[0], 2);
    CHECK_EQ(d0.state, 0);
    CHECK_EQ(d0.sector, 4);
    CHECK_EQ(d0.object, 1);
    CHECK_EQ(d0.phase_prev, 0u);
    CHECK_EQ(d0.phase_next, 0u);
    CHECK_EQ(d0.step, 700);
    CHECK_EQ(d0.param5, 300);
    CHECK_EQ(d0.param6, 0);
    CHECK_EQ(d0.v[0], 64);
    CHECK_EQ(d0.v[1], 64);
    CHECK_EQ(d0.v[2], 64);
    CHECK_EQ(d0.n[0], 132);
    CHECK_EQ(d0.n[1], 133);
    CHECK_EQ(d0.n[2], 134);

    const gm::SaveDoor& d20 = save.doors[20];
    CHECK_EQ(d20.flags, 2);
    CHECK_EQ(d20.sector, 48);
    CHECK_EQ(d20.object, 6);
    CHECK_EQ(d20.step, 800);
    CHECK_NEAR(d20.delta[0], 3.19584207277515e-05, 1e-18);
    CHECK_EQ(d20.n[0], -1);
    CHECK_EQ(d20.n[1], 50);
    CHECK_EQ(d20.n[2], 135);

    const gm::SaveDoor& d24 = save.doors[24];
    CHECK_EQ(d24.flags, 3);
    CHECK_EQ(d24.ids[1], 32);
    CHECK_EQ(d24.sector, 54);
    CHECK_EQ(d24.step, 800);
    CHECK_EQ(d24.param5, 400);
}

void check_sprites(const gm::Save& save) {
    CHECK_EQ(save.sprites.size(), 4u);
    const gm::SaveBlock* block = save.find_block(6);
    CHECK(block != nullptr);
    if (!block) {
        return;
    }
    CHECK_EQ(block->count, 4u);
    CHECK_EQ(block->size, 1 + 4 + 4 * gm::kSaveSpriteRecordSize);

    constexpr std::array<std::uint32_t, 4> kRuntime = {
        5719472, 5719664, 5719856, 5720048};
    constexpr std::array<std::array<std::int32_t, 3>, 4> kPos = {{
        {15, 328, 143},
        {31, 920, 143},
        {15, 0, 127},
        {15, -160, 127},
    }};
    for (std::size_t i = 0; i < save.sprites.size(); ++i) {
        const gm::SaveSprite& s = save.sprites[i];
        CHECK_NEAR(s.scale_x, 1.5, 0.0);
        CHECK_NEAR(s.scale_y, 1.5, 0.0);
        CHECK_EQ(s.runtime_object, kRuntime[i]);
        CHECK_EQ(s.pos, kPos[i]);
        CHECK_EQ(s.param, 24);
        CHECK_EQ(s.flags, 2u);
        CHECK_EQ(s.effect_id, 363);
        CHECK_EQ(s.param16, 4);
        CHECK_EQ(s.spawn_time, 0u);
    }
}

void check_sounds(const gm::Save& save) {
    CHECK_EQ(save.sounds.size(), 6u);
    const gm::SaveBlock* block = save.find_block(7);
    CHECK(block != nullptr);
    if (!block) {
        return;
    }
    CHECK_EQ(block->count, 6u);
    CHECK_EQ(block->size, 1 + 4 + 6 * gm::kSaveSoundRecordSize);

    const gm::SaveSound& s0 = save.sounds[0];
    CHECK_EQ(s0.x, -616);
    CHECK_EQ(s0.y, -168);
    CHECK_EQ(s0.z, 71);
    CHECK_EQ(s0.volume, 19);
    CHECK_EQ(s0.sample, 128);
    CHECK_NEAR(s0.pitch, 0.05, 1e-15);

    const gm::SaveSound& s4 = save.sounds[4];
    CHECK_EQ(s4.x, 1368);
    CHECK_EQ(s4.y, 608);
    CHECK_EQ(s4.z, 23);
    CHECK_EQ(s4.volume, 19);
    CHECK_EQ(s4.sample, 129);

    const gm::SaveSound& s5 = save.sounds[5];
    CHECK_EQ(s5.x, 1552);
    CHECK_EQ(s5.y, 608);
    CHECK_EQ(s5.z, 23);
    CHECK_EQ(s5.volume, 19);
    CHECK_EQ(s5.sample, 114);
    CHECK_NEAR(s5.pitch, 0.1, 1e-15);
}

void check_things(const gm::Save& save) {
    CHECK_EQ(save.things.size(), 6u);
    const gm::SaveBlock* block = save.find_block(3);
    CHECK(block != nullptr);
    if (!block) {
        return;
    }
    CHECK_EQ(block->count, 6u);
    CHECK_EQ(block->size, 1 + gm::kSaveThingSlots + 6 * gm::kSaveThingRecordSize);
    for (std::size_t i = 0; i < gm::kSaveThingSlots; ++i) {
        CHECK_EQ(save.thing_present[i], i < 6);
    }
    for (std::size_t i = 0; i < save.things.size(); ++i) {
        CHECK_EQ(save.things[i].slot, i);
    }
}

void check_file(const GoldenSave& golden) {
    const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / golden.file);
    CHECK_EQ(bytes.size(), kShippedSize);
    if (bytes.size() != kShippedSize) {
        return;
    }
    CHECK_EQ(fnv1a(bytes), golden.fnv);

    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    if (!save) {
        std::fprintf(stderr, "FAIL %s: parse error: %s\n", golden.file, error.c_str());
        ++g_failures;
        return;
    }
    CHECK_EQ(save->size, kShippedSize);
    check_switches(*save);
    check_doors(*save);
    check_sprites(*save);
    check_sounds(*save);
    check_things(*save);

    // Every block keeps its verbatim payload, including the newly typed ones.
    for (const gm::SaveBlock& block : save->blocks) {
        CHECK_EQ(block.raw.size(), block.size - 1);
    }

    std::string write_error;
    std::optional<std::vector<std::uint8_t>> rebuilt = save->to_bytes(write_error);
    if (!rebuilt) {
        std::fprintf(stderr, "FAIL %s: to_bytes: %s\n", golden.file, write_error.c_str());
        ++g_failures;
        return;
    }
    CHECK_EQ(rebuilt->size(), bytes.size());
    CHECK(*rebuilt == bytes);
    CHECK_EQ(fnv1a(*rebuilt), golden.fnv);

    // rebuild_save refreshes offsets/raw and verifies the typed fields.
    std::string rebuild_error;
    std::optional<std::vector<std::uint8_t>> verified =
        gm::rebuild_save(*save, true, rebuild_error);
    if (!verified) {
        std::fprintf(stderr, "FAIL %s: rebuild: %s\n", golden.file,
                     rebuild_error.c_str());
        ++g_failures;
        return;
    }
    CHECK(*verified == bytes);
    CHECK_EQ(save->size, bytes.size());
}

void check_saved0_edits() {
    const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / "saved0.sbt");
    if (bytes.size() != kShippedSize) {
        std::fprintf(stderr, "FAIL: saved0.sbt missing\n");
        ++g_failures;
        return;
    }
    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    if (!save) {
        std::fprintf(stderr, "FAIL saved0: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    const std::size_t b1 = block_payload(*save, 1);
    const std::size_t b2 = block_payload(*save, 2);
    const std::size_t b3 = block_payload(*save, 3);
    const std::size_t b6 = block_payload(*save, 6);
    const std::size_t b7 = block_payload(*save, 7);

    save->switches[3].sound = 7; // was 127
    save->doors[12].step = 123;  // was 99999
    save->sprites[2].pos[0] = 99; // was 15
    save->sounds[4].volume = 42;  // was 19
    save->things[1].raw[0x40] ^= 0x5a;

    const std::size_t switch_at = b1 + 4 + 3 * gm::kSaveSwitchRecordSize + 0x1c;
    const std::size_t door_at = b2 + 4 + 12 * gm::kSaveDoorRecordSize + 0x94;
    const std::size_t sprite_at = b6 + 4 + 2 * gm::kSaveSpriteRecordSize + 0x0c;
    // Block 7 is column-major: x/y/z/volume/sample i32 columns, then f64.
    const std::size_t sound_at = b7 + 4 + 3 * 4 * 6 + 4 * 4;
    const std::size_t thing_at =
        b3 + 1 + 1 * (gm::kSaveThingRecordSize + 1) + 0x40;
    const std::vector<std::pair<std::size_t, std::size_t>> expected = {
        {switch_at, switch_at + 4},
        {door_at, door_at + 4},
        {sprite_at, sprite_at + 4},
        {sound_at, sound_at + 4},
        {thing_at, thing_at + 1},
    };

    std::string rebuild_error;
    std::optional<std::vector<std::uint8_t>> patched =
        gm::rebuild_save(*save, true, rebuild_error);
    if (!patched) {
        std::fprintf(stderr, "FAIL saved0 edit: %s\n", rebuild_error.c_str());
        ++g_failures;
        return;
    }
    CHECK_EQ(patched->size(), bytes.size());

    std::size_t changed = 0;
    std::size_t outside = 0;
    std::array<bool, 5> hit{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if ((*patched)[i] == bytes[i]) {
            continue;
        }
        ++changed;
        bool in_range = false;
        for (std::size_t k = 0; k < expected.size(); ++k) {
            if (i >= expected[k].first && i < expected[k].second) {
                in_range = true;
                hit[k] = true;
            }
        }
        if (!in_range) {
            ++outside;
        }
    }
    CHECK(changed > 0);
    CHECK_EQ(outside, 0u);
    for (bool h : hit) {
        CHECK(h);
    }
    std::vector<std::pair<std::size_t, std::size_t>> runs = diff_runs(bytes, *patched);
    CHECK_EQ(runs.size(), 5u);

    // rebuild_save refreshed the model from the patched bytes.
    CHECK_EQ(save->switches[3].sound, 7);
    CHECK_EQ(save->doors[12].step, 123);
    CHECK_EQ(save->sprites[2].pos[0], 99);
    CHECK_EQ(save->sounds[4].volume, 42);
    CHECK_EQ(save->things[1].raw[0x40], static_cast<std::uint8_t>(
                                             bytes[thing_at] ^ 0x5a));

    std::string again_error;
    std::optional<std::vector<std::uint8_t>> again = save->to_bytes(again_error);
    CHECK(again.has_value());
    if (again) {
        CHECK(*again == *patched);
    }

    // Diff between the two shipped saves: things slot 0 only.
    const std::vector<std::uint8_t> other = read_all(kGame / "SAVEGAME" / "saved1.sbt");
    std::string other_error;
    std::optional<gm::Save> save1 = gm::parse_save(other, other_error);
    if (save1) {
        CHECK(save1->things.size() == save->things.size());
        for (std::size_t i = 0; i < save1->things.size() && i < save->things.size(); ++i) {
            if (i == 0) {
                CHECK(save1->things[i].raw != save->things[i].raw);
            } else if (i != 1) { // slot 1 was edited above
                CHECK(save1->things[i].raw == save->things[i].raw);
            }
        }
    }
}

void check_negatives() {
    const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / "saved0.sbt");
    if (bytes.size() != kShippedSize) {
        std::fprintf(stderr, "FAIL: saved0.sbt missing\n");
        ++g_failures;
        return;
    }
    std::string error;
    std::optional<gm::Save> base = gm::parse_save(bytes, error);
    if (!base) {
        std::fprintf(stderr, "FAIL saved0: %s\n", error.c_str());
        ++g_failures;
        return;
    }
    const std::size_t b1 = block_payload(*base, 1);
    const std::size_t b2 = block_payload(*base, 2);
    const std::size_t b3 = block_payload(*base, 3);
    const std::size_t b6 = block_payload(*base, 6);
    const std::size_t b7 = block_payload(*base, 7);

    // Counts above the documented limits.
    {
        std::vector<std::uint8_t> bad = bytes;
        put32(bad, b2, 101);
        CHECK(expect_parse_error(bad, "count 101 out of range"));
    }
    {
        std::vector<std::uint8_t> bad = bytes;
        put32(bad, b1, 0x201);
        CHECK(expect_parse_error(bad, "count 513 out of range"));
    }
    {
        std::vector<std::uint8_t> bad = bytes;
        put32(bad, b6, 500);
        CHECK(expect_parse_error(bad, "count 500 out of range"));
    }
    {
        std::vector<std::uint8_t> bad = bytes;
        put32(bad, b7, 128);
        CHECK(expect_parse_error(bad, "count 128 out of range"));
    }

    // Truncated record: keep the count, cut the payload in the middle.
    {
        const std::size_t cut =
            b2 + 4 + 2 * gm::kSaveDoorRecordSize + gm::kSaveDoorRecordSize / 2;
        std::vector<std::uint8_t> bad(bytes.begin(),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(expect_parse_error(bad, "truncated door record"));
    }
    {
        const std::size_t cut =
            b1 + 4 + 4 * gm::kSaveSwitchRecordSize + 3;
        std::vector<std::uint8_t> bad(bytes.begin(),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(expect_parse_error(bad, "truncated switch record"));
    }
    {
        const std::size_t cut =
            b6 + 4 + 1 * gm::kSaveSpriteRecordSize + gm::kSaveSpriteRecordSize / 2;
        std::vector<std::uint8_t> bad(bytes.begin(),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(expect_parse_error(bad, "truncated sprite record"));
    }
    {
        const std::size_t cut =
            b7 + 4 + 2 * gm::kSaveSoundRecordSize + 10;
        std::vector<std::uint8_t> bad(bytes.begin(),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(expect_parse_error(bad, "truncated sound record"));
    }

    // Bad stride: count 1 but only 183 bytes before the terminator, so the
    // 0xff is consumed as part of the record and the stream loses its end.
    {
        std::vector<std::uint8_t> bad(bytes.begin(),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(b2 - 1));
        bad.push_back(2);
        const std::size_t count_at = bad.size();
        bad.resize(count_at + 4);
        put32(bad, count_at, 1);
        bad.insert(bad.end(), gm::kSaveDoorRecordSize - 1, 0);
        bad.push_back(0xff);
        CHECK(expect_parse_error(bad, "no 0xff terminator"));
    }

    // Invalid thing presence flag and truncated thing body.
    {
        std::vector<std::uint8_t> bad = bytes;
        bad[b3] = 2;
        CHECK(expect_parse_error(bad, "presence flag 2 is not 0/1"));
    }
    {
        const std::size_t cut = b3 + gm::kSaveThingSlots + gm::kSaveThingRecordSize / 2;
        std::vector<std::uint8_t> bad(bytes.begin(),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(expect_parse_error(bad, "truncated thing record"));
    }

    // Writer-side guards on an otherwise valid model.
    {
        gm::Save copy = *base;
        while (copy.doors.size() <= gm::kSaveMaxDoors) {
            copy.doors.push_back(copy.doors.front());
        }
        std::string write_error;
        CHECK(!copy.to_bytes(write_error));
        CHECK(write_error.find("doors: count 101 out of range") != std::string::npos);
    }
    {
        gm::Save copy = *base;
        copy.thing_present[9] = true;
        std::string write_error;
        CHECK(!copy.to_bytes(write_error));
        CHECK(write_error.find("things: 6 records for 7 presence flags") !=
              std::string::npos);
    }
    {
        gm::Save copy = *base;
        copy.things[0].slot = 1;
        std::string write_error;
        CHECK(!copy.to_bytes(write_error));
        CHECK(write_error.find("expected slot 0") != std::string::npos);
    }
}

} // namespace

int main() {
    if (!fs::exists(kGame / "SAVEGAME" / "saved0.sbt") ||
        !fs::exists(kGame / "SAVEGAME" / "saved1.sbt")) {
        std::fprintf(stderr, "save_blocks: %s not installed, skipping\n",
                     kGame.c_str());
        return 0;
    }

    for (const GoldenSave& golden : kSaves) {
        check_file(golden);
    }
    check_saved0_edits();
    check_negatives();

    if (g_failures != 0) {
        std::fprintf(stderr, "save_blocks: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("save_blocks: ok (2/2 byte-exact, typed blocks 1/2/3/6/7)\n");
    return 0;
}
