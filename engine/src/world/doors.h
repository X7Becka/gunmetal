#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "res/sbt.h"
#include "res/sbw.h"

namespace gm {

/// Door animation runs on the engine's fixed 91 Hz tick.  The runtime phase is
/// a 16.16-ish fraction in ``0..65536``: ``open_fraction = phase / 65536``.
inline constexpr int kDoorTickHz = 91;
inline constexpr std::uint32_t kDoorPhaseFull = 65536;
inline constexpr double kDoorPhaseToFraction = 1.0 / 65536.0;

/// Proven runtime state word (door record ``+0x80``):
/// ``-1`` closing, ``0`` idle (at either end), ``+1`` opening.
enum class DoorState : int {
    Closing = -1,
    Closed = 0,
    Opening = 1,
};

const char* door_state_name(DoorState state);

/// One ``ADD_DOOR`` / runtime ``DOOR`` record.  The first group of fields is
/// the public state API, the second group keeps the data the state machine
/// needs (and that the save format exposes at the documented offsets).
struct Door {
    int id = 0;              // first non-zero of ids[0..2] (GENERATE's key)
    int sector = -1;         // .sbw sector index
    int object = -1;         // object index inside that sector
    int kind = 0;            // flags & 0xf (1..3 in shipped data)
    DoorState state = DoorState::Closed;
    std::uint32_t timer = 0; // ticks left before auto-close, 0 = stays open
    bool locked = false;     // runtime only; no level/save field carries it
    double open_fraction = 0.0;

    int flags = 0;                  // full flags word (bit 0x40 = usable)
    std::array<int, 5> ids{};       // E1..E5; E4 links a second leaf
    std::array<double, 3> closed{}; // D1..D3 (raw operand frame)
    std::array<double, 3> open{};   // D4..D6 (raw operand frame)
    std::array<double, 3> size{};   // D7..D9
    std::int32_t step = 0;          // record +0x94: phase units per 91 Hz tick
    std::uint32_t phase = 0;        // 0..kDoorPhaseFull
    std::uint32_t hold_ticks = 0;   // opt-in auto-close delay, not decoded
    bool sliding = false;           // D4..D6 differ from D1..D3
    bool linked = false;            // E4 != 0
    bool bound = false;             // (sector, object) exists in the .sbw

    bool moving() const { return state != DoorState::Closed; }
};

/// One ``ADD_SWITCH`` record.  ``id`` is the record index (the level format has
/// no separate switch identity), ``target`` is the GENERATE id (operand 2).
struct Switch {
    int id = 0;
    int target = 0;
    bool pressed = false;
    int kind = 0;
    int radius = 0;
    std::array<int, 3> pos{};
};

/// One stop of a runtime ``LIFT`` record: ``(floor/button id, height, sound)``.
struct LiftStop {
    int floor_id = 0;
    int height = 0;
    int sound = 0;
};

/// A lift.  The runtime record layout (docs/RECORDS.md §4) is parsed from
/// ``LIFT`` record type 18; ``ADD_LIFT`` script blocks are kept as raw words
/// because the 0x94-byte script layout has not been mapped onto the record.
struct Lift {
    int id = 0;      // runtime record has no id: the record index is used
    int kind = 0;    // +0x34
    int x = 0;       // +0x3c
    int y = 0;       // +0x40
    int z = 0;       // +0x44
    int speed = 0;   // +0x54: height units per 91 Hz tick
    int height = 0;  // +0x5c: current height
    int target = 0;  // +0x60: target height
    int stop_index = 0;
    bool moving = false;
    std::array<int, 3> call_ids{}; // +0x70..: floor button ids
    std::vector<LiftStop> stops;
    std::array<std::uint32_t, 37> head{}; // ADD_LIFT script block, raw
    bool script = false;                  // built from ADD_LIFT, not type 18

    bool movable() const { return speed > 0 && !stops.empty(); }
};

/// The state of one level object that a door owns.  This is what
/// :func:`WorldState::apply_to_objects` produces; it is deliberately data-only
/// (no renderer/collision types) so the world logic stays headless.
struct ObjectDoorState {
    int door_id = 0;
    int sector = -1;
    int object = -1;
    DoorState state = DoorState::Closed;
    double open_fraction = 0.0;
    bool sliding = false;
    bool linked = false;
    bool bound = false;
};

struct WorldEvent {
    enum class Kind {
        SwitchPressed,
        DoorOpening,
        DoorClosing,
        DoorOpened,
        DoorClosed,
        DoorLocked,
        LiftCalled,
        NoTarget,
    };

