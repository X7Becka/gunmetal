// Level objectives and secrets (`M6.4.5`, slice 2; docs/OBJECTIVES_STATE.md).
//
// Pins the pure module `game/objectives.h` against the numbers and addresses of
// `docs/OBJECTIVES.md` (§2/§3/§5).  No game file is opened: this is state, not
// a format.  Every checked number is printed.
//
//   * `DEFINE_LEVEL_OBJECTIVES` (103): `declare(0..30)` succeeds, the count and
//     the empty mask are pinned for all 31 values; `declare(31)` is refused
//     with the `0x45bdf6` cap text and `declare(-1)` as a negative (§2.1);
//   * the mask `ds:0x80f9d0`: `set(i)` puts exactly `1 << i`, repeats are
//     no-ops (the original `or`, `0x45be49`), `set(i, false)` is the documented
//     MODEL clear, and no bit outside `[0, count)` can be produced by the API
//     (mask 0x1f at count 5, 0x3fffffff at count 30) (§2.1);
//   * condition 7 (`0x45e9f0`): 0 declared is false (`0x45e9fa`), all declared
//     bits is true, one missing bit is false, higher bits do not matter (§2.2);
//   * a smaller re-declaration keeps the older bits in the field but hides them
//     from `declared_mask()`/`all_done()` (proven: 103 touches only the count);
//   * the save value (block 11, entity `-3`, opcode `0x68`) round-trips through
//     `save_mask()`/`load_mask()`, including foreign bits from an older, larger
//     declaration, which the original loads unchanged (§2.5);
//   * secrets: `add()` 101 times -> 101 (`0x4536b0`, no cap); `level3` 8 sites
//     on 6 declared and `level15` 4 on 5 reproduce the corpus anomalies, so the
//     tally may pass or stay below the declaration; `reset()` is `0x45280f`
//     (0) and keeps the declaration, `clear_declared()` is `0x45708a` (§3);
//   * negatives: `set(-1)`, `set(30)` at count 30, `set(31)`, `declare(31)`,
//     `declare(-1)`, `set_declared(-1)` -- all refused with a text, state
//     untouched; `done(-1)`/`done(31)` are quiet `false` queries (§2, header);
//   * determinism: the same sequence run twice gives identical numbers.

#include "game/objectives.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

/// `1 << count` as an unsigned mask; count is 0..30 here.
std::uint32_t bits(int count) {
    return count <= 0 ? 0u : (std::uint32_t{1} << count) - 1u;
}

// --- 103: the declaration cap and the empty mask ----------------------------
void test_declare() {
    for (int count = 0; count <= gm::kMaxObjectives; ++count) {
        gm::Objectives objectives;
        CHECK(objectives.declare(count));
        CHECK_EQ(objectives.count(), count);
        CHECK_EQ(objectives.mask(), 0u);
        CHECK_EQ(objectives.declared_mask(), 0u);
        CHECK(!objectives.all_done()); // no declared bit can be set yet
        CHECK_EQ(objectives.save_mask(), 0u);
        CHECK(objectives.last_error().empty());
    }
    std::printf("objectives: declare 0..30 ok (31 counts), mask=0x00000000, "
                "all_done=0, cap=%d (0x45bdf6)\n",
                gm::kMaxObjectives);

    // The cap is refused and leaves the previous declaration untouched.
    gm::Objectives objectives;
    CHECK(objectives.declare(5));
    CHECK(!objectives.declare(31));
    const std::string cap = objectives.last_error();
    std::printf("objectives: declare(31): ok=0 count=%d error=\"%s\"\n", objectives.count(),
                cap.c_str());
    CHECK_EQ(objectives.count(), 5);
    CHECK(objectives.mask() == 0u);
    CHECK(cap.find("exceeds 30") != std::string::npos);
    CHECK(cap.find("0x45bdf6") != std::string::npos);

    CHECK(!objectives.declare(-1));
    const std::string negative = objectives.last_error();
    std::printf("objectives: declare(-1): ok=0 count=%d error=\"%s\"\n", objectives.count(),
                negative.c_str());
    CHECK_EQ(objectives.count(), 5);
    CHECK(negative.find("negative") != std::string::npos);

    // A clean declaration clears the note of the refusal.
    CHECK(objectives.declare(7));
    CHECK(objectives.last_error().empty());
    CHECK_EQ(objectives.count(), 7);
}

