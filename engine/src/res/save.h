#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// Gunmetal save container (``SAVEGAME/saved<N>.sbt``), see docs/SAVE.md.
///
///     u8   name[0x50]     display title in bytes 0..11 ("A SAVED GAME", "2")
///     u32  reserved       +0x50, 0 in both shipped saves
///     u8   level[0x50]    +0x54, NUL-terminated ("LEVEL0")
///     u32  F[22]          +0xa4, header counters/timers (docs/SAVE_FIELDS.md §2)
///     char slots[4][16]   +0xfc, K-9 / "" / coargus / none
///     [u8 id][payload]... stream of typed blocks, 0xff = EOF
///
/// There is no checksum, no alignment and no trailing data after ``0xff``.
/// Block sizes are derived from their counts; the byte right after the last
/// payload byte must be the next block id (or ``0xff``).
constexpr std::size_t kSaveHeaderSize = 0x13c;
constexpr std::size_t kSaveLevelNameAt = 0x54;
constexpr std::size_t kSaveLevelNameLen = 0x50;
constexpr std::size_t kSaveFieldsAt = 0xa4;
constexpr std::size_t kSaveFieldCount = 22;
constexpr std::size_t kSaveNameSlotAt = 0xfc;
constexpr std::size_t kSaveNameSlotCount = 4;
constexpr std::size_t kSaveNameSlotLen = 16;
constexpr std::uint8_t kSaveBlockEof = 0xff;

/// Raw dword at +0x0c.  In ``saved0`` it is 0x34, in ``saved1`` it is 0
/// because the bytes are the tail of the 12-byte title ("A SAVED GAME"),
/// not a container version.  Exposed for parity with tools/re/savefile.py,
/// which prints it as "version"; do not use it for feature gating.
constexpr std::uint32_t kSaveVersionWord = 0x34;

/// Gauges are stored in hundredths of a point: 10000 == 100.00 (0x44408a).
constexpr double kSaveGaugeScale = 100.0;

struct SaveHeader {
    std::string title;                        ///< bytes 0..11 up to the first NUL
    std::uint32_t version = 0;                ///< raw u32 at +0x0c, see kSaveVersionWord
    std::uint32_t reserved = 0;               ///< u32 at +0x50
    std::string level_name;                   ///< +0x54, up to the first NUL
    std::array<std::uint32_t, kSaveFieldCount> fields{}; ///< F0..F21 at +0xa4
    std::array<std::string, kSaveNameSlotCount> name_slots{}; ///< +0xfc
};

/// One ``[u8 id][payload]`` record of the stream that follows the header.
struct SaveBlock {
    std::uint8_t id = 0;
    std::size_t offset = 0; ///< absolute file offset of the id byte
    std::size_t size = 0;   ///< id byte + payload
    /// Records for blocks with an explicit u32 count (1,2,4,5,6,7,15,16,
    /// 17,18,20), present player slots for block 9, present slots for block 3,
    /// referenced samples for block 12 (non-zero counts).
    /// 0 for the fixed-size raw blocks (8,19).
    std::uint32_t count = 0;
    /// Verbatim payload bytes (everything after the id byte).  The writer
    /// copies these for every block except 9, so untouched blocks round-trip
    /// byte-for-byte even when their bodies are still opaque.  ``size`` is
    /// ``raw.size() + 1`` for every block produced by parse_save().
    std::vector<std::uint8_t> raw;
};

/// Sizes/offsets of the block-9 layout (docs/SAVE_FIELDS.md §9.2).
constexpr std::size_t kSavePlayerSlots = 8;
constexpr std::size_t kSavePlayerStructSize = 0x3510;
constexpr std::size_t kSavePlayerAttachmentAt = 0x317c;
constexpr std::size_t kSavePlayerAttachmentSlots = 16;
constexpr std::size_t kSavePlayerAttachmentRecord = 16;
constexpr std::size_t kSavePlayerPairHeaderSize = 0x50;
constexpr std::size_t kSavePlayerPairCount = 2;

