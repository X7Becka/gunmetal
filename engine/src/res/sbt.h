#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SbtRecord {
    std::uint8_t type = 0;
    std::string name;
    std::size_t offset = 0; // includes the type byte
    std::size_t size = 0;   // includes the type byte
    std::vector<std::uint8_t> payload;
};

struct SbtHandle {
    std::uint32_t tag = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
};

struct SbtSob {
    std::string name;
    std::uint32_t amount = 0;
    std::uint32_t ptr = 0;
};

struct SbtScript {
    std::uint32_t things = 0;
    std::uint32_t handles = 0;
    std::uint32_t code_size = 0;
    std::uint8_t flag = 0;
};

/// SECTOR_STUFF (record/save block 8): fixed 256 x 0x58 = 0x5800 bytes,
/// runtime array 0x800b60..0x806360 (cleared by 0x46b1b0, read 0x46b7b0,
/// written 0x46b7f0).  Layout and evidence: docs/RECORDS.md §2,
/// docs/WORLD_FIELDS.md; Python oracle gm/sbt.py ``SectorSlot``.
inline constexpr std::size_t kSbtSectorSlots = 256;
inline constexpr std::size_t kSbtSectorSlotSize = 0x58;
inline constexpr std::size_t kSbtSectorStuffSize = kSbtSectorSlots * kSbtSectorSlotSize;

/// One 0x58-byte SECTOR_STUFF slot.  Field names follow gm/sbt.py with one
/// correction: ``+0x40`` and ``+0x44`` are two 32-bit values, not two u16 plus
/// padding — the consumer loads them as dwords (0x4450f6/0x4450fc) and
/// multiplies both by dt.  ``SET_SECTOR_DAMAGE`` (0x457961) fills them from one
/// operand: ``+0x40 = value & 0xffff`` (dword store at 0x45799a zeroes the
/// upper half) and ``+0x44 = value >> 16`` arithmetic (0x4579a0 sign-extends).
/// Every byte of the slot is typed, so the two "gaps" older maps mentioned do
/// not exist; ``raw`` still keeps the verbatim bytes for debugging.
struct SbtSectorSlot {
    std::array<double, 3> move{};  ///< +0x00 SET_SECTOR_MOVEMENT dx,dy,dz per tick
    std::array<double, 3> extra{}; ///< +0x18 SET_SECTOR_MOVEMENT tail (no reader found)
    double sound_param = 0.0;      ///< +0x30 SET_SECTOR_SOUND ``D`` operand
    std::int32_t event_a = 0;      ///< +0x38 SET_SECTOR_EVENTS, signed id
    std::int32_t event_b = 0;      ///< +0x3c SET_SECTOR_EVENTS, signed id
    std::uint32_t damage_a = 0;    ///< +0x40 damage dword 1 (low operand half)
    std::int32_t damage_b = 0;     ///< +0x44 damage dword 2 (high half, sign-extended)
    std::int32_t sound = 0;        ///< +0x48 SOUND_SAMPLE_TABLE index
    std::int32_t sound_count = 0;  ///< +0x4c ``N`` operand, >0 = sound active
    std::uint32_t flags = 0;       ///< +0x50 SET_SECTOR_FLAGS (0x800bb0 for slot 0)
    std::uint32_t unused = 0;      ///< +0x54 hypothesis: dead word, kept raw
    std::array<std::uint8_t, kSbtSectorSlotSize> raw{}; ///< verbatim slot bytes

    /// True when every byte of the slot is zero (the cleared state).
    bool empty() const;
};

/// Decodes a SECTOR_STUFF payload.  ``payload`` must be exactly
/// kSbtSectorStuffSize bytes; anything else is an error (``out`` untouched).
bool parse_sector_stuff(std::span<const std::uint8_t> payload,
                        std::vector<SbtSectorSlot>& out, std::string& error);

/// Re-encodes 256 slots from their typed fields (``raw`` is ignored), the
/// inverse of parse_sector_stuff.  Returns an empty vector when ``slots``
/// does not hold exactly kSbtSectorSlots entries.
std::vector<std::uint8_t> encode_sector_stuff(const std::vector<SbtSectorSlot>& slots);

// --- records 1 / 2 / 18: typed view + writer --------------------------------
//
// The same three arrays appear twice: as ``.sbt`` records (type 1 SWITCH,
// 2 DOOR, 18 LIFT) and as save blocks 1, 2 and 18.  The record payload of a
// type is ``u32 count`` + ``count`` records; a save block drops the leading
// count (the block carries it once, in the same place).  Parsers below take
// the bare record body, so they serve both containers.
//
// Every byte of a door and a switch record is covered by a named field, and
// every lift word that has no proven meaning is kept as ``unknown_*`` instead
// of being dropped, so ``encode(parse(x)) == x`` is byte-exact.  Offsets:
// docs/DOOR_FIELDS.md §1–§3, docs/SAVE_BLOCKS.md §2–§3, docs/RECORDS.md §4.

