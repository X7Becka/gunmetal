#include "res/sbt.h"

#include "core/bytes.h"

#include <cstring>
#include <string_view>

namespace gm {
namespace {

constexpr const char kMagic[] = "BINARY SAVE";
constexpr std::size_t kHeaderSize = 84;

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t offset = 0;
    std::string* error = nullptr;

    bool need(std::size_t n) {
        if (offset + n > data.size()) {
            if (error) {
                *error = "truncated .sbt at 0x" + std::to_string(offset);
            }
            return false;
        }
        return true;
    }

    bool u8(std::uint8_t& out) {
        if (!need(1)) {
            return false;
        }
        out = data[offset++];
        return true;
    }

    bool u32(std::uint32_t& out) {
        if (!need(4)) {
            return false;
        }
        out = rd32(data.data() + offset);
        offset += 4;
        return true;
    }

    bool skip(std::size_t n) {
        if (!need(n)) {
            return false;
        }
        offset += n;
        return true;
    }

    bool bytes(std::size_t n, std::vector<std::uint8_t>& out) {
        if (!need(n)) {
            return false;
        }
        out.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                   data.begin() + static_cast<std::ptrdiff_t>(offset + n));
        offset += n;
        return true;
    }
};

bool read_n(Reader& r, std::uint32_t limit, std::uint32_t& n) {
    if (!r.u32(n)) {
        return false;
    }
    if (n > limit) {
        if (r.error) {
            *r.error = "record count " + std::to_string(n) + " exceeds " + std::to_string(limit);
        }
        return false;
    }
    return true;
}

std::string fixed_name(const std::uint8_t* p, std::size_t width) {
    std::size_t len = 0;
    while (len < width && p[len] != 0) {
        ++len;
    }
    return std::string(reinterpret_cast<const char*>(p), len);
}

double rdf64(const std::uint8_t* p) {
    const std::uint64_t bits = rd64(p);
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

void wr32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

void wrf64(std::uint8_t* p, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    wr32(p, static_cast<std::uint32_t>(bits));
    wr32(p + 4, static_cast<std::uint32_t>(bits >> 32));
}

std::int32_t rdi32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(rd32(p));
}

std::string size_error(const char* what, std::size_t size, std::size_t record) {
    return std::string(what) + " body is " + std::to_string(size) +
           " bytes, not a multiple of " + std::to_string(record);
}

} // namespace

