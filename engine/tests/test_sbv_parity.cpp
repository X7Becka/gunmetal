// Full-corpus SBV decoder parity test (C++ vs the Python reference).
//
// Unlike `test_sbv_video.cpp` (all 500 Intro frames + a sample of the other two
// clips), this sweep pins **every** shipped frame: Intro.sbv (500),
// orient.sbv (4523) and winner.sbv (1820) -- 6843 frames in total.  Each frame
// block is decoded by `gm::decode_sbv_frame` and the packed RGB is hashed with
// FNV-1a 64.
//
// Goldens were produced by decoding the same frame blocks with
// `gm.sbv.Sbv.decode_frame` (the 1:1 transliteration of gundx.exe 0x49ee8c /
// 0x49e33b) and hashing the RGB buffer with FNV-1a 64 (offset basis
// 0xcbf29ce484222325, prime 0x100000001b3).  Per clip we pin
//
//   * the frame count, file size, data_start and the block-size range,
//   * the **chained** hash over all frames: the per-frame 64-bit hash, written
//     little-endian, is folded into one running FNV-1a 64 state, so the
//     aggregate pins every frame in playback order,
//   * six reference frames (first, second, quarter, half, three quarters, last)
//     with their block size and RGB hash,
//   * a global chain over all three clips in the order above.
//
// The sweep then decodes the whole corpus a second time in reverse order and
// compares every per-frame hash with the first pass: frames are intra-coded, so
// the result must be independent of both repetition and ordering.
//
// Reference manifest (temporary, uncommitted):
//   python3 /tmp/opencode/sbv_fnv_gen.py   # -> per-clip aggregate + per-frame
// The test needs no Python at runtime: all goldens are compile-time constants.
#include "res/sbv.h"
#include "res/sbv_video.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

constexpr std::uint64_t kFnvBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
constexpr int kWidth = 320;
constexpr int kHeight = 160;
constexpr std::size_t kRgbBytes = static_cast<std::size_t>(kWidth) * kHeight * 3;

