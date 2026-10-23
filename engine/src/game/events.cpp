#include "game/events.h"

#include "res/sbt.h"
#include "res/sbtvm.h"
#include "world/doors.h"
#include "world/things.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>

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
    // and refreshes the slot it finds.  The stored slot is truncated first, so
    // a long input dedups against its own 0x31-byte prefix.
    const std::string_view stored_text = text.substr(0, kMaxTextLength);
    for (TextMessage& message : messages_) {
        if (message.kind == kind && message.font == font &&
            message.text == stored_text) {
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
            // The replacement occupies the same slot and may be expired too.
        } else {
            ++index;
        }
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
        result.error = parsed.error;
        return result;
    }
    // `event_dispatch` runs on the frame clock: drop messages whose deadline
    // has passed before reporting new text.  Without this a full registry
    // would keep stale messages forever and silently drop every new one.
    registry.expire(now);
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
        // `0x43577c` resolves kind >= 0x101 to the polygon centroid;
        // WorldState::build already computed it from the .sbw triple.
        if (source.is_poly() && source.world_position.has_value()) {
            effect.centroid = *source.world_position;
            effect.has_centroid = true;
        }
        if (dispatcher.set_switch(static_cast<std::uint32_t>(source.id), effect)) {
            ++installed;
        }
    }
    return installed;
}

std::size_t EventRuntime::install_switches(std::span<const Switch> switches) {
    return gm::install_switches(dispatcher_, switches);
}

EventParseResult EventRuntime::accept_network_blob(std::span<const std::uint8_t> bytes) {
    return queue_.accept_network_blob(bytes);
}

EventDispatchResult EventRuntime::pump_network(std::span<const std::uint8_t> bytes,
                                               std::uint32_t now) {
    EventParseResult accepted = queue_.accept_network_blob(bytes);
    if (!accepted.ok) {
        EventDispatchResult rejected;
        rejected.ok = false;
        rejected.error = accepted.error;
        return rejected;
    }
    // The accepted records are already parsed and the queue now holds a copy
    // of the blob.  Drop that copy before dispatching to keep the drain
    // semantics of pump(): only a new blob can produce events again.
    queue_.clear();
    return dispatcher_.dispatch(accepted, now, text_);
}

EventDispatchResult EventRuntime::pump(std::uint32_t now) {
    return dispatcher_.dispatch(queue_, now, text_);
}

std::size_t post_switch_events(EventRuntime& runtime, const WorldState& world,
                               std::int16_t source) {
    return world.post_switch_events(runtime.queue(), source);
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

namespace {

std::uint32_t read_u32_at(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

/// The entity table of the compiled SCRIPT record: distinct entity ids mapped
/// to `[offset, end)` byte ranges of `level.script_code`.
///
/// The engine looks an id up with a linear search over the id column
/// (`0x457c49`/`0x45fa5c`); several shipped levels carry a second, DEATHMATCH
/// copy of the same ids further down the table, so the first row wins, exactly
/// like the lookup.  The block ends at the next handle offset of the level.
class EntityBlocks {
public:
    explicit EntityBlocks(const SbtLevel& level) {
        const std::size_t n = level.script.handles;
        if (n == 0 || level.script_handles_data.size() < n * 12) {
            error_ = "level has no SCRIPT handle table";
            return;
        }
        const auto* data = level.script_handles_data.data();
        std::vector<std::uint32_t> all;
        all.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            all.push_back(read_u32_at(data + (2 * n + i) * 4));
        }
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t id = read_u32_at(data + (n + i) * 4);
            const std::uint32_t offset = read_u32_at(data + (2 * n + i) * 4);
            if (by_id_.find(id) != by_id_.end()) {
                continue; // first row wins
            }
            if (offset >= level.script_code.size()) {
                continue;
            }
            const auto bound = std::upper_bound(all.begin(), all.end(), offset);
            const std::uint32_t end =
                bound != all.end() ? *bound
                                   : static_cast<std::uint32_t>(
                                         level.script_code.size());
            if (end <= offset) {
                continue;
            }
            by_id_.emplace(id, std::array<std::uint32_t, 2>{offset, end});
            ids_.push_back(id);
        }
    }

    [[nodiscard]] bool valid() const { return error_.empty(); }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] const std::vector<std::uint32_t>& ids() const { return ids_; }
    [[nodiscard]] std::size_t size() const { return ids_.size(); }

    bool span(std::uint32_t id, std::uint32_t& offset, std::uint32_t& end) const {
        const auto it = by_id_.find(id);
        if (it == by_id_.end()) {
            return false;
        }
        offset = it->second[0];
        end = it->second[1];
        return end > offset;
    }

private:
    std::vector<std::uint32_t> ids_;
    std::map<std::uint32_t, std::array<std::uint32_t, 2>> by_id_;
    std::string error_;
};

