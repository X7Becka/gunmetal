#include "audio/sou1.h"

#include "core/bytes.h"
#include "res/sou1.h"

#include <algorithm>
#include <cstring>

namespace gm::audio {
namespace {

// The codec itself lives in res/sou1.cpp: one table image (the 0x1d80-byte
// delta curve at 0x850540, the 16 step words at 0x850500 and the 32 row
// pointers at 0x850480) and one nibble loop shared with the res API and the
// parity test.  This module adds the source-rate/cursor presentation and the
// container checks the streaming API needs; it does not re-implement the
// tables (a second copy is exactly how codecs drift apart).
constexpr std::size_t kHeaderBytes = 16;

int slots_per_byte(std::uint32_t rate) {
    if (rate < 10000) {
        return 8; // four output copies per code, 0x852cc0
    }
    if (rate < 18000) {
        return 4; // two copies, 0x852c00
    }
    return 2;     // one copy, 0x852b40
}

} // namespace

std::optional<Sou1Format> parse(std::span<const std::uint8_t> blob,
                                std::string& error, bool strict) {
    if (blob.size() < 4) {
        error = "truncated SOU1 magic (" + std::to_string(blob.size()) + " bytes)";
        return std::nullopt;
    }
    if (std::memcmp(blob.data(), "SOU1", 4) != 0) {
        error = "not a SOU1 blob (magic '" +
                std::string(reinterpret_cast<const char*>(blob.data()), 4) + "')";
        return std::nullopt;
    }
    if (blob.size() < kHeaderBytes) {
        error = "truncated SOU1 header (" + std::to_string(blob.size()) + " bytes)";
        return std::nullopt;
    }

    Sou1Format format;
    format.sample_rate = rd32(blob.data() + 4);
    format.flags = rd32(blob.data() + 8);
    format.declared_length = rd32(blob.data() + 12);
    if (format.sample_rate == 0) {
        error = "SOU1 header has sample rate 0";
        return std::nullopt;
    }
    if (format.flags & ~std::uint32_t{1}) {
        error = "implausible SOU1 flag word " + std::to_string(format.flags);
        return std::nullopt;
    }

    const std::size_t available = blob.size() - kHeaderBytes;
    // The loader stores `length = payload + 1` for the stereo entries (its
    // second-half offset is length/2), so a stereo blob may declare one byte
    // more than it carries; Python clamps with min(length, available).  A
    // larger overrun is a real truncation.
    const std::size_t slack = (format.flags & 1u) ? 1 : 0;
    if (format.declared_length > available + slack) {
        error = "truncated SOU1 payload: header declares " +
                std::to_string(format.declared_length) + " bytes, " +
                std::to_string(available) + " present";
        return std::nullopt;
    }
    if (strict && (format.declared_length & 1u)) {
        error = "odd SOU1 payload length " + std::to_string(format.declared_length);
        return std::nullopt;
    }
    // The loader masks the low bit (0x40b0dd): a trailing odd byte is ignored.
    format.payload_bytes =
        std::min<std::size_t>(format.declared_length, available) & ~std::size_t{1};
    format.channels = 1 + static_cast<int>(format.flags & 1u);
    format.slots_per_byte = slots_per_byte(format.sample_rate);
    return format;
}

std::optional<Sou1Sound> decode(std::span<const std::uint8_t> blob,
                                std::string& error, Sou1Options options) {
    const auto format = parse(blob, error, options.strict);
    if (!format) {
        return std::nullopt;
    }
    const auto raw = gm::decode_sou1_raw(blob, error, options.volume);
    if (!raw) {
        return std::nullopt;
    }

    Sou1Sound sound;
    sound.sample_rate = format->sample_rate;
    sound.channels = raw->channels;
    sound.samples = std::move(raw->samples);
    return sound;
}

std::optional<Sou1Stream> Sou1Stream::open(std::span<const std::uint8_t> blob,
                                           std::string& error,
                                           Sou1Options options) {
    auto sound = decode(blob, error, options);
    if (!sound) {
        return std::nullopt;
    }
    Sou1Stream stream;
    stream.sound_ = std::move(*sound);
    return stream;
}

std::size_t Sou1Stream::read(std::span<std::int32_t> out) {
    const std::size_t n = std::min(out.size(), remaining());
    std::copy_n(sound_.samples.data() + cursor_, n, out.data());
    cursor_ += n;
    return n;
}

} // namespace gm::audio
