// SOU2 decoder parity test against the whole shipped music archive
// (MUSIC/SOU.IDX + MUSIC/SOU.DAT, 3092 entries, every one SOU2).
//
// The payload is engine "format 4": 13 interleaved sub-streams decoded by
// gundx.exe 0x852d80, driven by the mixer at 0x409658 in 1024-frame blocks.
// The expected values below are FNV-1a 64 hashes over every decoded record in
// SOU.IDX order:
//
//   raw  - the 32-bit mix accumulators, interleaved L/R, little-endian
//   pcm  - the 16-bit interleaved PCM from gm::decode_sou2 (v >> 1, saturated)
//
// Reference generation: the engine's own routine was executed with
// tools/re/x86mini.py (local, uncommitted extensions for the flag/LEA gaps)
// and compared sample-by-sample with this decoder on full tracks covering
// every code path (level-2 clean and both fractional variants, level-3 clean
// and both fractional variants), both sample rates, and the two degenerate
// entries.  The hashes below then pin every one of the 3092 records.
#include "core/bytes.h"
#include "res/bigfile.h"
#include "res/sou2.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

// Shipped MUSIC/SOU.DAT corpus constants (verified with the Python reader).
constexpr std::size_t kEntries = 3092;
constexpr std::uint64_t kTotalFrames = 224059265ull; // 7001.85 s
constexpr std::size_t kRate32000 = 3090;
constexpr std::size_t kRate16000 = 2;

// Aggregate parity hashes over all 3092 decoded records (see file comment).
constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint64_t kRawFnv = 0x01f9d974baf293e4ull;
constexpr std::uint64_t kPcmFnv = 0x2fd47d85665dc344ull;
constexpr std::uint64_t kPcmNonZero = 309885750ull;
constexpr std::int64_t kMaxAbsRaw = 778304; // L20I06

// Per-record spot checks, including both sample rates, the largest tracks, the
// two degenerate entries and records that exercise the level-3 gain cursor.
struct RecordHash {
    const char* name;
    std::uint64_t raw;
    std::uint64_t pcm;
};

constexpr RecordHash kRecordHashes[] = {
    {"DM1A01", 0x1fc8fed7652ef776ull, 0x57ae8e94b96636e0ull},
    {"DM1A02", 0x32bcc5d67feb6476ull, 0x7357896d91d58f8bull},
    {"DM3L07", 0x164d178c61dab6e1ull, 0xd377ef4be351e7abull},
    {"DM4I04", 0x52572c4a5d63d34bull, 0x3534a90ab553ba35ull},
    {"DM5H01", 0x6163dd9452e913f7ull, 0x172665e23a32bcd6ull},
    {"L01BONUS", 0xbd5a4ee701d61b27ull, 0x891a3da1690a5280ull},
    {"LOADSCRN", 0xc076e713aad8453aull, 0xaae23106cca4c23eull},
    {"L04I01", 0x14650fb0739d0383ull, 0x14650fb0739d0383ull},
    {"L06A01", 0x47fe0d7eaf8e51e3ull, 0x315446a086a23133ull},
    {"L20J19", 0x569a06959b584ab2ull, 0x32dbb2e042a27f1full},
    {"PAMN02", 0xbbb460d789e4e14aull, 0x9658ae6e9f874758ull},
    {"L21L14", 0x2cc201b5231943a1ull, 0x157a223fb4349522ull},
    {"L21A04", 0xf363d969102358e3ull, 0x775f4610c63c7acfull},
    {"L26B11", 0x377f230969700a11ull, 0xcbcd8ed97b64378bull},
    {"L11D02", 0xf64daefdec6f792aull, 0x78a179515e4ee004ull},
    {"L02E06", 0xdc03092cdb9e4f3full, 0x84ea3dabfaad9854ull},
    {"L13R03", 0x524f034d1a0b52a9ull, 0x0951e95b99c970a7ull},
    {"ORIENT", 0xe3ae036ae68dfa27ull, 0xc3b5927f7341418dull},
    {"WINNER", 0xab654876c66854ffull, 0x4f03da7ef55c1673ull},
    {"OPENING", 0xf8643bff422811e0ull, 0x70ac6d8c9d08a57bull},
    // Maximum level-3 gain-table cursor (0x6c0 bytes).
    {"DM2M03", 0x1ce35f15fa81613cull, 0x85164ef9622b131cull},
    {"L01C07", 0x85a416556516f95full, 0xb4f95eb8c12a2f1full},
    {"S01I08", 0x349183d37c1f6a79ull, 0xa502d98a21253f6aull},
    // Loudest decoded record (|raw| = 778304; the PCM clip is heavy here).
    {"L20I06", 0x40aa142202d128feull, 0x3e547367cb77bfaaull},
};

std::uint64_t fnv_u32(std::uint64_t h, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h = (h ^ ((v >> (i * 8)) & 0xFFu)) * kFnvPrime;
    }
    return h;
}

std::uint64_t fnv_u16(std::uint64_t h, std::uint16_t v) {
    h = (h ^ (v & 0xFFu)) * kFnvPrime;
    return (h ^ (v >> 8)) * kFnvPrime;
}