/// Operand -> u32, the rule of `tools/re/impact.py` / `weapons.cpp`.
std::uint32_t as_uint(const SbtValue& value) {
    if (value.kind == SbtValue::Kind::UInt) {
        return value.u;
    }
    if (value.kind == SbtValue::Kind::Real && std::isfinite(value.d) &&
        value.d >= 0.0 && value.d <= 4294967295.0) {
        return static_cast<std::uint32_t>(value.d);
    }
    return 0;
}

/// Guards of the mutually exclusive IF branches proven in the shipped data
/// (the same model as `Weapons::load()`'s `collect_event_blasts`).
struct BlastGuard {
    bool air = true;
    bool water = true;
    bool ground = true;
    bool not_ground = true;
    bool deathmatch = true;
    bool not_deathmatch = true;
};

bool guarded_condition(const std::string& condition) {
    return condition == "SOURCE_UNDERWATER" ||
           condition.rfind("SOURCE_FROM_GROUND", 0) == 0 ||
           condition == "DEATHMATCH_GAME";
}

BlastGuard branch_guard(BlastGuard guard, const std::string& condition,
                        bool truth) {
    if (condition == "SOURCE_UNDERWATER") {
        (truth ? guard.air : guard.water) = false;
    } else if (condition.rfind("SOURCE_FROM_GROUND", 0) == 0) {
        (truth ? guard.not_ground : guard.ground) = false;
    } else if (condition == "DEATHMATCH_GAME") {
        (truth ? guard.not_deathmatch : guard.deathmatch) = false;
    }
    return guard;
}

/// VM op 75 operands: flags, no_direction, plate, hull, radius, event.
std::optional<Blast> blast_from_insn(const SbtInsn& insn) {
    if (insn.name != "CAUSE_BLAST_RADIUS_DAMAGE" || insn.args.size() < 6) {
        return std::nullopt;
    }
    Blast blast;
    blast.flags = as_uint(insn.args[0]);
    blast.no_direction = as_uint(insn.args[1]) != 0;
    blast.plate = static_cast<std::int32_t>(as_uint(insn.args[2]));
    blast.hull = static_cast<std::int32_t>(as_uint(insn.args[3]));
    blast.radius = static_cast<int>(as_uint(insn.args[4]));
    blast.event = as_uint(insn.args[5]);
    return blast;
}

void count_commands(const std::vector<SbtInsn>& insns,
                    std::size_t& generates, std::size_t& blasts) {
    for (const SbtInsn& insn : insns) {
        if (insn.name == "GENERATE") {
            ++generates;
        } else if (insn.name == "CAUSE_BLAST_RADIUS_DAMAGE") {
            ++blasts;
        }
        if (!insn.body.empty()) {
            count_commands(insn.body, generates, blasts);
        }
    }
}

void scan_event_block(const SbtLevel& level, const EntityBlocks& blocks,
                      const std::vector<SbtInsn>& insns,
                      std::vector<Blast>& out, const BlastGuard& guard,
                      int depth, std::vector<std::uint32_t>& stack,
                      EventRegistryBuildStats* stats);

void collect_event(const SbtLevel& level, const EntityBlocks& blocks,
                   std::uint32_t event_id, int depth, const BlastGuard& guard,
                   std::vector<std::uint32_t>& stack, std::vector<Blast>& out,
                   EventRegistryBuildStats* stats) {
    if (event_id == 0 || event_id == 0xffffffffU) {
        return;
    }
    if (depth > kEventBlastChainDepth) {
        if (stats != nullptr) {
            ++stats->chain_depth_cut;
        }
        return;
    }
    if (std::find(stack.begin(), stack.end(), event_id) != stack.end()) {
        if (stats != nullptr) {
            ++stats->chain_cycles;
        }
        return;
    }
    std::uint32_t offset = 0;
    std::uint32_t end = 0;
    if (!blocks.span(event_id, offset, end)) {
        return;
    }
    std::string error;
    const std::span<const std::uint8_t> bytes(level.script_code.data() + offset,
                                              end - offset);
    const auto program = decode_sbt_script(bytes, error);
    if (!program) {
        if (stats != nullptr) {
            ++stats->decode_failures;
        }
        return;
    }
    stack.push_back(event_id);
    scan_event_block(level, blocks, program->insns, out, guard, depth, stack,
                     stats);
    stack.pop_back();
}

