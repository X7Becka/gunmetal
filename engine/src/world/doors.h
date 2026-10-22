#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "res/sbt.h"
#include "res/sbw.h"

namespace gm {

/// Door animation runs on the engine's fixed 91 Hz tick.  The model's counter
/// goes to ``kDoorPhaseFull`` (65536) so that ``step`` arithmetic stays exact;
/// the runtime word is clamped to ``0xffff`` (see ``kDoorPhaseMax``) and
/// ``open_fraction`` keeps the legacy ``phase / 65536`` API.
inline constexpr int kDoorTickHz = 91;
inline constexpr std::uint32_t kDoorPhaseFull = 65536;
inline constexpr double kDoorPhaseToFraction = 1.0 / 65536.0;

/// The runtime word itself is clamped to ``0xffff`` and divided by **65535**
/// (``0x44f335..0x44f349`` clamp, ``0x44f369``/``0x44e0ab`` constant
/// ``1/65535``).  Geometry uses this pair, not ``kDoorPhaseFull``.
inline constexpr std::uint32_t kDoorPhaseMax = 0xFFFF;
inline constexpr double kDoorPhaseMaxToFraction = 1.0 / 65535.0;

/// ``SOUND_SAMPLE_TABLE`` (record 12) holds 300 samples; a door/lift sound
/// word is an index into it.  ``-1`` and ``-2`` appear in shipped data; the
/// engine queues the sound regardless and lets the mixer drop it, so
/// ``DoorSound::silent()`` is the model's own filter.
inline constexpr int kDoorSampleCount = 300;

/// The engine's 3D-source key of a door's looped movement sound:
/// ``0x44f5e5`` computes ``1000000 + door index`` (``LEA EAX,[EBX+0xf4240]``)
/// and hands it to ``0x46b9f0`` as the *class* of the request.  The mixer uses
/// the class to update the running voice and to stop it when the door comes to
/// rest (``0x46ba90`` -> ``0x40bf80``).
inline constexpr int kDoorSoundClassBase = 1000000;

/// The engine's 3D-sound queue holds at most 256 entries (``0x46b8d0``:
/// ``DAT_0062fa18 < 0x100``); further requests are dropped silently.
inline constexpr std::size_t kMaxSoundRequests = 256;

/// Proven runtime state word (door record ``+0x80``):
/// ``-1`` closing, ``0`` idle (at either end), ``+1`` opening.
enum class DoorState : int {
    Closing = -1,
    Closed = 0,
    Opening = 1,
};

const char* door_state_name(DoorState state);

/// One sound slot of a door/lift record: ``sample`` is the
/// ``SOUND_SAMPLE_TABLE`` index (``-1`` = silent), ``volume`` the word the
/// engine pushes next to it (``0x46b8d0``/``0x46ba90``/``0x46b9f0``).
struct DoorSound {
    int sample = -1;
    int volume = 0;
    bool silent() const { return sample < 0 || sample >= kDoorSampleCount; }
};

/// Which engine call site produced a sound request.  The names follow the
/// disassembly, not the level data.
///
/// * ``0x44f690`` (GENERATE) plays the door's **start** slot when the door's
///   state flips; the sound is one-shot and its class is 0.
/// * ``0x44f3f0`` (the tick) plays the door's **movement** slot every tick
///   while the door moves — a *looped* voice whose class is
///   ``1000000 + door index`` — and the **stop** slot when the leaf reaches
///   either end.
/// * ``0x44f3f0`` -> ``0x46ba90`` asks the mixer to **stop** the looped
///   movement sound of the door that just came to rest.
/// * ``0x44f960`` (the type-2 switch handler) plays the **switch** sound
///   (``+0x1c`` sample, ``+0x18`` volume) after the generate event.
enum class SoundSource : std::uint8_t {
    None = 0,
    DoorStart,
    DoorMove,
    DoorStop,
    DoorMoveStop,
    Switch,
};

/// One portable sound request — the model counterpart of one entry of the
/// engine's 3D-sound queue (``0x62ce18``, stride ``0x2c``, filled by
/// ``0x46b8d0``/``0x46b840``/``0x46b9f0``/``0x46ba90``).
struct SoundRequest {
    /// ``SOUND_SAMPLE_TABLE`` index (``0x83d960``).  ``-1`` = no sample; the
    /// model never queues a request with an invalid sample (the engine does
    /// queue it and lets the mixer drop it — see `DoorSound::silent()`).
    int sample = -1;
    /// Base volume of the request (queue ``+0x1c``).  The mixer computes
    /// ``volume - atten * distance`` (``0x46c246``).
    int volume = 0;
    /// World position of the source (queue ``+0x08/+0x0c/+0x10``).  The
    /// engine truncates it to ``int`` (``0x40e0e0`` = ``__ftol``) before the
    /// request is queued.
    std::array<double, 3> position{};
    /// The request is a looped voice (queue ``+0x28``).
    bool loop = false;
    /// The engine call site that produced the request.
    SoundSource source = SoundSource::None;
    /// The 3D-source key (queue ``+0x14``): ``1000000 + door index`` for the
    /// door's looped movement sound, 0 for the one-shots.
    int class_id = 0;
    /// False when the model cannot compute the position (the engine plays the
    /// switch sound at the polygon centroid of ``0x43577c``, and the world
    /// model has no ``.sbw`` geometry).
    bool position_known = true;
};

/// The object transform the engine writes for a door leaf.
///
/// ``0x44dfc0`` lerps ``closed..open`` by ``phase/65535`` into ``center``,
/// picks a rotation axis from ``axis[]`` and calls ``0x434887``, which builds
/// ``basis`` as a rotation by ``-angle`` about that axis and stores
/// ``origin = center - basis^T * size``.  ``basis`` is row-major: row ``i`` is
/// ``basis[3i..3i+2]`` and a local point ``v`` maps to
/// ``origin + basis^T * v`` (the engine passes the three rows to the transform
/// consumers; ``0x434887`` itself only reads them back as ``basis[3i+j]``).
struct DoorTransform {
    std::array<double, 3> origin{};  ///< object ``+0x00``
    std::array<double, 9> basis{};   ///< object ``+0x18``, identity when not rotating
    std::array<double, 3> center{};  ///< lerp(closed, open, fraction) before the size shift
    int axis = -1;                   ///< 0/1/2 = rotation axis, -1 = pure translation
    double angle = 0.0;              ///< value handed to sin/cos by ``0x434887``
    bool rotating() const { return axis >= 0; }
};

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

