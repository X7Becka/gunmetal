#pragma once

// Clean-room runtime for movement-type 6 projectiles (`0x4752d0`) plus a
// separate minimal RGM/homing mode for movement-type 8 (`0x475930`).
//
// The original moves one movement-type 6 SoA record with a semi-implicit
// Euler step:
//
//     vz += 0.2 * type.gravity * level_gravity * dt
//     desired = pos + speed*dt*dir;  desired.z -= vz*dt
//     resolved = swept-sphere collision(desired)
//     if resolved != desired: reflect/damp (or expire on the last hit)
//
// It does not collide with players or things geometrically.  The only
// player/thing hit check is the proximity table (`THING_TYPES+0x218`), done
// before movement and only for grenade-like types (docs/PROJECTILE_CPP.md).
//
// The RGM branch is opt-in through `ProjectileState::homing`.  It has no
// gravity, damping, hits counter or proximity table: it turns toward a
// caller-supplied target, steps `speed * dt` and destroys (never expires) on
// world contact or when the target is reached (docs/PROJECTILE_CPP.md §6).
//
// Units: `speed` is world units per 91 Hz tick, `vertical_speed` is units per
// tick, `homing_turn` is radians per tick, `dt` is an integer tick count.  The
// layer above converts to/from units/second when talking to Weapon.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "render/collide.h"

namespace gm {

class EventQueue;
struct Thing;
struct Weapon;     // game/weapons.h
struct ShotResult; // game/weapons.h

/// Fixed 91 Hz tick used by the projectile step (docs/PROJECTILE_CPP.md §2).
inline constexpr double kProjectileTickHz = 91.0;
inline constexpr double kProjectileGravityScale = 0.2; // 0x4752d0
inline constexpr int kProjectileProximitySlots = 8;

/// One entry of the proximity table (`THING_TYPES +0x21c..+0x2dc`).
struct ProximityEntry {
    std::uint32_t event = 0; ///< +0x21c; 0 = inactive
    int low = 0;             ///< +0x23c, lower distance band
    int high = 0;            ///< +0x25c, activation radius
    int cooldown = 0;        ///< +0x27c
    Vec3d offset{};          ///< +0x29c/+0x2bc/+0x2dc
};

/// Everything needed to spawn one projectile.  Fill it from a loaded Thing or
/// from the weapon/charge path; the core never reads THING_TYPES itself.
struct ProjectileSpawn {
    int thing_id = -1;          ///< id in Things (used by event extra)
    int owner_player = -1;      ///< 0..7, -1 = no player owner
    int owner_thing = -1;       ///< Thing::id, -1 = no thing owner
    Vec3d position{};
    Vec3d direction{0.0, 0.0, 1.0}; ///< unit direction after spread
    double speed = 0.0;         ///< world units per tick (TT+0x10 + charge)
    double gravity = 0.0;       ///< TT+0x78 multiplier
    double damping = 0.0;       ///< TT+0x80
    double level_gravity = 0.0; ///< SET_LEVEL_GRAVITY; 0 by default
    int hits_left = 0;          ///< TT+0x1c8; 0 = unlimited
    int life_ticks = 0;         ///< TT+0x1e8; 0 = no lifetime
    int shape = 1;              ///< TT+0x1a0: 1 sphere, 2 box
    double radius = 0.0;        ///< TT+0xf8 for shape 1
    std::array<double, 3> box{}; ///< TT+0x100/108/110 for shape 2
    std::uint32_t collide_event = 0;
    std::uint32_t expire_event = 0;
    std::uint32_t destroy_event = 0;
    std::uint32_t timeout_event = 0;
    std::uint32_t create_event = 0;
    std::vector<ProximityEntry> proximity; ///< 0x4752d0 reads entry 0 only