void scan_event_block(const SbtLevel& level, const EntityBlocks& blocks,
                      const std::vector<SbtInsn>& insns,
                      std::vector<Blast>& out, const BlastGuard& guard,
                      int depth, std::vector<std::uint32_t>& stack,
                      EventRegistryBuildStats* stats) {
    for (std::size_t i = 0; i < insns.size(); ++i) {
        const SbtInsn& insn = insns[i];
        if (insn.name == "IF" && guarded_condition(insn.condition)) {
            const BlastGuard taken = branch_guard(guard, insn.condition, true);
            const BlastGuard fallthrough =
                branch_guard(guard, insn.condition, false);
            std::uint32_t else_target = 0;
            std::vector<SbtInsn> body;
            for (const SbtInsn& child : insn.body) {
                if (child.name == "ELSE") {
                    else_target = child.target;
                } else {
                    body.push_back(child);
                }
            }
            scan_event_block(level, blocks, body, out, taken, depth, stack,
                             stats);
            if (else_target == 0 && !insn.body.empty() &&
                insn.body.back().name == "ELSE") {
                else_target = insn.body.back().target;
            }
            if (else_target == 0 && i + 1 < insns.size() &&
                insns[i + 1].name == "ELSE") {
                // Sibling ELSE of the C++ decoder: the else-code follows it.
                else_target = insns[i + 1].target;
            }
            if (else_target != 0) {
                std::size_t j = i + 1;
                while (j < insns.size() && insns[j].offset < else_target) {
                    ++j;
                }
                if (j > i + 1) {
                    std::vector<SbtInsn> fall(insns.begin() + i + 1,
                                              insns.begin() + j);
                    scan_event_block(level, blocks, fall, out, fallthrough,
                                     depth, stack, stats);
                }
                i = j - 1;
            }
            continue;
        }
        if (auto blast = blast_from_insn(insn)) {
            blast->air = guard.air;
            blast->water = guard.water;
            blast->ground = guard.ground;
            blast->not_ground = guard.not_ground;
            blast->deathmatch = guard.deathmatch;
            blast->not_deathmatch = guard.not_deathmatch;
            out.push_back(*blast);
        }
        if (insn.name == "GENERATE" && !insn.args.empty()) {
            collect_event(level, blocks, as_uint(insn.args[0]), depth + 1,
                          guard, stack, out, stats);
        }
        if (!insn.body.empty()) {
            scan_event_block(level, blocks, insn.body, out, guard, depth,
                             stack, stats);
        }
    }
}

} // namespace

bool build_event_registry(const SbtLevel& level, EventBlastRegistry& out,
                          std::string* error, EventRegistryBuildStats* stats) {
    EventRegistryBuildStats local;
    EventRegistryBuildStats& s = stats != nullptr ? *stats : local;
    s = EventRegistryBuildStats{};
    out.clear();

    const EntityBlocks blocks(level);
    s.entity_blocks = level.script.handles;
    if (!blocks.valid() || level.script_code.empty()) {
        const std::string message =
            blocks.error().empty() ? "level has no SCRIPT code" : blocks.error();
        if (error != nullptr) {
            *error = message;
        }
        return false;
    }
    s.ids = blocks.size();

    // Census: direct op 2 / op 75 of every distinct block, no chain following.
    for (std::uint32_t id : blocks.ids()) {
        std::uint32_t offset = 0;
        std::uint32_t end = 0;
        if (!blocks.span(id, offset, end)) {
            continue;
        }
        std::string decode_error;
        const std::span<const std::uint8_t> bytes(
            level.script_code.data() + offset, end - offset);
        const auto program = decode_sbt_script(bytes, decode_error);
        if (!program) {
            ++s.decode_failures;
            continue;
        }
        ++s.scripts_decoded;
        count_commands(program->insns, s.generate_commands, s.blast_commands);
    }

    // Registry: every id gets its own blasts plus the `GENERATE` chain.
    for (std::uint32_t id : blocks.ids()) {
        std::vector<Blast> blasts;
        std::vector<std::uint32_t> stack;
        collect_event(level, blocks, id, 0, BlastGuard{}, stack, blasts, &s);
        if (blasts.empty()) {
            continue;
        }
        s.blasts += blasts.size();
        ++s.ids_with_blasts;
        out.set(id, std::move(blasts));
    }
    return true;
}

