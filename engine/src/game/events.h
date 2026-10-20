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
// The queue's overflow check is `0x750e < used_bytes` *before* appending a
// record, so a record may start at 0x750e and the effective capacity is
// 0x750e + 0x1b.  We preserve that rule rather than silently changing it.
//
// See docs/EVENTS.md for the Ghidra evidence.

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

private:
    bool can_append(std::size_t record_size, std::size_t overflow) const;
    std::size_t threshold_for(EventType type) const;
    void append(const Event& event);

    std::size_t overflow_check_ = 0;
    std::vector<std::uint8_t> bytes_;
};

} // namespace gm
