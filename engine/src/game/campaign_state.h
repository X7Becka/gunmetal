#pragma once

// Campaign progress (`M9.1`, slice 2): the mission counter `ds:0x80f9cc`, the
// 66 `NEW_LEVEL`/`NEXT_LEVEL` sites of the 36 shipped levels, the HQ restart,
// objectives, secrets and the `GUNMETAL.HI` best times.
//
// The facts come from `docs/CAMPAIGN_FLOW.md`, taken from `gundx.exe` v1.36
// (read only) and the shipped `.sbt`/`SAVEGAME` files; the runnable Python
// counterpart is `tools/re/campaign_flow.py`.  Proven in the original:
//
//   * the counter is `ds:0x80f9cc`, u32, the **number of the next mission**
//     (§2).  New game writes 1 (`0x442a0e`, `0x442a4c`, `0x442cf7`), the
//     "empty level" path writes 1 too (`0x442a39`..`0x442a4c`; name `LEVEL0`
//     from `0x4ab6d4`);
//   * `NEXT_LEVEL` (cid 0x3e, `0x457b11`) builds `sprintf(buf, "LEVEL%ld",
//     counter)` (`0x457b17`..`0x457b21`, format `0x4b8588`) and then increments
//     the counter (`0x457b26`..`0x457b2f`);
//   * `NEW_LEVEL "stem"` (cid 0x3d, `0x457af3`) copies the stem and does
//     **not** touch the counter (§3);
//   * `NEXT_LEVEL_NUMBER n` (cid 8, `0x45ea26`) is `counter == n` (§2);
//   * "Restart level from HQ" calls `0x4428f0(1)`; the `param & 1` branch
//     decrements the counter (`0x44295e`) and clamps it to >= 1
//     (`0x442973`).  It also requires the current name not to be `LEVEL0`
//     (`0x442952`).  The menu entry is `0x4bd3fc` -> `0x488931` -> `0x48da09`,
//     hidden for `LEVEL20`/`SECRT1`/`SECRT2` (`0x4888e4`); loading a save
//     (`0x442340`) does not change the counter (§2, the fix to FORMATS §15
//     spelled out in `CAMPAIGN_FLOW.md` §11);
//   * a mission returns to its HQ by name with `NEW_LEVEL "<hq>"` in the
//     `entity -8` script; the HQ's `entity 305` runs `NEXT_LEVEL`, so the next
//     mission number comes from the counter, not from the HQ (§3);
//   * 66 transition sites in 36 levels: 60 `NEW_LEVEL` + 6 `NEXT_LEVEL`
//     (§3, `--summary`; `docs/VM_SCENE.md` §4).  The 6 `NEXT_LEVEL` sites are
//     the five HQs plus `level19`: the only mission without an HQ (§2/§3);
//   * levels: 24 missions `level1`..`level24`, 5 HQs `level0`, `level00`,
//     `level000`, `level0a`, `level0b` (`COMMAND POST`), 4 deathmatch maps and
//     3 secret levels (§8).  `level24` has **no campaign exit**: its `-8`
//     (`0xf799`) is `IF DEATHMATCH_GAME { NEW_LEVEL "DM1" }` and nothing else
//     (§7), so the campaign ends by the absence of a transition, not by
//     `END_GAME` (0 sites in the shipped corpus);
//   * objectives (§4): 25 levels declare them, 51 primary lines, 35
//     declarations, 48 `LEVEL_OBJECTIVES_COMPLETE` sites, at most 30 per level
//     (`cmp eax,0x1e`, `0x45bdf6`).  Completion bits are a mask
//     (`OR_LEVEL_OBJECTIVES_BITS` 104, `0x45be40`, reset `0x4570a3`);
//   * secrets (§5): 101 declared (`SET_LEVEL_SECRETS` 94 -> `0x80d8c0`),
//     105 `INCREMENT_SOURCEPLAYER_SECRET_TALLY` sites (95, `0x45b978` ->
//     `0x4536b0`), per-player tally at `player+0x33cc` and reset on every
//     level start (`0x45280f`);
//   * `GUNMETAL.HI` (§6): 576 bytes = 9 records of `i32 seconds` + `char
//     name[60]`; the scan/append is `0x45c880`, name compare is
//     case-insensitive (`0x416850`), the minimum per name is `0x45c8d6`;
//     duplicates are legal because a better time is only appended.
//
// Model, not a port -- every such place is marked `MODEL`:
//
//   * `new_level()` is the assignment's name for "start the next level" and is
//     the proven `NEXT_LEVEL` step (`0x457b2e`).  The original increments
//     blindly; the model refuses to leave the shipped campaign, so the counter
//     stays in `1..25` (`CAMPAIGN_FLOW.md` §1: 24 missions, `level24` played at
//     25).  The `NEW_LEVEL` command itself changes nothing and is only a table
//     lookup here (`next_level()`).
//   * `next_level(level)` returns the next **campaign** file: a mission's `-8`
//     gives its HQ, an HQ (or `level19`) runs `NEXT_LEVEL` and the counter
//     picks `level<counter>`.  Which of several `NEW_LEVEL` branches the game
//     takes (the `DEATHMATCH_GAME` branches, `level4`/`level16` secret branches,
//     two HQs in `level6`) is not modelled: the table keeps the campaign one
//     (`level6` prefers `level00`, its alternative is `campaign_alt`).
//   * stems are reported lowercase (`level2`) to match the shipped file names;
//     the original builds the buffer `LEVEL<n>` and `load_level` (`0x442ef0`)
//     resolves the file.
//   * `load(counter)` clamps to `>= 1`; the original reads the save field as is
//     (`0x45586e`) and only the restart path clamps.
//   * `CampaignStats` is bookkeeping of this module: the original stores no
//     such counters (mission count lives only in the counter, `hq_visits`,
//     `restarts` and `objectives_done` have no field at all).
//   * `on_objective()` counts goals; the original ORs bits into `0x80f9d0`
//     (idempotent per bit).  No bits or indices are modelled.
//   * `add_secret()` increments as `0x4536b0` does -- no cap and no dedupe.
//     The data proves the tally can exceed the declared count (`level3` has 8
//     sites for 6 declared, `secrt2` 2 sites and no declaration, `temple` 1/0),
//     it does **not** prove "at most one per secret" (`level15`: 4 sites for 5
//     declared), so no such rule is applied.
//   * `GUNMETAL.HI` records hold the name up to the first NUL (the engine
//     compares/prints a C string); the shipped file has zero bytes after every
//     terminator, so its round-trip is byte exact.  A file with more than 9
//     records is refused because the model holds exactly the shipped table.
//
// Pure state: no runtime, no VM, no menu, no clock, no RNG, no rendering; the
// only I/O is `load_hi()`/`save_hi()`.  Not wired into `GameRuntime`, the
// frontend or the save writer, and it has no objective/briefing texts
// (docs/CAMPAIGN_STATE.md).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace gm {

