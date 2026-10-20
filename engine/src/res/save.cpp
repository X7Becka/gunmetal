#include "res/save.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace gm {
namespace {

// Container limits.  Counts match tools/re/save_fields.py LIMITS; the file cap
// matches save_rebuild.py MAX_FILE.  They exist so a corrupt count cannot make
// the parser read (or reserve) an unbounded amount of data.
constexpr std::size_t kMaxSaveBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxBlocks = 32;
constexpr std::uint32_t kMaxSwitches = 0x200;
constexpr std::uint32_t kMaxDoors = 100;
constexpr std::uint32_t kMaxThingTypes = 0x80;
constexpr std::uint32_t kMaxNamedItems = 0x31;
constexpr std::uint32_t kMaxSprites = 0x1f3;
constexpr std::uint32_t kMaxSounds = 0x7f;
constexpr std::uint32_t kMaxPolyHandles = 0x1f3;
constexpr std::uint32_t kMaxObjectHandles = 0x12b;
constexpr std::uint32_t kMaxSectorHandles = 0xff;
constexpr std::uint32_t kMaxLifts = 0x1e;
constexpr std::uint32_t kMaxLiftStops = 0xc8;
constexpr std::uint32_t kMaxSobTable = 0xc8;
constexpr std::uint32_t kMaxScriptThings = 0xc8;
constexpr std::uint32_t kMaxScriptNamed = 0x400;
constexpr std::uint32_t kMaxScriptCode = 0x30000;

constexpr std::size_t kBlockStrideSwitch = 0x20;
constexpr std::size_t kBlockStrideDoor = 0xb8;
constexpr std::size_t kBlockStrideSprite = 0x2c;
constexpr std::size_t kBlockStrideSound = 28;
constexpr std::size_t kBlockStrideThingType = 936;
constexpr std::size_t kBlockStrideNamedItem = 52;
constexpr std::size_t kBlockStrideLift = 168;
constexpr std::size_t kBlockStrideLiftStop = 12;
constexpr std::size_t kBlockStrideSob = 20;
constexpr std::size_t kBlockStrideScriptThing = 40;
constexpr std::size_t kBlockStrideScriptHandle = 12;
constexpr std::size_t kThingSlots = 260;
constexpr std::size_t kThingRecord = 0x528;
constexpr std::size_t kItemSize = 0x7080;
constexpr std::size_t kSectorStuffSize = 0x5800;
constexpr std::size_t kSampleCountsSize = 300 * 4;
constexpr std::size_t kSampleNamesSize = 300 * 9;
constexpr std::size_t kScriptTablesSize = 9 * 0x400;
constexpr std::size_t kPlayerSlots = 8;
constexpr std::size_t kPlayerStruct = 0x3510;
constexpr std::size_t kPlayerAttachments = 16;
constexpr std::size_t kPlayerAttachmentAt = 0x317c;
constexpr std::size_t kPlayerAttachmentRecord = 16;
constexpr std::size_t kPlayerPairHeader = 0x50;
constexpr std::size_t kPlayerPairCount = 2;

// Field offsets inside the 0x3510 player struct (docs/SAVE_FIELDS.md §9.2).
constexpr std::size_t kPlayerPositionAt = 0x0000;
constexpr std::size_t kPlayerAnglesAt = 0x0018;
constexpr std::size_t kPlayerExistsAt = 0x3178;
constexpr std::size_t kPlayerHistoryAt = 0x3204;
constexpr std::size_t kPlayerPresentAt = 0x3210;
constexpr std::size_t kPlayerObjectAt = 0x3214;
constexpr std::size_t kPlayerArmorMaxAt = 0x3244;
constexpr std::size_t kPlayerArmorAt = 0x3248;
constexpr std::size_t kPlayerShieldMaxAt = 0x324c;
constexpr std::size_t kPlayerShieldsAt = 0x3250;
constexpr std::size_t kPlayerWeaponSlotAt = 0x3294;
constexpr std::size_t kPlayerWeaponMaskAt = 0x3298;
constexpr std::size_t kPlayerAmmoAt = 0x329c;
constexpr std::size_t kPlayerAmmoSlots = 12;

constexpr std::int32_t kMaxGauge = 1000000;

// Anchor scan of savefile.py: 256-byte samples every 2048 bytes, skipping
// mostly-zero chunks (they match everywhere) and requiring 3 hits.
constexpr std::size_t kAnchorLen = 256;
constexpr std::size_t kAnchorStep = 2048;
constexpr std::size_t kAnchorZeroLimit = 200;
constexpr std::size_t kAnchorMinHits = 3;
constexpr std::size_t kNoHit = static_cast<std::size_t>(-1);

std::string hex(std::size_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%zx", value);
    return std::string(buf);
}

std::string cstr(std::span<const std::uint8_t> bytes) {
    std::size_t n = 0;
    while (n < bytes.size() && bytes[n] != 0) {
        ++n;
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), n);
}

