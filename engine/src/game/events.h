#pragma once

// Event queue records from Gunmetal's `event_post` (0x4795b0).
//
// The original writes variable-size records into one of two byte buffers:
//
//   type 1, 0x1b bytes: u8 type, u32 id, u16 param, i16 a, i16 b,
//                       i32 x, i32 y, i32 z, u32 extra
//   type 2, 7 bytes:    u8 type, u32 switch_index, i16 source
//   type 3, 0x17 bytes: u8 type, u32 id, u16 param,
//                       i32 x, i32 y, i32 z, u32 extra
//   type 4..12:         u8 type=channel+5, u8 font, NUL-terminated text,
//                       then one uninitialized gap byte (dispatcher step len+4)
//
// `event_post` takes doubles and truncates them with `__ftol`; the record
// coordinates are signed 32-bit, not floats.  Types 4..12 are text/chat, type
// 2 is Trigger Switch.  The original dispatch table also has no type 0/13+:
// it prints an error and drops the whole queue.
//
// The text records do not raise an event: `event_dispatch` puts them into the
// message registry of `0x619860` (ten slots, dedup on text/kind/font, a
// lifetime of `now + 0x111`).  That registry is `TextRegistry` below.
//
// The queue's overflow check is `0x750e < used_bytes` *before* appending a
// record, so a record may start at 0x750e and the effective capacity is
// 0x750e + 0x1b.  We preserve that rule rather than silently changing it.
//
// See docs/EVENTS.md for the Ghidra evidence.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

enum class EventType : std::uint8_t {
    generate_short = 1,  ///< type 1, 0x1b bytes
    trigger_switch = 2,  ///< type 2, 7 bytes
    generate = 3,        ///< type 3, 0x17 bytes
    text_system = 4,     ///< channel -1
    text_channel0 = 5,
    text_channel1 = 6,
    text_channel2 = 7,
    text_channel3 = 8,
    text_channel4 = 9,
    text_channel5 = 10,
    text_channel6 = 11,
    text_channel7 = 12,
};

constexpr bool is_text_type(std::uint8_t type) noexcept { return type >= 4 && type <= 12; }
constexpr int text_channel(std::uint8_t type) noexcept { return static_cast<int>(type) - 5; }

struct Event {
    EventType type = EventType::generate;
    std::int32_t id = 0;
    std::uint16_t param = 0;
    std::int16_t a = 0; ///< type 1 only
    std::int16_t b = 0; ///< type 1 only
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint32_t extra = 0;
    std::uint32_t switch_index = 0; ///< type 2 only
    std::int16_t source = 0;        ///< type 2 only
    std::int8_t channel = 0;        ///< text only, -1..7
    std::uint8_t font = 0;          ///< text only, 0..0x1e
    std::string text;               ///< text only
};

struct EventParseResult {
    bool ok = true;
    std::string error;
    std::vector<Event> events;
};

/// One slot of the message registry of the original (`0x619860`).
///
/// The original keeps the text in a 0x32 byte slot (`strncpy` with the last
/// byte forced to NUL, so at most 0x31 characters) and the other fields in
/// parallel arrays: the kind/channel (`0x619838`), the font (`0x619a58`), the
/// deadline (`0x619810`) and the update counter (`0x619a80`).
struct TextMessage {
    std::string text;           ///< at most 0x31 characters
    std::int32_t kind = 0;      ///< channel: -1 = system, 0..7 = players
    std::int32_t font = 0;      ///< font index (0..0x1e)
    std::uint32_t deadline = 0; ///< the frame the message dies at
    std::uint32_t updated = 0;  ///< the update counter of the slot
};

/// The message registry of the original game: `FUN_00463ab0` posts a message,
/// `FUN_00463e00` expires them, `FUN_00463bc0`/`FUN_00463ef0` draw them.
///
/// The registry holds at most ten messages.  A message with the same
/// (text, kind, font) as an existing one refreshes that slot instead of
/// taking a second one; when the registry is full a new message is dropped.
/// An expired message (`deadline < now`) is removed by moving the *last*
/// slot into the freed one, exactly like the original.
class TextRegistry {
public:
    static constexpr std::size_t kMaxMessages = 10;
    static constexpr std::size_t kTextCapacity = 0x32; ///< the strncpy limit
    static constexpr std::size_t kMaxTextLength = kTextCapacity - 1;
    /// The lifetime of the original: the dispatcher posts `now + 0x111`.
    static constexpr std::uint32_t kLifetimeFrames = 0x111;

    /// `FUN_00463a90`: drop everything and reset the update counter.
    void clear();

    /// `FUN_00463ab0`: post a message with an explicit deadline.  Returns
    /// false when the registry is full and the message is dropped.
    bool post(std::string_view text, std::int32_t font, std::int32_t kind,
              std::uint32_t deadline);

    /// Post a message that lives `kLifetimeFrames` frames.
    bool post_lifetime(std::string_view text, std::int32_t font, std::int32_t kind,
                       std::uint32_t now);

    /// `FUN_00463e00`: drop the messages whose deadline is in the past.
    void expire(std::uint32_t now);