inline constexpr std::size_t kSbtSwitchRecordSize = 0x20;
inline constexpr std::size_t kSbtDoorRecordSize = 0xB8;
inline constexpr std::size_t kSbtLiftRecordSize = 0xA8;
inline constexpr std::size_t kSbtLiftStopSize = 0x0C;

/// One 0x20-byte SWITCH record (``0x5a4af0``).  All eight words are copied
/// verbatim from the ``ADD_SWITCH`` operands (writer ``0x44e5f0``).
struct SbtSwitchRecord {
    std::int32_t kind = 0;      ///< +0x00 0 ordinary, 0x101 aim, 0x102..0x104 source
    std::int32_t target = 0;    ///< +0x04 GENERATE id
    std::int32_t radius = 0;    ///< +0x08 trigger radius (0x44fc30)
    std::array<std::int32_t, 3> pos{}; ///< +0x0c world x/y/z
    std::int32_t aux = 0;       ///< +0x18 extra sound argument
    std::int32_t sound = -1;    ///< +0x1c SOUND_SAMPLE_TABLE index, -1 = silent
};

/// One 0xB8-byte DOOR record (``0x8381a0``).  ``closed``/``open`` are the
/// **folded** box corners the loader builds (``+0x00 += +0x30`` at
/// ``0x44e368``), i.e. the runtime values a save contains.
struct SbtDoorRecord {
    std::array<double, 3> closed{};     ///< +0x00 D1..D3 + D7..D9
    std::array<double, 3> open{};       ///< +0x18 D4..D6 + D7..D9
    std::array<double, 3> size{};       ///< +0x30 D7..D9
    std::array<double, 3> axis{};       ///< +0x48 A1..A3 / 65535 (rotation, rad per phase unit)
    double sound_level = 0.03;          ///< +0x60 constant 0.03 pushed with every door sound
    std::int32_t flags = 0;             ///< +0x68 kind in the low nibble
    std::array<std::int32_t, 5> ids{};  ///< +0x6c E1..E5 (E4/E5 = on-open/on-close events)
    std::int32_t state = 0;             ///< +0x80 -1 closing, 0 idle, +1 opening
    std::int32_t sector = -1;           ///< +0x84
    std::int32_t object = -1;           ///< +0x88
    std::uint32_t phase_prev = 0;       ///< +0x8c committed phase, 0..0xffff
    std::uint32_t phase_next = 0;       ///< +0x90 phase computed by the tick
    std::int32_t step = 0;              ///< +0x94 phase units per 91 Hz tick
    std::int32_t dwell = 0;             ///< +0x98 auto-close delay, ticks
    std::int32_t timer = 0;             ///< +0x9c remaining dwell
    std::array<std::int32_t, 3> volume{}; ///< +0xa0/+0xa4/+0xa8 v0/v1/v2
    std::array<std::int32_t, 3> sample{{-1, -1, -1}}; ///< +0xac/+0xb0/+0xb4 n0/n1/n2
};

/// One 0xA8-byte LIFT record (``0x836de0``).  ``basis`` is the six doubles at
/// ``+0x00``: ``basis[0..2]`` are the base angles and ``basis[3..5]`` the deltas
/// added to them over the travel — ``0x44e6f0`` builds ``value[i] =
/// basis[i] + basis[3+i] * height/65535`` and hands ``(value[1], value[0],
/// value[2])`` to the basis builder ``0x41e025``.  ``pos_delta`` is the matching
/// position increment.
struct SbtLiftRecord {
    std::array<double, 6> basis{};      ///< +0x00..+0x30 base/delta pairs
    std::uint32_t handle = 0;           ///< +0x30 runtime model handle
    std::int32_t kind = 0;              ///< +0x34 tick requires 1
    std::int32_t sob = 0;               ///< +0x38 SOB model index (not an entity id)
    std::array<std::int32_t, 3> pos{};        ///< +0x3c x/y/z
    std::array<std::int32_t, 3> pos_delta{};  ///< +0x48 dx/dy/dz (scaled by height)
    std::int32_t speed = 0;             ///< +0x54 height units per tick
    std::int32_t dwell = 0;             ///< +0x58 ticks waiting at a stop
    std::int32_t height = 0;            ///< +0x5c committed height, 0..0xffff
    std::int32_t height_tick = 0;       ///< +0x60 height of the current tick
    std::int32_t wait = 0;              ///< +0x64 remaining wait
    std::int32_t direction = 0;         ///< +0x68 -1/0/+1
    std::int32_t stop_index = 0;        ///< +0x6c
    std::array<std::int32_t, 3> call_ids{}; ///< +0x70..+0x78
    std::int32_t call_event = 0;        ///< +0x7c event posted when called
    std::int32_t call_radius = 0;       ///< +0x80
    std::int32_t volume_a = 0;          ///< +0x84 call/depart volume
    std::int32_t volume_b = 0;          ///< +0x88 loop/arrive volume
    std::int32_t unknown_8c = 0;        ///< +0x8c no reader found
    std::int32_t sample_call = -1;      ///< +0x90 sample: call/depart
    std::int32_t sample_move = -1;      ///< +0x94 sample: loop while moving
    std::int32_t sample_arrive = -1;    ///< +0x98 sample: arrival
    std::int32_t stop_count = 0;        ///< +0x9c
    std::int32_t stop_start = 0;        ///< +0xa0 first index in the three columns
    std::int32_t unknown_a4 = 0;        ///< +0xa4 no reader found
};