std::uint32_t le32(std::span<const std::uint8_t> data, std::size_t off) {
    // The struct fields are little-endian i32/u32/f64; off is always checked
    // by the caller (fixed 0x3510 struct).
    return static_cast<std::uint32_t>(data[off]) |
           (static_cast<std::uint32_t>(data[off + 1]) << 8) |
           (static_cast<std::uint32_t>(data[off + 2]) << 16) |
           (static_cast<std::uint32_t>(data[off + 3]) << 24);
}

std::int32_t le_i32(std::span<const std::uint8_t> data, std::size_t off) {
    return static_cast<std::int32_t>(le32(data, off));
}

std::uint64_t le64(std::span<const std::uint8_t> data, std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = 8; i-- > 0;) {
        v = (v << 8) | data[off + i];
    }
    return v;
}

double le_f64(std::span<const std::uint8_t> data, std::size_t off) {
    return std::bit_cast<double>(le64(data, off));
}

/// Bounds-checked cursor over the save bytes.  Every failure keeps the first
/// error message and leaves the caller with nullopt.
class Reader {
public:
    Reader(std::span<const std::uint8_t> data, std::size_t pos, std::string& error)
        : data_(data), pos_(pos), error_(error) {}

    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return data_.size() - pos_; }

    bool need(std::size_t n, const char* what) {
        if (n > remaining()) {
            fail("truncated " + std::string(what) + " at " + hex(pos_) + " (need " +
                 std::to_string(n) + ", have " + std::to_string(remaining()) + ")");
            return false;
        }
        return true;
    }

    bool u8(std::uint8_t& value, const char* what) {
        if (!need(1, what)) {
            return false;
        }
        value = data_[pos_++];
        return true;
    }

    bool u32(std::uint32_t& value, const char* what) {
        if (!need(4, what)) {
            return false;
        }
        value = le32(data_, pos_);
        pos_ += 4;
        return true;
    }

    bool count(std::uint32_t& value, std::uint32_t limit, const char* what) {
        if (!u32(value, what)) {
            return false;
        }
        if (value > limit) {
            fail(std::string(what) + ": count " + std::to_string(value) +
                 " out of range [0, " + std::to_string(limit) + "]");
            return false;
        }
        return true;
    }

    bool bytes(std::size_t n, std::span<const std::uint8_t>& out, const char* what) {
        if (!need(n, what)) {
            return false;
        }
        out = data_.subspan(pos_, n);
        pos_ += n;
        return true;
    }

    bool skip(std::size_t n, const char* what) {
        if (!need(n, what)) {
            return false;
        }
        pos_ += n;
        return true;
    }

    void fail(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_;
    std::string& error_;
};

