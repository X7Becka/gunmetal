#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game/projectile.h"
#include "render/collide.h"
#include "res/sbt.h"
#include "res/vm_exec.h"
#include "world/thing_motion.h"

namespace gm {

class EventQueue;

/// Thing runtime is driven by the level's fixed 91 Hz tick (same clock as the
/// doors and the main loop).  The runtime currently has no AI; a tick only
/// advances the per-instance age (docs/THINGS_CPP.md).
inline constexpr int kThingTickHz = 91;

/// The recursive static walk descends into nested IF/ELSE bodies.  The shipped
/// corpus peaks at depth 5; the limit turns a hostile block into a diagnostic
/// instead of unbounded recursion (gm/scene.py's MAX_THING_DEPTH).
inline constexpr int kThingMaxDepth = 64;

/// One instance built by an ``ADD_THING*`` condition.
///
/// ``type`` is the ``G`` operand, an index into the level's ``THING_TYPES``
/// record; ``0xFFFFFFFF`` is the engine's "none" and is stored as ``-1``.
/// ``model`` is the ``SOB_TABLE`` name resolved through ``THING_TYPES+0x1a4``
/// (``model_index`` keeps the raw table index).  ``position`` is present only
/// for the absolute variants (``ADD_THING`` / ``ADD_THING_EX``); the
/// source-relative variants keep it empty, exactly like the raw operands.
struct Thing {
    int id = 0;                 // runtime instance id, unique inside Things
    int type = -1;              // THING_TYPES index; -1 = 0xFFFFFFFF (none)
    std::string type_name;      // THING_TYPES name, empty for an invalid type
    std::string model;          // SOB_TABLE model name, empty when unresolved
    int model_index = -1;       // raw SOB_TABLE index, -1 when none
    std::optional<std::array<int, 3>> position;
    std::vector<double> angles;
    std::uint32_t flags = 0;    // source entity flags (runtime flag word is unknown)
    int parent = 0;             // source entity id (the sub-script's owner)
    bool alive = true;
    std::uint32_t script = 0;   // byte offset of the entity sub-script
    std::string kind;           // ADD_THING / ADD_THING_EX / ...
    int owner = 0;              // first integer operand in front of the type
    std::uint32_t site = 0;     // byte offset of the EVAL/IF instruction
    int occ = 0;                // per-path execution count (loops build twice)
    int depth = 0;              // 0 = top level, >0 = inside an IF/ELSE body
    std::vector<std::string> path; // enclosing IF labels, outermost first
    bool executed = false;      // true for the VM-built view
    std::uint64_t age = 0;      // ticks since creation (no AI yet)

    // THING_TYPES fields copied by 0x46d8b0; names/roles are proven in
    // docs/SOE.md.  They are exposed for AI wiring but not yet used by the
    // C++ state machine.
    int movement_type = -1;          // +0x1b4, 0..18
    double speed = 0.0;              // +0x10 (double), world units per tick
    double turn_accel = 0.0;         // +0x20, angular acceleration
    double turn_max = 0.0;           // +0x28, max angular speed
    // +0x90/+0x98/+0xa0 (double): pitch/roll/yaw rate of movement type 1,
    // copied to instance +0x48/+0x50/+0x58 by 0x46d8b0.
    std::array<double, 3> angle_rate{};
    double climb_speed = 0.0;        // +0x18 (double), vertical seek speed (+0xa8)
    std::uint32_t flags_a = 0;       // +0x1a8, instance +0x360 (bit 0 = slot used)
    std::uint32_t flags_b = 0;       // +0x1ac, instance +0x364 (0x40000000 lean,
                                     //   0x08000000/0x10000000 impulse bounce)
    std::uint32_t flags_c = 0;       // +0x1b0, instance +0x368
    double arrive_radius = 50.0;     // +0x70, arrival distance
    double leash_distance = 0.0;     // +0xd0, hard leash radius
    double gravity = 0.0;            // +0x78, type gravity multiplier
    double damping = 0.0;            // +0x80, speed multiplier after a bounce
    int collision_shape = 1;         // +0x1a0, 1 = sphere, 2 = box
    double radius = 0.0;             // +0xf8, sphere radius
    std::array<double, 3> box{};     // +0x100/+0x108/+0x110, box half-extents
    std::vector<ProximityEntry> proximity; // THING_TYPES+0x218, 8 records
    double search_radius = 0.0;      // +0x58 (double), target search radius
    int target_zone_threshold = 200; // +0x1cc, default 200
    int target_history_offset = 0;   // +0x1d0, target position-history lag
    int max_health = 0;              // +0x1d4, 0 = damage-immune
    int health = 0;                  // +0x1d4 current copy at spawn
    std::array<int, 4> shields{};    // +0x1d8..+0x1e4
    std::array<int, 4> shield_max{}; // +0x1d8..+0x1e4 max copies
    int burn_timer = 0;              // status -1 accumulator
    std::uint32_t status_flags = 0;  // runtime 0x18/0x40 bits
    int hits_left = 0;               // +0x1c8 geometry hits
    int expire_ticks = 0;            // +0x1e8 lifetime in 91 Hz ticks
    bool expired = false;            // lifetime reached; event posted once
    // THING_TYPES +0x320 count and +0x324 + 4*i ids.  The ids are location
    // identifiers, already resolved by the .soe loader (0x47db3d through
    // 0x460000); `Things::location()` turns them into runtime positions.
    // Clamped to 32 entries (0x47daea rejects 0x20 < count), data only.
    std::vector<int> path_locations;
    std::uint32_t timeout_event = 0; // +0x1c0
    std::uint32_t collide_event = 0; // +0x1c4, posted by 0x4752d0 on collision
    std::uint32_t expire_event = 0;  // +0x1ec, 0x4703a0 when hits/lifetime ends
    std::uint32_t create_event = 0;  // +0x1f0
    std::uint32_t damaged_event = 0; // +0x1f4
    std::uint32_t destroy_event = 0; // +0x1f8

