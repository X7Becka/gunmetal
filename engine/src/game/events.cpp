#include "game/events.h"

#include "world/doors.h"
#include "world/things.h"

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


bool EventDispatcher::set_switch(std::uint32_t index, const SwitchEffect& effect) {
    if (index >= kMaxSwitches) {
        return false;
    }
    switches_[index] = effect;
    if (static_cast<std::size_t>(index) + 1 > count_) {
        count_ = static_cast<std::size_t>(index) + 1;
    }
    return true;
}

bool EventDispatcher::clear_switch(std::uint32_t index) {
    if (index >= kMaxSwitches) {
        return false;
    }
    switches_[index] = SwitchEffect{};
    return true;
}

const SwitchEffect* EventDispatcher::switch_at(std::uint32_t index) const {
    if (index >= kMaxSwitches) {
        return nullptr;
    }
    const SwitchEffect& effect = switches_[index];
    return effect.event_id == 0 ? nullptr : &effect;
}

EventDispatchResult EventDispatcher::dispatch(const EventParseResult& parsed,
                                              std::uint32_t now,
                                              TextRegistry& registry) {
    EventDispatchResult result;
    if (!parsed.ok) {
        result.ok = false;
        return result;
    }
    for (const Event& event : parsed.events) {
        const std::uint8_t type = static_cast<std::uint8_t>(event.type);
        if (type == static_cast<std::uint8_t>(EventType::generate_short) ||
            type == static_cast<std::uint8_t>(EventType::generate)) {
            result.generated.push_back(event);
            continue;
        }
        if (type == static_cast<std::uint8_t>(EventType::trigger_switch)) {
            if (event.switch_index >= kMaxSwitches) {
                continue;
            }
            const SwitchEffect& effect = switches_[event.switch_index];
            if (effect.event_id == 0) {
                continue; // inactive switch: 0x44f960 does nothing
            }
            const bool centroid = effect.kind >= 0x101 && effect.has_centroid;
            const double x = centroid ? effect.centroid[0] : effect.x;
            const double y = centroid ? effect.centroid[1] : effect.y;
            const double z = centroid ? effect.centroid[2] : effect.z;
            Event generated;
            generated.type = EventType::generate;
            generated.id = static_cast<std::int32_t>(effect.event_id);
            generated.x = static_cast<std::int32_t>(x);
            generated.y = static_cast<std::int32_t>(y);
            generated.z = static_cast<std::int32_t>(z);
            // 0x44f960 passes `source - 0x10000` as the last generate argument.
            generated.extra = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(event.source) - 0x10000);
            result.generated.push_back(generated);
            if (effect.sound_index > 0) {
                SoundCommand sound;
                sound.index = effect.sound_index;
                sound.sound_id = effect.sound_id;
                sound.x = x;
                sound.y = y;
                sound.z = z;
                result.sounds.push_back(sound);
            }
            continue;
        }
        if (is_text_type(type)) {
            if (post_event(registry, event, now)) {
                ++result.text_posted;
            }
        }
    }
    return result;
}

EventDispatchResult EventDispatcher::dispatch(EventQueue& queue, std::uint32_t now,
                                              TextRegistry& registry) {
    EventParseResult parsed = queue.drain();
    return dispatch(parsed, now, registry);
}