bool SbtSectorSlot::empty() const {
    for (const std::uint8_t b : raw) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

bool parse_sector_stuff(std::span<const std::uint8_t> payload,
                        std::vector<SbtSectorSlot>& out, std::string& error) {
    if (payload.size() != kSbtSectorStuffSize) {
        error = "SECTOR_STUFF payload is " + std::to_string(payload.size()) +
                " bytes, expected " + std::to_string(kSbtSectorStuffSize);
        return false;
    }
    std::vector<SbtSectorSlot> slots(kSbtSectorSlots);
    for (std::size_t i = 0; i < kSbtSectorSlots; ++i) {
        const std::uint8_t* p = payload.data() + i * kSbtSectorSlotSize;
        SbtSectorSlot& s = slots[i];
        std::memcpy(s.raw.data(), p, kSbtSectorSlotSize);
        for (std::size_t k = 0; k < 3; ++k) {
            s.move[k] = rdf64(p + 0x00 + k * 8);
            s.extra[k] = rdf64(p + 0x18 + k * 8);
        }
        s.sound_param = rdf64(p + 0x30);
        s.event_a = static_cast<std::int32_t>(rd32(p + 0x38));
        s.event_b = static_cast<std::int32_t>(rd32(p + 0x3c));
        s.damage_a = rd32(p + 0x40);
        s.damage_b = static_cast<std::int32_t>(rd32(p + 0x44));
        s.sound = static_cast<std::int32_t>(rd32(p + 0x48));
        s.sound_count = static_cast<std::int32_t>(rd32(p + 0x4c));
        s.flags = rd32(p + 0x50);
        s.unused = rd32(p + 0x54);
    }
    out = std::move(slots);
    return true;
}

std::vector<std::uint8_t> encode_sector_stuff(const std::vector<SbtSectorSlot>& slots) {
    if (slots.size() != kSbtSectorSlots) {
        return {};
    }
    std::vector<std::uint8_t> out(kSbtSectorStuffSize, 0);
    for (std::size_t i = 0; i < kSbtSectorSlots; ++i) {
        std::uint8_t* p = out.data() + i * kSbtSectorSlotSize;
        const SbtSectorSlot& s = slots[i];
        for (std::size_t k = 0; k < 3; ++k) {
            wrf64(p + 0x00 + k * 8, s.move[k]);
            wrf64(p + 0x18 + k * 8, s.extra[k]);
        }
        wrf64(p + 0x30, s.sound_param);
        wr32(p + 0x38, static_cast<std::uint32_t>(s.event_a));
        wr32(p + 0x3c, static_cast<std::uint32_t>(s.event_b));
        wr32(p + 0x40, s.damage_a);
        wr32(p + 0x44, static_cast<std::uint32_t>(s.damage_b));
        wr32(p + 0x48, static_cast<std::uint32_t>(s.sound));
        wr32(p + 0x4c, static_cast<std::uint32_t>(s.sound_count));
        wr32(p + 0x50, s.flags);
        wr32(p + 0x54, s.unused);
    }
    return out;
}

// --- records 1 / 2 / 18: typed view + writer --------------------------------

bool parse_switch_block(std::span<const std::uint8_t> body,
                        std::vector<SbtSwitchRecord>& out, std::string& error) {
    if (body.size() % kSbtSwitchRecordSize != 0) {
        error = size_error("SWITCH", body.size(), kSbtSwitchRecordSize);
        return false;
    }
    const std::size_t count = body.size() / kSbtSwitchRecordSize;
    if (count > kSbtMaxSwitches) {
        error = "SWITCH body holds " + std::to_string(count) + " records, limit " +
                std::to_string(kSbtMaxSwitches);
        return false;
    }
    std::vector<SbtSwitchRecord> records(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = body.data() + i * kSbtSwitchRecordSize;
        SbtSwitchRecord& s = records[i];
        s.kind = rdi32(p + 0x00);
        s.target = rdi32(p + 0x04);
        s.radius = rdi32(p + 0x08);
        for (std::size_t k = 0; k < 3; ++k) {
            s.pos[k] = rdi32(p + 0x0c + k * 4);
        }
        s.aux = rdi32(p + 0x18);
        s.sound = rdi32(p + 0x1c);
    }
    out = std::move(records);
    return true;
}

std::vector<std::uint8_t> encode_switch_block(const std::vector<SbtSwitchRecord>& records) {
    if (records.size() > kSbtMaxSwitches) {
        return {};
    }
    std::vector<std::uint8_t> out(records.size() * kSbtSwitchRecordSize, 0);
    for (std::size_t i = 0; i < records.size(); ++i) {
        std::uint8_t* p = out.data() + i * kSbtSwitchRecordSize;
        const SbtSwitchRecord& s = records[i];
        wr32(p + 0x00, static_cast<std::uint32_t>(s.kind));
        wr32(p + 0x04, static_cast<std::uint32_t>(s.target));
        wr32(p + 0x08, static_cast<std::uint32_t>(s.radius));
        for (std::size_t k = 0; k < 3; ++k) {
            wr32(p + 0x0c + k * 4, static_cast<std::uint32_t>(s.pos[k]));
        }
        wr32(p + 0x18, static_cast<std::uint32_t>(s.aux));
        wr32(p + 0x1c, static_cast<std::uint32_t>(s.sound));
    }
    return out;
}

bool parse_door_block(std::span<const std::uint8_t> body, std::vector<SbtDoorRecord>& out,
                      std::string& error) {
    if (body.size() % kSbtDoorRecordSize != 0) {
        error = size_error("DOOR", body.size(), kSbtDoorRecordSize);
        return false;
    }
    const std::size_t count = body.size() / kSbtDoorRecordSize;
    if (count > kSbtMaxDoors) {
        error = "DOOR body holds " + std::to_string(count) + " records, limit " +
                std::to_string(kSbtMaxDoors);
        return false;
    }
    std::vector<SbtDoorRecord> records(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = body.data() + i * kSbtDoorRecordSize;
        SbtDoorRecord& d = records[i];
        for (std::size_t k = 0; k < 3; ++k) {
            d.closed[k] = rdf64(p + 0x00 + k * 8);
            d.open[k] = rdf64(p + 0x18 + k * 8);
            d.size[k] = rdf64(p + 0x30 + k * 8);
            d.axis[k] = rdf64(p + 0x48 + k * 8);
        }
        d.sound_level = rdf64(p + 0x60);
        d.flags = rdi32(p + 0x68);
        for (std::size_t k = 0; k < 5; ++k) {
            d.ids[k] = rdi32(p + 0x6c + k * 4);
        }
        d.state = rdi32(p + 0x80);
        d.sector = rdi32(p + 0x84);
        d.object = rdi32(p + 0x88);
        d.phase_prev = rd32(p + 0x8c);
        d.phase_next = rd32(p + 0x90);
        d.step = rdi32(p + 0x94);
        d.dwell = rdi32(p + 0x98);
        d.timer = rdi32(p + 0x9c);
        for (std::size_t k = 0; k < 3; ++k) {
            d.volume[k] = rdi32(p + 0xa0 + k * 4);
            d.sample[k] = rdi32(p + 0xac + k * 4);
        }
    }
    out = std::move(records);
    return true;
}

std::vector<std::uint8_t> encode_door_block(const std::vector<SbtDoorRecord>& records) {
    if (records.size() > kSbtMaxDoors) {
        return {};
    }
    std::vector<std::uint8_t> out(records.size() * kSbtDoorRecordSize, 0);
    for (std::size_t i = 0; i < records.size(); ++i) {
        std::uint8_t* p = out.data() + i * kSbtDoorRecordSize;
        const SbtDoorRecord& d = records[i];
        for (std::size_t k = 0; k < 3; ++k) {
            wrf64(p + 0x00 + k * 8, d.closed[k]);
            wrf64(p + 0x18 + k * 8, d.open[k]);
            wrf64(p + 0x30 + k * 8, d.size[k]);
            wrf64(p + 0x48 + k * 8, d.axis[k]);
            wr32(p + 0xa0 + k * 4, static_cast<std::uint32_t>(d.volume[k]));
            wr32(p + 0xac + k * 4, static_cast<std::uint32_t>(d.sample[k]));
        }
        wrf64(p + 0x60, d.sound_level);
        wr32(p + 0x68, static_cast<std::uint32_t>(d.flags));
        for (std::size_t k = 0; k < 5; ++k) {
            wr32(p + 0x6c + k * 4, static_cast<std::uint32_t>(d.ids[k]));
        }
        wr32(p + 0x80, static_cast<std::uint32_t>(d.state));
        wr32(p + 0x84, static_cast<std::uint32_t>(d.sector));
        wr32(p + 0x88, static_cast<std::uint32_t>(d.object));
        wr32(p + 0x8c, d.phase_prev);
        wr32(p + 0x90, d.phase_next);
        wr32(p + 0x94, static_cast<std::uint32_t>(d.step));
        wr32(p + 0x98, static_cast<std::uint32_t>(d.dwell));
        wr32(p + 0x9c, static_cast<std::uint32_t>(d.timer));
    }
    return out;
}

bool parse_lift_block(std::span<const std::uint8_t> payload, SbtLiftBlock& out,
                      std::string& error) {
    Reader r{payload, 0, &error};
    std::uint32_t count = 0;
    if (!read_n(r, kSbtMaxLifts, count)) {
        return false;
    }
    std::vector<SbtLiftRecord> lifts(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!r.need(kSbtLiftRecordSize)) {
            error = "truncated LIFT record " + std::to_string(i);
            return false;
        }
        const std::uint8_t* p = payload.data() + r.offset;
        r.offset += kSbtLiftRecordSize;
        SbtLiftRecord& l = lifts[i];
        for (std::size_t k = 0; k < 6; ++k) {
            l.basis[k] = rdf64(p + k * 8);
        }
        l.handle = rd32(p + 0x30);
        l.kind = rdi32(p + 0x34);
        l.sob = rdi32(p + 0x38);
        for (std::size_t k = 0; k < 3; ++k) {
            l.pos[k] = rdi32(p + 0x3c + k * 4);
            l.pos_delta[k] = rdi32(p + 0x48 + k * 4);
            l.call_ids[k] = rdi32(p + 0x70 + k * 4);
        }
        l.speed = rdi32(p + 0x54);
        l.dwell = rdi32(p + 0x58);
        l.height = rdi32(p + 0x5c);
        l.height_tick = rdi32(p + 0x60);
        l.wait = rdi32(p + 0x64);
        l.direction = rdi32(p + 0x68);
        l.stop_index = rdi32(p + 0x6c);
        l.call_event = rdi32(p + 0x7c);
        l.call_radius = rdi32(p + 0x80);
        l.volume_a = rdi32(p + 0x84);
        l.volume_b = rdi32(p + 0x88);
        l.unknown_8c = rdi32(p + 0x8c);
        l.sample_call = rdi32(p + 0x90);
        l.sample_move = rdi32(p + 0x94);
        l.sample_arrive = rdi32(p + 0x98);
        l.stop_count = rdi32(p + 0x9c);
        l.stop_start = rdi32(p + 0xa0);
        l.unknown_a4 = rdi32(p + 0xa4);
    }
    std::uint32_t stops = 0;
    if (!read_n(r, kSbtMaxLiftStops, stops) ||
        !r.skip(static_cast<std::size_t>(stops) * 3 * 4)) {
        return false;
    }
    const std::uint8_t* columns = payload.data() + r.offset - static_cast<std::size_t>(stops) * 12;
    std::vector<SbtLiftStop> stop_list(stops);
    for (std::size_t i = 0; i < stops; ++i) {
        stop_list[i].floor_id = rdi32(columns + i * 4);
        stop_list[i].height = rdi32(columns + stops * 4 + i * 4);
        stop_list[i].sound = rdi32(columns + stops * 8 + i * 4);
    }
    if (r.offset != payload.size()) {
        error = "LIFT payload has " + std::to_string(payload.size() - r.offset) +
                " trailing bytes";
        return false;
    }
    out.lifts = std::move(lifts);
    out.stops = std::move(stop_list);
    return true;
}

