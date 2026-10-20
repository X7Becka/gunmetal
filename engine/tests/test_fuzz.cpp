// In-process fuzz matrix for the C++ resource readers.
//
// This is the C++ twin of tools/fuzz_formats.py: every reader is fed empty,
// short-truncated (all cuts 0..256), randomly truncated, bit-flipped and
// extreme-dword inputs.  A case may parse successfully or reject the input
// (``nullopt``/``false``/an exception); it may not crash, hang, throw a
// non-standard exception, or turn a handful of header bytes into a huge
// allocation.
//
// Resource limits:
//   * exactly one wall-clock budget per case (checked after the call; a real
//     hang is caught by the CTest TIMEOUT property on the `fuzz` test),
//   * a hard cap on the number of cases, so a bug in a generator cannot make
//     the suite run forever,
//   * a replacement global operator new that refuses any single allocation
//     above kMaxSingleAllocation and marks the case as a trust failure.  This
//     catches counters like "texture count = 0xFFFFFFFF" before the OS has to
//     deal with a 100 GiB reserve.
//
// Run directly:
//   ./gm_fuzz_tests --quick     # default, a few thousand cases
//   ./gm_fuzz_tests --full      # more random cuts/flips/payload mutations
//
// The shipped game files are optional seeds: without them the synthetic
// seeds still exercise every parser entry point.  Real files are read
// read-only and capped at kMaxSeedBytes.
#include "res/bigfile.h"
#include "res/sbc.h"
#include "res/sbm.h"
#include "res/sbt.h"
#include "res/sbv.h"
#include "res/sbv_video.h"
#include "res/sbw.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<std::uint8_t>;
using ParseFn = void (*)(std::span<const std::uint8_t>);

int g_failures = 0;

// Any single allocation above this is a trust-boundary failure for an input
// that is at most a few hundred kilobytes.  The largest legitimate buffer in
// this suite is an 8 MiB seed copy and a ~1 MiB SBV plane buffer.
constexpr std::size_t kMaxSingleAllocation = 64u << 20; // 64 MiB
constexpr std::size_t kMaxSeedBytes = 8u << 20;         // 8 MiB

bool g_oversized_allocation = false;

