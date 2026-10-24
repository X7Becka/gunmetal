#pragma once

// Level objectives and secrets (`M6.4.5`, slice 2): the two pieces of level
// state the scripts own and the engine only stores.
//
// Every address below is a full VA of `gundx.exe` v1.36 (ImageBase `0x400000`)
// and refers to `docs/OBJECTIVES.md` (cited as O), unless another document is
// named.  The shipped `.sbt` corpus gives the data (§5 of O).
//
// Proven by code (O §2/§3/§7):
//
//   * `DEFINE_LEVEL_OBJECTIVES` (103, `0x45bdf0`) refuses more than 30 goals:
//     `cmp eax,0x1e` at `0x45bdf6` jumps to the error path `0x45c4c1`.  It
//     writes the count to `ds:0x80d7ec` (`0x45be04`) and the text pointers to
//     `ds:0x80d800 + 4*i` (`0x45be11`/`0x45be21`, step `0x45be34`); the texts
//     themselves stay in the level code, so this module stores no strings.
//   * `OR_LEVEL_OBJECTIVES_BITS` (104, `0x45be40`) ORs the value into the mask
//     `ds:0x80f9d0` (`0x45be49`/`0x45be4e`).  It is the **only** writer besides
//     the level reset `0x4570a3` (O §2.3: exactly seven dword references, the
//     other four are readers).
//   * "all objectives done" (cid 7, `0x45e9f0`) is a loop: `count <= 0` is
//     false (`0x45e9fa`), otherwise every `1 << i` for `i < count` must be in
//     the mask (`0x45ea00`..`0x45ea17`).  Higher bits are ignored.
//   * level reset `0x456f50` zeroes the count (`0x45709d`), the mask
//     (`0x4570a3`) and the declared secrets (`0x45708a`).
//   * the mask is saved: the sub-script of entity `-3`
//     (`INTERNAL_LEVEL_RESUME`, created at `0x462e0c`) gets opcode `0x68` = 104
//     plus the mask, and only when the mask is non-zero (`0x462f2a`..`0x462f60`,
//     O §2.5).  This is block 11 of the save (`docs/CAMPAIGN_FLOW.md` §4).
//   * `SET_LEVEL_SECRETS` (94, `0x45b81e`) writes the declared number to
//     `ds:0x80d8c0`; `INCREMENT_SOURCEPLAYER_SECRET_TALLY` (95, `0x45b978`)
//     calls `0x4536b0`, which increments `player+0x33cc` = `0x81c40c` once
//     (stride `0x3510`, O §3.1).  There is no cap and no dedupe: the corpus
//     proves the tally can exceed the declaration (`level3`: 8 sites for 6
//     declared, O §3.4).
//   * the tally is cleared on every level start (`0x45280f`, O §3.1).
//
// Model, not a port -- marked `MODEL`:
//
//   * this class stores the count and the mask only.  The 30 text pointers and
//     the HUD (`0x487337`, markers `0x4833e0`) are not implemented; the
//     seconds/level-end screen (`0x483c07`) and the `-3`/`0x68` replay are not
//     emulated either.
//   * `set(index, false)` clears a bit.  The original's opcode 104 only ORs;
//     clearing is this module's API for a session that revises its own state,
//     and must not be read as something the scripts can do.
//   * `declare()` does **not** touch the mask (proven: 103 writes only the
//     count and the pointers).  A later, smaller declaration therefore leaves
//     older bits in the field; they are preserved as `mask()` and are ignored
//     by `all_done()`/`declared_mask()`, exactly as condition 7 ignores them.
//   * `load_mask()` stores the 32-bit field as is: a save may carry bits from
//     an earlier, larger declaration and the original loads them unchanged.
//     They survive `save_mask()` bit for bit (round-trip), but are invisible to
//     `declared_mask()`/`all_done()`.
//   * `done(index)` is a quiet query: an index outside `[0, count())` returns
//     `false` and does not raise a text (the original's readers never go out of
//     range).  `set()` on such an index is a refusal with a text.
//   * `Secrets::add()` saturates at `INT_MAX` and reports it, instead of the
//     undefined i32 overflow of the original; the byte-code cannot reach it.
//   * `Secrets::set_declared()` is caller input (this module reads no `94`
//     table and no save): a negative number is refused with a text, any
//     non-negative one is accepted -- the corpus' 101 is a **sum over 36
//     levels**, not a per-level cap (O §3.4).
//
// Pure state: no runtime, no VM, no save I/O, no HUD, no clock, no RNG and no
// rendering.  Nothing here is wired into `GameRuntime`, the save writer or the
// front-end (docs/OBJECTIVES_STATE.md).

#include <cstdint>
#include <string>