std::vector<std::uint8_t> encode_lift_block(const SbtLiftBlock& block) {
    if (block.lifts.size() > kSbtMaxLifts || block.stops.size() > kSbtMaxLiftStops) {
        return {};
    }
    const std::size_t stop_count = block.stops.size();
    for (const SbtLiftRecord& l : block.lifts) {
        if (l.stop_count < 0 || l.stop_start < 0 ||
            static_cast<std::size_t>(l.stop_count) + static_cast<std::size_t>(l.stop_start) >
                stop_count) {
            return {};
        }
    }
    std::vector<std::uint8_t> out(4 + block.lifts.size() * kSbtLiftRecordSize + 4 +
                                  stop_count * 3 * 4,
                                  0);
    wr32(out.data(), static_cast<std::uint32_t>(block.lifts.size()));
    std::uint8_t* records = out.data() + 4;
    for (std::size_t i = 0; i < block.lifts.size(); ++i) {
        std::uint8_t* p = records + i * kSbtLiftRecordSize;
        const SbtLiftRecord& l = block.lifts[i];
        for (std::size_t k = 0; k < 6; ++k) {
            wrf64(p + k * 8, l.basis[k]);
        }
        wr32(p + 0x30, l.handle);
        wr32(p + 0x34, static_cast<std::uint32_t>(l.kind));
        wr32(p + 0x38, static_cast<std::uint32_t>(l.sob));
        for (std::size_t k = 0; k < 3; ++k) {
            wr32(p + 0x3c + k * 4, static_cast<std::uint32_t>(l.pos[k]));
            wr32(p + 0x48 + k * 4, static_cast<std::uint32_t>(l.pos_delta[k]));
            wr32(p + 0x70 + k * 4, static_cast<std::uint32_t>(l.call_ids[k]));
        }
        wr32(p + 0x54, static_cast<std::uint32_t>(l.speed));
        wr32(p + 0x58, static_cast<std::uint32_t>(l.dwell));
        wr32(p + 0x5c, static_cast<std::uint32_t>(l.height));
        wr32(p + 0x60, static_cast<std::uint32_t>(l.height_tick));
        wr32(p + 0x64, static_cast<std::uint32_t>(l.wait));
        wr32(p + 0x68, static_cast<std::uint32_t>(l.direction));
        wr32(p + 0x6c, static_cast<std::uint32_t>(l.stop_index));
        wr32(p + 0x7c, static_cast<std::uint32_t>(l.call_event));
        wr32(p + 0x80, static_cast<std::uint32_t>(l.call_radius));
        wr32(p + 0x84, static_cast<std::uint32_t>(l.volume_a));
        wr32(p + 0x88, static_cast<std::uint32_t>(l.volume_b));
        wr32(p + 0x8c, static_cast<std::uint32_t>(l.unknown_8c));
        wr32(p + 0x90, static_cast<std::uint32_t>(l.sample_call));
        wr32(p + 0x94, static_cast<std::uint32_t>(l.sample_move));
        wr32(p + 0x98, static_cast<std::uint32_t>(l.sample_arrive));
        wr32(p + 0x9c, static_cast<std::uint32_t>(l.stop_count));
        wr32(p + 0xa0, static_cast<std::uint32_t>(l.stop_start));
        wr32(p + 0xa4, static_cast<std::uint32_t>(l.unknown_a4));
    }
    std::uint8_t* counts = records + block.lifts.size() * kSbtLiftRecordSize;
    wr32(counts, static_cast<std::uint32_t>(stop_count));
    std::uint8_t* columns = counts + 4;
    for (std::size_t i = 0; i < stop_count; ++i) {
        wr32(columns + i * 4, static_cast<std::uint32_t>(block.stops[i].floor_id));
        wr32(columns + stop_count * 4 + i * 4,
             static_cast<std::uint32_t>(block.stops[i].height));
        wr32(columns + stop_count * 8 + i * 4,
             static_cast<std::uint32_t>(block.stops[i].sound));
    }
    return out;
}

