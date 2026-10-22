#pragma once

// Minimal, deterministic thing AI state machine (docs/AI_CPP.md).
//
// This is a scaffold, not a port of the original engine AI: it has no
// navigation, no animation, no audio and no renderer/SDL dependency.  It only
// advances one agent per 91 Hz tick from the *already known* home position of a
// thing (`Things` runtime instance), measures the distance to up to eight player
// eyes (falling back to visible things), checks line of sight through
// `render/collide.h`, and switches between the states idle / patrol / alert /
// attack / dead.
//
// Determinism: agents are stored in `Things::things()` order and ticked in that
// order; there is no random source, no clock read, and no map iteration.  The
// only external input is the caller-provided player eye and collision world.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "render/collide.h" // Vec3d, CollisionWorld
#include "world/ai_move.h"  // AiPathMode/AiPathState (A4, data only)
#include "world/things.h"   // Things

namespace gm {

/// Engine tick: the same fixed 91 Hz clock as things, doors and the player.
inline constexpr int kAiTickHz = 91;
inline constexpr double kAiTickSeconds = 1.0 / 91.0;

/// 0x4344df: the AI distance metric is octagonal, not Euclidean.
/// `max + 0.25 * (mid + min)` over absolute component differences.
double octagonal_distance(const Vec3d& a, const Vec3d& b);

/// Target position history: the original keeps `history[240][3]` in the
/// tracked object (struct+0x30) and wraps `history_index` at 240
/// (struct+0x3204, docs/SAVE_FIELDS.md §9.2).  The AI mirrors that shape.
inline constexpr int kAiHistorySlots = 240;
inline constexpr int kAiHistoryMaxLag = kAiHistorySlots - 1; // 239 = 0xef
/// The original scans players 0..7 and things 8..0x10b (`FUN_00473a00`).
inline constexpr int kAiMaxPlayers = 8;

/// Effective lag from `Thing::target_history_offset` (+0x1d0): `0` keeps the
/// live target position, positive values are clamped to 1..239 — the engine's
/// 1..0xef clamp in `0x473eed` / `0x476523` (docs/SOE.md).  Shipped data uses
/// 0..10.
int ai_history_lag(int raw_offset);

/// Fixed ring of world positions, one slot per accepted tick.  `next` is the
/// next slot to overwrite (the original's `history_index`); `count` saturates
/// at 240 so "not enough history yet" is explicit rather than zero-filled.
struct TargetHistory {
    std::array<Vec3d, kAiHistorySlots> slots{};
    int next = 0;  ///< next slot to write, wraps at 240
    int count = 0; ///< valid entries, 0..240

    void reset();
    void record(const Vec3d& position);
    std::size_t size() const { return static_cast<std::size_t>(count); }
    bool empty() const { return count == 0; }

    /// `lag` is clamped to 0..kAiHistoryMaxLag; `0` is the newest entry, `1`
    /// one tick older.  Returns false when fewer than `lag + 1` entries exist.
    bool at(int lag, Vec3d& out) const;
};

enum class AiState : std::uint8_t {
    idle,
    patrol,
    alert,
    attack,
    dead,
};

const char* ai_state_name(AiState state);

/// Maximum entries of the AI weapon table (`THING_TYPES +0x218`, runtime
/// `+0x474`): `0x47dc2c` clamps the loaded count to 8.
inline constexpr int kAiMaxWeapons = 8;

/// One validated entry of the AI weapon table.  The source is the already
/// loaded `Thing::proximity` (`THING_TYPES +0x218`), which is the same table
/// the projectile proximity check uses; no fields are duplicated in `Thing`.
///
/// `count` is the table size (0..8), the remaining fields describe one entry
/// selected by `ai_weapon_select()`.  The ids and bands are data only: the AI
/// posts `AiEventType::weapon` with `event`, it never spawns a projectile or
/// applies damage.
struct AiWeapon {
    int count = 0;           ///< entries in `Thing::proximity`, clamped to 0..8
    std::uint32_t event = 0; ///< +0x21c, 0 = inactive entry
    int low = 0;             ///< +0x23c, inclusive lower distance band
    int high = 0;            ///< +0x25c, inclusive upper distance band
    int cooldown = 0;        ///< +0x27c, 91 Hz ticks (loader already ×0.91)
    Vec3d offset{};          ///< +0x29c/+0x2bc/+0x2dc, data only

