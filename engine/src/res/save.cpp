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
constexpr std::uint32_t kMaxSwitches = kSaveMaxSwitches;
constexpr std::uint32_t kMaxDoors = kSaveMaxDoors;
constexpr std::uint32_t kMaxThingTypes = 0x80;
constexpr std::uint32_t kMaxNamedItems = 0x31;
constexpr std::uint32_t kMaxSprites = kSaveMaxSprites;
constexpr std::uint32_t kMaxSounds = kSaveMaxSounds;
constexpr std::uint32_t kMaxPolyHandles = 0x1f3;
constexpr std::uint32_t kMaxObjectHandles = 0x12b;
constexpr std::uint32_t kMaxSectorHandles = 0xff;
constexpr std::uint32_t kMaxLifts = 0x1e;
constexpr std::uint32_t kMaxLiftStops = 0xc8;
constexpr std::uint32_t kMaxSobTable = 0xc8;
constexpr std::uint32_t kMaxScriptThings = 0xc8;
constexpr std::uint32_t kMaxScriptNamed = 0x400;
constexpr std::uint32_t kMaxScriptCode = 0x30000;

constexpr std::size_t kBlockStrideSwitch = kSaveSwitchRecordSize;
constexpr std::size_t kBlockStrideDoor = kSaveDoorRecordSize;
constexpr std::size_t kBlockStrideSprite = kSaveSpriteRecordSize;
constexpr std::size_t kBlockStrideSound = kSaveSoundRecordSize;
constexpr std::size_t kBlockStrideThingType = 936;
constexpr std::size_t kBlockStrideNamedItem = 52;
constexpr std::size_t kBlockStrideLift = 168;
constexpr std::size_t kBlockStrideLiftStop = 12;
constexpr std::size_t kBlockStrideSob = 20;
constexpr std::size_t kBlockStrideScriptThing = 40;
constexpr std::size_t kBlockStrideScriptHandle = 12;
constexpr std::size_t kThingSlots = kSaveThingSlots;
constexpr std::size_t kThingRecord = kSaveThingRecordSize;
constexpr std::size_t kItemSize = 0x7080;
constexpr std::size_t kSectorStuffSize = 0x5800;
constexpr std::size_t kSampleCountsSize = 300 * 4;
constexpr std::size_t kSampleNamesSize = 300 * 9;
constexpr std::size_t kScriptTablesSize = 9 * 0x400;
constexpr std::size_t kPlayerSlots = kSavePlayerSlots;
constexpr std::size_t kPlayerStruct = kSavePlayerStructSize;
constexpr std::size_t kPlayerAttachments = kSavePlayerAttachmentSlots;
constexpr std::size_t kPlayerAttachmentAt = kSavePlayerAttachmentAt;
constexpr std::size_t kPlayerAttachmentRecord = kSavePlayerAttachmentRecord;
constexpr std::size_t kPlayerPairHeader = kSavePlayerPairHeaderSize;
constexpr std::size_t kPlayerPairCount = kSavePlayerPairCount;

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

// Field offsets inside the typed blocks of docs/SAVE_BLOCKS.md.
constexpr std::size_t kDoorDeltaAt = 0x48;
constexpr std::size_t kDoorConstantAt = 0x60;
constexpr std::size_t kDoorFlagsAt = 0x68;
constexpr std::size_t kDoorIdsAt = 0x6c;
constexpr std::size_t kDoorStateAt = 0x80;
constexpr std::size_t kDoorSectorAt = 0x84;
constexpr std::size_t kDoorObjectAt = 0x88;
constexpr std::size_t kDoorPhasePrevAt = 0x8c;
constexpr std::size_t kDoorPhaseNextAt = 0x90;
constexpr std::size_t kDoorStepAt = 0x94;
constexpr std::size_t kDoorParam5At = 0x98;
constexpr std::size_t kDoorParam6At = 0x9c;
constexpr std::size_t kDoorVAt = 0xa0;
constexpr std::size_t kDoorNAt = 0xac;

constexpr std::size_t kSpriteScaleXAt = 0x00;
constexpr std::size_t kSpriteScaleYAt = 0x04;
constexpr std::size_t kSpriteRuntimeAt = 0x08;
constexpr std::size_t kSpritePosAt = 0x0c;
constexpr std::size_t kSpriteParamAt = 0x18;
constexpr std::size_t kSpriteReserved0At = 0x19;
constexpr std::size_t kSpriteFlagsAt = 0x1c;
constexpr std::size_t kSpriteEffectAt = 0x20;
constexpr std::size_t kSpriteParam16At = 0x24;
constexpr std::size_t kSpriteReserved1At = 0x26;
constexpr std::size_t kSpriteSpawnAt = 0x28;


