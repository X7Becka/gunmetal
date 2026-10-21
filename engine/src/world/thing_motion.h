#pragma once

// Thing movement automata (`0x470c70` switch on `inst+0x390 movement_type`).
//
// The original keeps the runtime state in a separate 0x528-byte record
// (`0x7ac080`, 260 slots); `Thing` in world/things.h is the *spawn* view built
// from `ADD_THING*`.  This module is the missing middle: the proven subset of
// the 0x528 record that the movement handlers actually read and write, plus
// the handlers themselves at the same arithmetic.  Docs: docs/THING_FIELDS.md
// §4a (field map, writers) and docs/AI_CPP.md §7 (automata).
//
// Angle layout (proven by 0x41e025 + the shipped saves, docs/THING_FIELDS.md
// §4):
//
//     instance +0x18 / +0x20 / +0x28 = pitch / roll / yaw   (current)
//     instance +0x30 / +0x38 / +0x40 = pitch / roll / yaw   (target)
//     instance +0x48 / +0x50 / +0x58 = pitch / roll / yaw   (rate)
//
// `0x41e025` builds the three matrix rows from (roll, pitch, yaw) and row 1 is
// the object's `right`, row 2 its `forward` (same convention as the proven
// `gm::camera_basis`).  Every live instance in saved0.sbt has zero pitch/roll
// and packs the yaw and yaw rate into the third slot.
//
// Units: `speed` and `vertical_speed` are world units per 91 Hz tick and `dt`
// is a tick count, exactly like the integer `param_2` of 0x470c70.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "game/projectile.h" // ProximityEntry
#include "render/collide.h"  // Vec3d, CollisionWorld