    bool absolute() const { return position.has_value(); }
};

/// The event block ids of one ``THING_TYPES`` record.  The ids are negative
/// ``GENERATE``-style entity handles, or 0 when the field is unused.
/// ``collide_event`` is the second string of the ten-field block (``+0x1c4``);
/// 0x4752d0 posts it after a collision.  ``expire_event`` (``+0x1ec``) goes
/// through 0x4703a0 when the per-impact hits counter or lifetime is exhausted.
struct ThingTypeEvents {
    int type = -1;
    int movement_type = -1; ///< +0x1b4, the 0x470c70 dispatch index
    double speed = 0.0; ///< +0x10 (double), world units per 91 Hz tick
    double turn_accel = 0.0; ///< +0x20, angular acceleration
    double turn_max = 0.0; ///< +0x28, max angular speed
    std::array<double, 3> angle_rate{}; ///< +0x90/98/a0, spin mode
    double climb_speed = 0.0; ///< +0x18, vertical seek speed (+0xa8)
    std::uint32_t flags_a = 0; ///< +0x1a8
    std::uint32_t flags_b = 0; ///< +0x1ac
    std::uint32_t flags_c = 0; ///< +0x1b0
    double arrive_radius = 50.0; ///< +0x70, arrival distance
    double leash_distance = 0.0; ///< +0xd0, hard leash radius
    double gravity = 0.0; ///< +0x78, type gravity multiplier
    double damping = 0.0; ///< +0x80, speed multiplier after a bounce
    int collision_shape = 1; ///< +0x1a0, 1 = sphere, 2 = box
    double radius = 0.0; ///< +0xf8, sphere radius
    std::array<double, 3> box{}; ///< +0x100/+0x108/+0x110
    std::vector<ProximityEntry> proximity; ///< THING_TYPES+0x218, up to 8
    std::vector<int> path_locations; ///< THING_TYPES+0x324[], up to 32 ids
    int expire_ticks = 0; ///< +0x1e8 lifetime in ticks
    std::uint32_t collide_event = 0;
    std::uint32_t expire_event = 0;
    std::uint32_t damaged_event = 0;
    std::uint32_t destroy_event = 0;
    std::uint32_t create_event = 0;
    std::uint32_t timeout_event = 0;
};

/// Look up one type without building the full ``Things`` runtime.  Returns an
/// empty optional for an invalid type or a level without a THING_TYPES record.
std::optional<ThingTypeEvents> thing_type_events(const SbtLevel& level,
                                                int type);

/// A VM context used by the executed view.  The default matrix is
/// ``game_mode 0/1 × playertype 0..3`` with ``RANDOM`` forked, which is the
/// ``--contexts all --random-unknown`` sweep that gives 7 617 things on the
/// shipped corpus (docs/NESTED_THINGS.md §4).
struct ThingContext {
    int game_mode = 0;
    int playertype = 0;
};

/// The eight contexts of the corpus sweep.
std::vector<ThingContext> default_thing_contexts();

struct ThingsOptions {
    bool build_static = true;   // top-level + recursive static views
    bool execute = true;        // run the VM to build the runtime instances
    bool strict = true;         // a bad/deep block is an error, not a skip
    bool random_unknown = true; // fork RANDOM (7617 needs it)
    bool follow_generate = false; // opt-in; off in the comparison sweep
    int max_steps = 200000;
    int max_paths = 64;
    double max_time = 5.0;
    std::vector<ThingContext> contexts; // empty -> default_thing_contexts()
};

struct ThingStats {
    std::size_t top_level = 0;          // static ADD_THING on the top level
    std::size_t nested = 0;             // recursive - top_level
    std::size_t recursive = 0;          // static recursive sites (deduplicated)
    std::size_t executed = 0;           // runtime instances (dedup by site, occ)
    std::size_t executed_top_level = 0; // runtime instances with depth == 0
    std::size_t executed_nested = 0;
    std::size_t spawns_static = 0;      // SET_PLAYER_START + SET_DEATHMATCH_START
    std::size_t spawns_executed = 0;
    std::size_t thing_types = 0;        // THING_TYPES records in the level
    std::size_t valid_model_refs = 0;   // THING_TYPES +0x1a4 -> SOB_TABLE
    std::size_t missing_model_refs = 0; // type record points outside SOB_TABLE
    std::size_t null_type_static = 0;   // 0xFFFFFFFF in the recursive view
    std::size_t null_type_things = 0;   // 0xFFFFFFFF runtime instances
    std::size_t invalid_type_things = 0; // other out-of-range type indices
    std::size_t missing_model_things = 0; // valid type, no model name
    std::size_t static_site_duplicates = 0; // duplicate static sites skipped
    std::size_t duplicate_ids = 0;          // must stay 0
    std::size_t blocks = 0;                 // unique entity sub-scripts
    std::size_t paths = 0;                  // VM paths over the context matrix
    std::size_t max_depth = 0;
    std::uint64_t ticks = 0;
    /// Movement automata counters (`set_motion_enabled`): instances stepped,
    /// steps that moved the body and steps that reported a type-18 push.
    std::size_t motion_steps = 0;
    std::size_t motion_moved = 0;
    std::size_t motion_pushed = 0;
    bool nesting_limit = false;
    bool decode_error = false;
    std::map<std::string, int> reasons; // VM stop reasons (recursion, ...)

