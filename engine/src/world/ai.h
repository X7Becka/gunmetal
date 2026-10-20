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

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/collide.h" // Vec3d, CollisionWorld
#include "world/things.h"   // Things

namespace gm {

/// Engine tick: the same fixed 91 Hz clock as things, doors and the player.
inline constexpr int kAiTickHz = 91;
inline constexpr double kAiTickSeconds = 1.0 / 91.0;

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
    double max_dt = 0.25;              ///< larger tick arguments are clamped
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
    int target = -1;            ///< 0 = the single player, -1 = none
    double alertness = 0.0;     ///< 1.0 visible, decays while unseen
    double timer = 0.0;         ///< seconds left in the current state
    double leash = 0.0;         ///< copy of the effective leash radius
    Vec3d home{};               ///< absolute `Thing::position`, never changes
    Vec3d position{};           ///< starts at home; only return movement changes it
    double health = 0.0;
    double distance = 0.0;      ///< last eye-to-eye distance (0 without a player)
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

    /// Set the one player eye point (world units).  Returns false and leaves
    /// the previous value untouched for NaN/inf.  `clear_player()` means "no
    /// player present"; `tick` is safe in that state.
    bool set_player(const Vec3d& eye);
    void clear_player();
    bool has_player() const { return has_player_; }

    /// Advance one or more fixed ticks.  `dt` defaults to exactly 1/91 s and is
    /// clamped to `config().max_dt`; non-finite or non-positive values are
    /// ignored and counted in `stats().ignored_ticks`.
    void tick(const CollisionWorld& world, double dt = kAiTickSeconds);

    /// Apply damage to one agent.  Unknown ids, dead agents, non-finite and
    /// non-positive amounts are rejected (and counted in `invalid_damage`).
    /// The AI state dies at `health <= 0`; the owning `Things` is not changed.
    bool damage(int thing_id, double amount);

    /// Test/debug hook: move an agent without changing its state.  Used to put
    /// an agent outside its leash; real movement (navigation) is not modelled.
    bool place(int thing_id, const Vec3d& position);

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
    Vec3d player_eye_{};
    bool has_player_ = false;
};

} // namespace gm