void set_player_fields(SavePlayer& player, std::span<const std::uint8_t> st) {
    for (std::size_t i = 0; i < 3; ++i) {
        player.position[i] = le_f64(st, kPlayerPositionAt + 8 * i);
        player.angles[i] = le_f64(st, kPlayerAnglesAt + 8 * i);
    }
    player.history_index = le_i32(st, kPlayerHistoryAt);
    player.exists = le32(st, kPlayerExistsAt);
    player.status_present = le_i32(st, kPlayerPresentAt);
    player.object_id = le_i32(st, kPlayerObjectAt);
    player.armor_max = le_i32(st, kPlayerArmorMaxAt);
    player.armor = le_i32(st, kPlayerArmorAt);
    player.shield_max = le_i32(st, kPlayerShieldMaxAt);
    for (std::size_t i = 0; i < player.shields.size(); ++i) {
        player.shields[i] = le_i32(st, kPlayerShieldsAt + 4 * i);
    }
    player.weapon_slot = le_i32(st, kPlayerWeaponSlotAt);
    player.weapon_mask = le32(st, kPlayerWeaponMaskAt);
    for (std::size_t i = 0; i < player.ammo.size(); ++i) {
        player.ammo[i] = le32(st, kPlayerAmmoAt + 4 * i);
    }
}

/// Validate the gauges/weapon of one present slot, mirroring
/// save_fields.check_save.  Corruption becomes a parse error instead of a
/// silently wrong HUD.
bool check_player(int slot, const SavePlayer& p, std::string& error) {
    const std::string who = "player " + std::to_string(slot);
    if (!(p.armor_max > 0 && p.armor_max <= kMaxGauge)) {
        error = who + ": armor_max " + std::to_string(p.armor_max) + " out of range [1, " +
                std::to_string(kMaxGauge) + "]";
        return false;
    }
    if (p.armor < 0 || p.armor > p.armor_max) {
        error = who + ": armor " + std::to_string(p.armor) + " > max " +
                std::to_string(p.armor_max);
        return false;
    }
    if (!(p.shield_max > 0 && p.shield_max <= kMaxGauge)) {
        error = who + ": shield_max " + std::to_string(p.shield_max) + " out of range [1, " +
                std::to_string(kMaxGauge) + "]";
        return false;
    }
    for (std::size_t i = 0; i < p.shields.size(); ++i) {
        if (p.shields[i] < 0 || p.shields[i] > p.shield_max) {
            error = who + ": shield " + std::to_string(i) + " value " +
                    std::to_string(p.shields[i]) + " outside [0, " +
                    std::to_string(p.shield_max) + "]";
            return false;
        }
    }
    if (p.weapon_slot < 0 || p.weapon_slot >= static_cast<std::int32_t>(kPlayerAmmoSlots)) {
        error = who + ": weapon slot " + std::to_string(p.weapon_slot) + " outside 0..11";
        return false;
    }
    for (double v : p.position) {
        if (!std::isfinite(v)) {
            error = who + ": non-finite position";
            return false;
        }
    }
    for (double v : p.angles) {
        if (!std::isfinite(v)) {
            error = who + ": non-finite angle";
            return false;
        }
    }
    return true;
}

bool parse_player_block(Reader& r, Save& save, std::size_t offset, std::uint32_t& count,
                        std::string& error) {
    SavePlayer player;
    player.block_offset = offset;
    std::array<std::span<const std::uint8_t>, kPlayerSlots> slots{};
    std::uint32_t present = 0;

    for (std::size_t i = 0; i < kPlayerSlots; ++i) {
        std::uint8_t flag = 0;
        if (!r.u8(flag, "player presence flag")) {
            return false;
        }
        if (flag > 1) {
            r.fail("player " + std::to_string(i) + ": presence flag " +
                   std::to_string(flag) + " is not 0/1");
            return false;
        }
        player.present[i] = flag != 0;
        if (!flag) {
            continue;
        }
        ++present;
        player.slot_offsets[i] = r.pos();
        std::span<const std::uint8_t> st;
        if (!r.bytes(kPlayerStruct, st, "player struct")) {
            return false;
        }
        slots[i] = st;
        std::uint32_t attachments = 0;
        for (std::size_t k = 0; k < kPlayerAttachments; ++k) {
            if (le32(st, kPlayerAttachmentAt + 4 * k) == 0) {
                continue;
            }
            if (!r.skip(kPlayerAttachmentRecord, "player attachment")) {
                return false;
            }
            ++attachments;
        }
        player.attachment_counts[i] = attachments;
    }

    // Two trailing {0x50 header, 0x3510 struct} pairs.  Their fields are not
    // interpreted (savefile.py ignores them); the headers are kept because
    // they name the embedded level ("LEVEL0" in the shipped saves).
    for (std::size_t i = 0; i < kPlayerPairCount; ++i) {
        std::span<const std::uint8_t> header;
        if (!r.bytes(kPlayerPairHeader, header, "player sub header")) {
            return false;
        }
        player.pair_headers[i] = cstr(header);
        if (!r.skip(kPlayerStruct, "player sub struct")) {
            return false;
        }
    }

    for (std::size_t i = 0; i < kPlayerSlots; ++i) {
        if (!slots[i].empty()) {
            set_player_fields(player, slots[i]);
            player.has_slot = true;
            if (!check_player(static_cast<int>(i), player, error)) {
                return false;
            }
            break;
        }
    }

    count = present;
    save.player = player;
    return true;
}