void put_u32(Bytes& out, std::size_t at, std::uint32_t value) {
    out[at + 0] = static_cast<std::uint8_t>(value & 0xFF);
    out[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

void append_u32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

bool read_file(const fs::path& path, Bytes& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.size() > kMaxSeedBytes) {
        out.resize(kMaxSeedBytes);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Allocation guard.  Replacing the global operator new is the only portable
// way to observe the size of an allocation inside a third-party (static)
// library on this toolchain.
// ---------------------------------------------------------------------------
void* operator_new_or_throw(std::size_t size) {
    if (size > kMaxSingleAllocation) {
        g_oversized_allocation = true;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

// ---------------------------------------------------------------------------
// Case drivers.
// ---------------------------------------------------------------------------
class Fuzzer {
public:
    Fuzzer(std::size_t max_cases, std::size_t case_budget_ms)
        : max_cases_(max_cases), case_budget_ms_(case_budget_ms) {}

    bool budget_left() const { return cases_ < max_cases_; }

    void fail(const std::string& message) {
        ++failure_count_;
        if (failures_.size() < 40) {
            failures_.push_back(message);
        }
    }

    template <typename Fn>
    void check(const std::string& label, Fn&& fn) {
        if (!budget_left()) {
            budget_hit_ = true;
            return;
        }
        ++cases_;
        g_oversized_allocation = false;
        const auto start = std::chrono::steady_clock::now();
        try {
            fn();
        } catch (const std::bad_alloc&) {
            const bool oversized = g_oversized_allocation;
            g_oversized_allocation = false;
            fail(label + (oversized
                              ? ": tried to allocate more than 64 MiB"
                              : ": std::bad_alloc"));
        } catch (const std::exception&) {
            // A normal exception is an acceptable rejection path for a
            // malformed blob (std::string/vector may throw length_error).
        } catch (...) {
            fail(label + ": non-standard exception");
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() > static_cast<long long>(case_budget_ms_)) {
            fail(label + ": case took " + std::to_string(elapsed.count()) + " ms");
        }
        if (g_oversized_allocation) {
            g_oversized_allocation = false;
            fail(label + ": tried to allocate more than 64 MiB");
        }
    }

    void report(const char* mode) const {
        std::printf("fuzz_cpp: %zu cases (%s), %zu failure(s)",
                    cases_, mode, failure_count_);
        if (budget_hit_) {
            std::printf(" [case cap reached]");
        }
        std::printf("\n");
        for (const std::string& line : failures_) {
            std::fprintf(stderr, "  %s\n", line.c_str());
        }
        if (failure_count_ != 0) {
            std::fprintf(stderr, "fuzz_cpp: FAILED\n");
        }
    }

    std::size_t failures() const { return failure_count_; }

private:
    std::size_t max_cases_;
    std::size_t case_budget_ms_;
    std::size_t cases_ = 0;
    std::size_t failure_count_ = 0;
    bool budget_hit_ = false;
    std::vector<std::string> failures_;
};

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL baseline: %s\n", what);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// Input generators (same shape as tools/fuzz_formats.py).
// ---------------------------------------------------------------------------
std::vector<std::size_t> truncation_points(std::size_t n, int random_cuts) {
    std::vector<std::size_t> cuts;
    // Every short prefix is fair game; 0 is the empty input.
    for (std::size_t i = 0; i <= std::min<std::size_t>(n, 256); ++i) {
        cuts.push_back(i);
    }
    for (std::size_t extra : {std::size_t{320}, std::size_t{512}, std::size_t{1024},
                              n / 2, n > 0 ? n - 1 : 0}) {
        if (extra <= n) {
            cuts.push_back(extra);
        }
    }
    std::mt19937 rng(static_cast<std::uint32_t>(0xC0FFEEu ^ n));
    for (int i = 0; i < random_cuts && n > 0; ++i) {
        cuts.push_back(static_cast<std::size_t>(rng() % (n + 1)));
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    cuts.erase(std::remove_if(cuts.begin(), cuts.end(),
                              [n](std::size_t cut) { return cut > n; }),
               cuts.end());
    return cuts;
}

// One changed byte per case at the start of the blob (header fields).
std::vector<Bytes> header_mutations(std::span<const std::uint8_t> data, int limit) {
    static constexpr std::uint8_t kValues[] = {0x00, 0x7F, 0x80, 0xFF};
    std::vector<Bytes> out;
    const std::size_t width = std::min<std::size_t>(64, data.size());
    for (std::size_t pos = 0; pos < width && static_cast<int>(out.size()) < limit; ++pos) {
        for (std::uint8_t value : kValues) {
            if (static_cast<int>(out.size()) >= limit) {
                break;
            }
            Bytes blob(data.begin(), data.end());
            blob[pos] = value;
            out.push_back(std::move(blob));
        }
    }
    return out;
}

std::vector<Bytes> bit_flips(std::span<const std::uint8_t> data, int limit) {
    std::vector<Bytes> out;
    if (data.empty()) {
        return out;
    }
    std::mt19937 rng(static_cast<std::uint32_t>(0xBADC0DEu ^ data.size()));
    for (int i = 0; i < limit; ++i) {
        Bytes blob(data.begin(), data.end());
        const std::size_t pos = rng() % blob.size();
        const unsigned bit = rng() % 8;
        blob[pos] ^= static_cast<std::uint8_t>(1u << bit);
        out.push_back(std::move(blob));
    }
    return out;
}

// 0/1/INT_MAX/UINT_MAX/INT_MIN in every dword of the first 32 bytes.
std::vector<Bytes> extreme_dwords(std::span<const std::uint8_t> data) {
    static constexpr std::uint32_t kValues[] = {0x00000000u, 0x00000001u, 0x7FFFFFFFu,
                                                0x80000000u, 0xFFFFFFFFu};
    std::vector<Bytes> out;
    for (std::size_t off = 0; off + 4 <= std::min<std::size_t>(32, data.size()); off += 4) {
        for (std::uint32_t value : kValues) {
            Bytes blob(data.begin(), data.end());
            put_u32(blob, off, value);
            out.push_back(std::move(blob));
        }
    }
    return out;
}

// Empty -> short prefixes -> header/extreme/bit mutations for one seed.
void fuzz_blob(Fuzzer& fz, const std::string& label, std::span<const std::uint8_t> seed,
               int random_cuts, int header_budget, int flip_budget, ParseFn parse) {
    for (std::size_t cut : truncation_points(seed.size(), random_cuts)) {
        if (!fz.budget_left()) {
            return;
        }
        fz.check(label + " trunc[" + std::to_string(cut) + "]",
                 [seed, cut, parse] { parse(seed.first(cut)); });
    }
    for (Bytes& blob : header_mutations(seed, header_budget)) {
        if (!fz.budget_left()) {
            return;
        }
        fz.check(label + " header", [&blob, parse] { parse(blob); });
    }
    for (Bytes& blob : bit_flips(seed, flip_budget)) {
        if (!fz.budget_left()) {
            return;
        }
        fz.check(label + " flip", [&blob, parse] { parse(blob); });
    }
    for (Bytes& blob : extreme_dwords(seed)) {
        if (!fz.budget_left()) {
            return;
        }
        fz.check(label + " extreme", [&blob, parse] { parse(blob); });
    }
}

// ---------------------------------------------------------------------------
// Parser wrappers.
// ---------------------------------------------------------------------------
void parse_sbv_only(std::span<const std::uint8_t> data) {
    std::string error;
    (void)gm::parse_sbv(data, error);
}

void parse_sbw_only(std::span<const std::uint8_t> data) {
    std::string error;
    (void)gm::parse_sbw(data, error);
}

void parse_sbt_only(std::span<const std::uint8_t> data) {
    std::string error;
    (void)gm::parse_sbt(data, error);
}

void parse_sbm_only(std::span<const std::uint8_t> data) {
    std::string error;
    (void)gm::parse_sbm(data, error);
}

void parse_sbc_only(std::span<const std::uint8_t> data) {
    std::string error;
    (void)gm::parse_sbc(data, error);
}

// ---------------------------------------------------------------------------
// Synthetic seeds (valid by construction; asserted once before fuzzing).
// ---------------------------------------------------------------------------
Bytes synthetic_sbv() {
    Bytes clip = {'S', 'B', 'V', '0'};
    append_u32(clip, 320); // width
    append_u32(clip, 160); // height
    append_u32(clip, 2);   // frame count
    append_u32(clip, 0);   // timestamp 0
    append_u32(clip, 4);   // end offset 0
    append_u32(clip, 5);   // timestamp 1
    append_u32(clip, 8);   // end offset 1
    clip.insert(clip.end(), {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});
    return clip;
}

Bytes synthetic_sbw() {
    // Three empty tables: textures, effects, sectors.
    return Bytes(12, 0);
}

Bytes synthetic_sbt() {
    Bytes sbt(84, 0);
    const char magic[] = "BINARY SAVE";
    std::memcpy(sbt.data(), magic, sizeof(magic) - 1);
    put_u32(sbt, 0x50, 0); // version 0
    sbt.push_back(255);    // END_OF_FILE
    return sbt;
}

Bytes synthetic_sbm() {
    Bytes sbm = {'S', 'B', 'M', '0', 2, 2, 0, 0, 0, 0, 0, 0}; // 2^2 x 2^2, one mip
    sbm.resize(12 + 16, 0);
    return sbm;
}

Bytes synthetic_sbc() {
    Bytes sbc = {'S', 'B', 'C', '0'};
    append_u32(sbc, 1);              // record count
    append_u32(sbc, (2u << 16) | 2u); // 2^2 x 2^2
    sbc.resize(sbc.size() + 16, 0);
    return sbc;
}

// ---------------------------------------------------------------------------
// BIGFILE: temp .idx/.dat pair, mutated index and shrinking DAT.
// ---------------------------------------------------------------------------
class TempDir {
public:
    TempDir() {
        static unsigned counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("gm_fuzz_cpp_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::error_code ec;
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& path, std::span<const std::uint8_t> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}

Bytes synthetic_bigfile_idx() {
    // Header is 12 bytes: "BIGFILE" + pad byte + u32 count at offset 8.
    Bytes idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E', 0};
    append_u32(idx, 2); // count
    const std::pair<const char*, std::uint32_t> entries[] = {{"FIRST", 0}, {"SECOND", 16}};
    for (const auto& [name, offset] : entries) {
        const std::size_t at = idx.size();
        idx.resize(at + 12, 0);
        std::memcpy(idx.data() + at, name, std::strlen(name));
        put_u32(idx, at + 8, offset);
    }
    return idx;
}

void fuzz_bigfile(Fuzzer& fz, int random_cuts, int header_budget, int flip_budget) {
    TempDir tmp;
    if (tmp.path().empty()) {
        expect(false, "cannot create a temporary directory for BIGFILE");
        return;
    }
    const fs::path idx_path = tmp.path() / "case.idx";
    const fs::path dat_path = tmp.path() / "case.dat";
    const Bytes idx = synthetic_bigfile_idx();
    Bytes dat(64);
    for (std::size_t i = 0; i < dat.size(); ++i) {
        dat[i] = static_cast<std::uint8_t>(i);
    }
    write_file(idx_path, idx);
    write_file(dat_path, dat);

    // Baseline: the synthetic pair must open and read exactly.
    {
        std::string error;
        auto file = gm::BigFile::open(idx_path, error);
        expect(file.has_value(), "synthetic BIGFILE opens");
        if (file) {
            expect(file->entries().size() == 2, "synthetic BIGFILE entry count");
            for (const auto& entry : file->entries()) {
                std::string read_error;
                auto blob = file->read(entry, read_error);
                expect(blob.has_value() && blob->size() == entry.size,
                       "synthetic BIGFILE entry read");
            }
        }
    }

    for (std::size_t cut : truncation_points(idx.size(), random_cuts)) {
        write_file(idx_path, std::span<const std::uint8_t>(idx).first(cut));
        fz.check("bigfile idx trunc[" + std::to_string(cut) + "]", [&] {
            std::string error;
            (void)gm::BigFile::open(idx_path, error);
        });
    }
    for (Bytes& blob : header_mutations(idx, header_budget)) {
        write_file(idx_path, blob);
        fz.check("bigfile idx header", [&] {
            std::string error;
            (void)gm::BigFile::open(idx_path, error);
        });
    }
    for (Bytes& blob : bit_flips(idx, flip_budget)) {
        write_file(idx_path, blob);
        fz.check("bigfile idx flip", [&] {
            std::string error;
            (void)gm::BigFile::open(idx_path, error);
        });
    }
    for (Bytes& blob : extreme_dwords(idx)) {
        write_file(idx_path, blob);
        fz.check("bigfile idx extreme", [&] {
            std::string error;
            (void)gm::BigFile::open(idx_path, error);
        });
    }

    // A valid index against a DAT that is shorter than the declared entries
    // must reject, not read out of range.  Enlarging the DAT is safe and must
    // keep the same entries; reading far past EOF must fail.
    write_file(idx_path, idx);
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{15},
                             std::size_t{16}, std::size_t{31}, std::size_t{32}, std::size_t{63},
                             std::size_t{64}, std::size_t{4096}}) {
        Bytes short_dat(size);
        write_file(dat_path, short_dat);
        fz.check("bigfile dat size=" + std::to_string(size), [&] {
            std::string error;
            auto file = gm::BigFile::open(idx_path, error);
            if (file) {
                for (const auto& entry : file->entries()) {
                    std::string read_error;
                    (void)file->read(entry, read_error);
                }
            }
        });
    }

    write_file(dat_path, dat);
    {
        std::string error;
        auto file = gm::BigFile::open(idx_path, error);
        expect(file.has_value(), "BIGFILE reopens for read guards");
        if (file) {
            const gm::BigEntry forged[] = {
                {"past-eof", 64, 1},
                {"huge-size", 0, 0xFFFFFFFFu},
                {"both-huge", 0xFFFFFFFFu, 0xFFFFFFFFu},
                {"at-eof", 64, 0},
                {"one-past", 63, 2},
            };
            for (const auto& entry : forged) {
                fz.check("bigfile read " + entry.name, [&] {
                    std::string read_error;
                    (void)file->read(entry, read_error);
                });
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SBV: container + frame codec.
// ---------------------------------------------------------------------------
Bytes compact_sbv(std::span<const std::uint8_t> raw, const gm::Sbv& clip, std::uint32_t keep) {
    keep = std::min(keep, clip.frame_count);
    Bytes out(raw.begin(), raw.begin() + 16);
    put_u32(out, 12, keep);
    const std::size_t table_bytes = 8 * static_cast<std::size_t>(keep);
    out.insert(out.end(), raw.begin() + 16, raw.begin() + 16 + table_bytes);
    const std::uint32_t end = keep == 0 ? 0 : clip.table[keep - 1].end_offset;
    out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(clip.data_start),
               raw.begin() + static_cast<std::ptrdiff_t>(clip.data_start + end));
    return out;
}

void fuzz_sbv_decoder(Fuzzer& fz, std::span<const std::uint8_t> clip_bytes, int flips,
                      int random_cuts) {
    std::string error;
    auto clip = gm::parse_sbv(clip_bytes, error);
    if (!clip || clip->frame_count == 0) {
        return;
    }
    const std::size_t block_end = clip->table.front().end_offset;
    if (block_end == 0 || clip->data_start + block_end > clip_bytes.size()) {
        return;
    }
    Bytes block(clip_bytes.begin() + static_cast<std::ptrdiff_t>(clip->data_start),
                clip_bytes.begin() + static_cast<std::ptrdiff_t>(clip->data_start + block_end));

    // The shipped block must still decode; otherwise the corpus seed is stale
    // and the mutation cases below would only test the rejection path.
    const int width = static_cast<int>(clip->width);
    const int height = static_cast<int>(clip->height);
    {
        std::string decode_error;
        auto frame = gm::decode_sbv_frame(block, width, height, decode_error);
        expect(frame.has_value(), "corpus SBV frame decodes");
    }

    // Frame blocks get the same short-prefix treatment as whole containers.
    for (std::size_t cut : truncation_points(block.size(), random_cuts)) {
        fz.check("sbv frame trunc[" + std::to_string(cut) + "]", [&block, cut, width, height] {
            std::string decode_error;
            (void)gm::decode_sbv_frame(std::span<const std::uint8_t>(block).first(cut), width,
                                       height, decode_error);
        });
    }

    // Header dwords of the *block* (not the container) are fair game too.
    for (Bytes& blob : extreme_dwords(block)) {
        fz.check("sbv frame extreme", [&blob, width, height] {
            std::string decode_error;
            (void)gm::decode_sbv_frame(blob, width, height, decode_error);
        });
    }
    for (Bytes& blob : bit_flips(block, flips)) {
        fz.check("sbv frame flip", [&blob, width, height] {
            std::string decode_error;
            (void)gm::decode_sbv_frame(blob, width, height, decode_error);
        });
    }
}

void fuzz_sbv_geometry(Fuzzer& fz) {
    const Bytes zeros(16384, 0);
    const int widths[] = {0, 1, 2, 7, 8, 15, 16, 17, 18, 31, 32, 48, 64, 160, 320, 321, 512, -1};
    const int heights[] = {0, 1, 2, 15, 16, 17, 31, 32, 33, 48, 160, 161, 1024, 1032, -1};
    for (int width : widths) {
        for (int height : heights) {
            const std::span<const std::uint8_t> block(zeros);
            fz.check("sbv geom " + std::to_string(width) + "x" + std::to_string(height),
                     [block, width, height] {
                         std::string decode_error;
                         (void)gm::decode_sbv_frame(block, width, height, decode_error);
                     });
        }
    }
    // Regressions for the width 8..15 / height 16 lift_2d hang: these must
    // reject quickly (CTest TIMEOUT is the backstop if the guard regresses).
    const std::span<const std::uint8_t> hang_block(zeros);
    for (int width : {8, 10, 14}) {
        for (int height : {16, 32}) {
            fz.check("sbv hang regression " + std::to_string(width) + "x" +
                         std::to_string(height),
                     [hang_block, width, height] {
                         std::string decode_error;
                         const auto frame =
                             gm::decode_sbv_frame(hang_block, width, height, decode_error);
                         if (frame.has_value()) {
                             std::fprintf(stderr, "FAIL baseline: %dx%d accepted\n", width,
                                          height);
                             ++g_failures;
                         }
                     });
        }
    }
}

void fuzz_sbv(Fuzzer& fz, int random_cuts, int header_budget, int flip_budget, int decode_flips) {
    const Bytes synthetic = synthetic_sbv();
    {
        std::string error;
        auto clip = gm::parse_sbv(synthetic, error);
        expect(clip.has_value() && clip->frame_count == 2, "synthetic SBV parses");
    }
    fuzz_blob(fz, "sbv synth", synthetic, random_cuts, header_budget, flip_budget,
              parse_sbv_only);

    // Extreme container fields: geometry, frame count and end offsets.
    for (std::uint32_t width : {0u, 1u, 7u, 8u, 15u, 16u, 321u, 0xFFFFFFFFu}) {
        for (std::uint32_t height : {0u, 15u, 16u, 17u, 1024u, 1025u, 0xFFFFFFFFu}) {
            Bytes blob = synthetic;
            put_u32(blob, 4, width);
            put_u32(blob, 8, height);
            fz.check("sbv geom header " + std::to_string(width) + "x" + std::to_string(height),
                     [&blob] { parse_sbv_only(blob); });
        }
    }
    for (std::uint32_t count : {0u, 1u, 2u, 200000u, 200001u, 0xFFFFFFFFu}) {
        Bytes blob = synthetic;
        put_u32(blob, 12, count);
        fz.check("sbv count=" + std::to_string(count), [&blob] { parse_sbv_only(blob); });
    }
    for (std::size_t at : {std::size_t{20}, std::size_t{28}}) {
        for (std::uint32_t end : {0u, 1u, 2u, 3u, 4u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
            Bytes blob = synthetic;
            put_u32(blob, at, end);
            fz.check("sbv end=" + std::to_string(end), [&blob] { parse_sbv_only(blob); });
        }
    }

    // Real clip: keep only a few frames so container mutations stay cheap,
    // then fuzz the first frame block through the pixel decoder.
    Bytes raw;
    if (read_file("/mnt/d/games/Gunmetal 3dfx/Vid/Intro.sbv", raw)) {
        std::string error;
        auto clip = gm::parse_sbv(raw, error);
        if (clip) {
            Bytes compact = compact_sbv(raw, *clip, 8);
            {
                std::string compact_error;
                auto check_clip = gm::parse_sbv(compact, compact_error);
                expect(check_clip.has_value(), "compact Intro.sbv parses");
            }
            fuzz_blob(fz, "sbv corpus", compact, random_cuts, header_budget, flip_budget,
                      parse_sbv_only);
            fuzz_sbv_decoder(fz, compact, decode_flips, random_cuts);
        }
    }

    // The synthetic block is short but exercises the decoder's truncation path.
    const Bytes zeros(65536, 0);
    {
        std::string decode_error;
        expect(gm::decode_sbv_frame(zeros, 320, 160, decode_error).has_value(),
               "synthetic 320x160 frame decodes");
        expect(gm::decode_sbv_frame(zeros, 16, 32, decode_error).has_value(),
               "synthetic 16x32 frame decodes");
    }

    // Regression: base_grid computed `hi - lo` in int32.  A block whose first
    // two dwords are INT_MAX/INT_MIN made that overflow (UB, trap in Debug).
    for (std::pair<std::uint32_t, std::uint32_t> pair :
         {std::pair{0x7FFFFFFFu, 0x80000000u}, std::pair{0x80000000u, 0x7FFFFFFFu}}) {
        Bytes block(65536, 0);
        put_u32(block, 0, pair.first);
        put_u32(block, 4, pair.second);
        fz.check("sbv base_grid overflow regression", [&block] {
            std::string decode_error;
            (void)gm::decode_sbv_frame(block, 320, 160, decode_error);
        });
    }
}

// ---------------------------------------------------------------------------
// SBW / SBT / SBM / SBC.
// ---------------------------------------------------------------------------
void fuzz_sbw(Fuzzer& fz, int random_cuts, int header_budget, int flip_budget) {
    const Bytes synthetic = synthetic_sbw();
    {
        std::string error;
        expect(gm::parse_sbw(synthetic, error).has_value(), "synthetic SBW parses");
    }
    fuzz_blob(fz, "sbw synth", synthetic, random_cuts, header_budget, flip_budget,
              parse_sbw_only);

    // 4-byte counter bombs: the pre-fix reader asked the allocator for ~160 GiB.
    for (std::uint32_t count :
         {0u, 1u, 65536u, 0x00FFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        Bytes bomb(4, 0);
        put_u32(bomb, 0, count);
        fz.check("sbw count=" + std::to_string(count), [&bomb] { parse_sbw_only(bomb); });
    }

    Bytes raw;
    if (read_file("/mnt/d/games/Gunmetal 3dfx/level0.sbw", raw) ||
        read_file("/mnt/d/games/Gunmetal 3dfx/LEVEL0.SBW", raw)) {
        fuzz_blob(fz, "sbw corpus", raw, random_cuts, header_budget, flip_budget,
                  parse_sbw_only);
    }
}

void fuzz_sbt(Fuzzer& fz, int random_cuts, int header_budget, int flip_budget) {
    const Bytes synthetic = synthetic_sbt();
    {
        std::string error;
        expect(gm::parse_sbt(synthetic, error).has_value(), "synthetic SBT parses");
    }
    fuzz_blob(fz, "sbt synth", synthetic, random_cuts, header_budget, flip_budget,
              parse_sbt_only);

    Bytes raw;
    if (read_file("/mnt/d/games/Gunmetal 3dfx/LEVEL0.SBT", raw)) {
        fuzz_blob(fz, "sbt corpus", raw, random_cuts, header_budget, flip_budget,
                  parse_sbt_only);
    }
}

void fuzz_sbm(Fuzzer& fz, int random_cuts, int header_budget, int flip_budget) {
    const Bytes synthetic = synthetic_sbm();
    {
        std::string error;
        expect(gm::parse_sbm(synthetic, error).has_value(), "synthetic SBM parses");
    }
    fuzz_blob(fz, "sbm synth", synthetic, random_cuts, header_budget, flip_budget,
              parse_sbm_only);

    // Regression: a mip count longer than 2^log2w/log2h used to shift by a
    // negative amount in `1 << (log2w - k)` (UB; trap in Debug builds).
    {
        Bytes bomb = synthetic_sbm();
        bomb[4] = 0; // log2w = 0
        bomb[5] = 0; // log2h = 0
        bomb[6] = 1; // two mip levels
        std::string error;
        expect(!gm::parse_sbm(bomb, error).has_value(),
               "SBM rejects mips beyond the dimensions");
        fz.check("sbm mip beyond dimensions", [&bomb] { parse_sbm_only(bomb); });
    }

    // First texture from the shipped PICS/SBM.IDX, when the game is present.
    std::string error;
    auto archive = gm::BigFile::open("/mnt/d/games/Gunmetal 3dfx/PICS/SBM.IDX", error);
    if (archive && !archive->entries().empty()) {
        std::string read_error;
        auto blob = archive->read(archive->entries().front(), read_error);
        if (blob && !blob->empty() && blob->size() <= kMaxSeedBytes) {
            fuzz_blob(fz, "sbm corpus", *blob, random_cuts, header_budget, flip_budget,
                      parse_sbm_only);
        }
    }
}

void fuzz_sbc(Fuzzer& fz, int random_cuts, int header_budget, int flip_budget) {
    const Bytes synthetic = synthetic_sbc();
    {
        std::string error;
        expect(gm::parse_sbc(synthetic, error).has_value(), "synthetic SBC parses");
    }
    fuzz_blob(fz, "sbc synth", synthetic, random_cuts, header_budget, flip_budget,
              parse_sbc_only);

    Bytes raw;
    if (read_file("/mnt/d/games/Gunmetal 3dfx/DM2.sbc", raw)) {
        fuzz_blob(fz, "sbc corpus", raw, random_cuts, header_budget, flip_budget,
                  parse_sbc_only);
    }
}

} // namespace

void* operator new(std::size_t size) {
    return operator_new_or_throw(size);
}
void* operator new[](std::size_t size) {
    return operator_new_or_throw(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

int main(int argc, char** argv) {
    bool full = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--full") {
            full = true;
        } else if (arg == "--quick") {
            full = false;
        } else {
            std::fprintf(stderr, "usage: %s [--quick|--full]\n", argv[0]);
            return 2;
        }
    }

    // Make sure the allocation guard is actually linked in; otherwise the
    // "huge counter" cases below would silently allocate gigabytes on hosts
    // with overcommit.
    {
        g_oversized_allocation = false;
        bool caught = false;
        try {
            void* probe = ::operator new(kMaxSingleAllocation + 1);
            std::free(probe);
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        expect(caught && g_oversized_allocation,
               "allocation guard is active (global operator new replaced)");
        g_oversized_allocation = false;
    }

    const int random_cuts = full ? 128 : 16;
    const int header_budget = full ? 192 : 32;
    const int flip_budget = full ? 256 : 32;
    const int decode_flips = full ? 192 : 24;
    const std::size_t max_cases = full ? 30000 : 8000;
    const std::size_t case_budget_ms = 1000;

    Fuzzer fz(max_cases, case_budget_ms);
    fuzz_bigfile(fz, random_cuts, header_budget, flip_budget);
    fuzz_sbv(fz, random_cuts, header_budget, flip_budget, decode_flips);
    fuzz_sbv_geometry(fz);
    fuzz_sbw(fz, random_cuts, header_budget, flip_budget);
    fuzz_sbt(fz, random_cuts, header_budget, flip_budget);
    fuzz_sbm(fz, random_cuts, header_budget, flip_budget);
    fuzz_sbc(fz, random_cuts, header_budget, flip_budget);

    fz.report(full ? "full" : "quick");
    if (g_failures != 0) {
        std::fprintf(stderr, "fuzz_cpp: %d baseline/sanity failure(s)\n", g_failures);
    }
    return (g_failures == 0 && fz.failures() == 0) ? 0 : 1;
}
