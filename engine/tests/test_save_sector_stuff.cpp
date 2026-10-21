// Save block 8 (SECTOR_STUFF) as a typed, writable block: saved0/saved1 are
// parsed field-for-field against the Python oracle (gm/sbt.py ``SectorSlot``,
// tools/re/records.py), the block re-encodes to the exact input bytes, a field
// edit lands on the right byte offset and survives a re-parse, and truncation
// is rejected.
//
// Goldens (payload = the 0x5800 bytes after the block id, identical in both
// saves):
//
//   block offset   0x29e2b (id byte), size 1 + 0x5800
//   payload FNV-1a 0x19fa6a27ba4b0c33
//   13 non-empty slots, the other 243 are all-zero:
//
//   python3 tools/re/records.py --type 8 --json \
//       "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt"
//
// Layout and evidence: docs/RECORDS.md §2 (reader 0x46b7b0, writer 0x46b7f0,
// SET_SECTOR_* handlers 0x457815..0x457a17).
#include "res/save.h"
#include "res/sbt.h"

#include <array>
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

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";
constexpr std::size_t kBlockOffset = 0x29e2b; // block id byte in saved0/saved1
constexpr std::size_t kPayloadOffset = kBlockOffset + 1;
constexpr std::uint64_t kPayloadFnv = 0x19fa6a27ba4b0c33ull;

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

std::uint64_t fnv1a(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    static_assert(sizeof(out) == sizeof(value));
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

/// Every one of the 0x58 bytes is covered by a typed field (the two 16-bit
/// gaps and the dead +0x54 word included), so ``raw`` is only a convenience
/// copy and is deliberately not compared: it goes stale as soon as a field is
/// edited, while the encoder rebuilds the slot from the fields.
bool same_slot(const gm::SbtSectorSlot& a, const gm::SbtSectorSlot& b) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (bits(a.move[i]) != bits(b.move[i]) || bits(a.extra[i]) != bits(b.extra[i])) {
            return false;
        }
    }
    return bits(a.sound_param) == bits(b.sound_param) && a.event_a == b.event_a &&
           a.event_b == b.event_b && a.damage_a == b.damage_a &&
           a.damage_b == b.damage_b && a.sound == b.sound &&
           a.sound_count == b.sound_count && a.flags == b.flags &&
           a.unused == b.unused;
}

struct GoldenSlot {
    int index;
    double sound_param;
    std::int32_t event_a, event_b, sound, sound_count;
};