bool parse_block(Reader& r, Save& save, std::uint8_t id, std::size_t offset,
                 std::uint32_t& count, std::string& error) {
    count = 0;
    switch (id) {
    case 1: { // switches
        if (!r.count(count, kMaxSwitches, "switches")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * kBlockStrideSwitch, "switch records");
    }
    case 2: { // doors
        if (!r.count(count, kMaxDoors, "doors")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * kBlockStrideDoor, "door records");
    }
    case 3: { // things: 260 x {u8 present; optional 0x528}
        std::uint32_t present = 0;
        for (std::size_t i = 0; i < kThingSlots; ++i) {
            std::uint8_t flag = 0;
            if (!r.u8(flag, "thing presence flag")) {
                return false;
            }
            if (flag > 1) {
                r.fail("things: slot " + std::to_string(i) + " presence flag " +
                       std::to_string(flag) + " is not 0/1");
                return false;
            }
            if (flag) {
                ++present;
                if (!r.skip(kThingRecord, "thing record")) {
                    return false;
                }
            }
        }
        count = present;
        return true;
    }
    case 4: // thing types
        if (!r.count(count, kMaxThingTypes, "thing types")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * kBlockStrideThingType,
                      "thing type records");
    case 5: // named items
        if (!r.count(count, kMaxNamedItems, "named items")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * kBlockStrideNamedItem,
                      "named item records");
    case 6: // sprites
        if (!r.count(count, kMaxSprites, "sprites")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * kBlockStrideSprite, "sprite records");
    case 7: // sound sources
        if (!r.count(count, kMaxSounds, "sound sources")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * kBlockStrideSound, "sound records");
    case 8: // SECTOR_STUFF, fixed 256 x 0x58
        return r.skip(kSectorStuffSize, "sector stuff");
    case 9: // player
        return parse_player_block(r, save, offset, count, error);
    case 11: { // SCRIPT runtime form: things, handles, code, tables
        std::uint32_t things = 0;
        if (!r.count(things, kMaxScriptThings, "script things")) {
            return false;
        }
        if (!r.skip(static_cast<std::size_t>(things) * kBlockStrideScriptThing,
                    "script things")) {
            return false;
        }
        std::uint32_t named = 0;
        if (!r.count(named, kMaxScriptNamed, "script named items")) {
            return false;
        }
        if (!r.skip(static_cast<std::size_t>(named) * kBlockStrideScriptHandle,
                    "script handles")) {
            return false;
        }
        std::uint32_t code = 0;
        if (!r.count(code, kMaxScriptCode, "script code")) {
            return false;
        }
        if (!r.skip(code, "script code")) {
            return false;
        }
        std::uint8_t flag = 0;
        if (!r.u8(flag, "script tables flag")) {
            return false;
        }
        if (flag > 1) {
            r.fail("script tables flag " + std::to_string(flag) + " is not 0/1");
            return false;
        }
        if (flag && !r.skip(kScriptTablesSize, "script tables")) {
            return false;
        }
        count = things;
        return true;
    }
    case 12: { // sound table: 300 counts + 300 x 9-byte names
        std::span<const std::uint8_t> counts;
        if (!r.bytes(kSampleCountsSize, counts, "sample counts")) {
            return false;
        }
        if (!r.skip(kSampleNamesSize, "sample names")) {
            return false;
        }
        std::uint32_t referenced = 0;
        for (std::size_t i = 0; i < 300; ++i) {
            if (le32(counts, 4 * i) != 0) {
                ++referenced;
            }
        }
        count = referenced;
        return true;
    }
    case 15: // poly handles, 4 columns
        if (!r.count(count, kMaxPolyHandles, "poly handles")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * 16, "poly handles");
    case 16: // object handles, 3 columns
        if (!r.count(count, kMaxObjectHandles, "object handles")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * 12, "object handles");
    case 17: // sector handles, 2 columns
        if (!r.count(count, kMaxSectorHandles, "sector handles")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * 8, "sector handles");
    case 18: { // lifts + stops
        std::uint32_t lifts = 0;
        if (!r.count(lifts, kMaxLifts, "lifts")) {
            return false;
        }
        if (!r.skip(static_cast<std::size_t>(lifts) * kBlockStrideLift, "lifts")) {
            return false;
        }
        std::uint32_t stops = 0;
        if (!r.count(stops, kMaxLiftStops, "lift stops")) {
            return false;
        }
        if (!r.skip(static_cast<std::size_t>(stops) * kBlockStrideLiftStop, "lift stops")) {
            return false;
        }
        count = lifts;
        return true;
    }
    case 19: // ITEM, fixed 300 x 0x60, fields are raw
        return r.skip(kItemSize, "item table");
    case 20: // SOB table
        if (!r.count(count, kMaxSobTable, "sob table")) {
            return false;
        }
        return r.skip(static_cast<std::size_t>(count) * kBlockStrideSob, "sob records");
    default:
        r.fail("unknown record type " + std::to_string(id) + " at " + hex(offset));
        return false;
    }
}