// --- 104: bits, idempotence, no bits outside the declaration ----------------
void test_set_and_mask() {
    gm::Objectives objectives;
    CHECK(objectives.declare(5));

    for (int index = 0; index < 5; ++index) {
        CHECK(objectives.set(index));
        CHECK_EQ(objectives.mask(), bits(index + 1));
        CHECK_EQ(objectives.declared_mask(), bits(index + 1));
        CHECK(objectives.done(index));
    }
    std::printf("objectives: set(0..4) -> mask=0x%08x declared=0x%08x high_bits=0x%08x\n",
                objectives.mask(), objectives.declared_mask(),
                objectives.mask() & ~bits(objectives.count()));
    CHECK_EQ(objectives.mask(), 0x1Fu);
    CHECK_EQ(objectives.mask() & ~bits(objectives.count()), 0u);

    // The original `or` (0x45be49): a repeat is a no-op.
    CHECK(objectives.set(2));
    CHECK_EQ(objectives.mask(), 0x1Fu);
    CHECK_EQ(objectives.count(), 5);
    CHECK(objectives.done(2));

    // MODEL clear: 104 cannot do this, the module's API can.
    CHECK(objectives.set(2, false));
    std::printf("objectives: set(2,false) MODEL clear -> mask=0x%08x done(2)=%d\n",
                objectives.mask(), objectives.done(2) ? 1 : 0);
    CHECK_EQ(objectives.mask(), 0x1Bu);
    CHECK(!objectives.done(2));
    CHECK(objectives.set(2, false)); // clearing twice is a no-op too
    CHECK_EQ(objectives.mask(), 0x1Bu);
    CHECK(objectives.set(2));
    CHECK_EQ(objectives.mask(), 0x1Fu);

    // A full 30-bit mask is exactly 0x3fffffff.
    gm::Objectives full;
    CHECK(full.declare(gm::kMaxObjectives));
    for (int index = 0; index < gm::kMaxObjectives; ++index) {
        CHECK(full.set(index));
    }
    std::printf("objectives: declare(30) + set(0..29) -> mask=0x%08x high_bits=0x%08x\n",
                full.mask(), full.mask() & ~bits(full.count()));
    CHECK_EQ(full.mask(), 0x3FFFFFFFu);
    CHECK_EQ(full.mask() & ~bits(full.count()), 0u);
}

// --- condition 7: all_done ------------------------------------------------
void test_all_done() {
    // Zero declared: the original's `jle` gives false (0x45e9fa).
    gm::Objectives empty;
    CHECK(empty.declare(0));
    CHECK(!empty.all_done());
    std::printf("objectives: all_done(count=0)=%d (0x45e9fa, count<=0 -> false)\n",
                empty.all_done() ? 1 : 0);

    gm::Objectives one;
    CHECK(one.declare(1));
    CHECK(!one.all_done());
    CHECK(one.set(0));
    CHECK(one.all_done());
    CHECK(one.set(0, false));
    CHECK(!one.all_done());

    gm::Objectives three;
    CHECK(three.declare(3));
    CHECK(three.set(0));
    CHECK(three.set(1));
    CHECK(!three.all_done());
    CHECK(three.set(2));
    CHECK(three.all_done());
    std::printf("objectives: all_done count=3: {} = 0, {0,1} = 0, {0,1,2} = 1\n");

    // Higher bits beyond the count are ignored by condition 7.
    gm::Objectives higher;
    CHECK(higher.declare(2));
    CHECK(higher.set(0));
    CHECK(higher.set(1));
    CHECK(higher.all_done());
    higher.load_mask(0xFFFFFFFFu);
    std::printf("objectives: all_done with mask=0x%08x count=2 -> %d (high bits ignored)\n",
                higher.mask(), higher.all_done() ? 1 : 0);
    CHECK(higher.all_done());
    CHECK_EQ(higher.declared_mask(), 0x3u);

    // A missing highest declared bit keeps it false at the 30-goal maximum.
    gm::Objectives full;
    CHECK(full.declare(gm::kMaxObjectives));
    full.load_mask(0x3FFFFFFFu);
    CHECK(full.all_done());
    CHECK(full.set(29, false));
    CHECK(!full.all_done());
    std::printf("objectives: all_done count=30 mask=0x%08x -> 1, without bit 29 -> 0\n",
                bits(30));
}