std::uint64_t fnv1a64(std::span<const std::uint8_t> data, std::uint64_t hash = kFnvBasis) {
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

/// Folds one per-frame hash (little-endian, 8 bytes) into the running chain.
std::uint64_t chain_frame(std::uint64_t hash, std::uint64_t frame_hash) {
    for (int i = 0; i < 8; ++i) {
        hash ^= (frame_hash >> (8 * i)) & 0xFFu;
        hash *= kFnvPrime;
    }
    return hash;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

struct RefFrame {
    int index;
    std::size_t block_size;
    std::uint64_t hash;
};

struct ClipGolden {
    const char* name;
    std::uint32_t frames;
    std::size_t file_size;
    std::size_t data_start;
    std::size_t min_block;
    std::size_t max_block;
    std::uint64_t chain;
    const RefFrame* refs;
    std::size_t ref_count;
};

// Python reference (`gm/sbv.py`, `decode_frame_rgb` -> FNV-1a 64).
constexpr RefFrame kIntroRefs[] = {
    {0, 5760, 0x8ac5f7c4f914345dull},   {1, 5640, 0xd3a4d69491bda01bull},
    {125, 5112, 0x074912723f1fc1d5ull}, {250, 4404, 0x9c3a91a85343b5e5ull},
    {375, 6472, 0x58b321bfee088dfaull}, {499, 1572, 0x208c7f4628e056b2ull},
};
constexpr RefFrame kOrientRefs[] = {
    {0, 1456, 0x1100fdb97cd50325ull},    {1, 1456, 0x1100fdb97cd50325ull},
    {1130, 8308, 0xc1819a9d22de6293ull}, {2261, 7820, 0x636004fbe922406dull},
    {3392, 8212, 0xfd9bf9f95d0c8d11ull}, {4522, 1456, 0x1100fdb97cd50325ull},
};
constexpr RefFrame kWinnerRefs[] = {
    {0, 10928, 0x5e889934213b528eull},  {1, 10956, 0xaabf5de9167fbdd2ull},
    {455, 8396, 0xc86374fe230c4218ull}, {910, 8612, 0xd0f4f81336e8526eull},
    {1365, 9176, 0xac27ff382e94d056ull}, {1819, 1456, 0x1100fdb97cd50325ull},
};

constexpr ClipGolden kClips[] = {
    {"Intro.sbv", 500, 2811092, 4016, 1572, 13928, 0x7021db0159c3775aull, kIntroRefs,
     std::size(kIntroRefs)},
    {"orient.sbv", 4523, 35005220, 36200, 1456, 13560, 0xeb2467c98a7902f2ull, kOrientRefs,
     std::size(kOrientRefs)},
    {"winner.sbv", 1820, 15343896, 14576, 1456, 14732, 0x709a638456a11cc3ull, kWinnerRefs,
     std::size(kWinnerRefs)},
};

constexpr std::uint32_t kTotalFrames = 6843;
constexpr std::uint64_t kGlobalChain = 0xcd8a6e8d5a29cb1full;

const fs::path kVideoDir = "/mnt/d/games/Gunmetal 3dfx/Vid";

/// Decodes every frame of one clip, then re-decodes it backwards.  Returns the
/// number of frames decoded (0 when the clip is missing/skipped) and folds the
/// chained hash into *chain.
std::uint32_t check_clip(const ClipGolden& golden, std::uint64_t& chain) {
    const fs::path path = kVideoDir / golden.name;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        std::printf("sbv_parity: %s not found, skipped\n", path.string().c_str());
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> raw = read_file(path);
    std::string error;
    const auto clip = gm::parse_sbv(raw, error);
    CHECK(clip.has_value());
    if (!clip) {
        std::fprintf(stderr, "sbv_parity: %s: %s\n", golden.name, error.c_str());
        return 0;
    }

    // --- container -------------------------------------------------------
    CHECK(clip->width == static_cast<std::uint32_t>(kWidth));
    CHECK(clip->height == static_cast<std::uint32_t>(kHeight));
    CHECK(clip->frame_count == golden.frames);
    CHECK(clip->file_size == golden.file_size);
    CHECK(clip->file_size == raw.size());
    CHECK(clip->data_start == golden.data_start);
    CHECK(clip->data_start == 16 + 8 * static_cast<std::size_t>(golden.frames));
    CHECK(clip->table.size() == golden.frames);
    CHECK(gm::sbv_offsets_valid(*clip));

    const std::size_t n = clip->table.size();
    std::vector<std::uint64_t> hashes(n, 0);
    std::vector<std::size_t> sizes(n, 0);
    std::size_t sum_blocks = 0;
    std::size_t min_block = SIZE_MAX;
    std::size_t max_block = 0;
    // Per-clip aggregate, independent of the global chain folded by the caller.
    std::uint64_t clip_chain = kFnvBasis;

    // --- pass 1: forward, every frame ------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t begin =
            clip->data_start + (i > 0 ? clip->table[i - 1].end_offset : 0u);
        const std::size_t end = clip->data_start + clip->table[i].end_offset;
        // Offsets must stay inside the file and be monotonically non-decreasing
        // (parse_sbv already enforces this; re-asserted here for the sweep).
        if (begin > end || end > raw.size() || begin < clip->data_start) {
            std::fprintf(stderr, "sbv_parity: %s frame %zu: bad range [%zu, %zu) of %zu\n",
                         golden.name, i, begin, end, raw.size());
            ++g_failures;
            continue;
        }
        const std::size_t size = end - begin;
        if (size == 0 || size < golden.min_block || size > golden.max_block) {
            std::fprintf(stderr, "sbv_parity: %s frame %zu: block size %zu outside [%zu, %zu]\n",
                         golden.name, i, size, golden.min_block, golden.max_block);
            ++g_failures;
        }
        sizes[i] = size;
        sum_blocks += size;

        const std::span<const std::uint8_t> block(raw.data() + begin, size);
        const auto frame = gm::decode_sbv_frame(block, kWidth, kHeight, error);
        if (!frame) {
            std::fprintf(stderr, "sbv_parity: %s frame %zu: decode failed: %s\n", golden.name, i,
                         error.c_str());
            ++g_failures;
            continue;
        }
        CHECK(frame->width == kWidth);
        CHECK(frame->height == kHeight);
        CHECK(frame->rgb.size() == kRgbBytes);
        hashes[i] = fnv1a64(frame->rgb);
        min_block = std::min(min_block, size);
        max_block = std::max(max_block, size);
        clip_chain = chain_frame(clip_chain, hashes[i]);
        chain = chain_frame(chain, hashes[i]);
    }

    CHECK(sum_blocks + clip->data_start == raw.size());
    CHECK(min_block == golden.min_block);
    CHECK(max_block == golden.max_block);
    if (clip_chain != golden.chain) {
        std::fprintf(stderr,
                     "sbv_parity: %s chain %016llx != %016llx (frames %zu/%u decoded)\n",
                     golden.name, static_cast<unsigned long long>(clip_chain),
                     static_cast<unsigned long long>(golden.chain), n, golden.frames);
        ++g_failures;
    }

    // --- reference frames -------------------------------------------------
    for (std::size_t r = 0; r < golden.ref_count; ++r) {
        const RefFrame& ref = golden.refs[r];
        if (ref.index < 0 || static_cast<std::size_t>(ref.index) >= n) {
            std::fprintf(stderr, "sbv_parity: %s bad ref index %d\n", golden.name, ref.index);
            ++g_failures;
            continue;
        }
        const auto at = static_cast<std::size_t>(ref.index);
        if (sizes[at] != ref.block_size || hashes[at] != ref.hash) {
            std::fprintf(stderr,
                         "sbv_parity: %s frame %d: size %zu/%zu hash %016llx/%016llx mismatch\n",
                         golden.name, ref.index, sizes[at], ref.block_size,
                         static_cast<unsigned long long>(hashes[at]),
                         static_cast<unsigned long long>(ref.hash));
            ++g_failures;
        }
    }

    // --- pass 2: reverse order, per-frame equality ------------------------
    std::size_t rechecked = 0;
    for (std::size_t i = n; i-- > 0;) {
        const std::size_t begin =
            clip->data_start + (i > 0 ? clip->table[i - 1].end_offset : 0u);
        const std::size_t end = clip->data_start + clip->table[i].end_offset;
        if (begin > end || end > raw.size()) {
            ++g_failures;
            continue;
        }
        const auto frame = gm::decode_sbv_frame(
            std::span<const std::uint8_t>(raw.data() + begin, end - begin), kWidth, kHeight,
            error);
        if (!frame || frame->rgb.size() != kRgbBytes || fnv1a64(frame->rgb) != hashes[i]) {
            std::fprintf(stderr, "sbv_parity: %s frame %zu: non-deterministic re-decode\n",
                         golden.name, i);
            ++g_failures;
            continue;
        }
        ++rechecked;
    }
    CHECK(rechecked == n);

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("sbv_parity: %-11s %4zu frames, chain %016llx, %.2f s (%zu re-decoded)\n",
                golden.name, n, static_cast<unsigned long long>(clip_chain), seconds, rechecked);
    return static_cast<std::uint32_t>(n);
}

// A block clipped to its plane header must be rejected, never read past its end.
// `PlaneMemory` reports any out-of-bounds plane access and `decode_sbv_frame`
// turns that into `nullopt`, so a successful decode over the whole corpus is
// also the "no OOB" proof; this is the explicit negative case.
void test_truncated_block() {
    const fs::path path = kVideoDir / "Intro.sbv";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return;
    }
    const std::vector<std::uint8_t> raw = read_file(path);
    std::string error;
    const auto clip = gm::parse_sbv(raw, error);
    CHECK(clip.has_value());
    if (!clip) {
        return;
    }
    const std::size_t end = clip->data_start + clip->table[0].end_offset;
    const std::span<const std::uint8_t> block(raw.data() + clip->data_start,
                                              end - clip->data_start);
    // The luma base grid alone needs 8 bounds + 1600 samples, so every cut
    // below 1608 bytes must be rejected before the entropy stream starts.
    for (const std::size_t cut : {std::size_t{0}, std::size_t{4}, std::size_t{12},
                                  std::size_t{100}, std::size_t{1000}, std::size_t{1500}}) {
        const auto frame = gm::decode_sbv_frame(block.first(cut), kWidth, kHeight, error);
        if (frame) {
            std::fprintf(stderr, "sbv_parity: truncated block (%zu bytes) was accepted\n", cut);
            ++g_failures;
        }
    }
}

} // namespace

int main() {
    std::uint64_t chain = kFnvBasis;
    std::uint32_t frames = 0;
    for (const ClipGolden& golden : kClips) {
        frames += check_clip(golden, chain);
    }
    test_truncated_block();

    if (frames != 0) {
        CHECK(frames == kTotalFrames);
        CHECK(chain == kGlobalChain);
        if (chain != kGlobalChain) {
            std::fprintf(stderr, "sbv_parity: global chain %016llx != %016llx\n",
                         static_cast<unsigned long long>(chain),
                         static_cast<unsigned long long>(kGlobalChain));
        }
    } else {
        std::printf("sbv_parity: no clips found, skipped\n");
        return g_failures == 0 ? 0 : 1;
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "sbv_parity: %d checks failed (%u/%u frames)\n", g_failures, frames,
                     kTotalFrames);
        return 1;
    }
    std::printf("sbv_parity: %u/%u frames match the Python RGB reference\n", frames, kTotalFrames);
    return 0;
}