    // RGM / homing branch (`movement_type == 8`, `0x475930`).  The target is
    // supplied by the calling layer: `target_index` uses the same slot scheme
    // as ProjectileTargets (players 0..7, things 8+index), while
    // `target_point` is a caller-managed world position.  When both are set
    // the index wins; `target_point` is the fallback.
    bool homing = false;                ///< select the separate homing step
    int target_index = -1;              ///< 0..7 player, 8+index thing, -1 none
    Vec3d target_point{};               ///< fallback target position
    bool has_target_point = false;      ///< `target_point` is valid
    bool destroy_on_contact = true;     ///< destroy on impact/target (never expire)
    double homing_turn = 0.0;           ///< max turn, radians per tick (0 = snap)
};

struct ProjectileState {
    ProjectileSpawn spawn;
    Vec3d position{};
    Vec3d direction{0.0, 0.0, 1.0};
    double speed = 0.0;            ///< units/tick
    double vertical_speed = 0.0;   ///< positive down, as in 0x4752d0
    double damping = 0.0;
    int hits_left = 0;
    int life_left = 0;
    int timeout_left = 0;
    int cooldown = 0;
    int last_target = -1;          ///< +0x460
    bool spent = false;            ///< expire/destroy already emitted
    bool dead = false;             ///< removed from the world