bool EventDispatcher::deserialize(std::span<const std::uint8_t> bytes,
                                  std::string* error) {
    if (bytes.size() < 4) {
        if (error != nullptr) {
            *error = "switch table is shorter than its count field";
        }
        return false;
    }
    const std::uint32_t count = get_u32(bytes, 0);
    if (count > kMaxSwitches) {
        if (error != nullptr) {
            *error = "switch table count exceeds 0x200";
        }
        return false;
    }
    const std::size_t record_bytes = static_cast<std::size_t>(count) * 0x20;
    if (bytes.size() < 4 + record_bytes) {
        if (error != nullptr) {
            *error = "switch table is truncated";
        }
        return false;
    }
    std::array<SwitchEffect, kMaxSwitches> parsed{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 4 + static_cast<std::size_t>(i) * 0x20;
        SwitchEffect effect;
        effect.kind = get_u32(bytes, at + 0x00);
        effect.event_id = get_u32(bytes, at + 0x04);
        effect.reserved = get_u32(bytes, at + 0x08);
        effect.x = get_i32(bytes, at + 0x0c);
        effect.y = get_i32(bytes, at + 0x10);
        effect.z = get_i32(bytes, at + 0x14);
        effect.sound_id = get_u32(bytes, at + 0x18);
        effect.sound_index = get_i32(bytes, at + 0x1c);
        parsed[i] = effect;
    }
    switches_ = parsed;
    count_ = count;
    return true;
}

std::size_t install_switches(EventDispatcher& dispatcher,
                             std::span<const Switch> switches) {
    std::size_t installed = 0;
    for (const Switch& source : switches) {
        if (source.id < 0 ||
            static_cast<std::size_t>(source.id) >= EventDispatcher::kMaxSwitches) {
            continue;
        }
        SwitchEffect effect;
        effect.kind = static_cast<std::uint32_t>(source.kind);
        effect.event_id = static_cast<std::uint32_t>(source.target);
        effect.x = source.pos[0];
        effect.y = source.pos[1];
        effect.z = source.pos[2];
        if (dispatcher.set_switch(static_cast<std::uint32_t>(source.id), effect)) {
            ++installed;
        }
    }
    return installed;
}

std::vector<std::uint8_t> EventDispatcher::serialize() const {
    std::vector<std::uint8_t> out;
    put_u32(out, static_cast<std::uint32_t>(count_));
    for (std::size_t i = 0; i < count_; ++i) {
        const SwitchEffect& effect = switches_[i];
        put_u32(out, effect.kind);
        put_u32(out, effect.event_id);
        put_u32(out, effect.reserved);
        put_i32(out, effect.x);
        put_i32(out, effect.y);
        put_i32(out, effect.z);
        put_u32(out, effect.sound_id);
        put_i32(out, effect.sound_index);
    }
    return out;
}


std::uint32_t thing_event_id(const Thing& thing, ThingEventKind kind) {
    switch (kind) {
    case ThingEventKind::create:
        return thing.create_event;
    case ThingEventKind::damaged:
        return thing.damaged_event;
    case ThingEventKind::destroy:
        return thing.destroy_event;
    case ThingEventKind::expire:
        return thing.expire_event;
    case ThingEventKind::timeout:
        return thing.timeout_event;
    case ThingEventKind::collide:
        return thing.collide_event;
    }
    return 0;
}

bool post_thing_event(EventQueue& queue, const Thing& thing, ThingEventKind kind) {
    const std::uint32_t id = thing_event_id(thing, kind);
    if (id == 0) {
        return false;
    }
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    if (thing.position.has_value()) {
        const auto& p = *thing.position;
        x = p[0];
        y = p[1];
        z = p[2];
    }
    // 0x4703a0/0x470290 pass `thing_index - 0xfff8` as the last argument.
    const std::uint32_t extra =
        static_cast<std::uint32_t>(thing.id - 0xfff8);
    return queue.post_generate(static_cast<std::int32_t>(id), 0,
                               static_cast<double>(x), static_cast<double>(y),
                               static_cast<double>(z), extra);
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

EventParseResult EventQueue::accept_network_blob(std::span<const std::uint8_t> bytes) {
    EventParseResult parsed = parse_network_blob(bytes);
    if (!parsed.ok) {
        return parsed;
    }
    const std::uint32_t length = get_u32(bytes, 0);
    if (length > kBufferSize) {
        parsed.ok = false;
        parsed.events.clear();
        parsed.error = "network event blob does not fit the dispatch buffer";
        return parsed;
    }
    bytes_.assign(bytes.begin() + 4, bytes.begin() + 4 + length);
    return parsed;
}

} // namespace gm