/// Missions `level1`..`level24` (`CAMPAIGN_FLOW.md` §1/§3).
inline constexpr int kCampaignMissions = 24;
/// HQs: `level0`, `level00`, `level000`, `level0a`, `level0b` (§3).
inline constexpr int kCampaignHqs = 5;
/// Levels of the shipped corpus: 24 missions + 5 HQs + 4 DM + 3 secret (§8).
inline constexpr int kCampaignLevelCount = 36;
/// `NEW_LEVEL` + `NEXT_LEVEL` sites over the 36 levels (§3).
inline constexpr int kCampaignTransitionSites = 66;
/// 60 `NEW_LEVEL` sites (§3, `--summary`).
inline constexpr int kCampaignNewLevelSites = 60;
/// 6 `NEXT_LEVEL` sites: the five HQs + `level19` (§3).
inline constexpr int kCampaignNextLevelSites = 6;

/// Counter of a fresh game (§2: `0x442a0e`/`0x442a4c`/`0x442cf7`).
inline constexpr int kCampaignFirstCounter = 1;
/// Counter while `level24` is played: 25, and the campaign ends there (§7).
inline constexpr int kCampaignLastCounter = 25;

/// Levels that declare objectives (25, §4).
inline constexpr int kCampaignObjectiveLevels = 25;
/// Primary objective lines (51, §4).
inline constexpr int kCampaignObjectiveLines = 51;
/// Objective declarations including variants (35, §4).
inline constexpr int kCampaignObjectiveDeclarations = 35;
/// `LEVEL_OBJECTIVES_COMPLETE` sites (48, §4).
inline constexpr int kCampaignObjectiveCompleteSites = 48;
/// `DEFINE_LEVEL_OBJECTIVES` cap: `cmp eax,0x1e` at `0x45bdf6` (§4).
inline constexpr int kCampaignMaxObjectives = 30;