/// One stop of a type-18 record: three parallel u32 columns.
struct SbtLiftStop {
    std::int32_t floor_id = 0; ///< column 0 (``0x5a9130``)
    std::int32_t height = 0;   ///< column 1 (``0x5a8e10``)
    std::int32_t sound = 0;    ///< column 2 (``0x5a8af0``)
};

/// Lift record type 18 payload: ``u32 n``, ``n`` records, ``u32 k``, three
/// columns of ``k`` stops.  ``stop_start``/``stop_count`` of every record must
/// fit inside ``k`` for a round-trip (the engine indexes the columns blindly).
struct SbtLiftBlock {
    std::vector<SbtLiftRecord> lifts;
    std::vector<SbtLiftStop> stops;
};

inline constexpr std::uint32_t kSbtMaxSwitches = 512;
inline constexpr std::uint32_t kSbtMaxDoors = 100;
inline constexpr std::uint32_t kSbtMaxLifts = 30;
inline constexpr std::uint32_t kSbtMaxLiftStops = 200;

/// Decode a bare record body (``count * record_size`` bytes, no leading count).
/// A body whose size is not a multiple of the record size, or that holds more
/// records than the engine's array bound, is rejected with ``error``.
bool parse_switch_block(std::span<const std::uint8_t> body,
                        std::vector<SbtSwitchRecord>& out, std::string& error);
bool parse_door_block(std::span<const std::uint8_t> body,
                      std::vector<SbtDoorRecord>& out, std::string& error);
/// Decode a full type-18 payload (``u32 n`` + records + ``u32 k`` + columns).
bool parse_lift_block(std::span<const std::uint8_t> payload, SbtLiftBlock& out,
                      std::string& error);

/// Inverse of the parsers above; empty vector/block on an out-of-range size.
std::vector<std::uint8_t> encode_switch_block(const std::vector<SbtSwitchRecord>& records);
std::vector<std::uint8_t> encode_door_block(const std::vector<SbtDoorRecord>& records);
std::vector<std::uint8_t> encode_lift_block(const SbtLiftBlock& block);
/// ``u32 count`` + body, i.e. the ``.sbt`` record payload of types 1/2.
std::vector<std::uint8_t> encode_counted(const std::vector<std::uint8_t>& body,
                                         std::uint32_t count);

/// Compiled ``.sbt`` level file: 84-byte header + typed record stream.
/// Layout: docs/FORMATS.md §7 and gm/sbt.py.
struct SbtLevel {
    std::uint32_t version = 0;
    std::vector<SbtRecord> records;
    std::vector<SbtHandle> poly_handles;
    std::vector<SbtHandle> object_handles;
    std::vector<SbtHandle> sector_handles;
    std::vector<SbtSob> sobs;
    SbtScript script;
    // SCRIPT record payload, kept for the VM/scene layer: per-thing rows,
    // the handle columns (3*n u32), the compiled bytecode and the optional
    // 9*0x400 tables.
    std::vector<std::uint8_t> script_things;
    std::vector<std::uint8_t> script_handles_data;
    std::vector<std::uint8_t> script_code;
    std::vector<std::uint8_t> script_tables;
    // SECTOR_STUFF (type 8): 256 slots when the record is present, empty
    // otherwise.  None of the 36 shipped level files carries it (only the
    // save stream does, block 8); a repeated record overwrites the view,
    // like the original's loader 0x46b7b0 overwrites 0x800b60.
    std::vector<SbtSectorSlot> sector_stuff;
    std::size_t file_end = 0;
};

std::optional<SbtLevel> parse_sbt(std::span<const std::uint8_t> data, std::string& error);
const char* sbt_type_name(std::uint8_t type);

} // namespace gm
