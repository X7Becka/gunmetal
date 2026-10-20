#pragma once

// Minimal, deterministic thing AI state machine (docs/AI_CPP.md).
//
// This is a scaffold, not a port of the original engine AI: it has no
// navigation, no animation, no audio and no renderer/SDL dependency.  It only
// advances one agent per 91 Hz tick from the *already known* home position of a
// thing (`Things` runtime instance), measures the distance to one player eye,
// checks line of sight through `render/collide.h`, and switches between the
// states idle / patrol / alert / attack / dead.
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
#include "world/things.h"   // Things

namespace gm {

/// Engine tick: the same fixed 91 Hz clock as things, doors and the player.
inline constexpr int kAiTickHz = 91;
inline constexpr double kAiTickSeconds = 1.0 / 91.0;

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
    double max_dt = 181.0 / 91.0;      ///< 0x470b20 clamps dt to 181 ticks (~1.99 s)
    double los_skin = 1e-3;            ///< "still visible" tolerance at the target
};

/// One agent, keyed by the runtime `Thing::id` from `Things`.
struct AiAgent {
    int thing_id = -1;
    int thing_type = -1;        ///< Thing::type, -1 = no record
    int movement_type = -1;     ///< 0..18; data only, no navigation yet
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
    Vec3d position{};           ///< starts at home; only return movement changes it
    double health = 0.0;
    /// The vitals of the owning thing, mirrored by `Ai::sync_vitals()`:
    /// the four shield gauges (hundredths) and the geometry hits left.
    std::array<int, 4> shields{};
    int hits_left = 0;
    int expire_ticks = 0;        ///< THING_TYPES +0x1e8, data only
    std::uint32_t collide_event = 0; ///< +0x1c4
    std::uint32_t expire_event = 0;  ///< +0x1ec
    double distance = 0.0;      ///< last eye-to-eye distance (0 without a player)
    /// Aim point from `target_history_offset` (lag 0 = live eye).  The state
    /// machine keeps using the live distance for range/LOS; this is the point
    /// an actual weapon layer should fire at.
    Vec3d aim_point{};
    double aim_distance = 0.0;
    bool has_aim = false;
    bool visible = false;       ///< last line-of-sight result
    bool returning = false;     ///< idle substate: walking home, no sight checks
    bool has_home = false;
};

enum class AiEventType : std::uint8_t {
    attach,
    sight,
    lost_sight,
    state,
    attack,
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
    double value = 0.0;       ///< distance, damage amount or health after damage
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
    /// an agent outside its leash; real movement (navigation) is not modelled.
    bool place(int thing_id, const Vec3d& position);

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
    const AiConfig& config() const { return config_; }

private:
    static bool line_of_sight(const CollisionWorld& world, const Vec3d& from_eye,
                              const Vec3d& to_eye, double distance, double skin);
    void set_state(AiAgent& agent, AiState next, std::uint64_t tick);
    void log(AiEventType type, const AiAgent& agent, double value, std::uint64_t tick);
    void refresh_stats();

    AiConfig config_;
    std::vector<AiAgent> agents_;
    std::vector<AiEvent> events_;
    AiStats stats_;
    std::array<TargetHistory, kAiMaxPlayers> histories_{};
    std::array<Vec3d, kAiMaxPlayers> player_eyes_{};
    int player_count_ = 0;
    /// The things the agents belong to (read only); set by `attach()`.
    const Things* things_ = nullptr;
};

} // namespace gm