/// First occurrence of *anchor* in *data*, or ``kNoHit``.  memchr + memcmp
/// instead of std::search so the debug build stays fast (the test would
/// otherwise spend a minute scanning the 36 shipped level scripts).
std::size_t find_anchor(std::span<const std::uint8_t> data,
                        std::span<const std::uint8_t> anchor) {
    if (anchor.empty() || anchor.size() > data.size()) {
        return kNoHit;
    }
    const std::uint8_t first = anchor[0];
    const std::size_t last = data.size() - anchor.size();
    std::size_t at = 0;
    while (at <= last) {
        const void* hit = std::memchr(data.data() + at, first, last - at + 1);
        if (!hit) {
            break;
        }
        at = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - data.data());
        if (std::memcmp(data.data() + at, anchor.data(), anchor.size()) == 0) {
            return at;
        }
        ++at;
    }
    return kNoHit;
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
               std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxSaveBytes) {
        error = path.string() + ": bad or oversized file";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

} // namespace

std::optional<Save> parse_save(std::span<const std::uint8_t> bytes, std::string& error) {
    error.clear();
    if (bytes.size() > kMaxSaveBytes) {
        error = "save is too large: " + std::to_string(bytes.size()) + " bytes exceeds " +
                std::to_string(kMaxSaveBytes);
        return std::nullopt;
    }
    if (bytes.size() < kSaveHeaderSize) {
        error = "save is too short for a header: " + std::to_string(bytes.size()) + " < " +
                hex(kSaveHeaderSize);
        return std::nullopt;
    }

    Save save;
    save.size = bytes.size();

    const std::span<const std::uint8_t> raw = bytes;
    save.header.title = cstr(raw.first(12));
    if (save.header.title.empty()) {
        error = "empty save title at +0x00";
        return std::nullopt;
    }
    for (char c : save.header.title) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e) {
            error = "save title is not printable ASCII";
            return std::nullopt;
        }
    }
    save.header.version = le32(raw, 0x0c);
    save.header.reserved = le32(raw, 0x50);
    save.header.level_name =
        cstr(raw.subspan(kSaveLevelNameAt, kSaveLevelNameLen));
    if (save.header.level_name.empty()) {
        error = "empty level name at " + hex(kSaveLevelNameAt);
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kSaveFieldCount; ++i) {
        save.header.fields[i] = le32(raw, kSaveFieldsAt + 4 * i);
    }
    for (std::size_t i = 0; i < kSaveNameSlotCount; ++i) {
        save.header.name_slots[i] =
            cstr(raw.subspan(kSaveNameSlotAt + i * kSaveNameSlotLen, kSaveNameSlotLen));
    }

    Reader r(bytes, kSaveHeaderSize, error);
    std::array<bool, 256> seen{};
    while (true) {
        if (r.remaining() == 0) {
            error = "record stream has no 0xff terminator";
            return std::nullopt;
        }
        const std::size_t offset = r.pos();
        std::uint8_t id = 0;
        if (!r.u8(id, "record type")) {
            return std::nullopt;
        }
        if (id == kSaveBlockEof) {
            if (r.remaining() != 0) {
                error = std::to_string(r.remaining()) + " trailing bytes after 0xff";
                return std::nullopt;
            }
            break;
        }
        if (seen[id]) {
            error = "duplicate block " + std::to_string(id) + " at " + hex(offset);
            return std::nullopt;
        }
        if (save.blocks.size() >= kMaxBlocks) {
            error = "too many blocks (> " + std::to_string(kMaxBlocks) + ")";
            return std::nullopt;
        }

        SaveBlock block;
        block.id = id;
        block.offset = offset;
        if (!parse_block(r, save, id, offset, block.count, error)) {
            if (error.empty()) {
                error = "failed to parse block " + std::to_string(id);
            }
            return std::nullopt;
        }
        block.size = r.pos() - offset;
        seen[id] = true;
        save.blocks.push_back(block);
    }

    return save;
}

