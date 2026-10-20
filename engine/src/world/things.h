#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "res/sbt.h"
#include "res/vm_exec.h"

namespace gm {

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
    double search_radius = 0.0;      // +0x58 (double), target search radius
    int target_zone_threshold = 200; // +0x1cc, default 200
    int target_history_offset = 0;   // +0x1d0, target position-history lag
    int max_health = 0;              // +0x1d4, 0 = damage-immune
    int health = 0;                  // +0x1d4 current copy at spawn
    std::array<int, 4> shields{};    // +0x1d8..+0x1e4
    std::array<int, 4> shield_max{}; // +0x1d8..+0x1e4 max copies
    int hits_left = 0;               // +0x1c8 geometry hits
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
    double speed = 0.0; ///< +0x10 (double), world units per 91 Hz tick
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

    /// Advance the 91 Hz clock.  No AI and no movement yet: every living
    /// instance gets ``age += ticks``; dead instances stay dead.
    void tick(std::uint64_t ticks = 1);

    bool kill(int id);
    bool revive(int id);
    bool set_alive(int id, bool alive);

    const ThingStats& stats() const { return stats_; }
    std::uint64_t clock() const { return clock_; }

private:
    std::vector<Thing> things_;
    std::vector<Thing> top_level_;
    std::vector<Thing> recursive_;
    std::map<int, std::size_t> by_id_;
    ThingStats stats_;
    std::uint64_t clock_ = 0;
    std::string error_;
};

} // namespace gm