    Kind kind = Kind::NoTarget;
    int switch_id = -1;
    int target = 0;
    int door_id = 0;
    int lift_id = 0;
};

struct WorldStats {
    std::size_t doors = 0;
    std::size_t switches = 0;
    std::size_t lifts = 0;
    std::size_t doors_sliding = 0;
    std::size_t doors_linked = 0;
    std::size_t doors_bound = 0;
    std::size_t doors_distinct_objects = 0;
    std::size_t script_doors = 0;
    std::size_t script_switches = 0;
    std::size_t script_lifts = 0;
    std::size_t record_doors = 0;
    std::size_t record_switches = 0;
    std::size_t record_lifts = 0;
    std::size_t bad_records = 0;
    std::size_t bad_bindings = 0;
    std::size_t switch_interactions = 0;
    std::size_t locked_hits = 0;
    std::size_t unmatched_targets = 0;
    std::size_t lift_calls = 0;
    std::uint64_t ticks = 0;
};

/// Headless doors/switches/lifts for one level.  ``build`` reads door and
/// switch records directly when the ``.sbt`` contains them (save-like files)
/// and otherwise falls back to the ``SCRIPT`` bytecode (shipped levels).
class EventQueue;

class WorldState {
public:
    /// Rebuild from *level* and bind every door to ``(sector, object)`` in
    /// *world*.  Returns false only when neither source can be read; malformed
    /// individual records are counted in ``stats().bad_records``.
    bool build(const SbtLevel& level, const SbwLevel& world, std::string* error = nullptr);

    /// Advance by *ticks* fixed 91 Hz steps (default one).  O(1) per door/lift:
    /// a huge count jumps to the end state instead of looping.
    void tick(std::uint64_t ticks = 1);

    /// Press switch ``switch_id`` (index into ``switches()``).  Returns false
    /// when no such switch exists; locked doors are skipped, unmatched targets
    /// only bump the stats.
    bool interact(int switch_id);

    /// Post a type-2 `Trigger Switch` record for every SwitchPressed event not
    /// yet posted.  `source` is copied to the record (the original takes it
    /// from the calling entity); returns the number posted.
    ///
    /// A cursor (`switch_event_cursor_`) advances over the already posted
    /// events, so a second call without a new `interact()` posts nothing.  A
    /// full queue stops the walk without losing the pending record:
    /// `clear_events()` and `clear()` reset the cursor.
    std::size_t post_switch_events(EventQueue& queue, std::int16_t source = 0) const;

    /// Door state mapped onto the level objects it owns.  ``bound`` is false
    /// for a door whose ``(sector, object)`` does not exist in the ``.sbw``.
    std::vector<ObjectDoorState> apply_to_objects() const;

    const WorldStats& stats() const { return stats_; }
    const std::vector<Door>& doors() const { return doors_; }
    /// Mutable view for state restore (``game/state.*``); the world owns the
    /// doors and callers must keep `(sector, object)` bindings intact.
    std::vector<Door>& doors_mutable() { return doors_; }
    const std::vector<Switch>& switches() const { return switches_; }
    const std::vector<Lift>& lifts() const { return lifts_; }
    const std::vector<WorldEvent>& events() const { return events_; }
    void clear_events() {
        events_.clear();
        switch_event_cursor_ = 0;
        switch_post_blocked_ = false;
        switch_event_source_valid_ = false;
    }

    Door* find_door(int id);
    const Door* find_door(int id) const;
    Lift* find_lift(int id);
    const Lift* find_lift(int id) const;

    /// Runtime controls that have no level-data field (or only an unproven one).
    void set_locked(int door_id, bool locked);
    void set_hold_ticks(int door_id, std::uint32_t ticks);
    void set_step(int door_id, std::int32_t phase_per_tick);

private:
    void clear();
    void rebuild_indexes();
    void add_door(Door door);
    void advance_door(Door& door, std::uint64_t ticks);
    void advance_lift(Lift& lift, std::uint64_t ticks);
    void start_lift(Lift& lift);

    std::vector<Door> doors_;
    std::vector<Switch> switches_;
    std::vector<Lift> lifts_;
    std::vector<WorldEvent> events_;
    /// Next `events_` entry `post_switch_events` has to look at.  Mutable
    /// because posting is logically const: it does not change the world, only
    /// how far the queue mirror has consumed the event log.
    mutable std::size_t switch_event_cursor_ = 0;
    /// A full queue left a switch record pending; only then does `tick()` keep
    /// the unposted tail.  Without a failed post the log is still frame-local.
    mutable bool switch_post_blocked_ = false;
    /// First caller's source for the pending tail; a retry must not rewrite it.
    mutable std::int16_t switch_event_source_ = 0;
    mutable bool switch_event_source_valid_ = false;
    WorldStats stats_;

    // GENERATE id (any of E1..E3) -> door indices.  A vector keeps the result
    // deterministic and lets one switch drive a linked door group.
    std::vector<std::pair<int, std::vector<std::size_t>>> door_targets_;
    // Lift call-button id -> lift index.
    std::vector<std::pair<int, std::size_t>> lift_targets_;
};

} // namespace gm
