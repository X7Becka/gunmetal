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

#include "game/weapons.h" // Blast, BlastDamageResult, Vitals, ThingVitals

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
class WorldState; // world/doors.h
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

struct SbtLevel; // res/sbt.h

/// Registry "THING_TYPES collide/expire event id -> CAUSE_BLAST_RADIUS_DAMAGE".
///
/// The original never attaches a blast to a weapon record: `event_post`
/// (`0x4795b0`) queues the collide (`THING_TYPES+0x1c4`, posted by `0x4752d0`)
/// or expire (`+0x1ec`, posted by `0x4703a0`) id, `event_dispatch` (`0x4799b0`)
/// turns it into `GENERATE`, and `generate_runner` (`0x45fa00`) executes the
/// entity sub-script, whose VM op 75 (`0x45775b`) calls the blast
/// (`0x477280`).  The blast therefore belongs to the *event id*, not to a
/// `Weapon`.
///
/// A registry instance is a deterministic, event-id-sorted map.  Build it once
/// from the loaded weapon table (`add_from_weapons`, which reuses the
/// `collect_event_blasts` result that `Weapons::load` already computed for
/// every THING weapon type) or fill it entry-by-entry.
class EventBlastRegistry {
public:
    /// Number of `CAUSE_BLAST_RADIUS_DAMAGE` entries of one event id.
    std::span<const Blast> find(std::uint32_t event_id) const;
    bool contains(std::uint32_t event_id) const { return !find(event_id).empty(); }

    /// Replace the blast list of `event_id`.
    void set(std::uint32_t event_id, std::vector<Blast> blasts);
    /// Append one blast (merges with an existing list).
    bool add(std::uint32_t event_id, const Blast& blast);
    /// Append every blast; false when `blasts` is empty.
    bool add(std::uint32_t event_id, std::span<const Blast> blasts);
    bool remove(std::uint32_t event_id);

    void clear();
    [[nodiscard]] bool empty() const { return entries_.empty(); }
    /// Number of registered event ids.
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    /// Total number of blasts over all event ids.
    [[nodiscard]] std::size_t blast_count() const;
    /// Registered event ids, ascending.
    std::vector<std::uint32_t> event_ids() const;

    /// Add the collide/expire blasts of every THING weapon type in `weapons`.
    /// `Weapons::load` already resolved `Weapon::impact_blasts` (collide event)
    /// and `expire_blasts` (expire event) with `collect_event_blasts`; this
    /// only re-reads the event ids from `THING_TYPES` (`thing_type_events`).
    /// Returns the number of newly registered event ids.  An existing id is
    /// overwritten (the same script yields the same blasts).
    std::size_t add_from_weapons(const Weapons& weapons, const SbtLevel& level);

private:
    struct Entry {
        std::uint32_t event_id = 0;
        std::vector<Blast> blasts;
    };
    std::vector<Entry> entries_; ///< sorted by event_id
};

/// Apply every blast registered for `event_id` at `center`.
///
/// This is the event-driven counterpart of `apply_weapon_blast()`: the medium
/// guards (`underwater`, `from_ground`, `deathmatch`) filter the collected
/// ops exactly like `Weapon::select_blasts()`, and the targets are passed to
/// `apply_blast_damage()` (`0x477280` -> `0x453b50` / `0x470500`).  A missing
/// event id returns an empty result.  `matched` (optional) receives the number
/// of blasts that passed the guards.
BlastDamageResult apply_event_blast(
    const EventBlastRegistry& registry, std::uint32_t event_id,
    const Vec3d& center, bool underwater, bool from_ground, bool deathmatch,
    int source_player, int source_thing,
    std::span<const Vec3d> player_positions, std::span<Vitals> player_vitals,
    std::span<const Vec3d> thing_positions, std::span<const bool> thing_alive,
    std::span<ThingVitals> thing_vitals, std::size_t* matched = nullptr);

/// Maximum number of `GENERATE` hops from an event block to the blasts it
/// reaches (`docs/EVENT_DAMAGE.md`).  The shipped scripts do not come close:
/// the longest chain that a shipped event needs is well inside this bound, and
/// the limit is what keeps a script cycle (event A -> B -> A) finite.
inline constexpr int kEventBlastChainDepth = 4;