    int flags = 0;                  // full flags word (bit 0x40 = editor "usable")
    std::array<int, 5> ids{};       // E1..E5; E4/E5 = on-open/on-close event ids
    std::array<double, 3> closed{}; // D1..D3 **folded** with D7..D9 (+0x00)
    std::array<double, 3> open{};   // D4..D6 **folded** with D7..D9 (+0x18)
    std::array<double, 3> size{};   // D7..D9
    std::int32_t step = 0;          // record +0x94: phase units per 91 Hz tick
    std::uint32_t phase = 0;        // 0..kDoorPhaseFull (runtime copy in +0x8c)
    std::uint32_t phase_prev = 0;   // +0x8c at the start of the last tick
    std::uint32_t phase_next = 0;   // +0x90 computed by the tick
    std::uint32_t hold_ticks = 0;   // record +0x98: auto-close dwell in ticks
    bool sliding = false;           // D4..D6 differ from D1..D3
    bool linked = false;            // legacy alias: ids[3] != 0 (see event_open)
    bool bound = false;             // (sector, object) exists in the .sbw
    std::array<double, 3> axis{};   // +0x48 A1..A3 / 65535 (radians per phase unit)
    double sound_level = 0.03;      // +0x60, constant in every shipped door
    DoorSound start_sound;          // +0xa0/+0xac v0/n0: GENERATE and auto-close
    DoorSound move_sound;           // +0xa4/+0xb0 v1/n1: looped while moving
    DoorSound stop_sound;           // +0xa8/+0xb4 v2/n2: fully open *and* fully closed
    int event_open = 0;             // +0x78 ids[3]: event_post when fully open
    int event_close = 0;            // +0x7c ids[4]: event_post when fully closed

    bool moving() const { return state != DoorState::Closed; }
    /// The engine only animates ``kind`` 1..3 (``0x44f2e0``: low nibble
    /// non-zero and ``< 4``); every shipped door is in range.
    bool active() const { return kind >= 1 && kind <= 3; }
    /// Phase as the geometry functions see it: clamped to ``0xffff``.
    std::uint32_t engine_phase() const { return std::min(phase, kDoorPhaseMax); }
    /// ``engine_phase() / 65535`` — what the leaf transform interpolates with.
    double motion_fraction() const {
        return static_cast<double>(engine_phase()) * kDoorPhaseMaxToFraction;
    }
};