namespace gm {

struct Thing;

/// `inst+0x390 movement_type` values as dispatched by 0x470c70.
enum class ThingMove : std::uint8_t {
    idle = 0,        ///< no handler: only the shared tick part runs
    spin = 1,        ///< inline case 1: angles += angle_rate*dt
    avoid = 2,       ///< 0x474260: three-probe wall avoiding walker
    avoid_left = 3,  ///< 0x4746b0(...,1): blocked -> always turn one way
    avoid_right = 4, ///< 0x4746b0(...,0)
    turret = 5,      ///< 0x474ea0: aim at the target inside angle limits
    projectile = 6,  ///< 0x4752d0: gravity, bounce, hits (game/projectile.h too)
    seek = 7,        ///< 0x475930(...,0): turn to target, walk, stop on contact
    seek_los = 8,    ///< 0x475930(...,1): the same with a line-of-sight gate
    path = 9,        ///< 0x475f10 case 9
    path_loop = 10,  ///< 0x475f10 case 10
    path_random = 11,///< 0x475f10 case 11 (rand(), not modelled)
    path_expire = 12,///< 0x475f10 case 12
    ram = 13,        ///< 0x474a10: 1.25r probe, target side choice
    seek_far = 14,   ///< 0x473240: seek + three-probe avoid
    seek_near = 15,  ///< 0x473240, other mode flags
    patrol = 16,     ///< 0x476070: aim along the trail, band-gated firing
    seek_17 = 17,    ///< 0x473240, third mode flag set
    push = 18,       ///< 0x476830
    unknown = 255,
};

/// One row of the dispatch table: type, handler address and status.
struct ThingMoveInfo {
    int type = 0;
    ThingMove move = ThingMove::unknown;
    std::string_view name;    ///< human name used by the docs
    std::string_view handler; ///< engine VA of the case body
    /// "proven"   - implemented here, arithmetic read from the handler;
    /// "external" - proven but implemented elsewhere (projectile.h, ai_move.h);
    /// "stub"     - dispatched, body not ported (see the reason string).
    std::string_view status;
    std::string_view note;
};

/// The 19 dispatch rows (types 0..18), in order.
const std::array<ThingMoveInfo, 19>& thing_move_table();
/// Row for `movement_type`; out-of-range types fall back to `unknown`.
const ThingMoveInfo& thing_move_info(int movement_type);

/// THING_TYPES fields the automata need, with the instance offset they are
/// copied to by 0x46d8b0 in the comment.
struct ThingMotionConfig {
    int movement_type = 0;             ///< +0x1b4 -> +0x390
    double speed = 0.0;                ///< +0x010 -> +0x0a0, units/tick
    double turn_accel = 0.0;           ///< +0x020 -> +0x060, rad/tick^2
    double turn_max = 0.0;             ///< +0x028 -> +0x068, rad/tick
    double radius = 0.0;               ///< +0x030 -> +0x078, collision radius
    std::array<double, 3> box{};       ///< +0x038/40/48 -> +0x080
    double gravity = 0.0;              ///< +0x078 -> +0x0d8, projectile multiplier
    double damping = 0.0;              ///< +0x080 -> +0x0c8, speed multiplier per bounce
    double arrive_radius = 50.0;       ///< +0x070 -> +0x0c0
    double search_radius = 0.0;        ///< +0x058 -> +0x0b0
    double climb_speed = 0.0;          ///< +0x018 -> +0x0a8 (vertical seek speed)
    std::array<double, 3> angle_rate{};///< +0x090/98/a0 -> +0x048 (spin mode)
    /// Angle limits of the turret mode: instance +0x228/+0x230 (pitch) and
    /// +0x238/+0x240 (yaw), copied from THING_TYPES +0x0a8/+0x0b0/+0x0b8/+0x0c0.
    /// A zero pair disables the clamp (0x474ea0 tests both fields).
    double pitch_limit_lo = 0.0;
    double pitch_limit_hi = 0.0;
    double yaw_limit_lo = 0.0;
    double yaw_limit_hi = 0.0;
    double level_gravity = 0.0;        ///< 0x80f840, set by the level script
    std::uint32_t flags_a = 0;         ///< +0x1a8 -> +0x360
    std::uint32_t flags_b = 0;         ///< +0x1ac -> +0x364
    std::uint32_t flags_c = 0;         ///< +0x1b0 -> +0x368
    int expire_ticks = 0;              ///< +0x1e8 type field
    std::vector<ProximityEntry> weapons; ///< +0x218 table (low/high/cooldown)
};

/// The proven fields of a spawned `Thing`; unknown fields stay at their
/// defaults.
ThingMotionConfig thing_motion_config(const Thing& thing);

/// The proven runtime state of one instance (the 0x528 subset the handlers
/// touch).  Field comments give the instance offset.
struct ThingMotionState {
    Vec3d position{};              ///< object +0x00..+0x10
    double pitch = 0.0;            ///< +0x18
    double roll = 0.0;             ///< +0x20
    double yaw = 0.0;              ///< +0x28
    double pitch_target = 0.0;     ///< +0x30
    double roll_target = 0.0;      ///< +0x38
    double yaw_target = 0.0;       ///< +0x40
    double pitch_rate = 0.0;       ///< +0x48
    double roll_rate = 0.0;        ///< +0x50
    double yaw_rate = 0.0;         ///< +0x58
    double last_yaw = 0.0;         ///< +0x70, previous yaw target
    Vec3d velocity{};              ///< +0x1b8, cleared every tick
    /// +0x0a0, the instance copy of THING_TYPES+0x10.  `0x46dd11` sets it at
    /// spawn and `0x4752d0` multiplies it by `+0xc8 damping` on every bounce;
    /// `thing_motion_spawn` initialises it from the config.
    double speed = 0.0;
    double vertical_speed = 0.0;   ///< +0x0f0, zeroed on floor contact
    double thrust_speed = 0.0;     ///< +0x1e8 (dormant, see docs)
    double thrust_accel = 0.0;     ///< +0x1d8
    double thrust_max = 0.0;       ///< +0x1e0
    int thrust_timer = 0;          ///< +0x51c
    /// Collision impulses written by 0x472120: `impulse_side` (+0x208) is
    /// added along the object's `right` axis in x/y, `impulse_up` (+0x210) is
    /// added to world z.  Both are cleared again by the next tick.
    double impulse_side = 0.0;     ///< +0x208
    double impulse_side_bias = 0.0;///< +0x20c, +/-2.5 set on a blocked step
    double impulse_up = 0.0;       ///< +0x210
    double impulse_up_bias = 0.0;  ///< +0x214
    double impulse_damping = 0.05; ///< +0x218
    double impulse_damping_up = 0.05; ///< +0x220
    int target = -1;               ///< +0x3e0, player 0..7 or thing+8
    int attack_cooldown = 0;       ///< +0x3e4
    int aim_index = -1;            ///< +0x504, patrol waypoint
    int aim_timer = 0;             ///< +0x50c
    bool expired = false;          ///< hits/lifetime exhausted (0x4703a0)
};

/// What the automata may know about the current target.  The engine finds it
/// in `0x473a00` / `0x476070`; here it is supplied by the caller so the module
/// stays free of players, things and the location table.
struct ThingMotionTarget {
    Vec3d position{};
    double yaw_error = 0.0;   ///< target azimuth - own yaw, radians (0x473a00)
    double pitch_error = 0.0; ///< always 0 in this build (0x473a00 zeroes it)
    /// The seventh output of `0x473a00` (`param_7`): the vertical error of
    /// `0x473240`, gated by `|e| >= 20.0` and added to velocity.z as-is when
    /// the climb step is larger than it.
    double vertical_error = 0.0;
    bool valid = false;
    bool visible = true;      ///< line of sight (type 8 gate)
};

/// A fresh instance: `speed` (and the three angular rates) copied from the
/// config, exactly the `0x46dd11` / `0x46d9b6..0x46d9f2` spawn block.
ThingMotionState thing_motion_spawn(const ThingMotionConfig& config);

struct ThingMotionInput {
    double dt = 1.0;                  ///< ticks; the engine passes an integer
    ThingMotionTarget target{};
    /// Geometry for the probes, the swept move and the floor test.  A null
    /// world makes every probe report "free" and every step succeed, which is
    /// what the headless tests without a .sbw use.
    const CollisionWorld* world = nullptr;
    bool apply_velocity = true;       ///< false keeps the displacement to the caller
};

/// What one step did; also the tracelog the tests pin.
struct ThingMotionStep {
    bool supported = false;  ///< the mode body is ported in this module
    bool moved = false;      ///< velocity assembled and applied
    bool blocked = false;    ///< the swept move was clamped by geometry
    bool grounded = false;   ///< vertical motion was clamped: floor/ceiling contact
    bool bounced = false;    ///< an impulse was reflected this step
    int turn = 0;            ///< 0 straight, 2 left, 3 right (proven indices)
    Vec3d displacement{};
    int fired_weapon = -1;   ///< index into cfg.weapons (patrol mode)
    std::uint32_t fired_event = 0; ///< its `+0x21c` event id
};

/// The three matrix rows of `0x41e025`: right, forward, up, from
/// (roll, pitch, yaw) exactly as `0x41e025(p1, p2, p3, roll, pitch, yaw)`
/// writes them, i.e. the proven `camera_basis` vectors.
std::array<Vec3d, 3> thing_basis(double roll, double pitch, double yaw);

/// Free distance along `direction` for a body of `radius`, capped at the
/// original's 300-unit probe.  Misses report `kThingProbeFree` (1e38, the
/// DBL_MAX-ish constant `0x47d2ced3/0x2a16a1b1` the handlers use).
inline constexpr double kThingProbeFree = 1e38;
inline constexpr double kThingProbeLength = 300.0; ///< 0x4072c000
double thing_probe(const CollisionWorld* world, const Vec3d& origin,
                   const Vec3d& direction, double radius = 0.0,
                   double max_distance = kThingProbeLength);

/// One tick of one instance: the shared part of `0x470c70`, then the mode
/// body, then the shared angle/velocity/impulse part.  Deterministic: no
/// rand(), no globals, no allocation.
ThingMotionStep thing_motion_step(ThingMotionState& state,
                                  const ThingMotionConfig& config,
                                  const ThingMotionInput& input);

/// `+0x1e8` thruster integrator of the shared tick part, exposed for the tests
/// because no shipped writer fills the accumulator (docs/THING_FIELDS.md §4a).
/// Speed moves toward `±max` while the signed `timer` runs, then decays to 0
/// at `accel` per tick.
void thing_thrust_step(double& speed, double accel, double max, int& timer,
                       double dt);

} // namespace gm
