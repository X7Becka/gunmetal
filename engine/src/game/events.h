#pragma once

// Event queue records from Gunmetal's `event_post` (0x4795b0).
//
// The original writes variable-size records into one of two byte buffers:
//
//   type 1, 0x1b bytes: u8 type, u32 id, u16 param, i16 a, i16 b,
//                       f32 x, f32 y, f32 z, u32 extra
//   type 3, 0x17 bytes: u8 type, u32 id, u16 param,
//                       f32 x, f32 y, f32 z, u32 extra
//
// `event_dispatch` (0x4799b0) switches on the type byte.  Types 1 and 3 are
// GENERATE events and are the only ones this module currently encodes; the
// original also accepts types 2..12 from other producers.
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
#include <vector>

namespace gm {

enum class EventType : std::uint8_t {
    generate_short = 1, ///< type 1, 0x1b bytes
    generate = 3,       ///< type 3, 0x17 bytes
};

struct Event {
    EventType type = EventType::generate;
    std::int32_t id = 0;
    std::uint16_t param = 0;
    std::int16_t a = 0; ///< type 1 only
    std::int16_t b = 0; ///< type 1 only
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    std::uint32_t extra = 0;
};

struct EventParseResult {
    bool ok = true;
    std::string error;
    std::vector<Event> events;
};

/// Append-only staging queue.  `drain()` is the equivalent of `event_dispatch`.
class EventQueue {
public:
    static constexpr std::size_t kOverflowCheck = 0x750e;
    static constexpr std::size_t kType1Size = 0x1b;
    static constexpr std::size_t kType3Size = 0x17;
    static constexpr std::size_t kMaxRecordSize = kType1Size;

    explicit EventQueue(std::size_t overflow_check = kOverflowCheck);

    /// Type 3 record (`a == b == 0` in the original call).
    bool post_generate(std::int32_t id, std::uint16_t param, float x, float y, float z,
                       std::uint32_t extra = 0);

    /// Type 1 record.
    bool post_generate_short(std::int32_t id, std::uint16_t param, std::int16_t a, std::int16_t b,
                             float x, float y, float z, std::uint32_t extra = 0);

    /// Generic form; only EventType::generate and generate_short are accepted.
    bool post(const Event& event);

    /// Raw queued bytes, exactly as the original staging buffer.
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    [[nodiscard]] bool empty() const { return bytes_.empty(); }
    [[nodiscard]] std::size_t overflow_check() const { return overflow_check_; }

    void clear() { bytes_.clear(); }

    /// Parse the queued bytes and clear the queue.  Unknown type/short record is
    /// an error and leaves no partial events in the result.
    EventParseResult drain();

    /// Parse a standalone buffer; `event_dispatch` uses the same layout.
    static EventParseResult parse(std::span<const std::uint8_t> bytes);

private:
    bool can_append(std::size_t record_size) const;
    void append(const Event& event);

    std::size_t overflow_check_ = kOverflowCheck;
    std::vector<std::uint8_t> bytes_;
};

} // namespace gm
