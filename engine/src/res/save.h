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
};

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
    std::array<std::int32_t, 4> shields{}; ///< struct+0x3250, front/back/left/right, hundredths
    std::int32_t weapon_slot = 0;     ///< struct+0x3294, 0..11
    std::uint32_t weapon_mask = 0;    ///< struct+0x3298, ``1 << slot`` per available slot
    std::array<std::uint32_t, 12> ammo{}; ///< struct+0x329c

    /// Headers of the two trailing {0x50 header, 0x3510 struct} pairs
    /// (``LEVEL0`` and an empty string in the shipped saves).
    std::array<std::string, 2> pair_headers{};

    /// Gauges in points (armor / 100.0), matching savefile.py's output.
    double armor_points() const { return static_cast<double>(armor) / kSaveGaugeScale; }
    double armor_max_points() const { return static_cast<double>(armor_max) / kSaveGaugeScale; }
    double shield_max_points() const { return static_cast<double>(shield_max) / kSaveGaugeScale; }
    double shield_points(int index) const {
        return static_cast<double>(shields[static_cast<std::size_t>(index)]) / kSaveGaugeScale;
    }
};

struct Save {
    SaveHeader header;
    std::vector<SaveBlock> blocks;
    std::optional<SavePlayer> player; ///< set when block 9 is present
    std::size_t size = 0;

    const SaveBlock* find_block(std::uint8_t id) const {
        for (const SaveBlock& b : blocks) {
            if (b.id == id) {
                return &b;
            }
        }
        return nullptr;
    }
};

/// Parse the container, the block chain and the player block.
///
/// Returns nullopt and fills ``error`` on a malformed save.  Every read is
/// bounds-checked and every count is clamped to the same limits
/// tools/re/save_fields.py uses, so no path reads outside ``bytes``.  The
/// function does not require any particular set of blocks: a save without
/// block 9 parses with ``player == std::nullopt``.
std::optional<Save> parse_save(std::span<const std::uint8_t> bytes, std::string& error);

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