    bool valid() const {
        return count > 0 && count <= kAiMaxWeapons && event != 0 && low <= high &&
               cooldown >= 0 && finite(offset);
    }
};

/// `THING_TYPES +0x1b4` -> A4 route family.  The original's path automaton
/// (`0x475f10`) handles 9 `path`, 10 `pathloop` and 12 `pathexpire`;
/// `pathrandom` (11) additionally calls `rand()` (`0x480e30`) and stays
/// unsupported.  Returns false for every other movement type.
bool ai_path_mode(int movement_type, AiPathMode& out);

/// Table size clamped to `kAiMaxWeapons`.
int ai_weapon_count(const Thing& thing);

/// Load and validate entry `index` of `thing.proximity`.  Returns false for an
/// out-of-range index, `event == 0` (inactive), `high < low`, a negative
/// cooldown or a non-finite offset; `out` is untouched then.
bool ai_weapon_at(const Thing& thing, int index, AiWeapon& out);

/// Select an active entry whose inclusive `[low, high]` band contains
/// `distance`.  `reverse` scans from the last entry down (the original's first
/// direction, `+0x468 == 0`), otherwise from the first entry up.  Returns
/// false when no entry matches; `out` is untouched then.
bool ai_weapon_select(const Thing& thing, double distance, bool reverse,
                      AiWeapon& out);

/// One deferred shot produced by an `AiEventType::weapon` event.
///
/// This is a plain data record, not a projectile: `Ai` never touches
/// `Projectiles`, never consumes ammo and never applies damage.  A caller
/// drains the queue (`take_weapon_shots`) and decides what to spawn; the
/// `game/projectile.h` bridge (`spawn_ai_shots`) is one such caller.
///
/// Units follow the game layer: `origin`/`aim` are world units,
/// `direction` is a unit vector and `speed` is **world units per second**
/// (0 = the weapon table carries no speed, so the caller must resolve it from
/// the projectile type/event).  `tick` is 1-based, like `AiEvent::tick`.
struct AiWeaponShot {
    int thing_id = -1;          ///< shooter Thing::id
    std::uint32_t event_id = 0; ///< weapon table +0x21c (GENERATE/event id)
    Vec3d origin{};             ///< muzzle: agent.position + AiWeapon::offset
    Vec3d direction{0.0, 0.0, 1.0}; ///< unit vector origin -> aim_point
    double speed = 0.0;         ///< world units/second, 0 = unresolved
    int target = -1;            ///< player slot 0..7, -1 = thing/none
    int target_thing = -1;      ///< Thing::id, -1 = player/none
    Vec3d aim{};                ///< aim point at the shot (for homing)
    double distance = 0.0;      ///< octagonal band distance used to select
    std::uint64_t tick = 0;     ///< 1-based AI tick that posted the event
};

/// Tunables.  The shipped `.soe` has fields for search radius (+0x58), target
/// zone threshold (+0x1cc), target history lag (+0x1d0), max HP (+0x1d4) and
/// movement type (+0x1b4), but the values below are **not** read from a thing
/// type yet: they are deliberate hypothesis constants for this scaffold.  See
/// docs/AI_CPP.md §3 for the proven/hypothesis split.
struct AiConfig {
    double sight_range = 512.0;        ///< world units, eye-to-eye
    double attack_range = 96.0;        ///< C++ zone radius; original +0x1cc is a target-zone threshold
    double leash = 384.0;              ///< world units, home -> agent
    double eye_height = 10.0;          ///< agent eye above its feet (LIGHT chassis)
    double alert_seconds = 0.5;        ///< visible + in range -> attack delay
    double lose_sight_seconds = 3.0;   ///< alert/attack -> patrol when unseen
    double patrol_after_seconds = 2.0; ///< idle -> patrol when no target
    double attack_period = 0.75;       ///< seconds between attack events
    double return_speed = 200.0;       ///< world units / second while returning
    double health = 100.0;             ///< per-agent hit points; original max is +0x1d4 (0 = immune)
    bool use_thing_fields = false;     ///< opt-in: use Thing's THING_TYPES copy
    /// Opt-in: while in `attack`, select a weapon from `Thing::proximity`
    /// (the `+0x218`/`+0x474` table) by the inclusive `[low, high]` distance
    /// band and the `cooldown` gate, then log `AiEventType::weapon`.  The
    /// event carries the table's `event` id; it never spawns a projectile or
    /// damages the player.  `false` keeps A2/A3/A4 and the historical scaffold
    /// byte-for-byte unchanged.
    bool use_weapon_table = false;
    /// Opt-in: additionally queue one `AiWeaponShot` per `weapon` event
    /// (requires `use_weapon_table`).  The queue is pure data: the AI still
    /// does not spawn a projectile or apply damage.  Default `false` keeps
    /// the A5 event stream and every other counter unchanged.
    bool use_weapon_shots = false;
    /// Optional muzzle speed of a queued shot, world units/second.  The AI
    /// weapon table has no speed field, so the honest default is `0` =
    /// unresolved: the caller (or `spawn_ai_shots`) takes it from the
    /// projectile type (`THING_TYPES +0x10 * 91`).  Non-finite/negative
    /// values are stored as 0.
    double weapon_shot_speed = 0.0;
    double max_dt = 181.0 / 91.0;      ///< 0x470b20 clamps dt to 181 ticks (~1.99 s)
    double los_skin = 1e-3;            ///< "still visible" tolerance at the target