/// Player state of block 9 (saver 0x452020).
///
/// Layout: 8 x {u8 present; if present: 0x3510 struct + 16 bytes per non-zero
/// u32 in struct+0x317c}, then two pairs of {char header[0x50]; 0x3510 struct}.
/// The shipped saves have exactly one present slot and no attachments.
struct SavePlayer {
    std::size_t block_offset = 0; ///< offset of the block id byte
    std::array<bool, 8> present{};
    std::array<std::size_t, 8> slot_offsets{}; ///< absolute offset of each present 0x3510 struct
    std::array<std::uint32_t, 8> attachment_counts{};

    /// Fields of the first present slot.  ``has_slot`` is false when the save
    /// carries a player block without a present slot (fields stay zeroed).
    bool has_slot = false;

    std::array<double, 3> position{}; ///< struct+0x00, world X/Y/Z
    std::array<double, 3> angles{};   ///< struct+0x18 (0x18/0x20/0x28, see SAVE_CPP.md)
    std::int32_t history_index = 0;   ///< struct+0x3204, next ring slot mod 240
    std::uint32_t exists = 0;         ///< struct+0x3178, non-zero while the slot is valid
    std::int32_t status_present = 0;  ///< struct+0x3210
    std::int32_t object_id = 0;       ///< struct+0x3214
    std::int32_t armor_max = 0;       ///< struct+0x3244, hundredths
    std::int32_t armor = 0;           ///< struct+0x3248, hundredths
    std::int32_t shield_max = 0;      ///< struct+0x324c, hundredths
    std::array<std::int32_t, 4> shields{}; ///< struct+0x3250, front/right/back/left, hundredths
    std::int32_t weapon_slot = 0;     ///< struct+0x3294, 0..11
    std::uint32_t weapon_mask = 0;    ///< struct+0x3298, ``1 << slot`` per available slot
    std::array<std::uint32_t, 12> ammo{}; ///< struct+0x329c

    /// Headers of the two trailing {0x50 header, 0x3510 struct} pairs
    /// (``LEVEL0`` and an empty string in the shipped saves).
    std::array<std::string, 2> pair_headers{};

    /// Verbatim 0x3510 struct of every present slot.  The writer copies these
    /// bytes and patches only the typed fields of the first present slot, so
    /// unknown structure bytes and non-zero attachment ids survive unchanged.
    std::array<std::vector<std::uint8_t>, kSavePlayerSlots> slot_raw{};
    /// Concatenated 16-byte records that follow a present slot (one per
    /// non-zero u32 in struct+0x317c).  Attachments are opaque; they are
    /// copied verbatim.
    std::array<std::vector<std::uint8_t>, kSavePlayerSlots> attachment_raw{};
    /// Verbatim header and struct of the two trailing pairs.
    std::array<std::array<std::uint8_t, kSavePlayerPairHeaderSize>,
               kSavePlayerPairCount>
        pair_header_raw{};
    std::array<std::array<std::uint8_t, kSavePlayerStructSize>,
               kSavePlayerPairCount>
        pair_struct_raw{};

    /// Gauges in points (armor / 100.0), matching savefile.py's output.
    double armor_points() const { return static_cast<double>(armor) / kSaveGaugeScale; }
    double armor_max_points() const { return static_cast<double>(armor_max) / kSaveGaugeScale; }
    double shield_max_points() const { return static_cast<double>(shield_max) / kSaveGaugeScale; }
    double shield_points(int index) const {
        return static_cast<double>(shields[static_cast<std::size_t>(index)]) / kSaveGaugeScale;
    }
};

/// Block 1 record (0x20), one per ``ADD_SWITCH`` operand set.
/// All eight dwords are the verbatim script operands (docs/SAVE_FIELDS.md §4).
constexpr std::size_t kSaveSwitchRecordSize = 0x20;
constexpr std::uint32_t kSaveMaxSwitches = 0x200;

struct SaveSwitch {
    std::int32_t kind = 0;   ///< +0x00: 0 ordinary, 0x101 transformed, 0x102..0x104 source
    std::int32_t id = 0;     ///< +0x04: entity id passed to GENERATE
    std::int32_t radius = 0; ///< +0x08: trigger radius for kinds 0x102..0x104
    std::array<std::int32_t, 3> pos{}; ///< +0x0c: world X/Y/Z
    std::int32_t aux = 0;    ///< +0x18: auxiliary sound argument (hypothesis)
    std::int32_t sound = -1; ///< +0x1c: sample index, -1 = silent
};

