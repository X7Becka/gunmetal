#pragma once

#include "res/sou2.h" // PcmClip / RawClip are shared by the SOU decoders

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// Parsed SOU0/SOU1 container header (``sounds/SOU.DAT`` entries).
///
/// Both formats share the 16-byte header; only the payload codec differs:
/// ``SOU0`` is raw unsigned 8-bit PCM, ``SOU1`` is the 4-bit Miles ADPCM this
/// module decodes.  The header is documented in ``docs/FORMATS.md`` §5 and was
/// checked against all 999 entries of ``sounds/SOU.IDX``.
struct Sou1Info {
    std::string magic;         // "SOU0" or "SOU1"
    std::uint32_t sample_rate = 0;
    std::uint32_t flags = 0;   // 0 mono, 1 stereo
    std::uint32_t declared_length = 0;
    std::size_t payload_bytes = 0; // effective codec bytes (declared length, truncated, even)
    int channels = 1;
    int slots_per_byte = 0;    // 8 / 4 / 2 output slots per payload byte
};

/// Parse the SOU0/SOU1 container.  Returns nullopt for any other magic
/// (including SOU2 — see ``res/sou2.h``) or a malformed header.
///
/// ``length`` is exact for the mono entries and one greater than the payload
/// for the three stereo ones, so the effective payload is
/// ``min(length, size - 16)`` rounded down to an even byte count (the loader
/// masks the low bit at 0x40b0dd).
std::optional<Sou1Info> parse_sou1_header(std::span<const std::uint8_t> blob,
                                          std::string& error);

/// Decode a SOU1 blob into the raw 32-bit mixer accumulators.
///
/// One call decodes the whole payload: the per-channel ``{acc, level}`` state
/// starts at zero (voice start 0x40a5e0 zeroes it) and survives between mixer
/// frames, so a single call is equivalent to the game's 1024-slot frames.
/// Output is interleaved mono/stereo at the mixer rate (32 kHz) and keeps the
/// duplicated slots of the 8/16 kHz variants (4/2 copies per code).  The clip's
/// ``sample_rate`` is therefore 32000 for every entry; the source rate
/// (8000/16000/32000) is the ``Sou1Info::sample_rate`` field.
///
/// The codec (gundx.exe 0x852b40 / 0x852c00 / 0x852cc0):
///
///     code  = byte >> 4, then byte & 15
///     acc   = (acc + curve[base + level/4 + code]) mod 2^32
///     level = (level + step[code]) mod 2^32
///     emit acc `copies` times
///
/// ``volume`` is the decoder table index 0..31 (31 = full volume, the game's
/// ``[0x84a6a8] = volume - 1``); the reference hashes use 31.
std::optional<RawClip> decode_sou1_raw(std::span<const std::uint8_t> blob,
                                       std::string& error, int volume = 31);

/// Decode a SOU1 blob into 16-bit PCM: ``clamp(value >> shift)`` per sample.
///
/// ``shift`` defaults to 1, the presentation shared with ``decode_sou2`` and
/// ``tools/re/sou_decode.py``.  The decoder's full scale on the shipped corpus
/// is about 2^20, so ``shift = 5`` maps it to int16 without heavy clipping and
/// is what ``docs/manifests/sou1-fnv.json`` records as ``fnv1a64_pcm16`` (and
/// what ``sou1_to_wav`` writes).  The samples are the 32 kHz slot stream,
/// interleaved, duplicates included — exactly the raw accumulators.
std::optional<PcmClip> decode_sou1(std::span<const std::uint8_t> blob,
                                   std::string& error, int volume = 31, int shift = 1);

/// Render a SOU1 blob as a canonical 16-bit PCM WAV at the *source* rate
/// (8000/16000/32000 Hz), dropping the duplicated slots:
///
///     frame i = channel sample i * copies, clamp(value >> shift)
///
/// ``shift`` defaults to 5 (full-scale mapping, see ``decode_sou1``).  The
/// bytes are byte-identical to ``tools/re/sou1_model.py``'s ``write_wav``.
std::optional<std::vector<std::uint8_t>> sou1_to_wav(std::span<const std::uint8_t> blob,
                                                     std::string& error, int volume = 31,
                                                     int shift = 5);

} // namespace gm