    // --- A2 movement (`world/ai_move.h`), opt-in -------------------------
    /// When `true`, `alert`/`attack` seek `AiAgent::aim_point` and
    /// `patrol`/`return` seek `home` through `ai_seek_step()` (octagonal
    /// arrival, swept-sphere wall clamp, hard leash clamp).  Inside
    /// `AiAgent::standoff` the agent stops and fires, and inside
    /// `standoff - move_standoff_backoff` it backs off (A3).  When `false`
    /// the historical scaffold runs unchanged: `position` only moves on the
    /// leash/return path.
    bool use_move_step = false;
    /// Fallbacks for the movement parameters when `use_thing_fields` is off
    /// or the type record is zero.  `move_speed` is in world units per
    /// 91 Hz tick, exactly like `THING_TYPES +0x10`.
    double move_speed = 1.0;
    double move_radius = 0.0;          ///< swept-sphere radius (Thing::radius)
    double move_arrive_radius = 50.0;  ///< +0x70, octagonal distance
    /// Stop distance (`+0x1cc`, report §3/§6.2): the shipped records default to
    /// 200.  `0` walks all the way to `move_arrive_radius`.  Opt-in only, like
    /// the rest of this block; with `use_thing_fields` the per-type
    /// `target_zone_threshold` wins.
    double move_standoff = 200.0;
    /// The proven -60 band of `0x473a00`: inside `standoff - backoff` the agent
    /// backs off, between that and `standoff` it stands and fires (A3).
    double move_standoff_backoff = 60.0;
    /// Hard leash radius (`+0xd0`, report §9); `0` disables the clamp.  The
    /// anchor is `AiAgent::leash_anchor`, which `attach` sets to `home`.
    double move_leash = 0.0;

