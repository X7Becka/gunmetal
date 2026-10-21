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

/// One 0x58-byte SECTOR_STUFF slot.  Field names follow gm/sbt.py; every
/// field is decoded verbatim (no normalisation) and the slot's original
/// bytes are kept in ``raw`` so the two 16-bit gaps and the dead +0x54 word
/// survive unchanged.
struct SbtSectorSlot {
    std::array<double, 3> move{};  ///< +0x00 SET_SECTOR_MOVEMENT dx,dy,dz per tick
    std::array<double, 3> extra{}; ///< +0x18 SET_SECTOR_MOVEMENT tail (no reader found)
    double sound_param = 0.0;      ///< +0x30 SET_SECTOR_SOUND ``D`` operand
    std::int32_t event_a = 0;      ///< +0x38 SET_SECTOR_EVENTS, signed id
    std::int32_t event_b = 0;      ///< +0x3c SET_SECTOR_EVENTS, signed id
    std::uint16_t damage_lo = 0;   ///< +0x40 SET_SECTOR_DAMAGE low half
    std::array<std::uint8_t, 2> pad_42{}; ///< +0x42 gap, kept raw
    std::uint16_t damage_hi = 0;   ///< +0x44 SET_SECTOR_DAMAGE high half
    std::array<std::uint8_t, 2> pad_46{}; ///< +0x46 gap, kept raw
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