void test_archive() {
    const fs::path idx = "/mnt/d/games/Gunmetal 3dfx/MUSIC/SOU.IDX";
    std::error_code ec;
    if (!fs::is_regular_file(idx, ec)) {
        std::printf("sou2: game data not present, parity test skipped\n");
        return;
    }

    std::string error;
    const auto big = gm::BigFile::open(idx, error);
    CHECK(big.has_value());
    if (!big) {
        std::printf("sou2: %s\n", error.c_str());
        return;
    }
    CHECK(big->entries().size() == kEntries);

    std::size_t n32 = 0;
    std::size_t n16 = 0;
    std::size_t bad_magic = 0;
    std::size_t bad_units = 0;
    std::size_t bad_frames = 0;
    std::size_t decode_errors = 0;
    std::size_t spot_checks = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t raw_fnv = kFnvBasis;
    std::uint64_t pcm_fnv = kFnvBasis;
    std::uint64_t pcm_nonzero = 0;
    std::int64_t max_abs_raw = 0;
    std::string first_error;

    for (const auto& entry : big->entries()) {
        const auto bytes = big->read(entry, error);
        CHECK(bytes.has_value());
        if (!bytes) {
            break;
        }
        CHECK(bytes->size() >= 20);
        if (bytes->size() < 20) {
            continue;
        }
        if (std::memcmp(bytes->data(), "SOU2", 4) != 0) {
            ++bad_magic;
            continue;
        }
        const std::uint32_t rate = gm::rd32(bytes->data() + 4);
        const std::uint32_t frames = gm::rd32(bytes->data() + 8);
        const std::uint32_t units = gm::rd32(bytes->data() + 16);
        if (rate == 32000) {
            ++n32;
        } else if (rate == 16000) {
            ++n16;
        }
        if (20 + static_cast<std::size_t>(units) * 4 != bytes->size()) {
            ++bad_units;
        }
        // Every entry but L06A01 (1 frame) is a multiple of 32 frames, the
        // mixer's block quantum (L04I01 has 0 frames and is trivially even).
        if (frames % 32 != 0) {
            ++bad_frames;
        }
        total_frames += frames;

        std::string clip_error;
        const auto raw = gm::decode_sou2_raw(*bytes, clip_error);
        const auto clip = gm::decode_sou2(*bytes, clip_error);
        if (!raw || !clip) {
            if (decode_errors == 0) {
                first_error = entry.name + ": " + clip_error;
            }
            ++decode_errors;
            continue;
        }
        CHECK(raw->sample_rate == static_cast<int>(rate));
        CHECK(raw->channels == 2);
        CHECK(raw->samples.size() == static_cast<std::size_t>(frames) * 2);
        CHECK(clip->sample_rate == static_cast<int>(rate));
        CHECK(clip->channels == 2);
        CHECK(clip->samples.size() == static_cast<std::size_t>(frames) * 2);
        if (raw->samples.size() != clip->samples.size()) {
            continue;
        }

        std::uint64_t rec_raw_fnv = kFnvBasis;
        std::uint64_t rec_pcm_fnv = kFnvBasis;
        for (std::size_t i = 0; i < raw->samples.size(); ++i) {
            const std::int32_t v = raw->samples[i];
            const std::uint32_t u = static_cast<std::uint32_t>(v);
            rec_raw_fnv = fnv_u32(rec_raw_fnv, u);
            raw_fnv = fnv_u32(raw_fnv, u);
            const std::int64_t mag = v < 0 ? -static_cast<std::int64_t>(v) : v;
            if (mag > max_abs_raw) {
                max_abs_raw = mag;
            }
        }
        for (const std::int16_t s : clip->samples) {
            const std::uint16_t u = static_cast<std::uint16_t>(s);
            rec_pcm_fnv = fnv_u16(rec_pcm_fnv, u);
            pcm_fnv = fnv_u16(pcm_fnv, u);
            if (s != 0) {
                ++pcm_nonzero;
            }
        }

        for (const RecordHash& ref : kRecordHashes) {
            if (entry.name == ref.name) {
                ++spot_checks;
                if (rec_raw_fnv != ref.raw || rec_pcm_fnv != ref.pcm) {
                    std::fprintf(stderr,
                                 "FAIL %s: raw %016" PRIx64 " (want %016" PRIx64
                                 "), pcm %016" PRIx64 " (want %016" PRIx64 ")\n",
                                 entry.name.c_str(), rec_raw_fnv, ref.raw, rec_pcm_fnv,
                                 ref.pcm);
                    ++g_failures;
                }
            }
        }
    }

    std::printf("sou2: %zu entries, SOU2 magic failures %zu, payload-size "
                "mismatches %zu, frames%%32 failures %zu\n",
                big->entries().size(), bad_magic, bad_units, bad_frames);
    std::printf("sou2: rates 32000=%zu 16000=%zu, total frames %" PRIu64 " (%.2f s)\n",
                n32, n16, total_frames, static_cast<double>(total_frames) / 32000.0);
    std::printf("sou2: raw fnv %016" PRIx64 ", pcm fnv %016" PRIx64
                ", pcm nonzero %" PRIu64 ", max |raw| %" PRId64 "\n",
                raw_fnv, pcm_fnv, pcm_nonzero, max_abs_raw);

    CHECK(bad_magic == 0);
    CHECK(bad_units == 1); // L06A01 declares units == 0 but carries 90592 bytes
    CHECK(bad_frames == 1); // L06A01
    CHECK(n32 == kRate32000);
    CHECK(n16 == kRate16000);
    CHECK(total_frames == kTotalFrames);
    CHECK(decode_errors == 0);
    CHECK(spot_checks == sizeof(kRecordHashes) / sizeof(kRecordHashes[0]));
    CHECK(raw_fnv == kRawFnv);
    CHECK(pcm_fnv == kPcmFnv);
    CHECK(pcm_nonzero == kPcmNonZero);
    CHECK(max_abs_raw == kMaxAbsRaw);

    if (decode_errors != 0) {
        std::fprintf(stderr, "FAIL: %zu/%zu SOU2 payloads failed; first: %s\n", decode_errors,
                     big->entries().size(), first_error.c_str());
    }
}

} // namespace

int main() {
    test_archive();
    if (g_failures != 0) {
        std::fprintf(stderr, "sou2: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sou2: ok\n");
    return 0;
}