namespace {

void chase_impl(const EventBlastRegistry& registry,
                const BlastDamageResult& applied,
                std::span<const std::uint32_t> damaged_events,
                std::span<const Vec3d> thing_positions,
                std::span<const bool> thing_alive,
                std::span<Vitals> player_vitals,
                std::span<ThingVitals> thing_vitals,
                const EventDamageChainContext& context, int depth,
                std::vector<std::uint32_t>& stack,
                EventDamageChainResult& out) {
    for (const AppliedBlastHit& hit : applied.hits) {
        if (hit.hit.is_player || hit.hit.index < 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(hit.hit.index);
        if (index >= damaged_events.size()) {
            continue;
        }
        if (!hit.thing_damage.ok) {
            continue;
        }
        if (!hit.thing_damage.damaged_event) {
            if (hit.thing_damage.dead) {
                ++out.stats.skipped_dead;
            }
            continue;
        }
        const std::uint32_t event_id = damaged_events[index];
        if (event_id == 0) {
            ++out.stats.skipped_no_event;
            continue;
        }
        if (depth > context.max_depth) {
            ++out.stats.skipped_depth;
            continue;
        }
        if (std::find(stack.begin(), stack.end(), event_id) != stack.end()) {
            ++out.stats.skipped_cycle;
            continue;
        }
        if (registry.find(event_id).empty()) {
            ++out.stats.skipped_missing;
            continue;
        }
        if (index >= thing_positions.size() || index >= thing_vitals.size()) {
            continue;
        }
        const Vec3d center = thing_positions[index];
        std::size_t matched = 0;
        BlastDamageResult one = apply_event_blast(
            registry, event_id, center, context.underwater, context.from_ground,
            context.deathmatch, context.source_player, context.source_thing,
            {}, player_vitals, thing_positions, thing_alive, thing_vitals,
            &matched);
        ++out.stats.events;
        out.stats.blasts += matched;
        out.stats.depth_max =
            std::max<std::size_t>(out.stats.depth_max,
                                  static_cast<std::size_t>(depth));
        out.damage.chain_reaction =
            out.damage.chain_reaction || one.chain_reaction;
        for (const AppliedBlastHit& chained : one.hits) {
            out.damage.hits.push_back(chained);
            out.centers.push_back(center);
        }
        out.stats.hits = out.damage.hits.size();
        stack.push_back(event_id);
        chase_impl(registry, one, damaged_events, thing_positions, thing_alive,
                   player_vitals, thing_vitals, context, depth + 1, stack, out);
        stack.pop_back();
    }
}

} // namespace

EventDamageChainResult chase_damaged_events(
    const EventBlastRegistry& registry, const BlastDamageResult& applied,
    std::span<const std::uint32_t> damaged_events,
    std::span<const Vec3d> thing_positions, std::span<const bool> thing_alive,
    std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
    const EventDamageChainContext& context) {
    EventDamageChainResult out;
    std::vector<std::uint32_t> stack;
    chase_impl(registry, applied, damaged_events, thing_positions, thing_alive,
               player_vitals, thing_vitals, context, 1, stack, out);
    out.stats.hits = out.damage.hits.size();
    return out;
}

std::span<const Blast> EventBlastRegistry::find(std::uint32_t event_id) const {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), event_id,
        [](const Entry& entry, std::uint32_t id) { return entry.event_id < id; });
    if (it == entries_.end() || it->event_id != event_id) {
        return {};
    }
    return it->blasts;
}