namespace gm {

/// `DEFINE_LEVEL_OBJECTIVES` cap: `cmp eax,0x1e`, `0x45bdf6` (O §2.1).
inline constexpr int kMaxObjectives = 30;
/// `objectives_count`, u32 (O §1).
inline constexpr std::uint32_t kObjectivesCountAddr = 0x80D7EC;
/// `objectives_bits`, u32, the only bit field of a level (O §1).
inline constexpr std::uint32_t kObjectivesMaskAddr = 0x80F9D0;
/// `DEFINE_LEVEL_OBJECTIVES` (O §2.1).
inline constexpr std::uint16_t kObjectiveDeclareOpcode = 103;
/// `OR_LEVEL_OBJECTIVES_BITS` (O §2.1).
inline constexpr std::uint16_t kObjectiveOrBitsOpcode = 104;
/// The opcode the save sub-script of entity `-3` carries (O §2.5).
inline constexpr std::uint16_t kObjectiveSaveOpcode = 0x68;

/// Declared secrets summed over the shipped corpus (101 on 25 levels, O §3.4).
inline constexpr int kCorpusSecretDeclared = 101;
/// `INCREMENT_SOURCEPLAYER_SECRET_TALLY` sites in the corpus (105 on 27
/// levels, O §3.4).
inline constexpr int kCorpusSecretSites = 105;
/// `player+0x33cc` = `0x81c40c`, the per-player tally (O §3.1).
inline constexpr std::uint32_t kSecretTallyOffset = 0x33CC;
inline constexpr std::uint32_t kSecretTallyAddr = 0x81C40C;

/// Objectives of one level: `ds:0x80d7ec` and `ds:0x80f9d0` (O §1).
///
/// A refusal always leaves a one-line reason in `last_error()` and changes
/// nothing.
class Objectives {
public:
    /// `DEFINE_LEVEL_OBJECTIVES` (103): *count* goals, `0..30` inclusive.
    ///
    /// Refused with a text for a negative count and for anything above
    /// `kMaxObjectives` (the `0x45bdf6` cap).  The declaration changes the
    /// count only; the mask and the previous count's bits stay (proven, see
    /// the header note).  The level-reset path is `reset()`.
    bool declare(int count, std::string* error = nullptr);

    /// `objectives_count` (`0x80d7ec`).
    int count() const { return count_; }

    /// Opcode 104: set (or, `MODEL`, clear) bit `1 << index`.
    ///
    /// *index* must be in `[0, count())`; anything else is refused with a text
    /// and leaves the mask unchanged.  Setting an already set bit is a no-op,
    /// as the original `or` is.
    bool set(int index, bool done = true, std::string* error = nullptr);

    /// Bit *index* of the mask.  `false` for an index outside `[0, count())`;
    /// quiet (no `last_error()`), because callers such as the HUD only walk
    /// `i < count`.
    bool done(int index) const noexcept;

    /// The mask field `ds:0x80f9d0` as stored, bit for bit.  Bits outside the
    /// declared range can only come from `load_mask()` (an older, larger
    /// declaration) and are kept, not masked away.
    std::uint32_t mask() const noexcept { return mask_; }

    /// The bits the player sees and condition 7 checks: `mask() & ((1<<count)-1)`.
    std::uint32_t declared_mask() const noexcept;

    /// Level load: the mask comes from the save (`0x456f50` resets it first,
    /// O §2.5).  Value is stored as is; bits above the current count are kept
    /// for a round-trip and ignored by `declared_mask()`/`all_done()`.
    void load_mask(std::uint32_t mask) noexcept;

    /// Block 11, entity `-3`, opcode `0x68`: the value the save writer emits
    /// when it is non-zero (O §2.5).  Byte-exact `load_mask()`/`save_mask()`
    /// round-trip.
    std::uint32_t save_mask() const noexcept { return mask_; }

    /// Condition 7 (`0x45e9f0`): `count <= 0` is false; otherwise every
    /// `1 << i`, `i < count`, must be set.  Bits above the count are ignored.
    bool all_done() const noexcept;

    /// Level reset `0x456f50` (`0x45709d` count, `0x4570a3` mask): both zero.
    void reset() noexcept;

    /// Reason of the last refusal (empty when the last call was clean).
    const std::string& last_error() const { return last_error_; }
    void clear_error() { last_error_.clear(); }

private:
    bool fail(std::string text, std::string* error) const;

    int count_ = 0;
    std::uint32_t mask_ = 0;
    mutable std::string last_error_;
};

/// Secrets of one level: `ds:0x80d8c0` and the player's tally `+0x33cc`.
///
/// The declared number is caller input (`SET_LEVEL_SECRETS`, opcode 94); the
/// tally is what opcode 95 increments.  No cap and no dedupe (O §3.1/§3.4).
class Secrets {
public:
    /// `INCREMENT_SOURCEPLAYER_SECRET_TALLY` (95, `0x45b978` ->
    /// `0x4536b0`): `tally += 1`.  No cap and no per-secret dedupe; the data
    /// proves the tally can pass the declared number.  `MODEL`: at `INT_MAX`
    /// the increment is refused with a text instead of overflowing.
    bool add(std::string* error = nullptr);

    /// Found secrets of the current level (`player+0x33cc`).
    int tally() const noexcept { return tally_; }

    /// Level start reset `0x45280f`: tally 0.  The declared number is
    /// untouched (`0x45708a` is a separate reset, see `clear_declared()`).
    void reset() noexcept { tally_ = 0; }

    /// `SET_LEVEL_SECRETS` (94, `0x45b81e`) -> `ds:0x80d8c0`.
    int declared() const noexcept { return declared_; }

    /// Caller input of the declared number: any non-negative value is
    /// accepted, a negative one is refused with a text and changes nothing.
    /// The corpus' 101 is the sum over 25 levels (O §3.4), not a cap.
    bool set_declared(int declared, std::string* error = nullptr);

    /// Level reset `0x45708a`: declared number back to 0.  `MODEL` as a
    /// separate call -- the original does both resets side by side in
    /// `0x456f50`; `reset()` keeps the caller's declaration so a per-level
    /// `set_declared()` survives a respawn-style tally reset.
    void clear_declared() noexcept { declared_ = 0; }

    /// Reason of the last refusal (empty when the last call was clean).
    const std::string& last_error() const { return last_error_; }
    void clear_error() { last_error_.clear(); }

private:
    bool fail(std::string text, std::string* error) const;

    int tally_ = 0;
    int declared_ = 0;
    mutable std::string last_error_;
};

} // namespace gm
