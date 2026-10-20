#include "game/events.h"

#include <bit>
#include <cmath>
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

void put_i32(std::vector<std::uint8_t>& out, std::int32_t value) {
    put_u32(out, static_cast<std::uint32_t>(value));
}

std::int32_t trunc_i32(double value) {
    if (!std::isfinite(value)) {
        return 0;
    }
    if (value <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(value); // truncation toward zero
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

std::int32_t get_i32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::int32_t>(get_u32(bytes, at));
}

} // namespace

EventQueue::EventQueue(std::size_t overflow_check) : overflow_check_(overflow_check) {}

bool EventQueue::can_append(std::size_t size, std::size_t overflow) const {
    // Original writers check `threshold < count` before appending; the shared
    // buffer itself is exactly 0x7530 bytes.
    if (bytes_.size() > overflow) {
        return false;
    }
    return bytes_.size() + size <= kBufferSize;
}

std::size_t EventQueue::threshold_for(EventType type) const {
    if (overflow_check_ != 0) {
        return overflow_check_;
    }
    if (type == EventType::trigger_switch) {
        return kOverflowSwitch;
    }
    if (is_text_type(static_cast<std::uint8_t>(type))) {
        return kOverflowText;
    }
    return kOverflowGenerate;
}

void EventQueue::append(const Event& event) {
    if (event.type == EventType::generate_short) {
        put_u8(bytes_, static_cast<std::uint8_t>(EventType::generate_short));
        put_u32(bytes_, static_cast<std::uint32_t>(event.id));
        put_u16(bytes_, event.param);
        put_i16(bytes_, event.a);
        put_i16(bytes_, event.b);
        put_i32(bytes_, event.x);
        put_i32(bytes_, event.y);
        put_i32(bytes_, event.z);
        put_u32(bytes_, event.extra);
    } else if (event.type == EventType::trigger_switch) {
        put_u8(bytes_, static_cast<std::uint8_t>(EventType::trigger_switch));
        put_u32(bytes_, event.switch_index);
        put_i16(bytes_, event.source);
    } else if (is_text_type(static_cast<std::uint8_t>(event.type))) {
        put_u8(bytes_, static_cast<std::uint8_t>(event.type));
        put_u8(bytes_, event.font);
        for (char ch : event.text) {
            put_u8(bytes_, static_cast<std::uint8_t>(ch));
        }
        put_u8(bytes_, 0);
        // The writer advances by len+4 but does not write this byte.
        put_u8(bytes_, 0);
    } else {
        put_u8(bytes_, static_cast<std::uint8_t>(EventType::generate));
        put_u32(bytes_, static_cast<std::uint32_t>(event.id));
        put_u16(bytes_, event.param);
        put_i32(bytes_, event.x);
        put_i32(bytes_, event.y);
        put_i32(bytes_, event.z);
        put_u32(bytes_, event.extra);
    }
}

bool EventQueue::post(const Event& event) {
    const auto raw_type = static_cast<std::uint8_t>(event.type);
    std::size_t size = 0;
    if (event.type == EventType::generate_short) {
        size = kType1Size;
    } else if (event.type == EventType::trigger_switch) {
        size = kType2Size;
    } else if (is_text_type(raw_type)) {
        size = event.text.size() + 4;
    } else if (event.type == EventType::generate) {
        size = kType3Size;
    } else {
        return false;
    }
    if (!can_append(size, threshold_for(event.type))) {
        return false;
    }
    append(event);
    return true;
}

bool EventQueue::post_generate(std::int32_t id, std::uint16_t param, double x, double y, double z,
                               std::uint32_t extra) {
    Event event;
    event.type = EventType::generate;
    event.id = id;
    event.param = param;
    event.x = trunc_i32(x);
    event.y = trunc_i32(y);
    event.z = trunc_i32(z);
    event.extra = extra;
    return post(event);
}

bool EventQueue::post_generate_short(std::int32_t id, std::uint16_t param, std::int16_t a,
                                     std::int16_t b, double x, double y, double z,
                                     std::uint32_t extra) {
    Event event;
    event.type = EventType::generate_short;
    event.id = id;
    event.param = param;
    event.a = a;
    event.b = b;
    event.x = trunc_i32(x);
    event.y = trunc_i32(y);
    event.z = trunc_i32(z);
    event.extra = extra;
    return post(event);
}

bool EventQueue::post_switch(std::uint32_t switch_index, std::int16_t source) {
    if (switch_index >= kMaxSwitches) {
        return false;
    }
    Event event;
    event.type = EventType::trigger_switch;
    event.switch_index = switch_index;
    event.source = source;
    return post(event);
}

bool EventQueue::post_text(std::int8_t channel, std::uint8_t font, std::string_view text) {
    if (channel < -1 || channel > 7 || font > 0x1e ||
        text.find('\0') != std::string_view::npos) {
        return false;
    }
    Event event;
    event.type = static_cast<EventType>(static_cast<std::uint8_t>(channel + 5));
    event.channel = channel;
    event.font = font;
    event.text = std::string(text);
    return post(event);
}