/// One polygon hit as the aimed/blast triggers see it: the engine's
/// ``(sector, object, polygon)`` triple plus the distance that the trigger
/// compares with ``Switch::radius``.  For ``kind == 0x101`` that is the length
/// of the aim ray (``0x43c51f``'s ``local_58``), for ``0x102``/``0x103`` the
/// euclidean distance from the blast point to the polygon (``0x43e541``).
struct PolygonHit {
    std::uint32_t sector = 0;
    std::uint32_t object = 0;
    std::uint32_t polygon = 0;
    double distance = 0.0;
    /// ``object+0x70`` of the hit polygon's object.  ``0x44fb30`` requires
    /// ``(0x428399(sector, object) & 0x10000) == 0`` before the blast trigger
    /// fires; the C++ caller passes the flag word it already has (0 = passes).
    std::uint32_t object_flags = 0;
};

/// One ``ADD_SWITCH`` record.  ``id`` is the record index (the level format has
/// no separate switch identity), ``target`` is the GENERATE id (operand 2).
///
/// Operands 4..6 are **polymorphic** (``docs/DOOR_FIELDS.md`` §2.2):
/// ``kind == 0`` stores a world position, every ``kind >= 0x101`` stores a
/// ``(sector, object, polygon)`` triple.  The engine never converts them.
struct Switch {
    int id = 0;
    int target = 0;
    bool pressed = false;
    int kind = 0;
    int radius = 0;
    /// ``kind 0``: world x/y/z.  ``kind >= 0x101``: (sector, object, polygon).
    std::array<int, 3> pos{};
    int aux = 0;             // +0x18, second sound argument
    int sound = -1;          // +0x1c SOUND_SAMPLE_TABLE index, -1 = silent
    /// Runtime only: the last `poll_triggers` found the entity inside the
    /// radius (``0x44fc30`` has no latch of its own, so this is bookkeeping
    /// for callers that want a rising edge).
    bool inside = false;

    /// ``0x44fc30`` can evaluate ``kind 0/1`` from a bare position; the aimed
    /// (``0x101``) and blast (``0x102``/``0x103``) kinds need a polygon hit.
    bool proximity() const { return kind == 0 || kind == 1; }
    /// Operands 4..6 are a polygon index, not a coordinate.
    bool is_poly() const { return kind >= 0x101; }
    /// The ``(sector, object, polygon)`` triple (meaningless for ``kind 0``).
    std::array<int, 3> poly() const { return pos; }
};

/// One stop of a runtime ``LIFT`` record: ``(floor/button id, height, sound)``.
struct LiftStop {
    int floor_id = 0;
    int height = 0;
    int sound = 0;
};

/// Where the cabin model is put: ``0x44e6f0`` translates it to
/// ``pos + pos_delta * height/65535`` and calls ``0x41e025`` for the basis.
struct LiftTransform {
    std::array<double, 3> origin{};
    std::array<double, 9> basis{};
    std::array<double, 3> angles{}; ///< the three values handed to ``0x41e025``
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

    // Proven runtime fields that the state model does not need but the
    // geometry/audio layers do (docs/DOOR_FIELDS.md §3).
    int sob = 0;                    // +0x38: SOB model index (not an entity id)
    std::array<double, 6> basis{};  // +0x00: base angles [0..2] + deltas [3..5]
    std::array<int, 3> pos_delta{}; // +0x48: position increment per full travel
    int dwell = 0;                  // +0x58: ticks waiting at a stop
    int height_tick = 0;            // +0x60: height of the current tick
    int wait = 0;                   // +0x64: remaining wait at a stop
    int direction = 0;              // +0x68: -1/0/+1
    int call_event = 0;             // +0x7c: event posted on a call
    int call_radius = 0;            // +0x80: call trigger radius
    DoorSound call_sound;           // +0x84/+0x90: call/depart sample
    DoorSound arrive_sound;         // +0x88/+0x98: arrival sample
    DoorSound move_sound;           // +0x88/+0x94: loop while moving

    bool movable() const { return speed > 0 && !stops.empty(); }
};