std::optional<SaveLevelMatch> find_save_level(std::span<const std::uint8_t> save,
                                              const std::filesystem::path& game,
                                              std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(game, ec)) {
        error = "game directory not found: " + game.string();
        return std::nullopt;
    }

    std::vector<std::filesystem::path> levels;
    std::filesystem::directory_iterator it(game, ec);
    if (ec) {
        error = "cannot read game directory: " + game.string();
        return std::nullopt;
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".sbt") {
            levels.push_back(entry.path());
        }
    }
    std::sort(levels.begin(), levels.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  return a.filename().string() < b.filename().string();
              });
    if (levels.empty()) {
        error = "no .sbt files in " + game.string();
        return std::nullopt;
    }

    std::optional<SaveLevelMatch> best;
    std::vector<std::uint8_t> level;
    std::string read_error;
    for (const std::filesystem::path& path : levels) {
        level.clear();
        if (!read_file(path, level, read_error)) {
            continue; // unreadable/oversized levels cannot be embedded scripts
        }
        const std::span<const std::uint8_t> level_span(level);
        std::size_t hits = 0;
        std::size_t first_hit = 0;
        std::size_t last_anchor = 0;
        // savefile.py: range(0, len-256, 2048) -- the last full anchor must
        // have 256 bytes after it.
        for (std::size_t off = 0; off + kAnchorLen < level_span.size(); off += kAnchorStep) {
            const auto anchor = level_span.subspan(off, kAnchorLen);
            const std::size_t zeros =
                static_cast<std::size_t>(std::count(anchor.begin(), anchor.end(), 0));
            if (zeros >= kAnchorZeroLimit) {
                continue;
            }
            const auto it2 = find_anchor(save, anchor);
            if (it2 == kNoHit) {
                continue;
            }
            const std::size_t at = it2;
            if (hits == 0) {
                first_hit = at;
            }
            last_anchor = std::max(last_anchor, at + kAnchorLen);
            ++hits;
        }
        if (hits >= kAnchorMinHits && (!best || hits > best->hits)) {
            best = SaveLevelMatch{path.filename().string(), hits, first_hit, last_anchor};
        }
    }

    if (!best) {
        error = "no installed .sbt matches the save (need >= " +
                std::to_string(kAnchorMinHits) + " anchors)";
        return std::nullopt;
    }
    return best;
}

} // namespace gm