/// Declared secrets over the corpus (101, §5).
inline constexpr int kCampaignSecretDeclared = 101;
/// `INCREMENT_SOURCEPLAYER_SECRET_TALLY` sites (105, §5).
inline constexpr int kCampaignSecretSites = 105;
/// `player+0x33cc`, the per-player secret tally (§5).
inline constexpr std::uint32_t kCampaignSecretOffset = 0x33CC;

/// `GUNMETAL.HI` record: `i32 seconds; char name[60]` (§6).
inline constexpr int kHiRecordSize = 0x40;
inline constexpr int kHiNameSize = 0x3C;
/// Records of the shipped file: 576 bytes = 9 records (§6).
inline constexpr int kHiRecords = 9;
inline constexpr int kHiFileSize = kHiRecordSize * kHiRecords;

/// Role of one shipped level (`CAMPAIGN_FLOW.md` §8).
enum class CampaignRole : std::uint8_t {
    hq,         ///< `level0`, `level00`, `level000`, `level0a`, `level0b`
    mission,    ///< `level1`..`level24`
    deathmatch, ///< `dm1`..`dm4`
    secret,     ///< `secrt1`, `secrt2`, `temple`
};

/// What the level does when it is left in the campaign (§3).
enum class CampaignExit : std::uint8_t {
    /// `NEXT_LEVEL` (`0x457b11`): name from the counter, counter + 1.
    next_level,
    /// `NEW_LEVEL "<hq>"` (`0x457af3`): counter untouched.
    new_level,
    /// `level24`: the `-8` script only leaves for a deathmatch game (§7).
    campaign_end,
    /// Not on the campaign path (`dm1`..`dm4`, secret levels).
    other,
};

/// One row of the transition table (§8).
struct CampaignLevel {
    /// File stem the engine loads, lowercase (`level2`, `level0b`).
    std::string_view stem;
    /// `SET_LEVEL_NAME` text (`0x45b73c`), as the hi-score table shows it.
    std::string_view name;
    CampaignRole role;
    /// `NEW_LEVEL` sites in the completion script(s), all branches (§8).
    int new_level_sites = 0;
    /// `NEXT_LEVEL` sites: 1 for the five HQs and `level19` (§3).
    int next_level_sites = 0;
    /// What the campaign does from this level.
    CampaignExit exit = CampaignExit::other;
    /// `NEW_LEVEL` target of a mission: its HQ (empty when not `new_level`).
    std::string_view target;
    /// `level6` accepts a second HQ (`level0a`, §3); empty otherwise.
    std::string_view campaign_alt;
};

/// All 36 shipped levels in table order (missions, HQs, DM, secret).
std::span<const CampaignLevel> campaign_levels() noexcept;

/// Row for *stem* (case-insensitive, no extension), or `nullptr`.
const CampaignLevel* campaign_level(std::string_view stem) noexcept;

/// Counters of this module.  No field of the original holds them (`MODEL`).
struct CampaignStats {
    int missions = 0;        ///< completed missions
    int hq_visits = 0;       ///< HQ `NEXT_LEVEL` executions (23 in one campaign)
    int secrets = 0;         ///< found secrets, `0x4536b0` tally
    int objectives_done = 0; ///< goals marked done
    int restarts = 0;        ///< "Restart level from HQ" calls
};

/// One `GUNMETAL.HI` record (§6).
struct HiRecord {
    int seconds = 0;
    std::string name; ///< latin1 bytes up to the first NUL
};

/// Campaign progress of one player.
///
/// The counter (`ds:0x80f9cc`) is the whole campaign state of the original;
/// everything else here is derived or module bookkeeping.  A refusal always
/// leaves a one-line reason in `last_error()`.
class CampaignState {
public:
    /// New game: the counter is 1 and the frontend loads `level0` (§2).
    void new_game();

    /// Load a save: the counter is taken as is, clamped to `>= 1` (§2).  No
    /// decrement happens here -- the `-1` of `0x44295e` belongs to
    /// `restart_level_from_hq()`.  The module counters restart and
    /// `next_stem()` is cleared: the saved level name is the save header's, not
    /// the counter's, so no next jump can be derived before the next call.
    void load(int counter);

    /// The counter, the number of the next mission (§2).
    int counter() const { return counter_; }