constexpr std::int32_t kMaxGauge = 1000000;

bool known_block_id(std::uint8_t id) {
    switch (id) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
    case 11: case 12: case 15: case 16: case 17: case 18: case 19: case 20:
        return true;
    default:
        return false;
    }
}

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

std::uint16_t le16(std::span<const std::uint8_t> data, std::size_t off) {
    return static_cast<std::uint16_t>(data[off]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8);
}

float le_f32(std::span<const std::uint8_t> data, std::size_t off) {
    return std::bit_cast<float>(le32(data, off));
}

void put32(std::vector<std::uint8_t>& data, std::size_t off, std::uint32_t value) {
    data[off] = static_cast<std::uint8_t>(value & 0xff);
    data[off + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    data[off + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    data[off + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

void put_i32(std::vector<std::uint8_t>& data, std::size_t off, std::int32_t value) {
    put32(data, off, static_cast<std::uint32_t>(value));
}

void put_f64(std::vector<std::uint8_t>& data, std::size_t off, double value) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < 8; ++i) {
        data[off + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff);
    }
}

void put16(std::vector<std::uint8_t>& data, std::size_t off, std::uint16_t value) {
    data[off] = static_cast<std::uint8_t>(value & 0xff);
    data[off + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

void put_f32(std::vector<std::uint8_t>& data, std::size_t off, float value) {
    put32(data, off, std::bit_cast<std::uint32_t>(value));
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

SaveSwitch decode_switch(std::span<const std::uint8_t> rec) {
    SaveSwitch s;
    s.kind = le_i32(rec, 0x00);
    s.id = le_i32(rec, 0x04);
    s.radius = le_i32(rec, 0x08);
    for (std::size_t i = 0; i < 3; ++i) {
        s.pos[i] = le_i32(rec, 0x0c + 4 * i);
    }
    s.aux = le_i32(rec, 0x18);
    s.sound = le_i32(rec, 0x1c);
    return s;
}

SaveDoor decode_door(std::span<const std::uint8_t> rec) {
    SaveDoor d;
    for (std::size_t i = 0; i < 9; ++i) {
        d.box[i] = le_f64(rec, 8 * i);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        d.delta[i] = le_f64(rec, kDoorDeltaAt + 8 * i);
    }
    d.constant = le_f64(rec, kDoorConstantAt);
    d.flags = le_i32(rec, kDoorFlagsAt);
    for (std::size_t i = 0; i < 5; ++i) {
        d.ids[i] = le_i32(rec, kDoorIdsAt + 4 * i);
    }
    d.state = le_i32(rec, kDoorStateAt);
    d.sector = le_i32(rec, kDoorSectorAt);
    d.object = le_i32(rec, kDoorObjectAt);
    d.phase_prev = le32(rec, kDoorPhasePrevAt);
    d.phase_next = le32(rec, kDoorPhaseNextAt);
    d.step = le_i32(rec, kDoorStepAt);
    d.param5 = le_i32(rec, kDoorParam5At);
    d.param6 = le_i32(rec, kDoorParam6At);
    for (std::size_t i = 0; i < 3; ++i) {
        d.v[i] = le_i32(rec, kDoorVAt + 4 * i);
        d.n[i] = le_i32(rec, kDoorNAt + 4 * i);
    }
    return d;
}

SaveSprite decode_sprite(std::span<const std::uint8_t> rec) {
    SaveSprite s;
    s.scale_x = le_f32(rec, kSpriteScaleXAt);
    s.scale_y = le_f32(rec, kSpriteScaleYAt);
    s.runtime_object = le32(rec, kSpriteRuntimeAt);
    for (std::size_t i = 0; i < 3; ++i) {
        s.pos[i] = le_i32(rec, kSpritePosAt + 4 * i);
    }
    s.param = rec[kSpriteParamAt];
    std::copy_n(rec.begin() + static_cast<std::ptrdiff_t>(kSpriteReserved0At),
                s.reserved0.size(), s.reserved0.begin());
    s.flags = le32(rec, kSpriteFlagsAt);
    s.effect_id = le_i32(rec, kSpriteEffectAt);
    s.param16 = le16(rec, kSpriteParam16At);
    std::copy_n(rec.begin() + static_cast<std::ptrdiff_t>(kSpriteReserved1At),
                s.reserved1.size(), s.reserved1.begin());
    s.spawn_time = le32(rec, kSpriteSpawnAt);
    return s;
}

void decode_sounds(std::span<const std::uint8_t> body, std::uint32_t count,
                   std::vector<SaveSound>& out) {
    // Block 7 is column-major: five i32 columns (x, y, z, volume, sample)
    // followed by one f64 pitch column (tools/re/save_fields.py parse_sounds).
    const std::size_t n = count;
    out.clear();
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        SaveSound s;
        s.x = le_i32(body, 0x00 + 4 * i);
        s.y = le_i32(body, 4 * n + 4 * i);
        s.z = le_i32(body, 8 * n + 4 * i);
        s.volume = le_i32(body, 12 * n + 4 * i);
        s.sample = le_i32(body, 16 * n + 4 * i);
        s.pitch = le_f64(body, 20 * n + 8 * i);
        out.push_back(s);
    }
}

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

/// Patch the typed fields of the first present slot into *st* (a 0x3510
/// buffer).  Everything else -- including attachment ids at +0x317c -- is
/// left untouched, so the structure size and unknown bytes are preserved.
void write_player_fields(std::vector<std::uint8_t>& st, const SavePlayer& player) {
    for (std::size_t i = 0; i < 3; ++i) {
        put_f64(st, kPlayerPositionAt + 8 * i, player.position[i]);
        put_f64(st, kPlayerAnglesAt + 8 * i, player.angles[i]);
    }
    put_i32(st, kPlayerHistoryAt, player.history_index);
    put32(st, kPlayerExistsAt, player.exists);
    put_i32(st, kPlayerPresentAt, player.status_present);
    put_i32(st, kPlayerObjectAt, player.object_id);
    put_i32(st, kPlayerArmorMaxAt, player.armor_max);
    put_i32(st, kPlayerArmorAt, player.armor);
    put_i32(st, kPlayerShieldMaxAt, player.shield_max);
    for (std::size_t i = 0; i < player.shields.size(); ++i) {
        put_i32(st, kPlayerShieldsAt + 4 * i, player.shields[i]);
    }
    put_i32(st, kPlayerWeaponSlotAt, player.weapon_slot);
    put32(st, kPlayerWeaponMaskAt, player.weapon_mask);
    for (std::size_t i = 0; i < player.ammo.size(); ++i) {
        put32(st, kPlayerAmmoAt + 4 * i, player.ammo[i]);
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

bool copy_string(std::vector<std::uint8_t>& out, std::size_t off, std::size_t len,
                 const std::string& value, const char* what, std::string& error) {
    if (value.size() > len) {
        error = std::string(what) + " is " + std::to_string(value.size()) +
                " bytes, maximum is " + std::to_string(len);
        return false;
    }
    std::copy(value.begin(), value.end(), out.begin() + static_cast<std::ptrdiff_t>(off));
    return true;
}

bool encode_header(const SaveHeader& header, std::vector<std::uint8_t>& out,
                   std::string& error) {
    out.assign(kSaveHeaderSize, 0);
    if (header.title.empty()) {
        error = "empty save title at +0x00";
        return false;
    }
    for (char c : header.title) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e) {
            error = "save title is not printable ASCII";
            return false;
        }
    }
    if (!copy_string(out, 0, 12, header.title, "save title", error)) {
        return false;
    }
    put32(out, 0x0c, header.version);
    put32(out, 0x50, header.reserved);
    if (header.level_name.empty()) {
        error = "empty level name at " + hex(kSaveLevelNameAt);
        return false;
    }
    if (!copy_string(out, kSaveLevelNameAt, kSaveLevelNameLen, header.level_name,
                     "level name", error)) {
        return false;
    }
    for (std::size_t i = 0; i < kSaveFieldCount; ++i) {
        put32(out, kSaveFieldsAt + 4 * i, header.fields[i]);
    }
    for (std::size_t i = 0; i < kSaveNameSlotCount; ++i) {
        if (!copy_string(out, kSaveNameSlotAt + i * kSaveNameSlotLen,
                         kSaveNameSlotLen, header.name_slots[i], "name slot", error)) {
            return false;
        }
    }
    return true;
}

bool encode_player(const SavePlayer& player, std::vector<std::uint8_t>& out,
                   std::string& error) {
    out.clear();
    std::size_t typed_slot = kPlayerSlots;
    for (std::size_t i = 0; i < kPlayerSlots; ++i) {
        if (player.present[i]) {
            typed_slot = i;
            break;
        }
    }
    if (typed_slot != kPlayerSlots && !player.has_slot) {
        error = "player has a present slot but no typed fields (has_slot is false)";
        return false;
    }
    if (player.has_slot && typed_slot == kPlayerSlots) {
        error = "player has typed fields but no present slot";
        return false;
    }

    out.reserve(1 + kPlayerSlots * (1 + kPlayerStruct));
    for (std::size_t i = 0; i < kPlayerSlots; ++i) {
        out.push_back(player.present[i] ? 1 : 0);
        if (!player.present[i]) {
            continue;
        }
        std::vector<std::uint8_t> st;
        if (player.slot_raw[i].empty()) {
            st.assign(kPlayerStruct, 0);
        } else {
            if (player.slot_raw[i].size() != kPlayerStruct) {
                error = "player " + std::to_string(i) + ": struct is " +
                        std::to_string(player.slot_raw[i].size()) + " bytes, expected " +
                        std::to_string(kPlayerStruct);
                return false;
            }
            st = player.slot_raw[i];
        }
        if (i == typed_slot) {
            if (!check_player(static_cast<int>(i), player, error)) {
                return false;
            }
            write_player_fields(st, player);
        }
        out.insert(out.end(), st.begin(), st.end());

        std::uint32_t ids = 0;
        for (std::size_t k = 0; k < kPlayerAttachments; ++k) {
            if (le32(st, kPlayerAttachmentAt + 4 * k) != 0) {
                ++ids;
            }
        }
        const std::vector<std::uint8_t>& attachments = player.attachment_raw[i];
        const std::size_t expected =
            static_cast<std::size_t>(ids) * kPlayerAttachmentRecord;
        if (attachments.size() != expected) {
            error = "player " + std::to_string(i) + ": " +
                    std::to_string(attachments.size()) + " attachment bytes for " +
                    std::to_string(ids) + " non-zero ids (expected " +
                    std::to_string(expected) + ")";
            return false;
        }
        out.insert(out.end(), attachments.begin(), attachments.end());
    }

    for (std::size_t i = 0; i < kPlayerPairCount; ++i) {
        std::string raw_name = cstr(player.pair_header_raw[i]);
        if (player.pair_headers[i] != raw_name) {
            std::vector<std::uint8_t> header(kPlayerPairHeader, 0);
            if (!copy_string(header, 0, kPlayerPairHeader, player.pair_headers[i],
                             "player sub header", error)) {
                return false;
            }
            out.insert(out.end(), header.begin(), header.end());
        } else {
            out.insert(out.end(), player.pair_header_raw[i].begin(),
                       player.pair_header_raw[i].end());
        }
        out.insert(out.end(), player.pair_struct_raw[i].begin(),
                   player.pair_struct_raw[i].end());
    }
    return true;
}

void append_switch(std::vector<std::uint8_t>& out, const SaveSwitch& s) {
    const std::size_t base = out.size();
    out.resize(base + kBlockStrideSwitch);
    put_i32(out, base + 0x00, s.kind);
    put_i32(out, base + 0x04, s.id);
    put_i32(out, base + 0x08, s.radius);
    for (std::size_t i = 0; i < 3; ++i) {
        put_i32(out, base + 0x0c + 4 * i, s.pos[i]);
    }
    put_i32(out, base + 0x18, s.aux);
    put_i32(out, base + 0x1c, s.sound);
}

void append_door(std::vector<std::uint8_t>& out, const SaveDoor& d) {
    const std::size_t base = out.size();
    out.resize(base + kBlockStrideDoor);
    for (std::size_t i = 0; i < 9; ++i) {
        put_f64(out, base + 8 * i, d.box[i]);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        put_f64(out, base + kDoorDeltaAt + 8 * i, d.delta[i]);
    }
    put_f64(out, base + kDoorConstantAt, d.constant);
    put_i32(out, base + kDoorFlagsAt, d.flags);
    for (std::size_t i = 0; i < 5; ++i) {
        put_i32(out, base + kDoorIdsAt + 4 * i, d.ids[i]);
    }
    put_i32(out, base + kDoorStateAt, d.state);
    put_i32(out, base + kDoorSectorAt, d.sector);
    put_i32(out, base + kDoorObjectAt, d.object);
    put32(out, base + kDoorPhasePrevAt, d.phase_prev);
    put32(out, base + kDoorPhaseNextAt, d.phase_next);
    put_i32(out, base + kDoorStepAt, d.step);
    put_i32(out, base + kDoorParam5At, d.param5);
    put_i32(out, base + kDoorParam6At, d.param6);
    for (std::size_t i = 0; i < 3; ++i) {
        put_i32(out, base + kDoorVAt + 4 * i, d.v[i]);
        put_i32(out, base + kDoorNAt + 4 * i, d.n[i]);
    }
}

void append_sprite(std::vector<std::uint8_t>& out, const SaveSprite& s) {
    const std::size_t base = out.size();
    out.resize(base + kBlockStrideSprite);
    put_f32(out, base + kSpriteScaleXAt, s.scale_x);
    put_f32(out, base + kSpriteScaleYAt, s.scale_y);
    put32(out, base + kSpriteRuntimeAt, s.runtime_object);
    for (std::size_t i = 0; i < 3; ++i) {
        put_i32(out, base + kSpritePosAt + 4 * i, s.pos[i]);
    }
    out[base + kSpriteParamAt] = s.param;
    std::copy(s.reserved0.begin(), s.reserved0.end(),
              out.begin() + static_cast<std::ptrdiff_t>(base + kSpriteReserved0At));
    put32(out, base + kSpriteFlagsAt, s.flags);
    put_i32(out, base + kSpriteEffectAt, s.effect_id);
    put16(out, base + kSpriteParam16At, s.param16);
    std::copy(s.reserved1.begin(), s.reserved1.end(),
              out.begin() + static_cast<std::ptrdiff_t>(base + kSpriteReserved1At));
    put32(out, base + kSpriteSpawnAt, s.spawn_time);
}


std::string count_error(const char* what, std::size_t count, std::size_t limit) {
    return std::string(what) + ": count " + std::to_string(count) +
           " out of range [0, " + std::to_string(limit) + "]";
}

bool encode_switches(const std::vector<SaveSwitch>& records, std::vector<std::uint8_t>& out,
                     std::string& error) {
    if (records.size() > kMaxSwitches) {
        error = count_error("switches", records.size(), kMaxSwitches);
        return false;
    }
    out.clear();
    out.resize(4);
    put32(out, 0, static_cast<std::uint32_t>(records.size()));
    out.reserve(4 + records.size() * kBlockStrideSwitch);
    for (const SaveSwitch& s : records) {
        append_switch(out, s);
    }
    return true;
}

bool encode_doors(const std::vector<SaveDoor>& records, std::vector<std::uint8_t>& out,
                  std::string& error) {
    if (records.size() > kMaxDoors) {
        error = count_error("doors", records.size(), kMaxDoors);
        return false;
    }
    out.clear();
    out.resize(4);
    put32(out, 0, static_cast<std::uint32_t>(records.size()));
    out.reserve(4 + records.size() * kBlockStrideDoor);
    for (const SaveDoor& d : records) {
        append_door(out, d);
    }
    return true;
}

bool encode_sprites(const std::vector<SaveSprite>& records, std::vector<std::uint8_t>& out,
                    std::string& error) {
    if (records.size() > kMaxSprites) {
        error = count_error("sprites", records.size(), kMaxSprites);
        return false;
    }
    out.clear();
    out.resize(4);
    put32(out, 0, static_cast<std::uint32_t>(records.size()));
    out.reserve(4 + records.size() * kBlockStrideSprite);
    for (const SaveSprite& s : records) {
        append_sprite(out, s);
    }
    return true;
}

bool encode_sounds(const std::vector<SaveSound>& records, std::vector<std::uint8_t>& out,
                   std::string& error) {
    if (records.size() > kMaxSounds) {
        error = count_error("sound sources", records.size(), kMaxSounds);
        return false;
    }
    out.clear();
    out.resize(4);
    put32(out, 0, static_cast<std::uint32_t>(records.size()));
    out.reserve(4 + records.size() * kBlockStrideSound);
    // Column-major, matching parse_sounds(): x, y, z, volume, sample, pitch.
    for (int column = 0; column < 5; ++column) {
        for (const SaveSound& s : records) {
            std::int32_t value = 0;
            switch (column) {
            case 0: value = s.x; break;
            case 1: value = s.y; break;
            case 2: value = s.z; break;
            case 3: value = s.volume; break;
            default: value = s.sample; break;
            }
            const std::size_t at = out.size();
            out.resize(at + 4);
            put_i32(out, at, value);
        }
    }
    for (const SaveSound& s : records) {
        const std::size_t at = out.size();
        out.resize(at + 8);
        put_f64(out, at, s.pitch);
    }
    return true;
}

bool encode_things(const std::array<bool, kThingSlots>& present,
                   const std::vector<SaveThing>& records, std::vector<std::uint8_t>& out,
                   std::string& error) {
    const std::size_t expected =
        static_cast<std::size_t>(std::count(present.begin(), present.end(), true));
    if (records.size() != expected) {
        error = "things: " + std::to_string(records.size()) + " records for " +
                std::to_string(expected) + " presence flags";
        return false;
    }
    out.clear();
    out.reserve(kThingSlots + records.size() * kThingRecord);
    std::size_t next = 0;
    for (std::size_t i = 0; i < kThingSlots; ++i) {
        out.push_back(present[i] ? 1 : 0);
        if (!present[i]) {
            continue;
        }
        if (records[next].slot != i) {
            error = "things: record " + std::to_string(next) + " is for slot " +
                    std::to_string(records[next].slot) + ", expected slot " +
                    std::to_string(i);
            return false;
        }
        const auto& raw = records[next].raw;
        out.insert(out.end(), raw.begin(), raw.end());
        ++next;
    }
    return true;
}

bool same_switch(const SaveSwitch& a, const SaveSwitch& b) {
    return a.kind == b.kind && a.id == b.id && a.radius == b.radius && a.pos == b.pos &&
           a.aux == b.aux && a.sound == b.sound;
}

bool same_double(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool same_float(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool same_door(const SaveDoor& a, const SaveDoor& b) {
    for (std::size_t i = 0; i < 9; ++i) {
        if (!same_double(a.box[i], b.box[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (!same_double(a.delta[i], b.delta[i])) {
            return false;
        }
    }
    return same_double(a.constant, b.constant) && a.flags == b.flags && a.ids == b.ids &&
           a.state == b.state && a.sector == b.sector && a.object == b.object &&
           a.phase_prev == b.phase_prev && a.phase_next == b.phase_next &&
           a.step == b.step && a.param5 == b.param5 && a.param6 == b.param6 &&
           a.v == b.v && a.n == b.n;
}

bool same_sprite(const SaveSprite& a, const SaveSprite& b) {
    return same_float(a.scale_x, b.scale_x) && same_float(a.scale_y, b.scale_y) &&
           a.runtime_object == b.runtime_object && a.pos == b.pos && a.param == b.param &&
           a.reserved0 == b.reserved0 && a.flags == b.flags &&
           a.effect_id == b.effect_id && a.param16 == b.param16 &&
           a.reserved1 == b.reserved1 && a.spawn_time == b.spawn_time;
}

bool same_sound(const SaveSound& a, const SaveSound& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.volume == b.volume &&
           a.sample == b.sample && same_double(a.pitch, b.pitch);
}

bool same_things(const Save& a, const Save& b) {
    if (a.thing_present != b.thing_present || a.things.size() != b.things.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.things.size(); ++i) {
        if (a.things[i].slot != b.things[i].slot || a.things[i].raw != b.things[i].raw) {
            return false;
        }
    }
    return true;
}

template <typename T, typename Eq>
bool same_vector(const std::vector<T>& a, const std::vector<T>& b, Eq eq) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!eq(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

bool same_typed_blocks(const Save& a, const Save& b) {
    return same_vector(a.switches, b.switches, same_switch) &&
           same_vector(a.doors, b.doors, same_door) && same_things(a, b) &&
           same_vector(a.sprites, b.sprites, same_sprite) &&
           same_vector(a.sounds, b.sounds, same_sound);
}

bool same_player(const SavePlayer& a, const SavePlayer& b) {
    if (a.has_slot != b.has_slot || a.present != b.present ||
        a.attachment_counts != b.attachment_counts || a.pair_headers != b.pair_headers) {
        return false;
    }
    if (!a.has_slot) {
        return true;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::bit_cast<std::uint64_t>(a.position[i]) !=
                std::bit_cast<std::uint64_t>(b.position[i]) ||
            std::bit_cast<std::uint64_t>(a.angles[i]) !=
                std::bit_cast<std::uint64_t>(b.angles[i])) {
            return false;
        }
    }
    return a.history_index == b.history_index && a.exists == b.exists &&
           a.status_present == b.status_present && a.object_id == b.object_id &&
           a.armor_max == b.armor_max && a.armor == b.armor &&
           a.shield_max == b.shield_max && a.shields == b.shields &&
           a.weapon_slot == b.weapon_slot && a.weapon_mask == b.weapon_mask &&
           a.ammo == b.ammo;
}

bool same_header(const SaveHeader& a, const SaveHeader& b) {
    return a.title == b.title && a.version == b.version && a.reserved == b.reserved &&
           a.level_name == b.level_name && a.fields == b.fields &&
           a.name_slots == b.name_slots;
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
        player.slot_raw[i].assign(st.begin(), st.end());
        std::uint32_t attachments = 0;
        for (std::size_t k = 0; k < kPlayerAttachments; ++k) {
            if (le32(st, kPlayerAttachmentAt + 4 * k) == 0) {
                continue;
            }
            std::span<const std::uint8_t> record;
            if (!r.bytes(kPlayerAttachmentRecord, record, "player attachment")) {
                return false;
            }
            player.attachment_raw[i].insert(player.attachment_raw[i].end(),
                                            record.begin(), record.end());
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
        std::copy(header.begin(), header.end(), player.pair_header_raw[i].begin());
        player.pair_headers[i] = cstr(header);
        std::span<const std::uint8_t> sub;
        if (!r.bytes(kPlayerStruct, sub, "player sub struct")) {
            return false;
        }
        std::copy(sub.begin(), sub.end(), player.pair_struct_raw[i].begin());
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
        save.switches.clear();
        save.switches.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::span<const std::uint8_t> rec;
            if (!r.bytes(kBlockStrideSwitch, rec, "switch record")) {
                return false;
            }
            save.switches.push_back(decode_switch(rec));
        }
        return true;
    }
    case 2: { // doors
        if (!r.count(count, kMaxDoors, "doors")) {
            return false;
        }
        save.doors.clear();
        save.doors.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::span<const std::uint8_t> rec;
            if (!r.bytes(kBlockStrideDoor, rec, "door record")) {
                return false;
            }
            save.doors.push_back(decode_door(rec));
        }
        return true;
    }
    case 3: { // things: 260 x {u8 present; optional 0x528}
        std::uint32_t present = 0;
        save.thing_present.fill(false);
        save.things.clear();
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
            save.thing_present[i] = flag != 0;
            if (!flag) {
                continue;
            }
            ++present;
            std::span<const std::uint8_t> rec;
            if (!r.bytes(kThingRecord, rec, "thing record")) {
                return false;
            }
            SaveThing thing;
            thing.slot = i;
            std::copy(rec.begin(), rec.end(), thing.raw.begin());
            save.things.push_back(thing);
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
    case 6: { // sprites
        if (!r.count(count, kMaxSprites, "sprites")) {
            return false;
        }
        save.sprites.clear();
        save.sprites.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::span<const std::uint8_t> rec;
            if (!r.bytes(kBlockStrideSprite, rec, "sprite record")) {
                return false;
            }
            save.sprites.push_back(decode_sprite(rec));
        }
        return true;
    }
    case 7: { // sound sources, column-major
        if (!r.count(count, kMaxSounds, "sound sources")) {
            return false;
        }
        std::span<const std::uint8_t> body;
        if (!r.bytes(static_cast<std::size_t>(count) * kBlockStrideSound, body,
                     "sound records")) {
            return false;
        }
        decode_sounds(body, count, save.sounds);
        return true;
    }
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
        const auto payload = bytes.subspan(offset + 1, block.size - 1);
        block.raw.assign(payload.begin(), payload.end());
        seen[id] = true;
        save.blocks.push_back(std::move(block));
    }

    return save;
}

std::optional<std::vector<std::uint8_t>> Save::to_bytes(std::string& error) const {
    error.clear();
    std::vector<std::uint8_t> out;
    if (!encode_header(header, out, error)) {
        return std::nullopt;
    }
    if (blocks.size() > kMaxBlocks) {
        error = "too many blocks (> " + std::to_string(kMaxBlocks) + ")";
        return std::nullopt;
    }

    std::array<bool, 256> seen{};
    bool player_written = false;
    for (const SaveBlock& block : blocks) {
        if (block.id == kSaveBlockEof) {
            error = "block id 0xff is reserved for the terminator";
            return std::nullopt;
        }
        if (seen[block.id]) {
            error = "duplicate block " + std::to_string(block.id);
            return std::nullopt;
        }
        if (!known_block_id(block.id)) {
            error = "unknown block id " + std::to_string(block.id);
            return std::nullopt;
        }
        seen[block.id] = true;

        out.push_back(block.id);
        std::vector<std::uint8_t> payload;
        bool have_payload = false;
        if (block.id == 9 && player) {
            if (!encode_player(*player, payload, error)) {
                return std::nullopt;
            }
            have_payload = true;
            player_written = true;
        } else {
            switch (block.id) {
            case 1:
                if (!switches.empty()) {
                    if (!encode_switches(switches, payload, error)) {
                        return std::nullopt;
                    }
                    have_payload = true;
                }
                break;
            case 2:
                if (!doors.empty()) {
                    if (!encode_doors(doors, payload, error)) {
                        return std::nullopt;
                    }
                    have_payload = true;
                }
                break;
            case 3: {
                const bool any_flag =
                    std::any_of(thing_present.begin(), thing_present.end(),
                                [](bool v) { return v; });
                if (any_flag || !things.empty()) {
                    if (!encode_things(thing_present, things, payload, error)) {
                        return std::nullopt;
                    }
                    have_payload = true;
                }
                break;
            }
            case 6:
                if (!sprites.empty()) {
                    if (!encode_sprites(sprites, payload, error)) {
                        return std::nullopt;
                    }
                    have_payload = true;
                }
                break;
            case 7:
                if (!sounds.empty()) {
                    if (!encode_sounds(sounds, payload, error)) {
                        return std::nullopt;
                    }
                    have_payload = true;
                }
                break;
            default:
                break;
            }
        }
        if (have_payload) {
            out.insert(out.end(), payload.begin(), payload.end());
        } else {
            if (block.raw.empty()) {
                error = "block " + std::to_string(block.id) + " has no raw payload";
                return std::nullopt;
            }
            out.insert(out.end(), block.raw.begin(), block.raw.end());
        }
        if (out.size() > kMaxSaveBytes) {
            error = "rebuilt save is too large (> " + std::to_string(kMaxSaveBytes) +
                    " bytes)";
            return std::nullopt;
        }
    }
    if (player && !player_written) {
        error = "player state is set but the block chain has no block 9";
        return std::nullopt;
    }
    out.push_back(kSaveBlockEof);
    if (out.size() > kMaxSaveBytes) {
        error = "rebuilt save is too large (> " + std::to_string(kMaxSaveBytes) +
                " bytes)";
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> rebuild_save(Save& save, bool verify,
                                                      std::string& error) {
    error.clear();
    std::optional<std::vector<std::uint8_t>> bytes = save.to_bytes(error);
    if (!bytes) {
        return std::nullopt;
    }

    std::string parse_error;
    std::optional<Save> parsed = parse_save(*bytes, parse_error);
    if (!parsed) {
        error = "rebuilt save does not re-parse: " + parse_error;
        return std::nullopt;
    }

    if (verify) {
        std::string again_error;
        std::optional<std::vector<std::uint8_t>> again = parsed->to_bytes(again_error);
        if (!again) {
            error = "rebuilt save is not serialisable: " + again_error;
            return std::nullopt;
        }
        if (*again != *bytes) {
            error = "rebuilt save is not byte-stable";
            return std::nullopt;
        }
        if (!same_header(save.header, parsed->header)) {
            error = "header fields did not survive the rebuild";
            return std::nullopt;
        }
        if (save.player.has_value() != parsed->player.has_value()) {
            error = "player block presence changed during the rebuild";
            return std::nullopt;
        }
        if (save.player && !same_player(*save.player, *parsed->player)) {
            error = "player fields did not survive the rebuild";
            return std::nullopt;
        }
        if (!same_typed_blocks(save, *parsed)) {
            error = "typed block fields did not survive the rebuild";
            return std::nullopt;
        }
        if (save.blocks.size() != parsed->blocks.size()) {
            error = "block count changed during the rebuild";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < save.blocks.size(); ++i) {
            if (save.blocks[i].id != parsed->blocks[i].id) {
                error = "block order changed during the rebuild";
                return std::nullopt;
            }
        }
    }

    save = std::move(*parsed);
    return bytes;
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
