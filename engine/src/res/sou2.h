#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// PCM clip decoded from a SOU2 blob (``MUSIC/SOU.DAT`` entries).
///
/// ``channels`` is always 2.  ``samples`` holds interleaved left/right 16-bit
/// samples produced from the raw 32-bit mix accumulators with
/// ``clamp(value >> 1)`` (the same convention tools/re/sou_decode.py uses for
/// the SOU1 decoders).
struct PcmClip {
    int sample_rate = 0;
    int channels = 0;
    std::vector<std::int16_t> samples; // interleaved
};

/// Raw 32-bit mix accumulators straight out of the codec.  Used by the parity
/// test (the 16-bit conversion saturates, which could hide small differences)
/// and by callers that want to apply their own volume/mixing.
struct RawClip {
    int sample_rate = 0;
    int channels = 0;
    std::vector<std::int32_t> samples; // interleaved
};

/// Decode one SOU2 blob into 16-bit stereo PCM.
///
/// Container (verified against all 3092 entries of MUSIC/SOU.DAT):
///
///     char magic[4]   "SOU2"
///     u32  sample_rate     32000 for 3090 entries, 16000 for 2
///     u32  frames          stereo frame count (meta1)
///     u32  meta2           not read by the loader (checksum/id of the source)
///     u32  units           payload = units * 4 bytes
///     u8   payload[units * 4]
///
/// The payload is engine "format 4": a bit stream shared by 13 interleaved
/// sub-streams (16 sub-streams when ``frames`` is not a multiple of 32 are not
/// used by the corpus).  The mixer (gundx.exe 0x409658) decodes one 1024-frame
/// block at a time by calling 0x852d80 once per sub-stream.  Sub-stream ``c``
/// owns the frame positions ``off + k * esi`` with
///
///     esi = 8 << (c >> 2)                         (c == 12: 8)
///     off = ((c & 3) << ((c >> 2) + 1)) + (1 << (c >> 2)) - 1   (c == 12: 7)
///
/// which partitions the 1024 frames exactly.  Every call starts with a level
/// (and, for levels 1/2, a shift) delta in unary+sign form, followed by a
/// 10-bit selector and the samples.  The per-sub-stream level/shift/last
/// sample/table cursor survive between blocks; the bit cursor is shared by all
/// 13 calls and is therefore advanced sequentially through the payload.
///
/// The bit stream consists of 32-bit little-endian dwords read MSB first.  The
/// codec is *not* the SOU1 nibble DPCM: the tables at 0x850480 / 0x850500
/// belong to 0x852b40 / 0x852c00 / 0x852cc0, while format 4 uses its own step
/// and gain tables at 0x8522c0 / 0x852340.  The gain table is a long unaligned
/// dword table (the per-sub-stream byte cursor reaches 0x6c0), so the exact
/// image of 0x852340..0x852ac0 is embedded in the decoder.
///
/// Shipped anomalies: L04I01 has 0 frames and no payload, while L06A01 claims
/// 1 frame and units == 0 even though the entry carries 90592 bytes of data.
/// The loader allocates ``units * 4`` bytes, so L06A01 has no code stream and
/// decodes as one silent frame; the trailing bytes are ignored (they are not
/// referenced by the container).
///
/// ``volume`` is the music volume global 0x84a68c (0..32; the game's mixer
/// uses the configured value).  The reference hashes use 32, the maximum.
///
/// Non-zero ``error`` is set (and nullopt returned) on a malformed container
/// or a truncated bit stream.  Levels the engine cannot decode leave the
/// sub-stream silent for that block (the engine's 0x852f36 path) rather than
/// failing the whole clip, matching the original mixer.
std::optional<PcmClip> decode_sou2(std::span<const std::uint8_t> blob, std::string& error,
                                   int volume = 32);

/// Same decode, but returns the raw 32-bit accumulators (before >>1/clamp).
std::optional<RawClip> decode_sou2_raw(std::span<const std::uint8_t> blob, std::string& error,
                                       int volume = 32);

} // namespace gm