/// Counters of one `build_event_registry()` call.  `generate_commands` and
/// `blast_commands` count the *direct* ops of every decoded entity block
/// (recursing into IF bodies, not following `GENERATE`), so they are the raw
/// data census; `blasts` is what the registry stores after following the
/// `GENERATE` chain and applying the guards.
struct EventRegistryBuildStats {
    std::size_t entity_blocks = 0;    ///< rows of the SCRIPT handle table
    std::size_t ids = 0;              ///< distinct event ids (first row wins)
    std::size_t scripts_decoded = 0;  ///< blocks that decoded cleanly
    std::size_t generate_commands = 0; ///< op 2 `GENERATE` anywhere in a block
    std::size_t blast_commands = 0;   ///< op 75 `CAUSE_BLAST_RADIUS_DAMAGE`
    std::size_t ids_with_blasts = 0;  ///< registered event ids
    std::size_t blasts = 0;           ///< registered blasts (with chains)
    std::size_t chain_cycles = 0;     ///< `GENERATE` back-edges cut
    std::size_t chain_depth_cut = 0;  ///< `GENERATE` hops past the limit
    std::size_t decode_failures = 0;  ///< blocks the VM decoder rejected
};

/// Build the level-wide "event id -> `CAUSE_BLAST_RADIUS_DAMAGE`" registry from
/// the compiled SCRIPT record.
///
/// Every distinct entity id of the handle table is decoded (first row wins,
/// exactly like the engine's linear search at `0x457c49`), scanned for op 75,
/// then its `GENERATE` (op 2) children are followed up to
/// `kEventBlastChainDepth`; a repeated id in the current chain is a cycle and
/// is cut.  IF guards (`SOURCE_UNDERWATER`, `SOURCE_FROM_GROUND`,
/// `DEATHMATCH_GAME`) travel into the `Blast` exactly like
/// `Weapons::load()`'s `collect_event_blasts()`.  `out` is replaced (cleared
/// first), so the result is deterministic and independent of the previous
/// contents.
///
/// Returns false with `error` when the level has no usable SCRIPT record (no
/// code or no handle table); `out` is then empty and `stats` still holds the
/// census that could be taken.  A level with a SCRIPT record but no blast op
/// returns true with an empty registry.
bool build_event_registry(const SbtLevel& level, EventBlastRegistry& out,
                          std::string* error = nullptr,
                          EventRegistryBuildStats* stats = nullptr);

/// Context of one `damaged_event` chain (the medium/deathmatch branch and the
/// attacker ids of the original `apply_event_blast()` call).
struct EventDamageChainContext {
    bool underwater = false;   ///< selects the water branch of a blast
    bool from_ground = true;   ///< `SOURCE_FROM_GROUND` true
    bool deathmatch = true;    ///< `DEATHMATCH_GAME` true
    int source_player = -1;    ///< attacker slot, -1 = world
    int source_thing = -1;     ///< attacker thing index, -1 = world
    int max_depth = kEventBlastChainDepth; ///< chained `damaged_event` limit
};

/// Counters of one `chase_damaged_events()` call.
struct EventDamageChainStats {
    std::size_t events = 0;          ///< `damaged_event` blasts that ran
    std::size_t blasts = 0;          ///< blasts that passed the guards
    std::size_t hits = 0;            ///< target hits (`damage.hits.size()`)
    std::size_t depth_max = 0;       ///< deepest chain step reached (1-based)
    std::size_t skipped_no_event = 0; ///< hit thing has `damaged_event == 0`
    std::size_t skipped_dead = 0;    ///< target died: only `destroy_event` follows
    std::size_t skipped_missing = 0; ///< event id absent from the registry
    std::size_t skipped_cycle = 0;   ///< event id already in the chain
    std::size_t skipped_depth = 0;   ///< chain would exceed `max_depth`
};

/// Result of the `damaged_event` chase.  `damage.hits[i]` was applied at
/// `centers[i]` (the position of the thing whose event posted it).
struct EventDamageChainResult {
    BlastDamageResult damage;
    std::vector<Vec3d> centers;
    EventDamageChainStats stats;
};

