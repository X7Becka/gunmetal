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

} // namespace

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
        case 8:
            if (!r.skip(0x5800)) {
                error = "bad SECTOR_STUFF record";
                return std::nullopt;
            }
            break;
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
            if (!r.u32(count) || !r.skip(static_cast<std::size_t>(count) * 40)) {
                error = "bad SCRIPT things";
                return std::nullopt;
            }
            std::uint32_t handles = 0;
            if (!r.u32(handles) || !r.skip(static_cast<std::size_t>(handles) * 12)) {
                error = "bad SCRIPT handles";
                return std::nullopt;
            }
            std::uint32_t code = 0;
            if (!r.u32(code) || !r.skip(code)) {
                error = "bad SCRIPT bytecode";
                return std::nullopt;
            }
            std::uint8_t flag = 0;
            if (!r.u8(flag) || (flag && !r.skip(9 * 0x400))) {
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