    std::size_t model_refs() const { return valid_model_refs + missing_model_refs; }
};

/// The level's thing runtime: static top-level / recursive views plus the
/// runtime instances built by the VM.  ``load`` is deterministic; the runtime
/// ids are assigned in discovery order (context, block, instruction order).
class Things {
public:
    Things() = default;

    /// Build the runtime from *level*.  *vm* is the state the caller built for
    /// the level (``vm_state_from_level``); its entity table, players and
    /// seed are reused, while the context matrix overrides game mode/type.
    /// When the optional context list is empty the eight-context corpus sweep
    /// is used.
    static Things load(const SbtLevel& level, const VmState& vm,
                       const ThingsOptions& options = {});

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    /// Runtime instances (``executed == true``), in deterministic order.
    const std::vector<Thing>& things() const { return things_; }
    /// Static top-level list (the historical 4 008 view).
    const std::vector<Thing>& static_top_level() const { return top_level_; }
    /// Static recursive list (7 563 sites on the corpus).
    const std::vector<Thing>& static_recursive() const { return recursive_; }

    const Thing* by_id(int id) const;
    Thing* by_id(int id);

    /// Things whose ``parent`` is *parent* (the source entity id).
    std::vector<const Thing*> children(int parent) const;
    std::vector<int> child_ids(int parent) const;

    /// The runtime location table (``0x80d8e0``, stride ``0x28``, count
    /// ``0x80d87c``): every executed ``ADD_LOCATION`` (op 196, handler
    /// ``0x45bea6``) in execution order, deduplicated by ``(site, occ)`` like
    /// the thing list.  ``thing_id`` is the location id, ``pos`` the three
    /// integer operands ``fild``ed by ``0x47602e``.  Empty when ``load`` ran
    /// with ``execute == false``.
    const std::vector<VmLocation>& locations() const { return locations_; }
    /// First table entry with ``thing_id == id`` -- the linear scan of
    /// ``0x476012``; ``nullptr`` when the id is not in the table.
    const VmLocation* location(int id) const;

    /// Advance the 91 Hz clock: every living instance gets ``age += ticks``;
    /// dead instances stay dead.  A living thing whose +0x1e8 lifetime is
    /// reached is marked `expired` and, when `events` is given, posts its
    /// +0x1ec expire event once.
    ///
    /// When the movement automata are enabled (`set_motion_enabled`), every
    /// living instance with a `movement_type` in 0..18 is stepped once per tick
    /// with the state `thing_motion_spawn` builds on first use.  The motion
    /// state is authoritative; `Thing::position` is left untouched (the int
    /// operands cannot carry the double state, and source-relative things have
    /// no absolute position at all).  Callers read `motion_state()`.
    void tick(std::uint64_t ticks = 1, EventQueue* events = nullptr);