    /// The messages to draw for `channel`: the system channel (-1) and the
    /// requested one, oldest update first (`FUN_00463bc0`).
    std::vector<const TextMessage*> select(std::int32_t channel) const;

    [[nodiscard]] const std::vector<TextMessage>& messages() const { return messages_; }
    [[nodiscard]] std::size_t size() const { return messages_.size(); }
    [[nodiscard]] bool empty() const { return messages_.empty(); }
    /// The global update counter (`0x619ab0`).
    [[nodiscard]] std::uint32_t counter() const { return counter_; }

private:
    std::vector<TextMessage> messages_;
    std::uint32_t counter_ = 0;
};

/// Feed one event of the queue into a registry (`event_dispatch`, the types
/// 4..12 branch).  Non-text events are ignored and return false.
bool post_event(TextRegistry& registry, const Event& event, std::uint32_t now);

class EventQueue;
struct Thing;
struct Switch; // world/doors.h
/// Event fields of one THING_TYPES record (docs/THINGS_CPP.md).
enum class ThingEventKind : std::uint8_t {
    create,  ///< +0x1f0
    damaged, ///< +0x1f4
    destroy, ///< +0x1f8
    expire,  ///< +0x1ec
    timeout, ///< +0x1c0
    collide, ///< +0x1c4
};

/// Event id for `kind`, or 0 when the shipped type has no such event.
std::uint32_t thing_event_id(const Thing& thing, ThingEventKind kind);

/// Post one thing event through the original queue.  The call site is the
/// original one (`0x4703a0`/`0x470290`): world position of the thing and
/// `extra = id - 0xfff8`; source-relative things without a world position use
/// zero coordinates (the original resolves a transform, which the C++ level
/// view does not retain).  Returns false for a zero event id or a full queue.
bool post_thing_event(EventQueue& queue, const Thing& thing, ThingEventKind kind);

/// One entry of the original runtime switch table `0x5a4af0` (stride 0x20),
/// written by the level/trigger setup and consumed by `FUN_0044f960`.
struct SwitchEffect {
    std::uint32_t kind = 0;     ///< +0x00; >= 0x101 uses the sector centroid
    std::uint32_t event_id = 0; ///< +0x04; 0 = inactive
    std::uint32_t reserved = 0; ///< +0x08, written by FUN_0044e5f0
    std::int32_t x = 0;         ///< +0x0c
    std::int32_t y = 0;         ///< +0x10
    std::int32_t z = 0;         ///< +0x14
    std::uint32_t sound_id = 0; ///< +0x18
    std::int32_t sound_index = 0; ///< +0x1c; positive = play
    std::array<double, 3> centroid{}; ///< sector centroid for kind >= 0x101
    bool has_centroid = false;
};

/// A 3D sound request from a type-2 switch (`FUN_0046b840`).  The caller owns
/// the audio backend; `rate`/`volume` are the literals of that call.
struct SoundCommand {
    std::int32_t index = 0;
    std::uint32_t sound_id = 0;
    std::uint32_t rate = 0x2b11;
    std::int32_t volume = 1000;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// Result of `event_dispatch`: generated world events, type-2 sound requests
/// and the number of text records handed to the registry.  Unknown/truncated
/// queues are rejected as a whole, matching the original.
struct EventDispatchResult {
    bool ok = true;
    std::vector<Event> generated;
    std::vector<SoundCommand> sounds;
    std::size_t text_posted = 0;
};

class EventQueue;

/// `event_dispatch` (`0x4799b0`) over the runtime switch table.  Types 1/3
/// become `generated`; type 2 looks up `switches_[index]`, emits a generate
/// event at the switch (or its centroid) and a sound request; types 4..12 go
/// to `TextRegistry`.  The table is deliberately caller-owned: the original
/// fills `0x5a4af0` from the level's trigger setup, which is not a record in
/// the `.sbt` parser yet.
class EventDispatcher {
public:
    static constexpr std::size_t kMaxSwitches = 0x200;

    EventDispatcher() = default;

    /// Install one table entry.  False for an out-of-range index.
    bool set_switch(std::uint32_t index, const SwitchEffect& effect);
    /// Remove an entry (sets it to all zeroes / inactive).
    bool clear_switch(std::uint32_t index);
    const SwitchEffect* switch_at(std::uint32_t index) const;

    /// Process already-parsed events.  When `parsed.ok` is false the whole
    /// queue is dropped and the result has `ok=false`.
    EventDispatchResult dispatch(const EventParseResult& parsed,
                                 std::uint32_t now, TextRegistry& registry);
    /// Drain and dispatch one queue.
    EventDispatchResult dispatch(EventQueue& queue, std::uint32_t now,
                                 TextRegistry& registry);

