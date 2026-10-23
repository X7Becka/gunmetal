#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "res/save.h"

namespace gm {

/// Engine-facing save writer (``M8.7``/``M9.4``, docs/SAVE_WRITE_CPP.md).
///
/// ``parse_save()`` (res/save.h) produces a `Save`: the typed header, the
/// block chain with every payload kept verbatim in ``SaveBlock::raw`` and the
/// typed views of blocks 1/2/3/6/7/8/9.  `Save::to_bytes()` already encodes
/// that model; `write_save()` is the stricter entry point used by callers that
/// must not hand a half-built file to the game:
///
///   * it validates the model against itself before encoding (block chain
///     contiguous from `kSaveHeaderSize`, ``offset``/``size`` inside
///     ``Save::size``, ``raw.size() == size - 1``, no duplicate/0xff/unknown
///     id, block 9 present exactly when ``Save::player`` is);
///   * it can carry the opaque part of the header (bytes 0x10..0x50, which
///     `parse_save()` never interprets) over from the original file, so a save
///     with non-zero padding still round-trips byte for byte;
///   * it re-parses the result and refuses to return bytes that do not decode
///     back to the same header, block chain and player fields.
///
/// Unknown/unparsed *block* bodies are preserved as opaque bytes by the
/// encoding itself (``SaveBlock::raw``), including blocks 4/5/11/12/15/16/17/
/// 18/19/20 that have no typed C++ view yet — see docs/SAVE_BLOCKS.md §1.
///
/// The model type is `Save`; `SaveGame` is the name used by the writer task
/// and by this API.
using SaveGame = Save;

/// Header bytes no reader interprets: 0x10..0x50 between the title/version
/// word and the ``reserved`` dword.  `parse_save()` skips them, so a canonical
/// writer fills them with zero; `write_save(..., original)` copies them from
/// the input instead.
constexpr std::size_t kSaveOpaqueHeaderAt = 0x10;
constexpr std::size_t kSaveOpaqueHeaderLen = 0x40;

/// Why `write_save()` refused a model.  The text in ``error`` is meant for a
/// human, the code for a caller that has to branch (a CLI exit status, a
/// fallback to ``Save::to_bytes()``, a diagnostic screen).
enum class SaveWriteCode {
    ok = 0,
    no_blocks,         ///< the model has no block chain at all
    block_bounds,      ///< offset/size/size accounting outside the file
    block_layout,      ///< duplicate/0xff id, raw and size disagree, no payload
    missing_player,    ///< block 9 without ``player`` or ``player`` without block 9
    not_representable, ///< a typed field cannot be encoded (title, gauges, counts)
    too_large,         ///< the result would exceed the 64 MiB container cap
    no_round_trip,     ///< the encoded bytes do not re-parse to the same model
};

/// Stable name of *code* (``"ok"``, ``"block_bounds"``, ...); never null.
const char* save_write_code_name(SaveWriteCode code);

/// Serialise *game* into *out*.
///
/// ``out`` is cleared on every call and only filled on success, so a failed
/// write never leaves a half-valid file behind.  *error* (when not null)
/// receives ``"write_save: <reason>"`` on failure and is cleared on success;
/// *code* (when not null) receives the reason code.
///
/// When *original* is non-empty it must be the exact file the model was parsed
/// from (same size); its opaque header bytes (0x10..0x50) are copied into the
/// result.  Without it those bytes are written as zero, which is what the two
/// shipped saves contain.
///
/// The bytes are always a complete container: ``kSaveHeaderSize`` header,
/// every block of ``game.blocks`` in vector order, then the 0xff terminator.
/// Identical rule to ``Save::to_bytes()`` and to the Python writer
/// ``tools/re/save_rebuild.py``.
bool write_save(const SaveGame& game, std::vector<std::uint8_t>& out,
                std::string* error, SaveWriteCode* code = nullptr,
                std::span<const std::uint8_t> original = {});

} // namespace gm