EventParseResult EventQueue::drain() {
    EventParseResult result = parse(bytes_);
    // The original sets count = 0 on both normal end and error.
    bytes_.clear();
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
            event.x = get_i32(bytes, at + 0x0b);
            event.y = get_i32(bytes, at + 0x0f);
            event.z = get_i32(bytes, at + 0x13);
            event.extra = get_u32(bytes, at + 0x17);
            result.events.push_back(event);
            at += kType1Size;
        } else if (raw_type == static_cast<std::uint8_t>(EventType::trigger_switch)) {
            if (bytes.size() - at < kType2Size) {
                result.ok = false;
                result.error = "truncated type 2 event at offset " + std::to_string(at);
                result.events.clear();
                return result;
            }
            Event event;
            event.type = EventType::trigger_switch;
            event.switch_index = get_u32(bytes, at + 1);
            event.source = get_i16(bytes, at + 5);
            if (event.switch_index >= kMaxSwitches) {
                result.ok = false;
                result.error = "switch index out of range at offset " + std::to_string(at);
                result.events.clear();
                return result;
            }
            result.events.push_back(event);
            at += kType2Size;
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
            event.x = get_i32(bytes, at + 7);
            event.y = get_i32(bytes, at + 0x0b);
            event.z = get_i32(bytes, at + 0x0f);
            event.extra = get_u32(bytes, at + 0x13);
            result.events.push_back(event);
            at += kType3Size;
        } else if (is_text_type(raw_type)) {
            const std::size_t text_start = at + 2;
            std::size_t end = text_start;
            while (end < bytes.size() && bytes[end] != 0) {
                ++end;
            }
            if (end >= bytes.size() || end + 1 >= bytes.size()) {
                result.ok = false;
                result.error = "unterminated text event at offset " + std::to_string(at);
                result.events.clear();
                return result;
            }
            Event event;
            event.type = static_cast<EventType>(raw_type);
            event.channel = static_cast<std::int8_t>(text_channel(raw_type));
            event.font = get_u8(bytes, at + 1);
            event.text.assign(reinterpret_cast<const char*>(bytes.data() + text_start),
                              end - text_start);
            result.events.push_back(event);
            at = end + 2; // NUL + uninitialized gap byte
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

// --- the message registry (0x619860) ----------------------------------------

void TextRegistry::clear() {
    // FUN_00463a90 also clears a flag at 0x619aa8 that the dispatcher never
    // reads; only the count and the update counter matter.
    messages_.clear();
    counter_ = 0;
}

bool TextRegistry::post(std::string_view text, std::int32_t font, std::int32_t kind,
                        std::uint32_t deadline) {
    // The original compares the stored text (strcmp), the kind and the font
    // and refreshes the slot it finds.
    for (TextMessage& message : messages_) {
        if (message.kind == kind && message.font == font && message.text == text) {
            message.deadline = deadline;
            message.updated = counter_;
            ++counter_;
            return true;
        }
    }
    if (messages_.size() >= kMaxMessages) {
        return false; // the original drops the message
    }
    TextMessage message;
    // strncpy(dst, src, 0x32) plus a forced NUL at the end of the slot: at
    // most 0x31 characters are kept.
    message.text.assign(text.substr(0, kMaxTextLength));
    message.kind = kind;
    message.font = font;
    message.deadline = deadline;
    message.updated = counter_;
    messages_.push_back(message);
    ++counter_;
    return true;
}

bool TextRegistry::post_lifetime(std::string_view text, std::int32_t font, std::int32_t kind,
                                 std::uint32_t now) {
    return post(text, font, kind, now + kLifetimeFrames);
}

void TextRegistry::expire(std::uint32_t now) {
    // FUN_00463e00: a message dies when `deadline < now`; the freed slot is
    // filled with the *last* message (the order of the registry changes).
    std::size_t index = 0;
    while (index < messages_.size()) {
        if (messages_[index].deadline < now) {
            messages_[index] = messages_.back();
            messages_.pop_back();
        }
        ++index;
    }
}

std::vector<const TextMessage*> TextRegistry::select(std::int32_t channel) const {
    // The original walks the slots from the end and inserts each one before
    // the first slot with a not smaller update counter: the result is sorted
    // by the update counter, the oldest update first.
    std::vector<const TextMessage*> out;
    for (std::size_t index = messages_.size(); index-- > 0;) {
        const TextMessage& message = messages_[index];
        if (message.kind != -1 && message.kind != channel) {
            continue;
        }
        auto position = out.begin();
        while (position != out.end() && (*position)->updated < message.updated) {
            ++position;
        }
        out.insert(position, &message);
    }
    return out;
}

bool post_event(TextRegistry& registry, const Event& event, std::uint32_t now) {
    if (!is_text_type(static_cast<std::uint8_t>(event.type))) {
        return false;
    }
    // The dispatcher posts `now + 0x111` as the deadline.
    return registry.post_lifetime(event.text, static_cast<std::int32_t>(event.font),
                                  static_cast<std::int32_t>(event.channel), now);
}

EventParseResult EventQueue::parse_network_blob(std::span<const std::uint8_t> bytes) {
    EventParseResult result;
    if (bytes.size() < 4) {
        result.ok = false;
        result.error = "network event blob is shorter than its length field";
        return result;
    }
    const std::uint32_t length = get_u32(bytes, 0);
    if (length > bytes.size() - 4) {
        result.ok = false;
        result.error = "network event blob is truncated: " + std::to_string(length) +
                       " bytes announced, " + std::to_string(bytes.size() - 4) + " available";
        return result;
    }
    return parse(bytes.subspan(4, length));
}

} // namespace gm