    // --- A4 path family (`0x475f10`), opt-in ------------------------------
    /// When `true`, a `patrol` agent with a route walks
    /// `AiAgent::path_waypoints` through `path_advance()` and `ai_seek_step()`
    /// instead of the A2 "walk home" step.  `alert`, `attack` and
    /// `idle(returning)` never touch the path, and the Euclidean target scan,
    /// LOS, standoff and both leash rules are unchanged.  An empty route with
    /// mode `path`/`loop` falls back to the A2 patrol behaviour; mode `expire`
    /// keeps the original's `path_count < 1` branch (the agent expires).
    /// Movement is driven by `use_path` alone; `use_move_step` only adds the
    /// alert/attack/return layers.  `false` keeps A2/A3 and the historical
    /// scaffold byte-for-byte.
    bool use_path = false;
    /// `THING_TYPES +0x1b4` movement type 9/10/12 (`path`/`pathloop`/
    /// `pathexpire`).  Ignored while `use_path` is false.
    AiPathMode path_mode = AiPathMode::loop;

    /// `pathrandom` (movement type 11) is non-deterministic in the original
    /// (`rand() % path_count` on every arrival).  It is opt-in twice: this
    /// switch must be on *and* `Ai::set_path_rng` must have provided the
    /// generator, otherwise a type-11 agent does not enter the route branch.
    bool use_path_random = false;
    /// Opt-in: fill every agent's route from the type's `+0x324[]` location
    /// ids through `Things::locations()` (`0x80d8e0`) and let the movement
    /// type (`+0x1b4`, 9/10/12) override `path_mode` per agent (A6).
    /// `false` keeps A2/A3/A4 byte-for-byte and leaves routes to `set_path`.
    bool load_path_from_types = false;
};

/// One agent, keyed by the runtime `Thing::id` from `Things`.
struct AiAgent {
    int thing_id = -1;
    int thing_type = -1;        ///< Thing::type, -1 = no record
    int movement_type = -1;     ///< 0..18; data only, A2 does not branch on it
    double search_radius = 0.0; ///< effective acquisition radius (strict <)
    int target_zone = 0;        ///< effective interaction radius (integer)
    int history_lag = 0;        ///< 0..239; 0 = current target position
    double max_health = 0.0;    ///< effective maximum HP
    bool immune = false;        ///< max_health == 0
    AiState state = AiState::idle;
    int target = -1;            ///< player index 0..7, -1 = no player target
    int target_thing = -1;       ///< Thing::id, -1 = no thing target
    bool target_is_thing = false; ///< `target_thing` is the selected target
    double alertness = 0.0;     ///< 1.0 visible, decays while unseen
    double timer = 0.0;         ///< seconds left in the current state
    double leash = 0.0;         ///< copy of the effective leash radius
    Vec3d home{};               ///< absolute `Thing::position`, never changes
    Vec3d position{};           ///< starts at home; return and opt-in A2 movement change it
    double health = 0.0;

    // --- movement (A2, only used when `AiConfig::use_move_step`) ----------
    double speed = 0.0;          ///< +0x10, world units per 91 Hz tick
    double radius = 0.0;         ///< swept-sphere radius (Thing::radius)
    double arrive_radius = 50.0; ///< +0x70, octagonal distance
    /// +0x1cc target zone.  With `use_move_step` this is the A3 engagement
    /// window: `> standoff` seek, `standoff - 60 < d <= standoff` stand and
    /// fire, `d + 60 <= standoff` retreat.  `0` disables the window.
    double standoff = 0.0;
    Vec3d leash_anchor{};        ///< hard-leash centre, `home` after attach
    double leash_distance = 0.0; ///< +0xd0, 0 = no clamp
    double heading = 0.0;        ///< atan2 of the last `move_delta`, radians
    double omega = 0.0;          ///< turn rate, reserved for the A6 integrator
    Vec3d move_delta{};          ///< displacement applied by the last tick
    /// The vitals of the owning thing, mirrored by `Ai::sync_vitals()`:
    /// the four shield gauges (hundredths) and the geometry hits left.
    std::array<int, 4> shields{};
    int hits_left = 0;
    int burn_timer = 0;          ///< mirrored status accumulator
    std::uint32_t status_flags = 0; ///< mirrored 0x18/0x40 status bits
    int expire_ticks = 0;        ///< THING_TYPES +0x1e8, data only
    std::uint32_t collide_event = 0; ///< +0x1c4
    std::uint32_t expire_event = 0;  ///< +0x1ec
    double distance = 0.0;      ///< last eye-to-target distance (0 without a target)
    /// Aim point from `target_history_offset` (lag 0 = live eye) or the
    /// current position of a thing target.  The state machine keeps using the
    /// live distance for range/LOS; this is the point an actual weapon layer
    /// should fire at.
    Vec3d aim_point{};
    double aim_distance = 0.0;
    bool has_aim = false;
    bool visible = false;       ///< last line-of-sight result
    bool returning = false;     ///< idle substate: walking home, no sight checks
    bool has_home = false;