    // --- movement automata (opt-in) ----------------------------------------

    /// Enable the `0x470c70` switch for this container.  *world* is the
    /// geometry for probes/sweeps (null = every probe reports free, like a
    /// headless test).  Enabling after a tick seeds the states at the next
    /// tick; disabling keeps the states but stops stepping them.
    void set_motion_enabled(bool enabled, const CollisionWorld* world = nullptr);
    bool motion_enabled() const { return motion_enabled_; }

    /// Target seen by the seek/turret/patrol modes of `things()[index]`.
    /// Out-of-range indices are ignored.
    void set_motion_target(std::size_t index, ThingMotionTarget target);

    /// Aim the automaton of `things()[index]` at a world point, filling the
    /// three errors the original computes in `0x473a00`:
    ///
    ///   * `position`  = *target*;
    ///   * `yaw_error` = bearing of the point in the body frame, measured from
    ///     the forward row (`0x437909` local transform + `0x4226e9` normalize
    ///     + `0x41e2b8` angle; `forward = (sin yaw, cos yaw)`, so a point on
    ///     +X with yaw 0 gives `+pi/2`);
    ///   * `pitch_error` = 0 (this build zeroes it, see docs/THING_FIELDS.md
    ///     §4a.5);
    ///   * `vertical_error` = `target.z - position.z` (the gate and the
    ///     velocity.z addition live in `thing_motion.cpp`).
    ///
    /// Requires a stepped motion state (`set_motion_enabled` + one `tick`):
    /// the body yaw/pitch/roll live there.  Returns false (and leaves the
    /// target untouched) for a disabled/out-of-range/never-stepped instance or
    /// a non-finite point.
    bool aim_motion_at(std::size_t index, const Vec3d& target, bool visible = true);

    /// Drop the target of `things()[index]` (`valid = false`); the automata
    /// then keep their current heading.  Out-of-range indices are ignored.
    void clear_motion_aim(std::size_t index);

    /// Index of *id* in `things()`, or `std::nullopt` when there is no such
    /// instance.  Indices are stable (`remove` keeps the slot).
    std::optional<std::size_t> index_of(int id) const;
    /// Bodies the type-18 push repels from; `0x476830` scans the player table,
    /// so one list is shared by every instance.
    void set_push_targets(std::vector<ThingPushTarget> targets);

    /// Motion state of `things()[index]`, or nullptr when motion is off, the
    /// index is out of range or the instance was never stepped.
    const ThingMotionState* motion_state(std::size_t index) const;
    /// Last step result of `things()[index]`, or nullptr like above.
    const ThingMotionStep* motion_last(std::size_t index) const;
    /// Current target of `things()[index]`, or nullptr when motion is off, the
    /// index is out of range or nothing was set yet.
    const ThingMotionTarget* motion_target(std::size_t index) const;

    /// Runtime spawn: append a thing, assign an id when `thing.id < 0`, mark it
    /// alive/executed and optionally post its +0x1f0 create event.  Returns the
    /// id, or -1 when an explicit id is already present.
    int spawn(Thing thing, EventQueue* events = nullptr);

    /// Runtime removal: mark the thing dead and optionally post its +0x1f8
    /// destroy event.  The record stays in `things()` so external indices and
    /// the deterministic order remain stable.
    bool remove(int id, EventQueue* events = nullptr);

    bool kill(int id);
    bool revive(int id);
    bool set_alive(int id, bool alive);

    const ThingStats& stats() const { return stats_; }
    std::uint64_t clock() const { return clock_; }

private:
    std::vector<Thing> things_;
    std::vector<Thing> top_level_;
    std::vector<Thing> recursive_;
    std::vector<VmLocation> locations_;
    std::map<int, std::size_t> by_id_;
    ThingStats stats_;
    std::uint64_t clock_ = 0;
    std::string error_;

    bool motion_enabled_ = false;
    const CollisionWorld* motion_world_ = nullptr;
    /// Index-aligned with `things_`; a nullopt slot has not been stepped yet.
    std::vector<std::optional<ThingMotionState>> motion_;
    std::vector<ThingMotionStep> motion_last_;
    std::vector<ThingMotionTarget> motion_targets_;
    /// True once a target was set/cleared for the slot, so `motion_target()`
    /// can distinguish "no aim yet" from "aim cleared".
    std::vector<bool> motion_target_seen_;
    std::vector<ThingPushTarget> push_targets_;
};

} // namespace gm
