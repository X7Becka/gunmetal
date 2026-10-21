// MSVC CRT `rand()` — the generator the original engine actually uses.
//
// Proven from `gundx.exe`:
//   0x480E30  int rand(void)
//             state = *(uint32_t*)(__getptd() + 0x14)
//             state = state * 0x343FD + 0x269EC3      ; 214013, 2531011
//             *(uint32_t*)(ptd + 0x14) = state
//             return (state >> 16) & 0x7FFF
//   0x480E20  void srand(uint32_t seed)  -> ptd[0x14] = seed
//   the only `srand` call site is 0x44A491 in the init routine, seeded with
//   `time(nullptr)` (0x480EF0), so a live original run is not reproducible.
//
// The state is per-thread (`_getptd()` TLS), which matters only for the
// original's multi-threaded audio path; the engine is single-threaded, so one
// instance is enough.  See docs/RNG.md.
#pragma once

#include <cstdint>

namespace gm {

class MsvcRand {
public:
    static constexpr std::uint32_t kMultiplier = 214013u; // 0x343FD
    static constexpr std::uint32_t kIncrement = 2531011u; // 0x269EC3
    static constexpr int kMax = 0x7FFF;

    explicit MsvcRand(std::uint32_t seed = 1u) : state_(seed) {}

    /// `srand(seed)`.
    void seed(std::uint32_t value) { state_ = value; }

    /// Full 32-bit LCG state after one step (the value stored back into the
    /// thread-local `_holdrand`); useful to compare against a trace.
    std::uint32_t advance() {
        state_ = state_ * kMultiplier + kIncrement;
        return state_;
    }

    /// `rand()`: `(state >> 16) & 0x7FFF`, so the range is `0..32767`.
    int next() { return static_cast<int>((advance() >> 16) & 0x7FFFu); }

    /// Current state without stepping (the last value written back).
    std::uint32_t state() const { return state_; }

private:
    std::uint32_t state_;
};

} // namespace gm
