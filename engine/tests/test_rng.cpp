// `game/rng.h` (docs/RNG.md): the original's CRT `rand()`.
//
// Goldens are the documented MSVC `srand(1)` sequence; the state progression
// is checked against the literal LCG from 0x480E30.  Negatives: a different
// seed must diverge, and the range stays inside 0..0x7FFF.
#include "game/rng.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);      \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

void test_msvc_sequence() {
    gm::MsvcRand rng(1u);
    // MSVC `srand(1)` golden sequence.
    CHECK_EQ(rng.next(), 41);
    CHECK_EQ(rng.next(), 18467);
    CHECK_EQ(rng.next(), 6334);
    CHECK_EQ(rng.next(), 26500);
    CHECK_EQ(rng.next(), 19169);

    // The raw state is the documented LCG step from seed 1.
    gm::MsvcRand raw(1u);
    CHECK_EQ(raw.advance(), std::uint32_t(2745024u)); // 1*214013 + 2531011
    CHECK_EQ(raw.state(), std::uint32_t(2745024u));
    CHECK_EQ(static_cast<int>((2745024u >> 16) & 0x7FFFu), 41);
}

void test_seed_and_bounds() {
    gm::MsvcRand a(1u);
    gm::MsvcRand b(2u);
    bool diverged = false;
    for (int i = 0; i < 8; ++i) {
        const int va = a.next();
        const int vb = b.next();
        CHECK(va >= 0);
        CHECK(va <= gm::MsvcRand::kMax);
        if (va != vb) {
            diverged = true;
        }
    }
    CHECK(diverged);

    // Re-seeding restarts the sequence; the default seed is 1 (`srand(1)`).
    gm::MsvcRand defaulted;
    CHECK_EQ(defaulted.next(), 41);
    defaulted.seed(1u);
    CHECK_EQ(defaulted.next(), 41);

    // Zero is a legal seed (the original's `time(nullptr)` path never is, but
    // `srand(0)` must not stall).
    gm::MsvcRand zero(0u);
    const int first = zero.next();
    const int second = zero.next();
    CHECK(first != second);
}

} // namespace

int main() {
    test_msvc_sequence();
    test_seed_and_bounds();
    if (g_failures != 0) {
        std::fprintf(stderr, "rng: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rng: ok\n");
    return 0;
}