/// ``height / 65535`` clamped to ``0..1`` — the cabin interpolation fraction.
double lift_fraction(const Lift& lift);
/// Cabin transform per ``0x44e6f0`` + ``0x41e025``.
LiftTransform lift_transform(const Lift& lift);

/// The rotation axis ``0x44dfc0`` picks: ``z`` wins over ``x``, which wins
/// over ``y`` (``-1`` = all three angle operands are zero).
int door_rotation_axis(const Door& door);
/// ``0x450590`` posts this fixed id (``0xffffffea``) when a door crushes an
/// entity; it is an engine event, not a level one.
inline constexpr int kDoorCrushEvent = -22;

/// Leaf transform at an explicit engine phase (``0..0xffff``).
DoorTransform door_transform_at(const Door& door, std::uint32_t engine_phase);
/// Leaf transform at the door's current phase (``0x44dfc0``).
DoorTransform door_transform(const Door& door);
/// The per-tick object motion vector ``0x44f2e0`` writes to ``obj+0x90``:
/// ``(open - closed) * (phase_next - phase_prev) / 65535`` with both phases
/// clamped.  This is the value the ray tracer (``0x437d05``) and the sector
/// AABB (``0x434fed``) consume, and ``0x440867`` marks such sectors dirty.
std::array<double, 3> door_motion_delta(const Door& door, std::uint32_t phase_prev,
                                        std::uint32_t phase_next);

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
    /// Origin + basis the engine would write into the ``.sbw`` object record
    /// this tick (``0x434887``).  Identity basis for the 1422 shipped doors
    /// whose three angle operands are zero.
    DoorTransform transform;
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
        /// ``0x450590`` kind 2: an entity blocked the leaf, the tick's phase is
        /// dropped and the door goes back to opening.
        DoorBlocked,
        /// ``0x450590`` kind 3: same, plus ``event_post(-22)`` — the engine's
        /// crush damage (``0x4795b0``).
        DoorCrush,
    };

    Kind kind = Kind::NoTarget;
    int switch_id = -1;
    int target = 0;
    int door_id = 0;
    int lift_id = 0;
    /// Sound cue attached to door/lift events (``-1`` = none).  Door sounds go
    /// through ``0x46b8d0``/``0x46b9f0`` with the object origin as position.
    int sample = -1;
    int volume = 0;
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
    std::size_t trigger_presses = 0;
    /// ``0x44fc30`` aimed-ray presses (``kind 0x101``).
    std::size_t aim_presses = 0;
    /// ``0x44fb30`` blast/proximity presses (``kind 0x102``/``0x103``).
    std::size_t blast_presses = 0;
    std::size_t blocked_hits = 0;
    std::size_t crush_hits = 0;
    /// Sound requests queued by the world model.
    std::size_t sounds_queued = 0;
    /// Requests dropped because the queue was full (`kMaxSoundRequests`).
    std::size_t sounds_dropped = 0;
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

    /// Same, but binds *source* to the posted ``SwitchPressed`` record right
    /// away (the engine carries the source inside the type-2 record,
    /// ``0x479810``).  `post_switch_events` keeps an already bound source.
    bool interact(int switch_id, std::int16_t source);

    /// The proximity half of ``0x44fc30``: for every switch with ``kind == 0``
    /// fire when ``pos`` is closer than ``radius`` to ``Switch.pos``, and for
    /// ``kind == 1`` fire when it is closer than ``radius`` to the leaf origin
    /// of the first door whose ``ids[0..2]`` match ``target``.  Distance is the
    /// engine's octagonal metric ``max + (mid + min)/4`` (``0x4344df``), the
    /// comparison is strict ``<``.
    ///
    /// ``0x44fc30`` has **no latch**: while the entity stays inside the
    /// radius the engine posts a type-2 record every frame and the level
    /// script decides what to do with the repeat.  `Switch::inside` records
    /// the last answer so a caller can build a rising edge itself.  Aimed
    /// switches (``0x101``) and the source/point variants (``0x102..0x104``)
    /// are not proximity triggers and are left alone here; ``0x104`` has
    /// `trigger_at`.
    std::size_t poll_triggers(const std::array<double, 3>& pos, std::int16_t source = 0);

    /// ``0x44fc30`` aimed half: a ``kind == 0x101`` switch fires when the aim
    /// ray hit **exactly** its ``(sector, object, polygon)`` and the ray length
    /// is ``<= radius`` (``local_58 <= piVar6[-1]``).  ``hit`` is what the
    /// caller's ray query returned (``CollisionWorld::raycast`` ->
    /// ``RayHit::triangle`` carries ``sector``/``object``/``source_poly``).
    std::size_t poll_aim(const PolygonHit& hit, std::int16_t source = 0);

    /// ``0x44fb30``: ``kind 0x102``/``0x103`` fire when the blast point is
    /// within ``radius`` of the polygon (euclidean distance to its vertices,
    /// ``0x43e541``).  The original first calls ``0x428399(sector, object)``,
    /// which returns ``object+0x70 & 0x10000``, and requires zero, so a
    /// culled/inactive object does not trigger.  The caller passes both the
    /// distance it measured and the object's flag word.
    std::size_t poll_blast(const PolygonHit& hit, std::int16_t source = 0);

    /// ``0x44fad0``: a ``kind == 0x102`` switch fires on an **exact**
    /// ``(sector, object, polygon)`` triple, with no distance test at all
    /// (the caller got the triple from an event record, not from a ray).
    std::size_t trigger_poly(const PolygonHit& hit, std::int16_t source = 0);

    /// ``0x44fbd0``: a ``kind == 0x104`` switch fires on an exact position
    /// match (the engine compares the three integers, not a distance).
    std::size_t trigger_at(const std::array<double, 3>& pos, std::int16_t source = 0);

    /// ``0x450590``: something stands in the doorway of ``(sector, object)``.
    /// Kinds 2 and 3 discard the phase the tick just computed
    /// (``phase = phase_prev``); kind 2 flips the state back to opening, kind 3
    /// posts the crush event (``-22``) at the interpolated box and also flips.
    /// Kind 1 and unknown pairs do nothing.  Returns true when a door reacted.
    bool block_contact(int sector, int object);

    /// Post a type-2 `Trigger Switch` record for every SwitchPressed event not
    /// yet posted.  `source` is copied to the record (the original takes it
    /// from the calling entity); returns the number posted.
    ///
    /// Every pending record remembers the `source` of the call that first saw
    /// it (`switch_event_sources_`), so a tail spanning several presses posts
    /// each record with its own source, not the first caller's.  A record is
    /// bound to a source exactly once: an already bound pending record keeps it
    /// when a different caller retries after a full queue.
    ///
    /// A cursor (`switch_event_cursor_`) advances over the already posted
    /// events, so a second call without a new `interact()` posts nothing.  A
    /// full queue stops the walk without losing the pending record:
    /// `clear_events()` and `clear()` reset the cursor.
    std::size_t post_switch_events(EventQueue& queue, std::int16_t source = 0) const;

    /// Post the GENERATE records the door state machine produced:
    /// ``0x44f3f0`` calls ``event_post(ids[3])`` when the leaf is fully open,
    /// ``event_post(ids[4])`` when it is fully closed and ``0x450590`` calls
    /// ``event_post(-22, sector, object, …)`` on a crush.  All three go through
    /// ``0x4795b0`` with ``param_3 = object != 0``, so the record is **type 1**
    /// with ``id``, ``param = sector``, ``a = object``, ``x/y/z`` = object
    /// origin and ``extra = -1``.  A zero id is dropped by the engine
    /// (``param_1 != 0`` gate).  The walk is cursor-based like
    /// `post_switch_events`: a full queue keeps the pending tail for the next
    /// frame, and an already posted record is never posted twice.
    std::size_t post_door_events(EventQueue& queue) const;

    /// Copy the pending sound requests into *out* and return how many were
    /// copied.  Every request leaves the queue exactly once: the cursor
    /// (`sound_cursor_`) advances over the consumed ones, so a second call
    /// without a new transition returns 0 and nothing is played twice.
    ///
    /// `limit` bounds how many requests are copied.  The rest stays pending
    /// (the tail survives the next `tick`, like the unposted door records of
    /// `post_door_events`).  The queue holds at most `kMaxSoundRequests`
    /// entries — the engine's 3D queue has the same cap and drops the rest
    /// (`stats().sounds_dropped`).
    std::size_t drain_sounds(std::vector<SoundRequest>& out,
                             std::size_t limit = static_cast<std::size_t>(-1));

    /// The requests that were not drained yet (oldest first).
    std::span<const SoundRequest> pending_sounds() const;

    /// Drop every pending sound request.
    void clear_sounds();

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
        switch_event_sources_.clear();
        switch_event_cursor_ = 0;
        switch_post_blocked_ = false;
        door_event_cursor_ = 0;
        door_post_blocked_ = false;
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
    void advance_door(std::size_t index, Door& door, std::uint64_t ticks);
    void advance_lift(Lift& lift, std::uint64_t ticks);
    void start_lift(Lift& lift);
    /// GENERATE path shared by `interact` and `poll_triggers`: toggle every
    /// door whose ``ids[0..2]`` match *target* and call every lift whose
    /// call id matches.  Returns the number of doors/lifts that reacted.
    std::size_t press_target(int switch_id, int target);
    /// Shared body of `poll_aim`/`poll_blast`: fire every switch whose `kind`
    /// is in *kinds*, whose `(sector, object, polygon)` equals the hit and
    /// whose radius covers `hit.distance`.  `counter` is the per-trigger stat.
    /// *object_gate* enables the ``0x428399`` flag test (blast path only).
    std::size_t press_polygon(const PolygonHit& hit, std::int16_t source,
                              std::span<const int> kinds, std::size_t& counter,
                              bool object_gate);
    /// Append *event* and reserve its source slot, keeping `events_` and
    /// `switch_event_sources_` index-aligned.  A `SwitchPressed` starts
    /// unbound: `post_switch_events` binds it to the caller's `source`.
    void log_event(WorldEvent event);
    /// Queue one sound request.  The model filters the sample itself (the
    /// engine queues the request unconditionally and lets the mixer drop an
    /// invalid one), so a request with an invalid sample never enters the
    /// queue.
    void queue_sound(SoundRequest request);
    /// Queue a door sound at the leaf's origin (the object's ``+0x00`` the
    /// engine reads through ``0x434419``).  `index` is the door's index in
    /// `doors_`: the looped movement sound carries the class
    /// ``1000000 + index``.
    void queue_door_sound(std::size_t index, const Door& door, SoundSource source,
                          const DoorSound& sound);
    /// Queue the "stop the looped movement sound" request of a door that came
    /// to rest (``0x46ba90`` with the class ``1000000 + index``).  Nothing is
    /// queued for a door whose movement slot is silent.
    void queue_door_move_stop(std::size_t index, const Door& door);
    /// Queue the switch sound (``0x44f960``): sample ``+0x1c``, volume
    /// ``+0x18`` (the base volume of the 3D request).  The engine plays it at
    /// the switch position (kind 0) or at the polygon centroid (kind >=
    /// 0x101); the world model has no geometry for the centroid.
    void queue_switch_sound(int switch_id);

    std::vector<Door> doors_;
    std::vector<Switch> switches_;
    std::vector<Lift> lifts_;
    std::vector<WorldEvent> events_;
    /// Source bound to each `events_` entry, index-aligned.  Empty optional =
    /// not bound yet (the press has not been posted).  Mutable because posting
    /// is logically const, like the cursor below.
    mutable std::vector<std::optional<std::int16_t>> switch_event_sources_;
    /// Next `events_` entry `post_switch_events` has to look at.  Mutable
    /// because posting is logically const: it does not change the world, only
    /// how far the queue mirror has consumed the event log.
    mutable std::size_t switch_event_cursor_ = 0;
    /// Next `events_` entry `post_door_events` has to look at, and whether a
    /// full queue left a door record pending across the tick.
    mutable std::size_t door_event_cursor_ = 0;
    mutable bool door_post_blocked_ = false;
    /// A full queue left a switch record pending; only then does `tick()` keep
    /// the unposted tail.  Without a failed post the log is still frame-local.
    mutable bool switch_post_blocked_ = false;
    /// Pending sound requests and the cursor of the requests that were
    /// already handed to the caller (`drain_sounds`).
    std::vector<SoundRequest> sound_requests_;
    std::size_t sound_cursor_ = 0;
    WorldStats stats_;

    // GENERATE id (any of E1..E3) -> door indices.  A vector keeps the result
    // deterministic and lets one switch drive a linked door group.
    std::vector<std::pair<int, std::vector<std::size_t>>> door_targets_;
    // Lift call-button id -> lift index.
    std::vector<std::pair<int, std::size_t>> lift_targets_;
};

} // namespace gm