    // Runtime copy of the RGM/homing configuration.  `target_point` may be
    // refreshed by the caller between ticks (Projectiles::by_thing_id) so a
    // homing round can track a moving point; a target index is resolved from
    // ProjectileTargets on every step instead.
    bool homing = false;
    int target_index = -1;
    Vec3d target_point{};
    bool has_target_point = false;
    bool destroy_on_contact = true;
    double homing_turn = 0.0;
};

enum class ProjectileStatus : std::uint8_t {
    flying,
    bounced,
    proximity,
    expired,
    destroyed,
};

struct ProjectileEvent {
    std::uint32_t id = 0;
    Vec3d position{};
    std::uint32_t extra = 0;
};

struct ProjectileTickResult {
    ProjectileStatus status = ProjectileStatus::flying;
    Vec3d position{};
    Vec3d direction{};
    double speed = 0.0;
    double vertical_speed = 0.0;
    int hits_left = 0;
    int life_left = 0;
    std::vector<ProjectileEvent> events; ///< original post order
};

/// What the projectile may hit through the proximity table.  The spans are
/// caller-owned and may be shorter than eight / the thing count; missing entries
/// are treated as absent.  `thing_parent` is -1 for a root; `thing_owner` is the
/// root owner id used by `FUN_00475860`.
struct ProjectileTargets {
    std::span<const Vec3d> player_positions;
    std::span<const double> player_radii;
    std::span<const std::uint8_t> player_kind;      ///< 0 and 7 are skipped
    std::span<const std::uint8_t> player_has_object; ///< 0 = no object3d
    std::span<const Vec3d> thing_positions;
    std::span<const double> thing_radii;            ///< +0xf8
    std::span<const std::uint8_t> thing_alive;
    std::span<const std::uint32_t> thing_flags2;    ///< 0x20000 = dead
    std::span<const int> thing_movement_type;       ///< 0/6/7/8 are skipped
    std::span<const int> thing_has_object;
    std::span<const int> thing_owner;
    std::span<const int> thing_parent;
};

/// Advance one projectile by `dt` ticks.  The state is updated in place; the
/// result mirrors the observable fields and carries the events in post order.
///
/// When `state.homing` is set, the separate RGM step is used: lifetime and
/// timeout still run first, then the heading turns toward `target_index`
/// (players 0..7, things 8+index from `targets`) or `target_point`, capped at
/// `homing_turn * dt` radians, and the round moves `speed * dt`.  Reaching the
/// target or touching the world with `destroy_on_contact` emits the destroy
/// event once and marks the state `dead`; the expire event is not used.
///
/// A floor contact that cannot separate within one gravity step is a rest, not
/// a hit (docs/PROJECTILE_CPP.md §1.7): the centre is pushed out along the
/// normal, the vertical speed is cleared and the status is `flying`, so the
/// caller does not blast a resting projectile every tick.
ProjectileTickResult projectile_step(ProjectileState& state,
                                     const ProjectileTargets& targets,
                                     const CollisionWorld& world, int dt);

/// Post every non-zero event in `result` through the original queue.  Returns
/// the number of accepted records.
std::size_t post_projectile_events(EventQueue& queue,
                                   const ProjectileTickResult& result);

/// Make a state from a loaded Thing.  Only proven fields are copied; the
/// caller supplies the current world position, unit direction and the speed
/// already including the charge term.
ProjectileState projectile_spawn_from_thing(const Thing& thing,
                                            const Vec3d& position,
                                            const Vec3d& direction,
                                            double speed_per_tick,
                                            int owner_player,
                                            int owner_thing,
                                            double level_gravity = 0.0);

/// Bridge one successful projectile shot to a `ProjectileSpawn` (the missing
/// `FIRE_SOURCEPLAYER_THING` -> `0x46ea40` -> `0x46d8b0` -> `0x4752d0` link).
///
/// `shot.projectile_speed` is units/second (`THING_TYPES +0x10 * 91` plus
/// `charge * operand[9] * 91`), while the runtime steps at 91 Hz, so
/// `spawn.speed = shot.projectile_speed / 91` in units/tick.  When the shot
/// carries no usable speed the weapon's `projectile_speed` is used instead.
/// `spawn.position` is the shot origin (the muzzle), not the ray impact
/// `shot.point`; `spawn.direction` is the post-spread direction, normalised.
///
/// `projectile_thing` is the `THING_TYPES` instance selected by the FIRE
/// sub-script (`movement_type == 6`); it supplies the proven gravity, damping,
/// collision shape/radius/box, hits, lifetime and event ids.  Its `id` becomes
/// `spawn.thing_id` (the event `extra` field), so pass the runtime instance
/// that will be registered in `Things`; `Projectiles::spawn` rejects a
/// duplicate non-negative id.  Passing `nullptr` is legal and yields safe
/// defaults: no events, sphere shape, radius 0, unlimited hits and no
/// lifetime, so the projectile just flies straight until the caller removes
/// it.  `movement_type == 8` sets `spawn.homing`; the target and turn rate are
/// still supplied by the calling layer, and the round flies straight until
/// they are.
///
/// Returns `nullopt` when the shot was rejected, was hitscan, or neither the
/// shot nor the weapon provides a finite positive projectile speed.  The
/// factory is pure: it consumes no ammo, posts no events and changes nothing.
std::optional<ProjectileSpawn> projectile_spawn_from_shot(
    const Weapon& weapon, const ShotResult& shot,
    const Thing* projectile_thing, int owner_player, int owner_thing,
    double level_gravity = 0.0);

/// Deterministic owner of a set of projectiles, ordered by spawn index.
class Projectiles {
public:
    /// Add a projectile and assign its runtime index.  The `thing_id` is kept
    /// as supplied; duplicate thing ids are rejected with -1.  When `events` is
    /// non-null and the spawn has a create event, it is posted at `position`.
    int spawn(ProjectileSpawn spawn, EventQueue* events = nullptr);

    /// Advance every live projectile in spawn order by `ticks` using the
    /// original accumulated-dt semantics (one `0x4752d0` step with `dt=ticks`,
    /// not N independent one-tick steps).  Events are posted in projectile
    /// order, then original event order.
    void tick(int ticks, const ProjectileTargets& targets,
              const CollisionWorld& world, EventQueue* events = nullptr);

    const std::vector<ProjectileState>& list() const { return projectiles_; }
    ProjectileState* by_thing_id(int thing_id);
    const ProjectileState* by_thing_id(int thing_id) const;

    /// Mark a projectile dead and spent.  When `events` is non-null, post the
    /// original +0x1f8 destroy event at the current position (expire alone
    /// does not remove the record).
    bool remove(int thing_id, EventQueue* events = nullptr);

private:
    std::vector<ProjectileState> projectiles_;
};

} // namespace gm