// Python oracle table (see the file header): every other field is zero.
constexpr std::array<GoldenSlot, 13> kGolden = {{
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

const GoldenSlot* golden_for(std::size_t index) {
    for (const GoldenSlot& row : kGolden) {
        if (static_cast<std::size_t>(row.index) == index) {
            return &row;
        }
    }
    return nullptr;
}

/// Field-for-field check of the typed block against the Python oracle and
/// against an independent parse of the same bytes.
void check_fields(const gm::Save& save) {
    CHECK_EQ(save.sector_stuff.size(), gm::kSbtSectorSlots);
    if (save.sector_stuff.size() != gm::kSbtSectorSlots) {
        return;
    }
    std::size_t non_empty = 0;
    std::size_t matched = 0;
    for (std::size_t i = 0; i < save.sector_stuff.size(); ++i) {
        const gm::SbtSectorSlot& slot = save.sector_stuff[i];
        non_empty += slot.empty() ? 0 : 1;
        const GoldenSlot* golden = golden_for(i);
        if (golden == nullptr) {
            CHECK(slot.empty());
            continue;
        }
        const bool ok =
            bits(slot.sound_param) == bits(golden->sound_param) &&
            slot.event_a == golden->event_a && slot.event_b == golden->event_b &&
            slot.sound == golden->sound && slot.sound_count == golden->sound_count &&
            bits(slot.move[0]) == bits(0.0) && bits(slot.move[1]) == bits(0.0) &&
            bits(slot.move[2]) == bits(0.0) && bits(slot.extra[0]) == bits(0.0) &&
            bits(slot.extra[1]) == bits(0.0) && bits(slot.extra[2]) == bits(0.0) &&
            slot.damage_a == 0 && slot.damage_b == 0 && slot.flags == 0 &&
            slot.unused == 0;
        CHECK(ok);
        matched += ok ? 1 : 0;
    }
    CHECK_EQ(non_empty, kGolden.size());
    CHECK_EQ(matched, kGolden.size());
}

void check_save(const char* name) {
    const fs::path path = kGame / "SAVEGAME" / name;
    const std::vector<std::uint8_t> bytes = read_all(path);
    CHECK(!bytes.empty());
    if (bytes.empty()) {
        return;
    }

    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    CHECK(save.has_value());
    if (!save) {
        std::fprintf(stderr, "%s: %s\n", name, error.c_str());
        return;
    }

    const gm::SaveBlock* block = save->find_block(8);
    CHECK(block != nullptr);
    if (block == nullptr) {
        return;
    }
    CHECK_EQ(block->offset, kBlockOffset);
    CHECK_EQ(block->size, std::size_t(1 + gm::kSbtSectorStuffSize));
    CHECK_EQ(block->count, 0u); // fixed-size block: no count dword
    CHECK_EQ(block->raw.size(), gm::kSbtSectorStuffSize);
    CHECK_EQ(fnv1a(block->raw.data(), block->raw.size()), kPayloadFnv);

    // The typed view is the parser's, field for field.
    std::vector<gm::SbtSectorSlot> direct;
    CHECK(gm::parse_sector_stuff(block->raw, direct, error));
    CHECK_EQ(direct.size(), save->sector_stuff.size());
    if (direct.size() == save->sector_stuff.size()) {
        bool same = true;
        for (std::size_t i = 0; i < direct.size(); ++i) {
            same = same && same_slot(direct[i], save->sector_stuff[i]);
        }
        CHECK(same);
    }

    check_fields(*save);

    // Typed fields -> the exact payload bytes.
    const std::vector<std::uint8_t> encoded = gm::encode_sector_stuff(save->sector_stuff);
    CHECK(encoded == block->raw);

    // Whole-file round-trip through the typed block (not the raw fallback).
    std::string rebuild_error;
    const auto rebuilt = save->to_bytes(rebuild_error);
    CHECK(rebuilt.has_value());
    if (rebuilt) {
        CHECK(*rebuilt == bytes);
    } else {
        std::fprintf(stderr, "%s to_bytes: %s\n", name, rebuild_error.c_str());
    }

    std::string verify_error;
    CHECK(gm::rebuild_save(*save, true, verify_error));
    if (!verify_error.empty()) {
        std::fprintf(stderr, "%s rebuild_save: %s\n", name, verify_error.c_str());
    }
    check_fields(*save);
}

/// Editing a typed field must move exactly the bytes of that field.
void check_edits() {
    const std::vector<std::uint8_t> bytes =
        read_all(kGame / "SAVEGAME" / "saved0.sbt");
    CHECK(!bytes.empty());
    if (bytes.empty()) {
        return;
    }
    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    CHECK(save.has_value());
    if (!save || save->sector_stuff.size() != gm::kSbtSectorSlots) {
        return;
    }

    const std::size_t slot_index = 9; // has sound/event data in the shipped save
    gm::SbtSectorSlot& slot = save->sector_stuff[slot_index];
    slot.move = {1.5, -2.25, 0.125};
    slot.extra = {10.0, 4096.5, -0.0};
    slot.sound_param = 0.25;
    slot.event_a = -7;
    slot.event_b = 123456;
    slot.damage_a = 0xbbaaff34u;
    slot.damage_b = static_cast<std::int32_t>(0xddcc8001u);
    slot.sound = -1;
    slot.sound_count = 99;
    slot.flags = 0x10;
    slot.unused = 0xfeedface;
    // raw is stale on purpose: the encoder must rebuild from the typed fields.

    const std::size_t base = kPayloadOffset + slot_index * gm::kSbtSectorSlotSize;
    std::string rebuild_error;
    const auto rebuilt = save->to_bytes(rebuild_error);
    CHECK(rebuilt.has_value());
    if (!rebuilt) {
        std::fprintf(stderr, "to_bytes after edit: %s\n", rebuild_error.c_str());
        return;
    }
    CHECK_EQ(rebuilt->size(), bytes.size());
    if (rebuilt->size() != bytes.size()) {
        return;
    }
    // Everything outside the edited 0x58 bytes is untouched.
    bool outside_same = true;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i >= base && i < base + gm::kSbtSectorSlotSize) {
            continue;
        }
        outside_same = outside_same && (*rebuilt)[i] == bytes[i];
    }
    CHECK(outside_same);
    CHECK((*rebuilt)[base + 0x38] != bytes[base + 0x38]); // the edit landed

    std::optional<gm::Save> reparsed = gm::parse_save(*rebuilt, error);
    CHECK(reparsed.has_value());
    if (!reparsed) {
        std::fprintf(stderr, "re-parse after edit: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(reparsed->sector_stuff.size(), gm::kSbtSectorSlots);
    if (reparsed->sector_stuff.size() != gm::kSbtSectorSlots) {
        return;
    }
    const gm::SbtSectorSlot& back = reparsed->sector_stuff[slot_index];
    CHECK(bits(back.move[0]) == bits(1.5));
    CHECK(bits(back.move[1]) == bits(-2.25));
    CHECK(bits(back.move[2]) == bits(0.125));
    CHECK(bits(back.extra[0]) == bits(10.0));
    CHECK(bits(back.extra[1]) == bits(4096.5));
    CHECK(bits(back.extra[2]) == bits(-0.0));
    CHECK(bits(back.sound_param) == bits(0.25));
    CHECK_EQ(back.event_a, -7);
    CHECK_EQ(back.event_b, 123456);
    CHECK_EQ(back.damage_a, 0xbbaaff34u);
    CHECK_EQ(back.damage_b, static_cast<std::int32_t>(0xddcc8001u));
    CHECK_EQ(back.sound, -1);
    CHECK_EQ(back.sound_count, 99);
    CHECK_EQ(back.flags, 0x10u);
    CHECK_EQ(back.unused, 0xfeedfaceu);

    // rebuild_save() with the edit keeps the typed fields and is stable.
    std::string verify_error;
    CHECK(gm::rebuild_save(*save, true, verify_error));
    if (!verify_error.empty()) {
        std::fprintf(stderr, "rebuild_save after edit: %s\n", verify_error.c_str());
    }
    CHECK_EQ(save->sector_stuff.size(), gm::kSbtSectorSlots);
    if (save->sector_stuff.size() == gm::kSbtSectorSlots) {
        CHECK(bits(save->sector_stuff[slot_index].sound_param) == bits(0.25));
    }
}

/// An all-zero block stays a valid typed block (256 empty slots).
void check_zero_block() {
    std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / "saved0.sbt");
    CHECK(!bytes.empty());
    if (bytes.size() < kPayloadOffset + gm::kSbtSectorStuffSize) {
        return;
    }
    std::memset(bytes.data() + kPayloadOffset, 0, gm::kSbtSectorStuffSize);

    std::string error;
    std::optional<gm::Save> save = gm::parse_save(bytes, error);
    CHECK(save.has_value());
    if (!save) {
        return;
    }
    CHECK_EQ(save->sector_stuff.size(), gm::kSbtSectorSlots);
    std::size_t non_empty = 0;
    for (const gm::SbtSectorSlot& slot : save->sector_stuff) {
        non_empty += slot.empty() ? 0 : 1;
    }
    CHECK_EQ(non_empty, std::size_t(0));
    CHECK(gm::encode_sector_stuff(save->sector_stuff) ==
          std::vector<std::uint8_t>(gm::kSbtSectorStuffSize, 0));
    const auto rebuilt = save->to_bytes(error);
    CHECK(rebuilt.has_value());
    if (rebuilt) {
        CHECK(*rebuilt == bytes);
    }

    // Clearing the typed vector falls back to SaveBlock::raw, byte for byte.
    save->sector_stuff.clear();
    const auto raw_path = save->to_bytes(error);
    CHECK(raw_path.has_value());
    if (raw_path) {
        CHECK(*raw_path == bytes);
    }
}

void check_negatives() {
    const std::vector<std::uint8_t> bytes = read_all(kGame / "SAVEGAME" / "saved0.sbt");
    CHECK(!bytes.empty());
    if (bytes.empty()) {
        return;
    }

    // Cut the file in the middle of the block-8 payload: the fixed read must
    // report a truncated "sector stuff", not guess a short block.
    {
        std::vector<std::uint8_t> cut(bytes.begin(),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(
                                                          kPayloadOffset + 0x100));
        std::string error;
        CHECK(!gm::parse_save(cut, error).has_value());
        CHECK(error.find("truncated sector stuff") != std::string::npos);
    }

    // A block-8 payload cut short by one byte (the file keeps the following
    // blocks, so only the stream realignment can catch it).
    {
        std::vector<std::uint8_t> short_one = bytes;
        short_one.erase(short_one.begin() +
                        static_cast<std::ptrdiff_t>(kPayloadOffset + gm::kSbtSectorStuffSize - 1));
        std::string error;
        const auto bad = gm::parse_save(short_one, error);
        CHECK(!bad.has_value());
    }

    // encode_sector_stuff() needs exactly 256 slots.
    {
        std::vector<gm::SbtSectorSlot> slots(255);
        CHECK(gm::encode_sector_stuff(slots).empty());
        slots.resize(257);
        CHECK(gm::encode_sector_stuff(slots).empty());
    }

    // A typed vector that is not 256 entries cannot be written into block 8.
    {
        std::string error;
        std::optional<gm::Save> save = gm::parse_save(bytes, error);
        CHECK(save.has_value());
        if (save) {
            save->sector_stuff.resize(4);
            CHECK(!save->to_bytes(error).has_value());
            CHECK(error.find("sector stuff") != std::string::npos);
        }
    }
}

} // namespace

int main() {
    if (!fs::exists(kGame / "SAVEGAME" / "saved0.sbt") ||
        !fs::exists(kGame / "SAVEGAME" / "saved1.sbt")) {
        std::fprintf(stderr, "save_sector_stuff: %s not installed, skipping\n",
                     kGame.c_str());
        return 0;
    }

    check_save("saved0.sbt");
    check_save("saved1.sbt");
    check_edits();
    check_zero_block();
    check_negatives();

    if (g_failures != 0) {
        std::fprintf(stderr, "save_sector_stuff: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("save_sector_stuff: ok (2/2 saved games, 13 slots, typed block 8)\n");
    return 0;
}