// --- 103 does not touch the mask; a smaller declaration hides older bits ----
void test_redeclaration() {
    gm::Objectives objectives;
    CHECK(objectives.declare(8));
    objectives.load_mask(0xFFu);

    CHECK(objectives.declare(3));
    std::printf("objectives: declare(8)->0xff, declare(3): count=%d mask=0x%08x "
                "declared=0x%08x all_done=%d\n",
                objectives.count(), objectives.mask(), objectives.declared_mask(),
                objectives.all_done() ? 1 : 0);
    CHECK_EQ(objectives.count(), 3);
    CHECK_EQ(objectives.mask(), 0xFFu);          // proven: 103 leaves the field
    CHECK_EQ(objectives.declared_mask(), 0x7u);  // what condition 7 sees
    CHECK(objectives.all_done());                // bits 0..2 are all set
    CHECK(!objectives.done(5));                  // outside [0, 3)
    CHECK(!objectives.done(8));

    CHECK(objectives.declare(9));
    CHECK(!objectives.all_done());               // bit 8 is not set
    CHECK_EQ(objectives.declared_mask(), 0xFFu);
    std::printf("objectives: declare(9) -> declared=0x%08x all_done=0 (bit 8 missing)\n",
                objectives.declared_mask());

    CHECK(objectives.declare(0));
    CHECK_EQ(objectives.declared_mask(), 0u);
    CHECK_EQ(objectives.mask(), 0xFFu);
    CHECK(!objectives.all_done());
}

// --- save block 11 (entity -3, opcode 0x68) --------------------------------
void test_save_load() {
    gm::Objectives source;
    CHECK(source.declare(5));
    CHECK(source.set(0));
    CHECK(source.set(2));
    CHECK(source.set(4));
    const std::uint32_t saved = source.save_mask();

    gm::Objectives loaded;
    CHECK(loaded.declare(5));
    loaded.load_mask(saved);
    std::printf("objectives: save_mask=0x%08x -> load_mask -> 0x%08x (round-trip=%d)\n", saved,
                loaded.mask(), loaded.mask() == saved ? 1 : 0);
    CHECK_EQ(loaded.mask(), saved);
    CHECK_EQ(loaded.save_mask(), saved);
    CHECK_EQ(loaded.declared_mask(), saved);
    CHECK(loaded.done(0));
    CHECK(!loaded.done(1));
    CHECK(loaded.done(2));
    CHECK(!loaded.done(3));
    CHECK(loaded.done(4));
    CHECK(!loaded.all_done()); // bit 1 and bit 3 are missing
    CHECK(loaded.set(1));
    CHECK(loaded.set(3));
    CHECK(loaded.all_done());
    CHECK_EQ(loaded.mask(), 0x1Fu);

    // Foreign bits from an older, larger declaration are loaded and saved as is
    // (the original does not mask the field; condition 7 ignores them).
    gm::Objectives foreign;
    CHECK(foreign.declare(5));
    foreign.load_mask(0xFFFFFFFFu);
    std::printf("objectives: load_mask(0xffffffff) count=5 -> mask=0x%08x declared=0x%08x "
                "all_done=%d\n",
                foreign.mask(), foreign.declared_mask(), foreign.all_done() ? 1 : 0);
    CHECK_EQ(foreign.mask(), 0xFFFFFFFFu);
    CHECK_EQ(foreign.declared_mask(), 0x1Fu);
    CHECK_EQ(foreign.save_mask(), 0xFFFFFFFFu);
    CHECK(foreign.all_done());
    CHECK(!foreign.done(5));
    CHECK(!foreign.done(31));

    gm::Objectives copy;
    CHECK(copy.declare(5));
    copy.load_mask(foreign.save_mask());
    CHECK_EQ(copy.mask(), foreign.mask());

    // Loading before any declaration (level reset then -3) keeps the value.
    gm::Objectives fresh;
    fresh.load_mask(0x3u);
    CHECK_EQ(fresh.count(), 0);
    CHECK_EQ(fresh.mask(), 0x3u);
    CHECK_EQ(fresh.declared_mask(), 0u);
    CHECK(!fresh.all_done());

    // load_mask clears a pending refusal note.
    fresh.set(-1);
    CHECK(!fresh.last_error().empty());
    fresh.load_mask(0x3u);
    CHECK(fresh.last_error().empty());

    // Level reset 0x456f50: count and mask both zero.
    loaded.reset();
    std::printf("objectives: reset -> count=%d mask=0x%08x\n", loaded.count(), loaded.mask());
    CHECK_EQ(loaded.count(), 0);
    CHECK_EQ(loaded.mask(), 0u);
    CHECK_EQ(loaded.save_mask(), 0u);
    CHECK(!loaded.all_done());
}