    /// File format of `FUN_0044f000` / `FUN_0044f0a0`: little-endian
    /// `u32 count` (0..0x200) followed by `count` records of 0x20 bytes
    /// (`kind, event_id, reserved, x, y, z, sound_id, sound_index`).
    bool deserialize(std::span<const std::uint8_t> bytes, std::string* error = nullptr);
    std::vector<std::uint8_t> serialize() const;

private:
    std::array<SwitchEffect, kMaxSwitches> switches_{};
    std::size_t count_ = 0;
};

/// Install the level's `ADD_SWITCH` records into the runtime table.  The
/// original runtime entry {kind, target, x, y, z} comes from the record; the
/// table index is `Switch::id` (the record index) and the GENERATE id is
/// `Switch::target`.
std::size_t install_switches(EventDispatcher& dispatcher,
                             std::span<const Switch> switches);

/// Append-only staging queue.  `drain()` is the equivalent of `event_dispatch`.
class EventQueue {
public:
    static constexpr std::size_t kBufferSize = 0x7530;
    static constexpr std::size_t kOverflowGenerate = 0x750e;
    static constexpr std::size_t kOverflowSwitch = 0x7526;
    static constexpr std::size_t kOverflowText = 0x74de;
    static constexpr std::size_t kOverflowCheck = kOverflowGenerate; // compatibility
    static constexpr std::size_t kType1Size = 0x1b;
    static constexpr std::size_t kType2Size = 7;
    static constexpr std::size_t kType3Size = 0x17;
    static constexpr std::size_t kMaxRecordSize = kType1Size;
    static constexpr std::size_t kMaxSwitches = 0x200;
    static constexpr std::size_t kMaxTextBytes = 0x32;
    static constexpr std::size_t kMaxMessages = 10;
    static constexpr std::size_t kTextGap = 1;

    /// `overflow_check == 0` keeps the per-type thresholds; a non-zero value
    /// overrides all of them (used by the synthetic tests).
    explicit EventQueue(std::size_t overflow_check = 0);

    /// Type 3 record (`a == b == 0` in the original call).  Coordinates are
    /// truncated to i32 like the original `__ftol`.
    bool post_generate(std::int32_t id, std::uint16_t param, double x, double y, double z,
                       std::uint32_t extra = 0);

    /// Type 1 record.
    bool post_generate_short(std::int32_t id, std::uint16_t param, std::int16_t a, std::int16_t b,
                             double x, double y, double z, std::uint32_t extra = 0);

    /// Type 2 (Trigger Switch), 7 bytes.
    bool post_switch(std::uint32_t switch_index, std::int16_t source);

    /// Types 4..12, channel -1..7, font 0..0x1e.
    bool post_text(std::int8_t channel, std::uint8_t font, std::string_view text);

    /// Generic form; supported types 1,2,3,4..12.
    bool post(const Event& event);

    /// Raw queued bytes, exactly as the original staging buffer.
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    [[nodiscard]] bool empty() const { return bytes_.empty(); }
    [[nodiscard]] std::size_t overflow_check() const { return overflow_check_; }

    void clear() { bytes_.clear(); }

    /// Parse the queued bytes and clear the queue.  Like the original, an
    /// unknown/truncated record drops the whole queue; no partial events are
    /// returned.
    EventParseResult drain();

    /// Parse a standalone buffer; `event_dispatch` uses the same layout.
    static EventParseResult parse(std::span<const std::uint8_t> bytes);

    /// The network event blob of the original: a `u32` byte count followed by
    /// that many event records.  `FUN_00465400` writes it into the state
    /// packet, `FUN_00466ae0` reads it and copies the records into the
    /// dispatch buffer.
    ///
    /// The original copies the announced length without checking it against
    /// the buffer; here a blob that is too short is an error instead of an
    /// overrun.
    static EventParseResult parse_network_blob(std::span<const std::uint8_t> bytes);

    /// Client side of `FUN_00466ae0`: validate a blob and replace the queued
    /// records with its announced bytes.  On error the queue is untouched.
    EventParseResult accept_network_blob(std::span<const std::uint8_t> bytes);

private:
    bool can_append(std::size_t record_size, std::size_t overflow) const;
    std::size_t threshold_for(EventType type) const;
    void append(const Event& event);

    std::size_t overflow_check_ = 0;
    std::vector<std::uint8_t> bytes_;
};

/// Small composition of the three runtime event pieces: queue, dispatcher
/// table and text registry.  The game loop owns one instance and calls
/// `pump(now)` once per tick; this is the C++ counterpart of the
/// `event_dispatch` + registry step.
class EventRuntime {
public:
    EventRuntime() = default;

    EventQueue& queue() { return queue_; }
    const EventQueue& queue() const { return queue_; }
    EventDispatcher& dispatcher() { return dispatcher_; }
    const EventDispatcher& dispatcher() const { return dispatcher_; }
    TextRegistry& text() { return text_; }
    const TextRegistry& text() const { return text_; }

    /// Install `ADD_SWITCH` records into the dispatcher table.
    std::size_t install_switches(std::span<const Switch> switches);

    /// Drain the queue through the dispatcher at `now` (91 Hz tick or the
    /// caller's clock).
    EventDispatchResult pump(std::uint32_t now);

private:
    EventQueue queue_;
    EventDispatcher dispatcher_;
    TextRegistry text_;
};

} // namespace gm