    /// Mission being played: `counter - 1` (1 while `counter == 2`), 0 when the
    /// counter is 1 (the starting HQ `level0`).  `MODEL`, derived from §2.
    int mission_index() const { return counter_ - kCampaignFirstCounter; }

    /// `NEXT_LEVEL` without the name step: increments the counter
    /// (`0x457b26`..`0x457b2f`).  Returns false, with a text, once the counter
    /// is 25 (`level24` is played) -- the shipped campaign has no mission 26.
    /// `MODEL`: the original increments unconditionally.
    bool new_level();

    /// One completion step of *level* (`NEW_LEVEL`/`NEXT_LEVEL` of §3).
    ///
    /// A mission stores its HQ in `next_stem()` and leaves the counter alone; an
    /// HQ or `level19` stores `level<counter>` and increments the counter; a
    /// non-campaign level or unknown stem is refused with a text; `level24`
    /// has no campaign exit (§7) and is refused.  Returns true when the
    /// campaign has a next file.
    bool next_level(const std::string& level);

    /// The file `next_level()` resolved (`level00`, `level2`, ...); empty before
    /// the first call.  `new_game()` sets it to `level0` (the frontend's level).
    const std::string& next_stem() const { return next_stem_; }

    /// "Restart level from HQ": counter - 1, clamped to `>= 1`
    /// (`0x4428f0(1)` -> `0x44295e`, clamp `0x442973`).  The original skips it
    /// for `LEVEL0` (`0x442952`); the caller is the pause menu entry only.
    void restart_level_from_hq();

    /// One completed mission (bookkeeping).
    void on_mission_complete();

    /// One found secret: the `0x4536b0` tally increment (§5).  No cap, no
    /// per-secret dedupe (see the header comment).
    void add_secret();

    /// Level start: the tally is reset (`0x45280f`, §5).
    void reset_level_secrets();

    /// One goal marked done (§4).
    void on_objective();

    /// Found secrets of the current level.
    int secrets() const { return stats_.secrets; }
    /// Goals marked done since `new_game()`/`load()`.
    int objectives_done() const { return stats_.objectives_done; }
    const CampaignStats& stats() const { return stats_; }

    /// True when the counter is 25, i.e. `level24` is the level being played.
    bool at_last_mission() const { return counter_ == kCampaignLastCounter; }

    // --- GUNMETAL.HI (§6) --------------------------------------------------

    /// Seconds of every loaded record, in file order.  Duplicates are legal and
    /// kept (§6: a better time is appended, the old record stays).
    std::array<int, kHiRecords> best_times{};
    /// Names of the same records.
    std::array<std::string, kHiRecords> best_names{};

    /// Records loaded (0..9).  `best_times`/`best_names` past it are zero.
    int hi_count() const { return hi_count_; }

    /// Read a `GUNMETAL.HI`.  Refused with a text for a missing file, a size
    /// that is not a multiple of 0x40, or more than 9 records; a 0-byte file is
    /// the empty table (0 records).  *error*, when given, also receives the
    /// text; `last_error()` always does.
    bool load_hi(const std::string& path, std::string* error = nullptr);

    /// Write the loaded records back: `hi_count() * 0x40` bytes, `i32`
    /// little-endian seconds and a NUL-padded 60-byte name.  Byte-exact
    /// round-trip of the shipped file.
    bool save_hi(const std::string& path, std::string* error = nullptr) const;

    /// Replace one record (index `0..hi_count()-1`).  Refused with a text for
    /// an out-of-range index, a negative time, a name over 60 bytes or an
    /// embedded NUL.
    bool set_hi_record(int index, int seconds, const std::string& name,
                       std::string* error = nullptr);

    /// Edit the seconds of one record only (`set_hi_record` with the current
    /// name); the saved file then differs in this record's 4 bytes.
    bool set_hi_time(int index, int seconds, std::string* error = nullptr);

    /// Best (minimum) time recorded for a level *name*, case-insensitive as
    /// `0x416850` (§6); `-1` when the name is absent.
    int best_time(std::string_view name) const;

    /// Reason of the last refusal (empty when the last call was clean).
    const std::string& last_error() const { return last_error_; }
    void clear_error() { last_error_.clear(); }

private:
    /// Store *text* as the last error and return false.
    bool fail(std::string text, std::string* error) const;

    int counter_ = kCampaignFirstCounter;
    CampaignStats stats_{};
    std::string next_stem_;
    int hi_count_ = 0;
    mutable std::string last_error_;
};

} // namespace gm