/// Chase the `damaged_event` (`THING_TYPES+0x1f4`, instance `+0x518`) of every
/// thing hit by `applied` and apply the registered blasts at that thing.
///
/// This is the event bridge of the original: a non-lethal blast hit posts
/// `damaged_event` (`0x470995`), `event_dispatch` turns it into `GENERATE`,
/// and the event's sub-script runs `CAUSE_BLAST_RADIUS_DAMAGE`
/// (`0x45775b`).  The chase is depth-first in hit order, so it is fully
/// deterministic.  A target that died in `applied` only has a `destroy_event`
/// (not modeled here) and is counted in `skipped_dead`.
///
/// `damaged_events[i]` is the event id of thing `i` (same index as
/// `thing_positions`/`thing_vitals`); shorter spans simply stop the chase for
/// indices outside them.  A missing id/registry entry is a no-op, not an
/// error.
EventDamageChainResult chase_damaged_events(
    const EventBlastRegistry& registry, const BlastDamageResult& applied,
    std::span<const std::uint32_t> damaged_events,
    std::span<const Vec3d> thing_positions, std::span<const bool> thing_alive,
    std::span<Vitals> player_vitals, std::span<ThingVitals> thing_vitals,
    const EventDamageChainContext& context = {});

/// One entry of the original runtime switch table `0x5a4af0` (stride 0x20),
/// written by the level/trigger setup and consumed by `FUN_0044f960`.
struct SwitchEffect {
    std::uint32_t kind = 0;     ///< +0x00; >= 0x101 uses the polygon centroid
    std::uint32_t event_id = 0; ///< +0x04; 0 = inactive
    std::uint32_t reserved = 0; ///< +0x08, written by FUN_0044e5f0
    std::int32_t x = 0;         ///< +0x0c
    std::int32_t y = 0;         ///< +0x10
    std::int32_t z = 0;         ///< +0x14
    std::uint32_t sound_id = 0; ///< +0x18
    std::int32_t sound_index = 0; ///< +0x1c; positive = play
    std::array<double, 3> centroid{}; ///< polygon centroid for kind >= 0x101
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
    /// Parse/validation text when `ok == false` (empty on success).
    std::string error;
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

    /// Client side of the network event packet: a valid blob (`u32` length
    /// plus records) replaces the queued records.  An invalid blob returns
    /// `ok=false` with `error` set and leaves the previous queue untouched.
    EventParseResult accept_network_blob(std::span<const std::uint8_t> bytes);

    /// Accept a network blob and dispatch it at `now` in one step.  On a valid
    /// blob the previous queue is replaced, the records are dispatched and the
    /// queue is drained, so a second `pump`/`pump_network` without new records
    /// posts nothing.  An invalid blob dispatches nothing and keeps the old
    /// queue (use `accept_network_blob` for the parse error); the result then
    /// has `ok=false`.
    EventDispatchResult pump_network(std::span<const std::uint8_t> bytes,
                                     std::uint32_t now);

    /// Drain the queue through the dispatcher at `now` (91 Hz tick or the
    /// caller's clock).
    EventDispatchResult pump(std::uint32_t now);

private:
    EventQueue queue_;
    EventDispatcher dispatcher_;
    TextRegistry text_;
};

/// Bridge between the world event log and the runtime queue: post every
/// `WorldEvent::SwitchPressed` the world has not mirrored yet as a type-2
/// `Trigger Switch` record.  `source` travels into the record and later into
/// the GENERATE `extra` field (`source - 0x10000`).
///
/// The caller installs the table first
/// (`runtime.install_switches(world.switches())`) and then calls
/// `runtime.pump(now)`; the type-2 record becomes a GENERATE event at the
/// switch position (or the sector centroid for `kind >= 0x101`) plus an
/// optional `SoundCommand`.  Returns the number of queued records; a second
/// call without a new `WorldState::interact()` returns 0.
std::size_t post_switch_events(EventRuntime& runtime, const WorldState& world,
                               std::int16_t source = 0);

} // namespace gm
