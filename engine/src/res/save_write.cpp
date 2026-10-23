// Strict front end of the C++ save writer (docs/SAVE_WRITE_CPP.md).
//
// The encoding itself lives in `Save::to_bytes()` (res/save.cpp), which already
// re-encodes the typed blocks 1/2/3/6/7/8/9 and copies every other payload out
// of `SaveBlock::raw`; a second copy of those 200+ lines of offsets here would
// only be a place to drift.  This file adds the things a caller needs before
// and after writing:
//
//   * model validation against the file it was parsed from,
//   * error codes instead of a bare string,
//   * optional opaque-header carry-over (bytes 0x10..0x50),
//   * a mandatory re-parse of the produced bytes with a typed comparison.
//
// Everything here is stdlib-only.
#include "res/save_write.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>

namespace gm {
namespace {

std::string hex(std::size_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%zx", value);
    return std::string(buf);
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Map a `Save::to_bytes()` message onto the public code.  The classes that
/// this file checks itself never reach here, so only a coarse split is needed.
SaveWriteCode classify_encode_error(const std::string& why) {
    if (contains(why, "too large")) {
        return SaveWriteCode::too_large;
    }
    if (contains(why, "no raw payload") || contains(why, "duplicate block") ||
        contains(why, "reserved for the terminator") ||
        contains(why, "unknown block id")) {
        return SaveWriteCode::block_layout;
    }
    return SaveWriteCode::not_representable;
}

/// The block chain must describe the file the model was parsed from: start at
/// the header boundary, no holes, and leave exactly one byte (the 0xff
/// terminator) at ``Save::size - 1``.
bool check_chain(const SaveGame& game, std::string& error, SaveWriteCode& code) {
    if (game.size == 0) {
        code = SaveWriteCode::block_bounds;
        error = "the model has no size (expected a parse_save() result)";
        return false;
    }
    if (game.size < kSaveHeaderSize + 2) {
        code = SaveWriteCode::block_bounds;
        error = "model size " + hex(game.size) + " is too short for a header and one block";
        return false;
    }

    std::array<bool, 256> seen{};
    std::size_t cursor = kSaveHeaderSize;
    for (const SaveBlock& block : game.blocks) {
        const std::string who = "block " + std::to_string(block.id);
        if (block.id == kSaveBlockEof) {
            code = SaveWriteCode::block_layout;
            error = "block id 0xff is the terminator, not a record";
            return false;
        }
        if (seen[block.id]) {
            code = SaveWriteCode::block_layout;
            error = "duplicate " + who;
            return false;
        }
        seen[block.id] = true;
        if (block.size == 0) {
            code = SaveWriteCode::block_bounds;
            error = who + " has size 0 (a parsed block is at least the id byte)";
            return false;
        }
        if (block.offset != cursor) {
            code = SaveWriteCode::block_bounds;
            error = who + " starts at " + hex(block.offset) + ", expected " + hex(cursor) +
                    " (the chain must be contiguous)";
            return false;
        }
        // cursor < game.size holds on entry, so this cannot wrap.
        if (block.size > game.size - cursor - 1) {
            code = SaveWriteCode::block_bounds;
            error = who + " ends at " + hex(cursor + block.size) + ", past the " +
                    hex(game.size) + "-byte file";
            return false;
        }
        if (block.raw.size() + 1 != block.size) {
            code = SaveWriteCode::block_bounds;
            error = who + ": raw payload is " + std::to_string(block.raw.size()) +
                    " bytes but the record size is " + std::to_string(block.size);
            return false;
        }
        cursor += block.size;
    }
    if (cursor + 1 != game.size) {
        code = SaveWriteCode::block_bounds;
        error = "block chain ends at " + hex(cursor) + " but the file is " + hex(game.size) +
                " bytes; expected the 0xff terminator at " + hex(game.size - 1);
        return false;
    }
    return true;
}

bool check_player_presence(const SaveGame& game, std::string& error, SaveWriteCode& code) {
    const bool has_block = game.find_block(9) != nullptr;
    if (game.player && !has_block) {
        code = SaveWriteCode::missing_player;
        error = "player state is set but the chain has no block 9";
        return false;
    }
    if (!game.player && has_block) {
        code = SaveWriteCode::missing_player;
        error = "the chain has block 9 but the model carries no player state";
        return false;
    }
    return true;
}

/// Header fields `parse_save()` reads; the opaque bytes are not part of it.
bool same_header(const SaveHeader& a, const SaveHeader& b) {
    return a.title == b.title && a.version == b.version && a.reserved == b.reserved &&
           a.level_name == b.level_name && a.fields == b.fields &&
           a.name_slots == b.name_slots;
}

bool same_player_fields(const SavePlayer& a, const SavePlayer& b) {
    if (a.has_slot != b.has_slot || a.present != b.present) {
        return false;
    }
    if (!a.has_slot) {
        return true;
    }
    return a.position == b.position && a.angles == b.angles &&
           a.history_index == b.history_index && a.exists == b.exists &&
           a.status_present == b.status_present && a.object_id == b.object_id &&
           a.armor_max == b.armor_max && a.armor == b.armor && a.shield_max == b.shield_max &&
           a.shields == b.shields && a.weapon_slot == b.weapon_slot &&
           a.weapon_mask == b.weapon_mask && a.ammo == b.ammo &&
           a.pair_headers == b.pair_headers;
}

/// Re-parse the bytes we are about to hand out and check that the model is
/// still inside them.  This is what makes ``true`` mean "the game reads the
/// same save we meant to write" as far as this project's reader can tell.
bool verify_round_trip(const SaveGame& game, const std::vector<std::uint8_t>& bytes,
                       std::string& error) {
    std::string parse_error;
    const std::optional<Save> again = parse_save(bytes, parse_error);
    if (!again) {
        error = "written save does not re-parse: " + parse_error;
        return false;
    }
    if (again->size != bytes.size()) {
        error = "written save re-parsed as " + std::to_string(again->size) + " bytes, wrote " +
                std::to_string(bytes.size());
        return false;
    }
    if (again->blocks.size() != game.blocks.size()) {
        error = "written save has " + std::to_string(again->blocks.size()) +
                " blocks, model has " + std::to_string(game.blocks.size());
        return false;
    }
    for (std::size_t i = 0; i < game.blocks.size(); ++i) {
        const SaveBlock& want = game.blocks[i];
        const SaveBlock& got = again->blocks[i];
        if (want.id != got.id || want.offset != got.offset || want.size != got.size) {
            error = "block " + std::to_string(i) + " moved: id " + std::to_string(want.id) +
                    " at " + hex(want.offset) + "+" + std::to_string(want.size) +
                    " became id " + std::to_string(got.id) + " at " + hex(got.offset) + "+" +
                    std::to_string(got.size);
            return false;
        }
    }
    if (!same_header(game.header, again->header)) {
        error = "header did not survive the round-trip";
        return false;
    }
    if (game.player && (!again->player || !same_player_fields(*game.player, *again->player))) {
        error = "player fields did not survive the round-trip";
        return false;
    }
    if (!game.player && again->player) {
        error = "re-parsed save gained a player block the model does not have";
        return false;
    }
    return true;
}

bool fail(std::vector<std::uint8_t>& out, std::string* error, SaveWriteCode* code,
          SaveWriteCode value, std::string message) {
    out.clear();
    if (code) {
        *code = value;
    }
    if (error) {
        *error = "write_save: " + std::move(message);
    }
    return false;
}

} // namespace

const char* save_write_code_name(SaveWriteCode code) {
    switch (code) {
    case SaveWriteCode::ok:
        return "ok";
    case SaveWriteCode::no_blocks:
        return "no_blocks";
    case SaveWriteCode::block_bounds:
        return "block_bounds";
    case SaveWriteCode::block_layout:
        return "block_layout";
    case SaveWriteCode::missing_player:
        return "missing_player";
    case SaveWriteCode::not_representable:
        return "not_representable";
    case SaveWriteCode::too_large:
        return "too_large";
    case SaveWriteCode::no_round_trip:
        return "no_round_trip";
    }
    return "unknown";
}

bool write_save(const SaveGame& game, std::vector<std::uint8_t>& out, std::string* error,
                SaveWriteCode* code, std::span<const std::uint8_t> original) {
    out.clear();
    if (error) {
        error->clear();
    }
    if (code) {
        *code = SaveWriteCode::ok;
    }

    if (game.blocks.empty()) {
        return fail(out, error, code, SaveWriteCode::no_blocks,
                    "the model has no blocks; a save always carries a typed block chain "
                    "(parse_save() fills Save::blocks)");
    }
    std::string why;
    SaveWriteCode value = SaveWriteCode::ok;
    if (!check_chain(game, why, value)) {
        return fail(out, error, code, value, why);
    }
    if (!check_player_presence(game, why, value)) {
        return fail(out, error, code, value, why);
    }
    if (!original.empty() && original.size() != game.size) {
        return fail(out, error, code, SaveWriteCode::block_bounds,
                    "original is " + std::to_string(original.size()) + " bytes, the model says " +
                        std::to_string(game.size));
    }

    std::optional<std::vector<std::uint8_t>> encoded = game.to_bytes(why);
    if (!encoded) {
        return fail(out, error, code, classify_encode_error(why), why);
    }

    // Bytes 0x10..0x50 are never read back, so they are safe to carry over
    // verbatim; everything else in the header is rebuilt from typed fields.
    if (!original.empty()) {
        std::memcpy(encoded->data() + kSaveOpaqueHeaderAt,
                    original.data() + kSaveOpaqueHeaderAt, kSaveOpaqueHeaderLen);
    }

    if (!verify_round_trip(game, *encoded, why)) {
        return fail(out, error, code, SaveWriteCode::no_round_trip, why);
    }

    out = std::move(*encoded);
    return true;
}

} // namespace gm