    // --- path (A4, used by `Ai::tick` when `AiConfig::use_path`) -----------
    /// Resolved location points (`THING_TYPES +0x324[]`), at most
    /// `kAiPathMaxPoints` (32), filled through `Ai::set_path()` or the
    /// opt-in `Ai::set_path_from_thing()` loader.
    std::vector<Vec3d> path_waypoints;
    /// `+0x4ec` index and `+0x4f4` pending flag of `0x475f10`.
    AiPathState path;
    /// Movement type 9/10/12 mapped by `ai_path_mode()`; the route family this
    /// agent walks.  Only used while `AiConfig::load_path_from_types` is set,
    /// otherwise `AiConfig::path_mode` still drives every agent.
    AiPathMode path_mode = AiPathMode::loop;

    // --- weapon table (A5, used when `AiConfig::use_weapon_table`) ---------
    /// Last selected entry; `count` is filled by `attach()` even when the
    /// opt-in is off (data only, no behaviour change).
    AiWeapon weapon;
    double weapon_cooldown = 0.0; ///< seconds left from the `+0x27c` ticks
    /// Scan direction of the next selection: `true` = last..first, matching
    /// the original's `+0x468 == 0` branch; toggled after every weapon event.
    bool weapon_reverse = true;
};

enum class AiEventType : std::uint8_t {
    attach,
    sight,
    lost_sight,
    state,
    attack,
    weapon,
    damage,
    death,
    leash,
    arrived,
};

const char* ai_event_name(AiEventType type);

/// One entry of the ordered event log.  `tick` is 1-based (`0` for attach).
struct AiEvent {
    AiEventType type = AiEventType::state;
    int thing_id = -1;
    AiState from = AiState::idle;
    AiState to = AiState::idle;
    int target = -1;          ///< player slot at the event, -1 for a thing
    int target_thing = -1;    ///< Thing::id at the event, -1 for a player
    Vec3d aim{};              ///< aim point at the event (see AiAgent::aim_point)
    double value = 0.0;       ///< distance, damage amount or health after damage
    /// `AiEventType::weapon`: the table's `+0x21c` event id, 0 otherwise.
    std::uint32_t event_id = 0;
    std::uint64_t tick = 0;
};

struct AiStats {
    std::size_t attached = 0;
    std::size_t skipped_without_position = 0;
    std::size_t idle = 0;
    std::size_t patrol = 0;
    std::size_t alert = 0;
    std::size_t attack = 0;
    std::size_t dead = 0;
    std::uint64_t ticks = 0;
    std::uint64_t los_checks = 0;
    std::uint64_t los_blocked = 0;
    std::uint64_t transitions = 0;
    std::uint64_t attacks = 0;
    std::uint64_t damage_events = 0;
    std::uint64_t deaths = 0;
    std::uint64_t ignored_ticks = 0;
    std::uint64_t clamped_ticks = 0;
    std::uint64_t invalid_damage = 0;
    std::uint64_t vitals_syncs = 0;  ///< `sync_vitals()` calls
    std::uint64_t thing_deaths = 0;  ///< agents killed by the thing's vitals
    std::uint64_t move_steps = 0;    ///< A2 steps that changed `position`
    std::uint64_t blocked_steps = 0; ///< A2 steps clamped by a wall
    std::uint64_t leash_clamps = 0;  ///< A2 steps cancelled by the hard leash
    std::uint64_t standoff_holds = 0; ///< A3 ticks standing inside the window
    std::uint64_t retreat_steps = 0;  ///< A3 steps away from the target
    std::uint64_t path_expiries = 0;  ///< A4 `pathexpire` agents turned dead
    std::uint64_t weapon_events = 0;  ///< A5 entries selected and posted
    std::uint64_t weapon_shots = 0;   ///< A5b shots queued for the caller
    std::size_t typed_agents = 0;   ///< agents with a THING_TYPES record
    std::uint64_t immune_hits = 0;  ///< damage rejected by max_health == 0
};

/// The level's AI runtime.  `attach` reads the thing runtime once; afterwards
/// the AI owns its state and never mutates `Things`.
class Ai {
public:
    explicit Ai(const AiConfig& config = {}) : config_(config) {}