std::vector<std::uint8_t> encode_counted(const std::vector<std::uint8_t>& body,
                                         std::uint32_t count) {
    std::vector<std::uint8_t> out(4 + body.size(), 0);
    wr32(out.data(), count);
    std::memcpy(out.data() + 4, body.data(), body.size());
    return out;
}

const char* sbt_type_name(std::uint8_t type) {
    switch (type) {
    case 1: return "SWITCH";
    case 2: return "DOOR";
    case 3: return "THING";
    case 4: return "THING_TYPES";
    case 5: return "NAMED_ITEMS";
    case 6: return "SPRITES";
    case 7: return "SOUND_SOURCES";
    case 8: return "SECTOR_STUFF";
    case 9: return "PLAYER_STATUS";
    case 11: return "SCRIPT";
    case 12: return "SOUND_SAMPLE_TABLE";
    case 15: return "POLY_HANDLE";
    case 16: return "OBJECT_HANDLE";
    case 17: return "SECTOR_HANDLE";
    case 18: return "LIFT";
    case 19: return "ITEM";
    case 20: return "SOB_TABLE";
    case 255: return "END_OF_FILE";
    default: return "UNKNOWN";
    }
}

std::optional<SbtLevel> parse_sbt(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < kHeaderSize) {
        error = "too short for an .sbt header";
        return std::nullopt;
    }
    if (std::memcmp(data.data(), kMagic, sizeof(kMagic) - 1) != 0) {
        error = "bad .sbt magic";
        return std::nullopt;
    }
    SbtLevel level;
    level.version = rd32(data.data() + 0x50);
    if (level.version != 0) {
        error = "unsupported .sbt version " + std::to_string(level.version);
        return std::nullopt;
    }

    Reader r{data, kHeaderSize, &error};
    bool terminated = false;
    while (r.offset < data.size()) {
        const std::size_t record_start = r.offset;
        std::uint8_t type = 0;
        if (!r.u8(type)) {
            return std::nullopt;
        }
        SbtRecord record;
        record.type = type;
        record.name = sbt_type_name(type);
        record.offset = record_start;
        const std::size_t payload_start = r.offset;

        if (type == 255) {
            record.size = 1;
            level.records.push_back(std::move(record));
            terminated = true;
            break;
        }

        std::uint32_t n = 0;
        switch (type) {
        case 1:
            if (!read_n(r, 0x200, n) || !r.skip(static_cast<std::size_t>(n) * 32)) {
                error = "bad SWITCH record";
                return std::nullopt;
            }
            break;
        case 2:
            if (!read_n(r, 100, n) || !r.skip(static_cast<std::size_t>(n) * 184)) {
                error = "bad DOOR record";
                return std::nullopt;
            }
            break;
        case 3: {
            std::uint8_t exists = 0;
            if (!r.u8(exists) || (exists && !r.skip(0x528))) {
                error = "bad THING record";
                return std::nullopt;
            }
            break;
        }
        case 4:
            if (!read_n(r, 0x80, n) || !r.skip(static_cast<std::size_t>(n) * 936)) {
                error = "bad THING_TYPES record";
                return std::nullopt;
            }
            break;
        case 5:
            if (!read_n(r, 0x31, n) || !r.skip(static_cast<std::size_t>(n) * 52)) {
                error = "bad NAMED_ITEMS record";
                return std::nullopt;
            }
            break;
        case 6:
            if (!read_n(r, 0x1f3, n) || !r.skip(static_cast<std::size_t>(n) * 44)) {
                error = "bad SPRITES record";
                return std::nullopt;
            }
            break;
        case 7:
            if (!read_n(r, 0x7f, n) || !r.skip(static_cast<std::size_t>(n) * 20 + static_cast<std::size_t>(n) * 8)) {
                error = "bad SOUND_SOURCES record";
                return std::nullopt;
            }
            break;
        case 8: {
            if (!r.need(kSbtSectorStuffSize)) {
                error = "bad SECTOR_STUFF record: " + error;
                return std::nullopt;
            }
            std::vector<SbtSectorSlot> slots;
            if (!parse_sector_stuff(r.data.subspan(r.offset, kSbtSectorStuffSize), slots, error)) {
                return std::nullopt;
            }
            r.offset += kSbtSectorStuffSize;
            level.sector_stuff = std::move(slots);
            break;
        }
        case 9: {
            std::uint8_t exists = 0;
            if (!r.u8(exists) || (exists && !r.skip(0x3510))) {
                error = "bad PLAYER_STATUS record";
                return std::nullopt;
            }
            break;
        }
        case 11: {
            // SCRIPT: things, handles, bytecode, optional 9*0x400 tables.
            std::uint32_t count = 0;
            if (!r.u32(count) ||
                !r.bytes(static_cast<std::size_t>(count) * 40, level.script_things)) {
                error = "bad SCRIPT things";
                return std::nullopt;
            }
            std::uint32_t handles = 0;
            if (!r.u32(handles) ||
                !r.bytes(static_cast<std::size_t>(handles) * 12, level.script_handles_data)) {
                error = "bad SCRIPT handles";
                return std::nullopt;
            }
            std::uint32_t code = 0;
            if (!r.u32(code) || !r.bytes(code, level.script_code)) {
                error = "bad SCRIPT bytecode";
                return std::nullopt;
            }
            std::uint8_t flag = 0;
            if (!r.u8(flag) || (flag && !r.bytes(9 * 0x400, level.script_tables))) {
                error = "bad SCRIPT tables";
                return std::nullopt;
            }
            level.script.things = count;
            level.script.handles = handles;
            level.script.code_size = code;
            level.script.flag = flag;
            break;
        }
        case 12:
            if (!r.skip(0x4b0 + 0xa8c)) {
                error = "bad SOUND_SAMPLE_TABLE record";
                return std::nullopt;
            }
            break;
        case 15:
        case 16:
        case 17: {
            const std::uint32_t limit = type == 15 ? 0x1f4 : (type == 16 ? 0x12c : 0x100);
            if (!read_n(r, limit, n) ||
                !r.skip(static_cast<std::size_t>(n) * (type == 15 ? 16u : (type == 16 ? 12u : 8u)))) {
                error = std::string("bad ") + sbt_type_name(type) + " record";
                return std::nullopt;
            }
            break;
        }
        case 18: {
            if (!read_n(r, 0x1e, n) || !r.skip(static_cast<std::size_t>(n) * 168)) {
                error = "bad LIFT record";
                return std::nullopt;
            }
            std::uint32_t k = 0;
            if (!read_n(r, 0xc8, k) || !r.skip(static_cast<std::size_t>(k) * 12)) {
                error = "bad LIFT stops";
                return std::nullopt;
            }
            break;
        }
        case 19:
            if (!r.skip(0x7080)) {
                error = "bad ITEM record";
                return std::nullopt;
            }
            break;
        case 20:
            if (!read_n(r, 0xc8, n) || !r.skip(static_cast<std::size_t>(n) * 20)) {
                error = "bad SOB_TABLE record";
                return std::nullopt;
            }
            break;
        default:
            error = "unsupported .sbt record type " + std::to_string(type);
            return std::nullopt;
        }

        record.size = r.offset - record_start;
        record.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(payload_start),
                              data.begin() + static_cast<std::ptrdiff_t>(r.offset));

        // Typed views of the records we already need.
        const std::uint8_t* p = record.payload.data();
        if ((type == 15 || type == 16 || type == 17) && record.payload.size() >= 4) {
            const std::uint32_t count = rd32(p);
            const std::uint32_t columns = type == 15 ? 4u : (type == 16 ? 3u : 2u);
            auto& out = type == 15 ? level.poly_handles
                                   : (type == 16 ? level.object_handles : level.sector_handles);
            out.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                SbtHandle h;
                h.tag = rd32(p + 4 + static_cast<std::size_t>(i) * 4);
                h.a = rd32(p + 4 + static_cast<std::size_t>(count + i) * 4);
                if (columns >= 3) {
                    h.b = rd32(p + 4 + static_cast<std::size_t>(count * 2 + i) * 4);
                }
                if (columns >= 4) {
                    h.c = rd32(p + 4 + static_cast<std::size_t>(count * 3 + i) * 4);
                }
                out.push_back(h);
            }
        } else if (type == 20 && record.payload.size() >= 4) {
            const std::uint32_t count = rd32(p);
            if (4 + static_cast<std::size_t>(count) * 20 <= record.payload.size()) {
                level.sobs.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const std::uint8_t* e = p + 4 + static_cast<std::size_t>(i) * 20;
                    SbtSob sob;
                    sob.name = fixed_name(e, 12);
                    sob.amount = rd32(e + 12);
                    sob.ptr = rd32(e + 16);
                    level.sobs.push_back(std::move(sob));
                }
            }
        }

        level.records.push_back(std::move(record));
    }

    if (!terminated) {
        error = "record stream has no END_OF_FILE terminator";
        return std::nullopt;
    }
    if (r.offset != data.size()) {
        error = std::to_string(data.size() - r.offset) + " trailing bytes after END_OF_FILE";
        return std::nullopt;
    }
    level.file_end = r.offset;
    return level;
}

} // namespace gm