/// Block 2 record (0xB8), runtime door state.
/// Everything in the record is covered by the proven/unknown map; the
/// unknown constant at +0x60 is kept verbatim so an untouched door round-trips.
constexpr std::size_t kSaveDoorRecordSize = 0xb8;
constexpr std::uint32_t kSaveMaxDoors = 100;

struct SaveDoor {
    std::array<double, 9> box{};   ///< +0x00: closed corner (3), open corner (3), size (3)
    std::array<double, 3> delta{}; ///< +0x48: per-tick box movement
    double constant = 0.03;        ///< +0x60: ADD_DOOR constant, no reader known
    std::int32_t flags = 0;        ///< +0x68: low nibble type 1..3, bit 0x40 usable
    std::array<std::int32_t, 5> ids{}; ///< +0x6c: five E operands
    std::int32_t state = 0;        ///< +0x80: -1 closing, 0 closed, +1 opening
    std::int32_t sector = 0;       ///< +0x84: sector index in the .sbw
    std::int32_t object = 0;       ///< +0x88: object index inside the sector
    std::uint32_t phase_prev = 0;  ///< +0x8c: animation phase before the tick
    std::uint32_t phase_next = 0;  ///< +0x90: animation phase after the tick
    std::int32_t step = 0;         ///< +0x94: phase increment per tick
    std::int32_t param5 = 0;       ///< +0x98: script L6, no reader known
    std::int32_t param6 = 0;       ///< +0x9c: script L7, no reader known
    std::array<std::int32_t, 3> v{}; ///< +0xa0: V operands of the VNVNVN tail
    std::array<std::int32_t, 3> n{}; ///< +0xac: N operands of the VNVNVN tail
};

/// Block 3 slot (0x528).  The game does not expose a field map for the body
/// yet, so the whole record is kept verbatim together with its slot index.
constexpr std::size_t kSaveThingSlots = 260;
constexpr std::size_t kSaveThingRecordSize = 0x528;

struct SaveThing {
    std::size_t slot = 0; ///< 0..259, index of the presence flag
    std::array<std::uint8_t, kSaveThingRecordSize> raw{}; ///< opaque runtime state
};

/// Block 6 record (0x2c), runtime sprite.
constexpr std::size_t kSaveSpriteRecordSize = 0x2c;
constexpr std::uint32_t kSaveMaxSprites = 0x1f3;

struct SaveSprite {
    float scale_x = 0.0f;        ///< +0x00: X scale (script arg 9)
    float scale_y = 0.0f;        ///< +0x04: Y scale (script arg 10)
    std::uint32_t runtime_object = 0; ///< +0x08: stale runtime handle, re-created on load
    std::array<std::int32_t, 3> pos{}; ///< +0x0c: world X/Y/Z
    std::uint8_t param = 0;      ///< +0x18: byte parameter (hypothesis)
    std::array<std::uint8_t, 3> reserved0{}; ///< +0x19: alignment/not mapped
    std::uint32_t flags = 0;     ///< +0x1c: render flags (hypothesis)
    std::int32_t effect_id = 0;  ///< +0x20: sprite/effect model id
    std::uint16_t param16 = 0;   ///< +0x24: 16-bit render parameter (hypothesis)
    std::array<std::uint8_t, 2> reserved1{}; ///< +0x26: alignment/padding
    std::uint32_t spawn_time = 0; ///< +0x28: level clock at creation
};

/// Block 7 source.  The file groups six parallel columns (x, y, z, volume,
/// sample as i32, then pitch as f64); SaveSound is the logical per-source
/// record and the writer transposes it back into columns.
constexpr std::size_t kSaveSoundRecordSize = 28;
constexpr std::uint32_t kSaveMaxSounds = 0x7f;

struct SaveSound {
    std::int32_t x = 0;      ///< +0x00
    std::int32_t y = 0;      ///< +0x04
    std::int32_t z = 0;      ///< +0x08
    std::int32_t volume = 0; ///< +0x0c
    std::int32_t sample = 0; ///< +0x10: SOUND_SAMPLE_TABLE index
    double pitch = 0.0;      ///< +0x14
};

