#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// One decoded 128x64 frame is always 8192 palette indices; the stored tile is
/// the transpose (64x128).  Loader 0x49fbc0 reads exactly 0x2000 bytes and the
/// blitter 0x49fe48 maps ``src[x*64+y] -> dst[y*128+x]``.
constexpr std::size_t kSofTileBytes = 0x2000;
constexpr int kSofTileWidth = 128;
constexpr int kSofTileHeight = 64;
constexpr int kSofTileSourceWidth = 64;
constexpr int kSofTileSourceHeight = 128;

/// Safety limits (docs/SECURITY.md): far above the 11637 shipped tiles.
constexpr std::size_t kSofMaxTiles = 1u << 20;
constexpr std::size_t kFdxMaxLines = 1u << 12;
constexpr std::uint32_t kFdxMaxCount = 1u << 20;
constexpr std::uint32_t kFdxMaxFps = 1000;

/// Whole ``.sof`` file, kept byte-exact so the writer also handles the partial
/// trailing tile some files carry.  ``tiles()`` excludes that tail.
struct Sof {
    std::vector<std::uint8_t> data;

    std::size_t tiles() const { return data.size() / kSofTileBytes; }
    std::size_t tail() const { return data.size() % kSofTileBytes; }
    bool aligned() const { return tail() == 0; }

    /// Stored tile (still 64x128, transposed).
    std::optional<std::span<const std::uint8_t>> tile(std::size_t index) const;
    /// Decoded 128x64 palette indices; nullopt when *index* is out of range.
    std::optional<std::vector<std::uint8_t>> frame(std::size_t index) const;
    std::vector<std::uint8_t> to_bytes() const { return data; }
};

/// Parse a ``.sof`` blob: a headerless stream of 0x2000-byte tiles.  The file
/// size is bounded by kSofMaxTiles; a partial final tile is preserved.
std::optional<Sof> parse_sof(std::span<const std::uint8_t> data, std::string& error);

/// Transpose helpers, exposed for round-trip tests.
std::vector<std::uint8_t> decode_sof_tile(std::span<const std::uint8_t> tile);
std::vector<std::uint8_t> encode_sof_tile(std::span<const std::uint8_t> pixels);

/// One ``count, fps`` line of a ``.fdx``/``Film/*.idx`` index.
struct FdxEntry {
    std::uint32_t count = 0;
    std::uint32_t fps = 0;
    std::size_t line = 0; // 0-based line in the source text
};

/// Text index used by the HUD loader 0x4958d0: the numeric suffix of
/// ``WEAPON8.SOF`` selects line 8 (``"%ld,%ld"`` parser 0x498e20).
struct Fdx {
    std::vector<std::uint8_t> data; // exact bytes, for a lossless writer
    std::vector<FdxEntry> entries;

    std::vector<std::uint8_t> to_bytes() const { return data; }
};

/// Parse and validate every non-blank line as ``count,fps``.  Blank lines are
/// preserved; ``fps <= 0`` is rejected like the engine's "invalid FPS" check.
std::optional<Fdx> parse_fdx(std::span<const std::uint8_t> data, std::string& error);

/// Canonical writer: ``"%d, %d"`` records separated by CRLF.
std::vector<std::uint8_t> build_fdx(const std::vector<FdxEntry>& entries);

} // namespace gm