// --- 94/95: declared secrets vs the tally ---------------------------------
void test_secrets() {
    gm::Secrets secrets;
    CHECK_EQ(secrets.tally(), 0);
    CHECK_EQ(secrets.declared(), 0);

    // 101 increments: the corpus' declared total, no cap and no dedupe.
    for (int i = 0; i < gm::kCorpusSecretDeclared; ++i) {
        CHECK(secrets.add());
    }
    CHECK_EQ(secrets.tally(), gm::kCorpusSecretDeclared);
    CHECK(secrets.set_declared(gm::kCorpusSecretDeclared));
    std::printf("secrets: add() x %d -> tally=%d declared=%d (0x4536b0, no cap)\n",
                gm::kCorpusSecretDeclared, secrets.tally(), secrets.declared());
    CHECK_EQ(secrets.tally(), secrets.declared());
    CHECK_EQ(gm::kCorpusSecretSites, 105);

    // level3: 8 increment sites on 6 declared -> the tally can pass it.
    gm::Secrets level3;
    CHECK(level3.set_declared(6));
    for (int i = 0; i < 8; ++i) {
        CHECK(level3.add());
    }
    std::printf("secrets: level3 8 sites / 6 declared -> tally=%d declared=%d over=%d\n",
                level3.tally(), level3.declared(), level3.tally() - level3.declared());
    CHECK_EQ(level3.tally(), 8);
    CHECK_EQ(level3.declared(), 6);

    // Reset 0x45280f is the tally only; the declaration survives.
    level3.reset();
    CHECK_EQ(level3.tally(), 0);
    CHECK_EQ(level3.declared(), 6);
    std::printf("secrets: reset() -> tally=%d declared=%d\n", level3.tally(),
                level3.declared());

    // level15: 4 sites on 5 declared -> the tally stays below it.
    gm::Secrets level15;
    CHECK(level15.set_declared(5));
    for (int i = 0; i < 4; ++i) {
        CHECK(level15.add());
    }
    std::printf("secrets: level15 4 sites / 5 declared -> tally=%d declared=%d under=%d\n",
                level15.tally(), level15.declared(), level15.declared() - level15.tally());
    CHECK_EQ(level15.tally(), 4);
    CHECK_EQ(level15.declared(), 5);

    // 0x45708a is the separate level-reset of the declared number.
    level15.clear_declared();
    CHECK_EQ(level15.declared(), 0);
    CHECK_EQ(level15.tally(), 4);

    // A clean set_declared clears the previous note.
    level15.set_declared(-1);
    CHECK(!level15.last_error().empty());
    CHECK(level15.set_declared(0));
    CHECK(level15.last_error().empty());
    CHECK_EQ(level15.declared(), 0);
}