void EventBlastRegistry::set(std::uint32_t event_id, std::vector<Blast> blasts) {
    if (blasts.empty()) {
        remove(event_id);
        return;
    }
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), event_id,
        [](const Entry& entry, std::uint32_t id) { return entry.event_id < id; });
    if (it != entries_.end() && it->event_id == event_id) {
        it->blasts = std::move(blasts);
        return;
    }
    entries_.insert(it, Entry{event_id, std::move(blasts)});
}

bool EventBlastRegistry::add(std::uint32_t event_id, const Blast& blast) {
    return add(event_id, std::span<const Blast>(&blast, 1));
}

bool EventBlastRegistry::add(std::uint32_t event_id,
                             std::span<const Blast> blasts) {
    if (blasts.empty()) {
        return false;
    }
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), event_id,
        [](const Entry& entry, std::uint32_t id) { return entry.event_id < id; });
    if (it != entries_.end() && it->event_id == event_id) {
        it->blasts.insert(it->blasts.end(), blasts.begin(), blasts.end());
        return true;
    }
    Entry entry;
    entry.event_id = event_id;
    entry.blasts.assign(blasts.begin(), blasts.end());
    entries_.insert(it, std::move(entry));
    return true;
}

bool EventBlastRegistry::remove(std::uint32_t event_id) {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), event_id,
        [](const Entry& entry, std::uint32_t id) { return entry.event_id < id; });
    if (it == entries_.end() || it->event_id != event_id) {
        return false;
    }
    entries_.erase(it);
    return true;
}

void EventBlastRegistry::clear() {
    entries_.clear();
}

std::size_t EventBlastRegistry::blast_count() const {
    std::size_t count = 0;
    for (const Entry& entry : entries_) {
        count += entry.blasts.size();
    }
    return count;
}

std::vector<std::uint32_t> EventBlastRegistry::event_ids() const {
    std::vector<std::uint32_t> ids;
    ids.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        ids.push_back(entry.event_id);
    }
    return ids;
}

std::size_t EventBlastRegistry::add_from_weapons(const Weapons& weapons,
                                                 const SbtLevel& level) {
    std::size_t added = 0;
    const auto add_type = [&](int thing_type,
                              const std::vector<Blast>& impact,
                              const std::vector<Blast>& expire) {
        if (thing_type < 0) {
            return;
        }
        const auto events = thing_type_events(level, thing_type);
        if (!events) {
            return;
        }
        const auto store = [&](std::uint32_t id, const std::vector<Blast>& list) {
            if (id == 0 || list.empty()) {
                return;
            }
            if (!contains(id)) {
                ++added;
            }
            set(id, list);
        };
        store(events->collide_event, impact);
        store(events->expire_event, expire);
    };
    for (const Weapon& weapon : weapons.table()) {
        add_type(weapon.thing_type, weapon.impact_blasts, weapon.expire_blasts);
        for (const ThingVariant& variant : weapon.thing_variants) {
            add_type(variant.thing_type, variant.impact_blasts,
                     variant.expire_blasts);
        }
    }
    return added;
}

BlastDamageResult apply_event_blast(
    const EventBlastRegistry& registry, std::uint32_t event_id,
    const Vec3d& center, bool underwater, bool from_ground, bool deathmatch,
    int source_player, int source_thing,
    std::span<const Vec3d> player_positions, std::span<Vitals> player_vitals,
    std::span<const Vec3d> thing_positions, std::span<const bool> thing_alive,
    std::span<ThingVitals> thing_vitals, std::size_t* matched) {
    BlastDamageResult result;
    if (matched != nullptr) {
        *matched = 0;
    }
    for (const Blast& blast : registry.find(event_id)) {
        // Same medium guards as `Weapon::select_blasts()`.
        if (!(underwater ? blast.water : blast.air)) {
            continue;
        }
        if (!(from_ground ? blast.ground : blast.not_ground)) {
            continue;
        }
        if (!(deathmatch ? blast.deathmatch : blast.not_deathmatch)) {
            continue;
        }
        if (matched != nullptr) {
            ++*matched;
        }
        BlastDamageResult one = apply_blast_damage(
            center, blast, source_player, source_thing, player_positions,
            player_vitals, thing_positions, thing_alive, thing_vitals);
        result.chain_reaction = result.chain_reaction || one.chain_reaction;
        result.hits.insert(result.hits.end(), one.hits.begin(), one.hits.end());
    }
    return result;
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