    /// Rebuild the agent list from the *executed* things view.  Only things
    /// with an absolute position can be agents; the rest are counted in
    /// `skipped_without_position`.  Returns the number of attached agents.
    std::size_t attach(const Things& things);
    /// The AI keeps a pointer to `things` for `sync_vitals`; a temporary would
    /// dangle on the next tick.
    std::size_t attach(Things&& things) = delete;

    /// Set the players' eye points (world units), one to eight entries.  The
    /// old single-player form is `set_player`.  Returns false and leaves the
    /// previous set untouched for an empty span, more than eight entries or a
    /// NaN/inf.  `clear_players()` means "no players"; `tick` is safe then.
    bool set_players(std::span<const Vec3d> eyes);
    bool set_player(const Vec3d& eye);
    void clear_players();
    void clear_player() { clear_players(); }
    bool has_player() const { return player_count_ > 0; }
    int player_count() const { return player_count_; }
    bool player_eye(int index, Vec3d& out) const;

    /// Advance one or more fixed ticks.  `dt` defaults to exactly 1/91 s and is
    /// clamped to `config().max_dt`; non-finite or non-positive values are
    /// ignored and counted in `stats().ignored_ticks`.
    void tick(const CollisionWorld& world, double dt = kAiTickSeconds);

    /// Apply damage to one agent.  Unknown ids, dead agents, non-finite and
    /// non-positive amounts are rejected (and counted in `invalid_damage`).
    /// The AI state dies at `health <= 0`; the owning `Things` is not changed.
    bool damage(int thing_id, double amount);

    /// Mirror the vitals of the owning things into the agents: the four
    /// shield gauges, the geometry hits left and (with
    /// `use_thing_fields`) the current health.  A thing that is dead
    /// (`alive == false`) or whose health reached zero turns its agent into
    /// `AiState::dead`.
    ///
    /// This is the bridge to the runtime damage path: the weapons apply
    /// `apply_thing_damage` to the thing, and the AI picks the result up on
    /// the next tick.  The AI never mutates `Things`.  Returns the number of
    /// agents that changed state.
    std::size_t sync_vitals();

    /// Test/debug hook: move an agent without changing its state.  Used to put
    /// an agent outside its leash; `use_move_step` (§7) can move it back.
    bool place(int thing_id, const Vec3d& position);

    /// Replace an agent's patrol route (A4, `0x475f10`).  At most
    /// `kAiPathMaxPoints` (32) points are kept; the cursor and the `expired`
    /// latch are reset.  Rejects unknown/dead ids and NaN/inf and leaves the
    /// previous route untouched then.  An empty span clears the route.  The
    /// route is walked only while `AiConfig::use_path` is set and the agent is
    /// in `patrol`.
    bool set_path(int thing_id, std::span<const Vec3d> waypoints);