// --- negatives: every refusal has a text and changes nothing ---------------
void test_negatives() {
    gm::Objectives objectives;
    CHECK(objectives.declare(30));
    CHECK(objectives.set(0));

    const std::uint32_t before = objectives.mask();
    CHECK(!objectives.set(-1));
    std::printf("objectives: set(-1): ok=0 mask=0x%08x error=\"%s\"\n", objectives.mask(),
                objectives.last_error().c_str());
    CHECK_EQ(objectives.mask(), before);
    CHECK(objectives.last_error().find("outside") != std::string::npos);

    CHECK(!objectives.set(30)); // index == count is already outside
    CHECK_EQ(objectives.mask(), before);
    CHECK(!objectives.set(31));
    std::printf("objectives: set(30)/set(31) at count=30: ok=0 mask=0x%08x error=\"%s\"\n",
                objectives.mask(), objectives.last_error().c_str());
    CHECK_EQ(objectives.mask(), before);
    CHECK(objectives.last_error().find("outside") != std::string::npos);

    // Quiet queries: false, no text, no state.
    CHECK(!objectives.done(-1));
    CHECK(!objectives.done(30));
    CHECK(!objectives.done(31));
    CHECK_EQ(objectives.mask(), before);
    CHECK(objectives.last_error().find("outside") != std::string::npos); // unchanged note

    // An undeclared object refuses any index (count 0).
    gm::Objectives none;
    CHECK(!none.set(0));
    CHECK_EQ(none.mask(), 0u);
    CHECK(none.last_error().find("outside") != std::string::npos);

    gm::Secrets secrets;
    CHECK(secrets.set_declared(101));
    CHECK(!secrets.set_declared(-1));
    std::printf("secrets: set_declared(-1): ok=0 declared=%d error=\"%s\"\n", secrets.declared(),
                secrets.last_error().c_str());
    CHECK_EQ(secrets.declared(), 101);
    CHECK_EQ(secrets.tally(), 0);
    CHECK(secrets.last_error().find("negative") != std::string::npos);
}

// --- determinism ----------------------------------------------------------
void test_determinism() {
    const auto run = [](gm::Objectives& objectives, gm::Secrets& secrets) {
        objectives.reset();
        objectives.declare(gm::kMaxObjectives);
        for (int index = 0; index < gm::kMaxObjectives; index += 2) {
            objectives.set(index);
        }
        for (int index = 1; index < gm::kMaxObjectives; index += 2) {
            objectives.set(index, false);
        }
        objectives.load_mask(objectives.save_mask() ^ 0xDEADBEEFu);
        for (int i = 0; i < 7; ++i) {
            secrets.add();
        }
        secrets.set_declared(9);
    };

    gm::Objectives first;
    gm::Secrets first_secrets;
    gm::Objectives second;
    gm::Secrets second_secrets;
    run(first, first_secrets);
    run(second, second_secrets);
    std::printf("objectives: determinism mask=0x%08x count=%d all_done=%d secrets=%d/%d\n",
                first.mask(), first.count(), first.all_done() ? 1 : 0, first_secrets.tally(),
                first_secrets.declared());
    CHECK_EQ(first.mask(), second.mask());
    CHECK_EQ(first.count(), second.count());
    CHECK_EQ(first.declared_mask(), second.declared_mask());
    CHECK_EQ(first.all_done(), second.all_done());
    CHECK_EQ(first_secrets.tally(), second_secrets.tally());
    CHECK_EQ(first_secrets.declared(), second_secrets.declared());
    CHECK_EQ(first_secrets.tally(), 7);
    // Every even bit 0..28 of the 30, then the documented XOR.
    CHECK_EQ(first.mask(), 0x15555555u ^ 0xDEADBEEFu);
    CHECK_EQ(first.count(), 30);
}

} // namespace

int main() {
    test_declare();
    test_set_and_mask();
    test_all_done();
    test_redeclaration();
    test_save_load();
    test_secrets();
    test_negatives();
    test_determinism();

    if (g_failures != 0) {
        std::fprintf(stderr, "objectives: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("objectives: ok (%d checks) declare=0..30 mask_full=0x3fffffff save_roundtrip=0x1f "
                "foreign=0xffffffff secrets=101/101 level3=8/6 level15=4/5\n",
                g_checks);
    return 0;
}
