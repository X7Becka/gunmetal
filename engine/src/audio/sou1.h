#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm::audio {

/// Decoded SOU1 sound, Python-compatible (`gm/sou1.py::decode_entry`).
///
/// ``samples`` are the *mixer slots*: interleaved 32-bit accumulators at the
/// 32 kHz mixer rate, with the per-code duplication of the 8/16 kHz variants
/// kept (4/2 copies).  ``sample_rate`` is the source rate from the container
/// header (8000/16000/32000), not the slot rate; drop the duplicates
/// (``32 / sample_rate`` slots per frame) to resample.  The representation is
/// exactly what ``tools/parity_sou1.py`` hashes: the FNV manifest
/// ``docs/manifests/sou1-fnv.json`` pins ``samples`` little-endian for all 983
/// records.
struct Sou1Sound {
    std::uint32_t sample_rate = 0;
    int channels = 1;
    std::vector<std::int32_t> samples; // interleaved, duplicates kept
};

/// Parsed SOU1 container (16-byte header, `docs/FORMATS.md` §5).
struct Sou1Format {
    std::uint32_t sample_rate = 0;
    std::uint32_t flags = 0;           // 0 mono, 1 stereo
    std::uint32_t declared_length = 0; // header field at +12
    std::size_t payload_bytes = 0;     // effective codec bytes (even)
    int channels = 1;
    int slots_per_byte = 0; // 8 / 4 / 2 output slots per payload byte
};

struct Sou1Options {
    /// Delta-curve row 0..31; 31 is full volume, the manifest's setting.
    /// The game derives it as ``volume - 1`` (``[0x84a6a8]``).
    int volume = 31;
    /// Reject odd declared lengths instead of applying the loader's
    /// ``length & ~1`` mask.  The shipped corpus has 473 odd *declared*
    /// lengths whose trailing byte the loader ignores, so this is off by
    /// default: strict mode is for hostile input, not for parity.
    bool strict = false;
};

/// Parse the SOU1 header.  Fails on a truncated header, a foreign magic, a
/// zero sample rate, an implausible flag word, a declared length larger than
/// the blob, and (in strict mode) an odd declared length.  Never throws.
std::optional<Sou1Format> parse(std::span<const std::uint8_t> blob,
                                std::string& error, bool strict = false);

/// Decode a whole SOU1 blob.  ``sample_rate`` is the source rate, ``samples``
/// the interleaved slot stream (see ``Sou1Sound``).  ``volume`` 0..31 selects
/// the row of the shared delta curve; the decoded state starts at
/// ``acc = level = 0`` (voice start 0x40a5e0) and runs to the end of the
/// payload.  Returns nullopt with ``error`` set on any container defect or on
/// an adaptation level that leaves the delta curve (``overrun``); no exception
/// escapes.
std::optional<Sou1Sound> decode(std::span<const std::uint8_t> blob,
                                std::string& error, Sou1Options options = {});

/// Streaming reader: opens (and validates) one blob, then hands out the
/// interleaved slots in caller-sized chunks without duplicating the decoded
/// buffer.  ``read`` copies only into the caller's span and returns the number
/// of samples written; 0 means end of stream (``done()``).
class Sou1Stream {
public:
    static std::optional<Sou1Stream> open(std::span<const std::uint8_t> blob,
                                          std::string& error,
                                          Sou1Options options = {});

    std::uint32_t sample_rate() const { return sound_.sample_rate; }
    int channels() const { return sound_.channels; }
    std::size_t total_samples() const { return sound_.samples.size(); }
    std::size_t remaining() const { return sound_.samples.size() - cursor_; }
    bool done() const { return cursor_ >= sound_.samples.size(); }
    void rewind() { cursor_ = 0; }

    /// Copy up to ``out.size()`` interleaved samples; returns the count.
    std::size_t read(std::span<std::int32_t> out);

private:
    Sou1Sound sound_;
    std::size_t cursor_ = 0;
};

} // namespace gm::audio
