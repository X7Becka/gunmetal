#include "game/events.h"

#include <bit>
#include <limits>

namespace gm {
namespace {

void put_u8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void put_i16(std::vector<std::uint8_t>& out, std::int16_t value) {
    put_u16(out, static_cast<std::uint16_t>(value));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void put_f32(std::vector<std::uint8_t>& out, float value) {
    put_u32(out, std::bit_cast<std::uint32_t>(value));
}

std::uint8_t get_u8(std::span<const std::uint8_t> bytes, std::size_t at) {
    return bytes[at];
}

std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[at + 1]) << 8U);
}

std::int16_t get_i16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::int16_t>(get_u16(bytes, at));
}

std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24U);
}

float get_f32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return std::bit_cast<float>(get_u32(bytes, at));
}

std::size_t record_size(EventType type) {
    return type == EventType::generate_short ? EventQueue::kType1Size : EventQueue::kType3Size;
}

} // namespace

EventQueue::EventQueue(std::size_t overflow_check) : overflow_check_(overflow_check) {}

bool EventQueue::can_append(std::size_t size) const {
    // Original: `if (0x750e < count) overflow;` then write the record.
    if (bytes_.size() > overflow_check_) {
        return false;
    }
    return bytes_.size() + size <= overflow_check_ + kMaxRecordSize;
}

void EventQueue::append(const Event& event) {
    if (event.type == EventType::generate_short) {
        put_u8(bytes_, static_cast<std::uint8_t>(EventType::generate_short));
        put_u32(bytes_, static_cast<std::uint32_t>(event.id));
        put_u16(bytes_, event.param);
        put_i16(bytes_, event.a);
        put_i16(bytes_, event.b);
        put_f32(bytes_, event.x);
        put_f32(bytes_, event.y);
        put_f32(bytes_, event.z);
        put_u32(bytes_, event.extra);
    } else {
        put_u8(bytes_, static_cast<std::uint8_t>(EventType::generate));
        put_u32(bytes_, static_cast<std::uint32_t>(event.id));
        put_u16(bytes_, event.param);
        put_f32(bytes_, event.x);
        put_f32(bytes_, event.y);
        put_f32(bytes_, event.z);
        put_u32(bytes_, event.extra);
    }
}

bool EventQueue::post(const Event& event) {
    if (event.type != EventType::generate && event.type != EventType::generate_short) {
        return false;
    }
    const std::size_t size = record_size(event.type);
    if (!can_append(size)) {
        return false;
    }
    append(event);
    return true;
}

bool EventQueue::post_generate(std::int32_t id, std::uint16_t param, float x, float y, float z,
                               std::uint32_t extra) {
    Event event;
    event.type = EventType::generate;
    event.id = id;
    event.param = param;
    event.x = x;
    event.y = y;
    event.z = z;
    event.extra = extra;
    return post(event);
}

bool EventQueue::post_generate_short(std::int32_t id, std::uint16_t param, std::int16_t a,
                                     std::int16_t b, float x, float y, float z,
                                     std::uint32_t extra) {
    Event event;
    event.type = EventType::generate_short;
    event.id = id;
    event.param = param;
    event.a = a;
    event.b = b;
    event.x = x;
    event.y = y;
    event.z = z;
    event.extra = extra;
    return post(event);
}

EventParseResult EventQueue::drain() {
    EventParseResult result = parse(bytes_);
    if (result.ok) {
        bytes_.clear();
    }
    return result;
}

EventParseResult EventQueue::parse(std::span<const std::uint8_t> bytes) {
    EventParseResult result;
    std::size_t at = 0;
    while (at < bytes.size()) {
        const std::uint8_t raw_type = get_u8(bytes, at);
        if (raw_type == static_cast<std::uint8_t>(EventType::generate_short)) {
            if (bytes.size() - at < kType1Size) {
                result.ok = false;
                result.error = "truncated type 1 event at offset " + std::to_string(at);
                result.events.clear();
                return result;
            }
            Event event;
            event.type = EventType::generate_short;
            event.id = static_cast<std::int32_t>(get_u32(bytes, at + 1));
            event.param = get_u16(bytes, at + 5);
            event.a = get_i16(bytes, at + 7);
            event.b = get_i16(bytes, at + 9);
            event.x = get_f32(bytes, at + 0x0b);
            event.y = get_f32(bytes, at + 0x0f);
            event.z = get_f32(bytes, at + 0x13);
            event.extra = get_u32(bytes, at + 0x17);
            result.events.push_back(event);
            at += kType1Size;
        } else if (raw_type == static_cast<std::uint8_t>(EventType::generate)) {
            if (bytes.size() - at < kType3Size) {
                result.ok = false;
                result.error = "truncated type 3 event at offset " + std::to_string(at);
                result.events.clear();
                return result;
            }
            Event event;
            event.type = EventType::generate;
            event.id = static_cast<std::int32_t>(get_u32(bytes, at + 1));
            event.param = get_u16(bytes, at + 5);
            event.x = get_f32(bytes, at + 7);
            event.y = get_f32(bytes, at + 0x0b);
            event.z = get_f32(bytes, at + 0x0f);
            event.extra = get_u32(bytes, at + 0x13);
            result.events.push_back(event);
            at += kType3Size;
        } else {
            result.ok = false;
            result.error = "unknown event type " + std::to_string(raw_type) +
                           " at offset " + std::to_string(at);
            result.events.clear();
            return result;
        }
    }
    return result;
}

} // namespace gm