    /// A6: resolve the owning thing's `THING_TYPES +0x324[]` ids through the
    /// runtime location table (`Things::locations()`, `0x80d8e0`; the linear
    /// scan of `0x476012`) and install them as the patrol route.  Positions
    /// are the raw integer operands (`fild` at `0x47602e`), at most 32 points;
    /// ids missing from the table are skipped, exactly like the original's
    /// `0x476023` miss branch leaves that point unset.  Movement type 9/10/12
    /// Generator for `pathrandom` (movement type 11, `0x480e30`).  A null
    /// pointer (the default) keeps the mode inert; the state is caller-owned
    /// so a replay can restore the original's sequence.
    void set_path_rng(MsvcRand* rng) { path_rng_ = rng; }
    MsvcRand* path_rng() const { return path_rng_; }

    /// also sets `AiAgent::path_mode`.  Returns false for an unknown/dead
    /// agent, a thing without ids, an unattached `Things`, or a route where no
    /// id resolved; the previous route is untouched then.
    bool set_path_from_thing(int thing_id);

    /// Ring of player 0's world positions.  `tick` appends every live player
    /// eye exactly once per accepted tick; `attach`/`clear_players` reset all
    /// rings.  Agents consume them through `AiAgent::aim_point` at their
    /// `history_lag`.
    const TargetHistory& target_history(int player = 0) const;
    void reset_target_history();

    /// Explicit append for tests/debug; NaN/inf is rejected.  The normal path
    /// is `tick`, which records each player eye once per accepted tick.
    bool record_target_position(const Vec3d& position);
    bool record_target_position(int player, const Vec3d& position);

    /// Position of player 0 `lag` ticks back: `lag == 0` is the live eye (or
    /// the newest entry without a player), positive lags are clamped to
    /// 1..239.  False when the ring is shorter than `lag + 1`.
    bool target_position(int lag, Vec3d& out) const;
    /// Same for one player slot, matching the original's per-player ring.
    bool target_position(int player, int lag, Vec3d& out) const;

    const std::vector<AiAgent>& agents() const { return agents_; }
    const AiAgent* agent(int thing_id) const;
    const AiStats& stats() const { return stats_; }
    const std::vector<AiEvent>& events() const { return events_; }
    void clear_events() { events_.clear(); }
    /// A5b: shots queued since the last drain, oldest first.  Empty unless
    /// `AiConfig::use_weapon_shots` is on and a `weapon` event fired.
    const std::vector<AiWeaponShot>& weapon_shots() const { return weapon_shots_; }
    /// Move the queued shots out and clear the queue; the caller owns them.
    std::vector<AiWeaponShot> take_weapon_shots();
    /// Discard the queued shots without spawning anything.
    void drain_weapon_shots() { weapon_shots_.clear(); }
    const AiConfig& config() const { return config_; }

private:
    static bool line_of_sight(const CollisionWorld& world, const Vec3d& from_eye,
                              const Vec3d& to_eye, double distance, double skin);
    void set_state(AiAgent& agent, AiState next, std::uint64_t tick);
    void log(AiEventType type, const AiAgent& agent, double value, std::uint64_t tick,
             std::uint32_t event_id = 0);
    void refresh_stats();

    AiConfig config_;
    MsvcRand* path_rng_ = nullptr;
    std::vector<AiAgent> agents_;
    std::vector<AiEvent> events_;
    std::vector<AiWeaponShot> weapon_shots_;
    AiStats stats_;
    std::array<TargetHistory, kAiMaxPlayers> histories_{};
    std::array<Vec3d, kAiMaxPlayers> player_eyes_{};
    int player_count_ = 0;
    /// The things the agents belong to (read only); set by `attach()`.
    const Things* things_ = nullptr;
};

} // namespace gm