struct Save {
    SaveHeader header;
    std::vector<SaveBlock> blocks;
    std::optional<SavePlayer> player; ///< set when block 9 is present

    /// Typed blocks 1/2/3/6/7 (docs/SAVE_BLOCKS.md).  parse_save fills them
    /// from the payload; to_bytes re-encodes a block from these vectors when
    /// they are non-empty (block 3: when any presence flag is set) and falls
    /// back to SaveBlock::raw otherwise, so an all-zero count still round-trips.
    std::vector<SaveSwitch> switches; ///< block 1, file order
    std::vector<SaveDoor> doors;      ///< block 2, file order
    std::array<bool, kSaveThingSlots> thing_present{}; ///< block 3 slot flags
    std::vector<SaveThing> things;    ///< block 3 present slots, ascending
    std::vector<SaveSprite> sprites;  ///< block 6, file order
    std::vector<SaveSound> sounds;    ///< block 7, file order

    std::size_t size = 0;

    const SaveBlock* find_block(std::uint8_t id) const {
        for (const SaveBlock& b : blocks) {
            if (b.id == id) {
                return &b;
            }
        }
        return nullptr;
    }

    /// Serialise the header and the block chain (``docs/SAVE_WRITER.md``).
    ///
    /// The header is rebuilt from the typed fields; every block body except 9
    /// is copied from ``SaveBlock::raw`` (without its id byte), and block 9 is
    /// rebuilt from ``player`` when present (otherwise its raw bytes are
    /// used).  Blocks are emitted in vector order and ``0xff`` is appended, so
    /// a parsed, unmodified save reproduces the input byte-for-byte.
    ///
    /// Returns nullopt and fills ``error`` when a field cannot be represented
    /// (title/level too long, invalid player gauges, missing raw payload,
    /// duplicate id, oversized output, ...).  This is a structural check; use
    /// rebuild_save() for a re-parse guarantee.
    std::optional<std::vector<std::uint8_t>> to_bytes(std::string& error) const;
};

/// Parse the container, the block chain and the player block.
///
/// Returns nullopt and fills ``error`` on a malformed save.  Every read is
/// bounds-checked and every count is clamped to the same limits
/// tools/re/save_fields.py uses, so no path reads outside ``bytes``.  The
/// function does not require any particular set of blocks: a save without
/// block 9 parses with ``player == std::nullopt``.
std::optional<Save> parse_save(std::span<const std::uint8_t> bytes, std::string& error);

/// Serialise *save*, re-parse the result and refresh *save* from those bytes.
///
/// With ``verify`` the rebuilt model must be stable (serialising the re-parsed
/// save yields identical bytes) and the typed header/player fields must
/// survive the round-trip unchanged; otherwise the call fails and ``error``
/// explains which field was lost.  On success the block offsets and raw
/// payloads in *save* describe the newly built file, so further edits start
/// from a consistent snapshot.
std::optional<std::vector<std::uint8_t>> rebuild_save(Save& save, bool verify,
                                                      std::string& error);

/// Result of matching a save against the installed level scripts.
struct SaveLevelMatch {
    std::string level_file;  ///< basename of the matching ``.sbt``, e.g. "level0.sbt"
    std::size_t hits = 0;    ///< anchors of that file found in the save
    std::size_t first_hit = 0; ///< file offset of the first matched anchor
    std::size_t last_anchor = 0; ///< end of the last matched 256-byte anchor
};

/// Find the shipped ``.sbt`` whose body is embedded in *save*.
///
/// Mirrors tools/re/savefile.py: sample 256 bytes every 2048, skip chunks
/// that are mostly zero, require at least three matches, keep the file with
/// the most matches.  ``last_anchor`` is the same heuristic state-start
/// boundary savefile.py prints (it lies inside the SCRIPT record, so it is
/// only useful for reporting; the real block chain is parsed from +0x13c).
std::optional<SaveLevelMatch> find_save_level(std::span<const std::uint8_t> save,
                                              const std::filesystem::path& game,
                                              std::string& error);

} // namespace gm
