#pragma once

// Deterministic weapon runtime: the level's `DEFINE_WEAPONS` / `DEFINE_AMMO`
// tables, the player's ammo/mask/selected-slot block from the save, a fixed
// 91 Hz cooldown/reload clock, and pure shot resolution against the collision
// world.  No render, audio, SDL or wall-clock state is touched.
//
// Frame and units: the engine world frame (X right, Y forward, Z up), world
// units, velocities in units/second.  One `tick()` is 1/91 s, the playback /
// player tick of docs/HUD_PLAYER.md and docs/PLAYER_CONTROLLER.md.
//
// Proven data (`docs/WEAPONS_CPP.md` §2):
//   * `DEFINE_WEAPONS` record fields d0..d20 -> Weapon (fire mode, period in
//     ticks, ammo type, ammo per shot, GENERATE/event ids, range, spread
//     tangents, muzzle node, price, weight, flags, class, charge rate);
//   * `player+0x3294` = selected slot, `+0x3298` = `1 << slot` mask,
//     `+0x329c[12]` = ammo per slot (docs/SAVE_FIELDS.md §9.2);
//   * the original fire path sets `player[0x3318] = weapon[+0x2c]` and
//     subtracts the fixed tick `dt` (`0x4453f8`..`0x44540c`); `+0x2c` is
//     already in ticks.  MG `17` = 17 ticks (~187 ms), not 17 ms.
//
// Hypotheses (no field exists in the shipped data, see §3 of the doc):
//   * `magazine` / `reload_ticks` / `damage` / `hitscan` / `projectile_speed`.
//     `load()` keeps magazine 0 = direct feed and no reload.  It resolves
//     BULLET/LASER hitscan and their direct damage from the entity sub-script;
//     THING weapons spawn an entity (0x45e19d -> 0x46ea40 -> 0x46d8b0) and
//     resolve THING_TYPES collide/expire events into impact/expire blasts;
//     `projectile_speed` is `+0x10 * 91` (units per tick, proven by 0x4752d0)
//     with the 12000 fallback only for a type without a speed.
//   * `apply_damage_gauge` is the exact 0x453b50 plate/hull model (hundredths,
//     flags, spill, statuses); `apply_damage` keeps the old synthetic API.
//   * spread sampling (uniform in the tangent square, deterministic seed).
//
// Recoil is *not* a weapon field: the original `SOURCEPLAYER_RECOIL` handler
// (`0x459f4d`) subtracts `sin(yaw)*|cos(view_pitch)|*D` from the horizontal
// velocity.  `recoil_velocity_delta` exposes exactly that formula so a caller
// can wire it to its own input/damage event (docs/SHAKE.md §6).

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "player/controller.h" // PlayerState
#include "render/collide.h"    // Vec3d, CollisionWorld

namespace gm {

class EventQueue;

struct GameState; // game/state.h; only load()/arsenal_from_game() need it

/// The save stores 12 ammo counters and documents `weapon_slot` as 0..11
/// (`player+0x3294/0x329c`, docs/SAVE_FIELDS.md §9.2).  The level table can be
/// longer (19 shipped weapons in level0/dm1), but only these 12 slots are
/// addressable by the save/HUD block.  The rest stay in `Weapons` for lookup.
constexpr int kWeaponSlots = 12;
/// Upper bound on a level's `DEFINE_WEAPONS` count (the save mask is a u32).
constexpr int kWeaponTableMax = 32;
/// Fixed tick rate used by the player, HUD playback and this runtime.
constexpr double kWeaponTickHz = 91.0;
/// `DEFINE_WEAPONS` charge rate is stored as `file * 0.91` in memory
/// (`0x450d34` `FMUL [0x4a2488]`, constant 0.91).  The value is a charge
/// fraction per fixed tick, so `GM_charge_rate` matches the runtime field.
constexpr double kWeaponChargeScale = 0.91;
/// Seconds in one tick.
constexpr double kWeaponTickSeconds = 1.0 / kWeaponTickHz;
/// Fallback projectile speed when a THING type has no `+0x10` value.  The
/// real speed is `THING_TYPES[type].+0x10` in world units per tick plus
/// `charge * operand[9]` (Ghidra 0x46f0f0); `0x4752d0` advances the position
/// by `speed * dt` at 91 Hz, so `load()` stores `speed * 91` in units/second.
constexpr double kDefaultProjectileSpeed = 12000.0;
/// Placeholder damage in points.  There is no damage field in the weapon
/// table; this value is deliberately conservative and is not game balance.
constexpr double kPlaceholderDamage = 1.0;
/// Default deterministic spread seed (SplitMix64); any value works.
constexpr std::uint64_t kWeaponSeed = 0x243f6a8885a308d3ULL;

/// One `CAUSE_BLAST_RADIUS_DAMAGE` (VM op 75).  `plate`/`hull` are integer
/// hundredths; `plate < 0` is a status and is not scaled by falloff.
struct Blast {
    std::int32_t plate = 0;
    std::int32_t hull = 0;
    int radius = 0;
    std::uint32_t flags = 0;       ///< bit0 source, bit1 full, bit2 players, bit3 things
    bool no_direction = false;     ///< operand 1
    std::uint32_t event = 0;       ///< event posted to each target
    /// Medium guards.  An op outside `IF SOURCE_UNDERWATER` has both true;
    /// the two branches of that IF set exactly one.  The original executes
    /// one branch, so callers filter with `select_blasts`.
    bool air = true;
    bool water = true;
    bool ground = true;        ///< `SOURCE_FROM_GROUND` true
    bool not_ground = true;    ///< `SOURCE_FROM_GROUND` false
    bool deathmatch = true;    ///< `DEATHMATCH_GAME` true
    bool not_deathmatch = true;///< `DEATHMATCH_GAME` false
};

/// One `SOURCEPLAYER_WEAPON_POWER_UP` branch (PLASMA CANNON): type 36 at
/// >=0.66, type 37 at >=0.33, type 38 below.  Each variant owns its own
/// THING_TYPES collide/expire blasts.
struct ThingVariant {
    double min_power_up = 0.0;
    int thing_type = -1;
    std::vector<Blast> impact_blasts;
    std::vector<Blast> expire_blasts;
};

/// One `DEFINE_WEAPONS` record mapped to the runtime fields.
///
/// Field provenance: `docs/FORMATS.md` §12.3 (consumer `0x450be0`) and
/// `docs/SHAKE.md` §7.1.  `fire_ticks` is the raw `+0x2c` value (fixed
/// ticks); `fire_period_ms` is its display conversion, `round(ticks*1000/91)`.
/// `magazine`/`reload_ticks` have no shipped-data source; `damage` and
/// `hitscan` are resolved from the FIRE script, `projectile_speed` from
/// THING_TYPES `+0x10` (units/tick × 91).
struct Weapon {
    std::string name;       ///< strings[0], e.g. "MACHINE GUN"
    std::string icon;       ///< strings[2], e.g. "WICON1"
    int slot = -1;          ///< index in the level table, 0..n-1
    std::uint32_t id = 0;   ///< d7 -> +0x4c (0xffff8ab5 in the shipped data)

    int fire_mode = 0;      ///< d0 -> +0x24, switch at 0x445ac0 (0/1/4/5)
    int fire_period_ms = 0; ///< display ms, round(fire_ticks*1000/91)
    int fire_ticks = 0;     ///< d1 -> +0x2c, cooldown in fixed ticks
    int ammo_type = -1;     ///< d2 -> +0x3c, index into ammo_types()
    int ammo_per_shot = 1;  ///< d3 -> +0x40

    std::uint32_t projectile_id = 0;     ///< d4 -> +0x30, GENERATE entity/event id
    std::uint32_t projectile_alt = 0;    ///< d5 -> +0x34, loop/secondary event id
    std::uint32_t state = 0;             ///< d6 -> +0x38, stop-loop event id
    std::uint32_t price = 0;             ///< d8 -> +0x44
    std::uint32_t weight = 0;            ///< d9 -> +0x48
    std::uint32_t flags = 0;             ///< d10 -> +0x28 (bits 1/0x40/0x80)
    int class_index = 0;                 ///< d11 -> +0x294, 0..8
    double charge_rate = 0.0;            ///< d12/13 -> +0x00, units/second
    double spread = 0.0;                 ///< d14/15 -> +0x08, tan(cone axis 1)
    double spread_y = 0.0;               ///< d16/17 -> +0x10, tan(cone axis 2)
    double range = 0.0;                  ///< d18/19 -> +0x18; 0 = unlimited
    int muzzle = 0;                      ///< d20 -> +0x20, 0..7

    // --- hypothesis layer (not in DEFINE_WEAPONS) --------------------------
    double damage = 0.0;        ///< points; BULLET/LASER resolved, THING fallback 1.0
    int magazine = 0;           ///< capacity; 0 = direct feed (original)
    int reload_ticks = 0;       ///< 0 = instant (original has no reload)
    bool hitscan = false;       ///< resolved: first BULLET/LASER true, THING false
    double projectile_speed = 0.0; ///< units/s when !hitscan; 0 = invalid
    int thing_type = -1;        ///< FIRE_SOURCEPLAYER_THING operand 2
    double projectile_charge = 0.0; ///< operand 9, air branch
    /// `CAUSE_BLAST_RADIUS_DAMAGE` ops reached from the impact source.
    /// For THING weapons this is THING_TYPES collide (+0x1c4); for hitscan it
    /// is the event in the FIRE operand (BULLET args[1], LASER args[3]).
    /// `expire_blasts` is the THING_TYPES expire event (+0x1ec).  Each vector
    /// follows one `GENERATE` child because grenade/plasma damage lives there.
    std::vector<Blast> impact_blasts;
    std::vector<Blast> expire_blasts;
    /// Blasts in the body of the chosen `IF FIRE_*` (EMP rings).  They run at
    /// the shot, centered on the shooter, not at the hit point.
    std::vector<Blast> inline_blasts;
    std::uint32_t impact_event = 0; ///< hitscan impact event id, 0 = none
    /// Nested `EVAL FIRE_SOURCEPLAYER_BULLET` in the impact event
    /// (DISINTEGRATOR: 400.00 points at range 200).
    double secondary_damage = 0.0;
    double secondary_range = 0.0;
    /// Power-up variants (PLASMA CANNON).  Empty for every other THING weapon;
    /// `thing_type`/`impact_blasts`/`expire_blasts` above stay the full-power
    /// default so existing callers keep working.
    std::vector<ThingVariant> thing_variants;
    const ThingVariant* variant_for_power(double power_up) const;

    /// Copy the blasts of one branch that apply to the current context.
    /// `expired=false` is the collision/impact event, `true` is expiry;
    /// `underwater`, `from_ground` and `deathmatch` select the guarded IF
    /// branches.  Neutral ops (no guard) pass every context.
    std::vector<Blast> select_blasts(bool expired, bool underwater,
                                     bool from_ground = true,
                                     bool deathmatch = true) const;
    /// Same filter, but uses the `SOURCEPLAYER_WEAPON_POWER_UP` variant when
    /// the weapon has one (PLASMA CANNON).  Falls back to the base vectors.
    std::vector<Blast> select_blasts_at_power(double power_up, bool expired,
                                              bool underwater,
                                              bool from_ground = true,
                                              bool deathmatch = true) const;
    /// Blasts in the chosen `IF FIRE_*` body (EMP rings); they run at the
    /// shot, centered on the shooter, not at the hit point.
    std::vector<Blast> select_inline_blasts(bool underwater,
                                            bool from_ground = true,
                                            bool deathmatch = true) const;
};

/// One `DEFINE_AMMO` record (`0x450b10`, record 0x38).
struct AmmoType {
    std::string name;       ///< "BULLETS", "CANNON SHELLS", ...
    std::uint32_t max = 0;  ///< +0x04, carried stock limit (5000 / 180 / ...)
    std::uint32_t pickup = 0; ///< +0x14, pickup object id
    std::uint32_t flags = 0;  ///< +0x00
    std::uint32_t lot = 0;    ///< +0x08, rounds per buy/sell lot
    std::uint32_t price = 0;  ///< +0x0c
    std::uint32_t weight = 0; ///< +0x10, per round
    std::uint32_t rounds_per_unit = 0; ///< +0x20
};

/// Runtime loadout.  `ammo` is the save block verbatim (`+0x329c`).
/// `magazine` is the optional hypothesis layer; it stays zero for loaded data,
/// which means "feed directly from ammo".  Cooldown/reload arrays are fixed-tick
/// counters; `seed` + `shot_index` make spread sampling reproducible.
struct Arsenal {
    std::array<std::uint32_t, kWeaponSlots> ammo{};
    std::array<std::uint32_t, kWeaponSlots> magazine{};
    std::uint32_t mask = 0;  ///< `1 << slot` for available weapons
    int current = 0;         ///< selected slot, 0..11

    std::array<int, kWeaponSlots> cooldown{};  ///< ticks until the next shot
    std::array<int, kWeaponSlots> reload_left{}; ///< ticks until reload ends
    std::array<bool, kWeaponSlots> reloading{};

    std::uint64_t seed = kWeaponSeed;
    std::uint64_t shot_index = 0; ///< successful shots, also the spread stream
};

enum class FireError : std::uint8_t {
    none = 0,
    slot_out_of_range,
    no_weapon,
    not_available,      ///< mask bit clear
    cooling_down,
    reloading,
    empty_magazine,
    empty_ammo,
    no_magazine,        ///< reload on a direct-feed weapon
    magazine_full,
    already_reloading,
    bad_direction,      ///< zero, NaN or inf
    bad_origin,
    bad_projectile_speed,
    bad_charge,
};

const char* fire_error_name(FireError error);

/// Outcome of one trigger pull.  `ok` is false for a rejected shot (nothing was
/// consumed and no cooldown was set); a successful shot can still miss the
/// world (`hit == false`).
struct ShotResult {
    bool ok = false;
    bool fired = false;
    FireError error = FireError::none;
    int slot = -1;
    std::uint32_t weapon_id = 0;
    bool projectile = false; ///< true when resolved as a projectile segment

    Vec3d origin{};
    Vec3d direction{};  ///< effective direction after spread
    Vec3d point{};      ///< impact point, or origin + direction * range on a miss
    Vec3d normal{};     ///< impact normal, zero on a miss
    bool hit = false;
    double distance = 0.0; ///< travelled distance (range on a miss)
    double time = 0.0;     ///< seconds; 0 for hitscan, distance/speed otherwise
    double damage = 0.0;   ///< w.damage at the moment of the shot
    double projectile_speed = 0.0; ///< effective units/s (base + charge*operand[9])
    double spread_x = 0.0; ///< sampled tangent offset, axis 1 (unitless)
    double spread_y = 0.0; ///< sampled tangent offset, axis 2 (unitless)

    std::uint32_t consumed = 0;
    std::uint32_t ammo_left = 0; ///< magazine when capacity > 0, else ammo
    int cooldown_left = 0;       ///< fire_ticks of the shot
};

/// Charge accumulator for the charge-capable fire modes (`0x4453d0`).
///
/// Original fields: player `+0x2fa0` (`charge`, latched value consumed by the
/// projectile spawner `0x46f0f0` as `speed = THING_TYPES[+0x10] +
/// charge * operand[9]`) and `+0x2fa8` (`accumulated`, grows while the trigger
/// is held by `dt * weapon[+0x00]`).  `state_event` is player `+0x35c`, the
/// dedup slot for the charge-start event (`weapon[+0x34]`) and its state value
/// (`weapon[+0x38]`).  The reset path `0x452730` (respawn) zeroes all three.
struct ChargeState {
    double charge = 0.0;      ///< player `+0x2fa0`, feed it to `fire(..., charge)`
    double accumulated = 0.0; ///< player `+0x2fa8`, 0..1
    std::uint32_t state_event = 0; ///< player `+0x35c`, 0 = not started

    /// Respawn reset: `0x452730` zeroes `+0x2fa0`, `+0x2fa8` and `+0x35c`
    /// together with the fire cooldown and the view-pitch state.
    void reset() {
        charge = 0.0;
        accumulated = 0.0;
        state_event = 0;
    }
};

/// What one input tick of the charge state machine produced.
struct ChargeOutcome {
    bool fire = false;      ///< the trigger resolves into a shot this tick
    bool started = false;   ///< charge-start event posted (`weapon[+0x34]`)
    std::uint32_t start_event = 0;
    std::uint32_t state_value = 0; ///< `weapon[+0x38]`, mirrors to `+0x35c`
    /// The pending `player[+0x35c]` state event (`weapon[+0x38]`) was posted
    /// and cleared this tick (`0x445a90..0x445a98`).  While the trigger is
    /// held the original alternates start/state events every tick, because the
    /// start event re-arms `+0x35c`.
    bool state_event_posted = false;
    std::uint32_t state_event = 0;
};

/// One tick of `0x4453d0`'s charge branch.
///
/// `ready` is `arsenal.cooldown[slot] == 0`: the original decrements the
/// cooldown at the top of the handler and returns before the charge branch
/// while it is still positive (`0x445aa2`), so a cooling weapon neither
/// accumulates nor latches.  `fire_mode` comes from `weapon.fire_mode`
/// (`DEFINE_WEAPONS` d0 -> `+0x24`):
/// 0 fires on the trigger edge, 1 fires every tick while held, 2..6 charge.
/// Charged modes accumulate `ticks * weapon.charge_rate` while held and latch
/// `charge = accumulated` on release; modes 3 and 5 additionally auto-fire at
/// full charge with `charge = 1`.  Modes 2/4/6 only fire on release.  The
/// 1.0 boundary is exact in the original (`FCOMP` against `1.0`, equal is
/// "not yet over"), so a tick that lands on exactly 1.0 does not fire.
ChargeOutcome update_charge(const Weapon& weapon, ChargeState& state,
                            bool ready, bool trigger_held, bool trigger_edge,
                            int ticks, bool suppress_state_event = false);

/// Immutable weapon/ammo tables plus the initial loadout from `GameState`.

///
/// Typical load:
///
///     auto state = gm::GameState::from_save(save, "level0", error);
///     auto table = gm::Weapons::load(*state, game / "level0.sbt", &error);
///     gm::Arsenal arsenal = table->initial();
///     gm::ShotResult shot = gm::fire(*table, arsenal, eye, dir, world);
///
/// `load()` never writes to the save or to the game install.
class Weapons {
public:
    Weapons() = default;
    explicit Weapons(std::vector<Weapon> slots,
                     std::vector<AmmoType> ammo_types = {});

    /// Read `level_path` (the level `.sbt`), decode `DEFINE_WEAPONS` and
    /// `DEFINE_AMMO` from the top-level SCRIPT block, and copy the player's
    /// ammo/mask/selected slot from `game` into `initial()`.
    ///
    /// Returns nullopt and fills `error` when the file cannot be read/parsed,
    /// the SCRIPT block is missing, a required table is missing/empty, a
    /// record has the wrong arity or a field is out of range (negative period,
    /// non-finite/negative spread or range, bad fire mode, ammo type index past
    /// the `DEFINE_AMMO` table, non-positive ammo per shot).
    ///
    /// BULLET/LASER weapon records are additionally resolved through the
    /// entity sub-script (`projectile_id`): `hitscan=true` and `damage` is the
    /// direct-hit gauge / 100.  THING records keep `hitscan=false`; their
    /// `thing_type` selects `THING_TYPES` collide/expire event blocks, whose
    /// blasts are collected into `impact_blasts`/`expire_blasts`.
    static std::optional<Weapons> load(const GameState& game,
                                       const std::filesystem::path& level_path,
                                       std::string* error = nullptr);

    /// Table-only variant (no save state); `initial()` keeps the defaults.
    static std::optional<Weapons> load(const std::filesystem::path& level_path,
                                       std::string* error = nullptr);

    std::size_t size() const { return slots_.size(); }
    bool empty() const { return slots_.empty(); }
    const std::vector<Weapon>& table() const { return slots_; }
    const std::vector<AmmoType>& ammo_types() const { return ammo_types_; }

    /// Weapon at table index `slot`, or nullptr when out of range.
    const Weapon* at(int slot) const;

    /// Ammo stock limit for `weapon.ammo_type`, 0 when the type is unknown.
    std::uint32_t ammo_max_for(int ammo_type) const;

    /// Loadout captured by `load(game, ...)`; default for the table-only form.
    const Arsenal& initial() const { return initial_; }

private:
    std::vector<Weapon> slots_;
    std::vector<AmmoType> ammo_types_;
    Arsenal initial_{};
};

/// Copy ammo/mask/selected slot from the parsed save (`game`).  Capsules are
/// not part of this bridge; cooldown/reload counters start at zero.
Arsenal arsenal_from_game(const GameState& game);

/// True when `slot` can fire now.  `why` receives the first failing reason.
bool can_fire(const Weapons& table, const Arsenal& arsenal, int slot,
              FireError* why = nullptr);

/// Resolve one trigger pull from `origin` along `dir` (need not be unit).
///
/// Validation happens before any state change: a bad slot, mask, cooldown,
/// reload, empty feed or non-finite/zero direction is returned with `ok=false`
/// and a `FireError`, and nothing is consumed.  On success the weapon's ammo
/// (or magazine) is decremented, the cooldown is set to `fire_ticks`, the
/// spread stream advances once, and the shot is resolved as a straight segment
/// of `range` world units.  A hitscan shot reports `time = 0`; a projectile
/// shot reports `distance / projectile_speed`.  The collision filter is the
/// default one (all surfaces, backfaces accepted), matching `CollisionWorld`.
ShotResult fire(const Weapons& table, Arsenal& arsenal, Vec3d origin, Vec3d dir,
                const CollisionWorld& world, double charge = 0.0);

/// Post the hitscan impact event (`weapon.impact_event`) at `shot.point`.
/// Returns false when the weapon has no event or the shot failed.  The caller
/// owns the queue and the source id semantics.
bool post_weapon_impact(EventQueue& queue, const Weapon& weapon,
                        const ShotResult& shot, std::uint32_t source = 0);

// --- hitscan ray and cone (0x477f90 / 0x4779f0) -----------------------------

/// Smallest projected depth of a target along the aim axis, in world units.
///
/// Proven: the constant `0.32` lives at `0x4a2658` and the original compares
/// the projected depth with `FCOMP QWORD PTR [0x4a2658]`.  The FPU sets C3 on
/// equality and the test is `TEST AH,0x41` (C0|C3), so a target is rejected
/// when `depth <= 0.32` and kept only when `depth > 0.32`:
/// `0x477f90` player branch `0x4781c1`, THING branch `0x4783cb`, thing shooter
/// `0x4779f0` at `0x477bc0`.
constexpr double kHitscanMinDepth = 0.32;

/// Result of one hitscan ray against the world geometry.
///
/// The ray stops at the nearest triangle.  `sector`/`object`/`polygon` are the
/// world point the original hands to the impact-event lookup
/// (`FUN_0043c51f` -> `FUN_0044fad0`, event kind `0x102`).
struct HitscanHit {
    bool hit = false;
    Vec3d point{};
    Vec3d normal{};
    double distance = 0.0;
    std::uint32_t sector = 0;
    std::uint32_t object = 0;
    std::uint32_t polygon = 0;  ///< source polygon index inside the object
    std::uint32_t triangle = 0; ///< index in `CollisionWorld::triangles()`
    bool front_face = false;
};

/// Cast one hitscan ray and return the nearest world hit.
///
/// Pure query: no spawn, no damage, no event.  `direction` need not be unit
/// (it is normalised internally); `max_distance` is the exclusive bound the
/// original uses in the target scan (`0x477f90`: a target is accepted only
/// when `distance < max_distance`, FCOMP + `TEST AH,0x1`), so a hit exactly
/// at `max_distance` is *not* a hit.  A non-finite origin/direction, a zero
/// direction and a non-positive or non-finite `max_distance` return a miss.
HitscanHit cast_hitscan(const CollisionWorld& world, const Vec3d& origin,
                        const Vec3d& direction, double max_distance);

/// Aim frame of the shooter, in the original's terms.
///
/// `forward`/`right`/`up` are the rows of the shooter's matrix
/// (`FUN_00437909` projects the target offset onto them): the row order is
/// lateral 1, depth, lateral 2.  `spread_x`/`spread_y` are the tangents of
/// the half angles (the `FIRE_SOURCEPLAYER_BULLET` operands 3 and 4; the
/// same values the weapon table stores at `+0x08`/`+0x10`, checked for
/// MACHINE GUN / CANNON / AUTOCANNON / RAILGUN).  `scale_x`/`scale_y`
/// are the per-target factors of a player target (`0x81c028`/`0x81c030`); a
/// THING target has none, so both are 1 there.
struct HitscanCone {
    Vec3d forward{0.0, 1.0, 0.0};
    Vec3d right{1.0, 0.0, 0.0};
    Vec3d up{0.0, 0.0, 1.0};
    double spread_x = 0.0;
    double spread_y = 0.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double min_depth = kHitscanMinDepth;
};

/// Exact cone test of the original (`0x477f90` for a player shooter,
/// `0x4779f0` for a THING shooter).
///
/// The target offset is projected on the aim frame; the target is inside the
/// cone when
///
///   * `depth > min_depth` (0.32 world units, `0x4a2658`), and
///   * `|lateral_x| < scale_x * depth * spread_x`, and
///   * `|lateral_y| < scale_y * depth * spread_y`.
///
/// The lateral comparisons are strict (`<`), exactly like the original's
/// `FCOMPP` + `TEST AH,0x1`.  The basis vectors must be the (unit) rows of
/// the shooter's matrix; a non-finite input returns false.
bool hitscan_in_cone(const HitscanCone& cone, const Vec3d& origin,
                     const Vec3d& target);

/// Normalised-cosine cone test: the target direction must not be further than
/// `acos(cos_limit)` away from `forward`.
///
/// This is the circular-cone form of the proven test above; the comparison is
/// inclusive (`cos >= cos_limit`), while the original's lateral test is
/// strict.  `hitscan_cos_limit()` converts a tangent half angle into the
/// matching limit.  A zero/NaN vector or a NaN limit returns false.
bool hitscan_in_cone(const Vec3d& forward, const Vec3d& to_target, double cos_limit);

/// Cosine limit equivalent to the proven tangent test: `1/sqrt(1 + tan^2)`.
///
/// A negative tangent describes an empty cone (the limit is above 1, so
/// nothing passes); an infinite tangent describes an unlimited cone (0).
double hitscan_cos_limit(double tan_half_angle);

/// Apply the shooter's spread scale (`0x477f90`, player field `0x81c020`).
///
/// The original divides the weapon spread by the scale only when the scale is
/// strictly greater than 1.0 (`FCOMP 1.0` + `TEST AH,0x41`); every other
/// value leaves the spread untouched.
double hitscan_scale_spread(double spread, double shooter_scale);

/// Start reloading `slot`.  Requires a magazine capacity > 0, a mask bit, a
/// non-full magazine and a non-empty reserve.  With `reload_ticks == 0` the
/// transfer is immediate; otherwise `tick()` completes it.  Switching slots
/// does not cancel a reload.
bool reload(const Weapons& table, Arsenal& arsenal, int slot,
            FireError* why = nullptr);

/// Advance every cooldown and reload counter by `ticks` fixed 91 Hz ticks.
/// Counters clamp at zero; a finished reload moves rounds from `ammo` into
/// `magazine`.  `ticks == 0` is a no-op.
void tick(const Weapons& table, Arsenal& arsenal, std::uint32_t ticks = 1);

/// Select `slot` (0..11) if the mask bit is set.  Selecting the current slot is
/// a successful no-op.  Does not touch cooldowns or reloads.
bool switch_slot(const Weapons& table, Arsenal& arsenal, int slot,
                 FireError* why = nullptr);

/// Quantise a millisecond fire period to fixed 91 Hz ticks: `ceil(ms*91/1000)`,
/// with a minimum of 1 for a positive period and 0 for a zero period.
int fire_ticks_from_ms(int period_ms);

// --- recoil hook -----------------------------------------------------------

/// Proven `SOURCEPLAYER_RECOIL` formula (`0x459f4d`, docs/SHAKE.md §6):
/// `dv = (-sin(yaw)*|cos(view_pitch)|*impulse,
///        -cos(yaw)*|cos(view_pitch)|*impulse, 0)`.
/// Non-finite inputs return a zero vector.
Vec3d recoil_velocity_delta(double yaw, double view_pitch, double impulse);

/// Add `recoil_velocity_delta(yaw, view_pitch, impulse)` to `player.vel`.
/// The caller owns the recoil magnitude; the weapon table has no such field.
void apply_recoil(PlayerState& player, double view_pitch, double impulse);

// --- damage / armor --------------------------------------------------------

/// Save gauges are integer hundredths (`10000 = 100.00`, docs/SAVE_FIELDS.md).
constexpr double kGaugeScale = 100.0;

/// Original quadrant order (`0x453b50`): 0 = +Y (front), 1 = +X (right),
/// 2 = -Y (back), 3 = -X (left).  This is not front/back/left/right.
enum class ShieldSide : std::uint8_t { front = 0, right = 1, back = 2, left = 3 };

/// Player gauges in the same units as the save (`+0x3244..+0x3260`).
struct Vitals {
    std::int32_t armor = 0;
    std::int32_t armor_max = 0;
    std::int32_t shield_max = 0;
    std::array<std::int32_t, 4> shields{}; ///< front/right/back/left (see ShieldSide)
    double mult_plate = 1.0;               ///< 0x81bff0, skipped with damage flag 4
    double mult_hull = 1.0;                ///< 0x81bff8, skipped with damage flag 4
    std::int32_t burn_timer = 0;            ///< status -1 accumulator
    std::int32_t hitstun = 0;               ///< 0x81c408
    std::uint32_t status_flags = 0;         ///< temporary 0x20/0xc0 bits
};

struct DamageResult {
    bool ok = false;
    const char* error = "";
    std::int32_t shield_absorbed = 0;
    std::int32_t armor_absorbed = 0;
    std::int32_t overflow = 0; ///< damage past both pools (still clamped)
    bool dead = false;         ///< armor reached zero
};

/// Apply `points` points of damage to one shield quadrant first, then the
/// armor.  This is the synthetic convenience API (kept for existing callers);
/// it is **not** the exact original plate/hull model.  Use
/// `apply_damage_gauge()` for `0x453b50` semantics.
DamageResult apply_damage(Vitals& vitals, double points,
                          ShieldSide side = ShieldSide::front);

/// Result of the exact `0x453b50` damage model (hundredths).
struct GaugeDamageResult {
    bool ok = false;
    const char* error = "";
    std::int32_t plate_absorbed = 0;
    std::int32_t hull_absorbed = 0;
    std::int32_t overflow = 0;
    std::array<bool, 4> broken{};
    bool dead = false;
    bool low_health = false;  ///< crossed below 50% (event -17)
    bool recovered = false;   ///< crossed back above 50% (event -18)
    std::int32_t stun = 0;
};

/// Exact player damage intake from `0x453b50`: `plate` is the selected shield
/// quadrant (front/right/back/left); `hull` is the armor.  Values are integer
/// hundredths (400 = 4.00).  `side` is -1 for the non-directional path; flags:
/// 1 = no direction, 2 = full damage per plate, 4 = already scaled.
/// Negative `plate` is a status: -1 burn, -2 freeze/electro.
GaugeDamageResult apply_damage_gauge(Vitals& vitals, std::int32_t plate,
                                     std::int32_t hull, std::uint32_t flags = 0,
                                     int side = -1);

/// One affected target from `0x477280`.
struct BlastHit {
    bool is_player = false;
    int index = 0;                 ///< player 0..7 or THING slot
    int distance = 0;              ///< integer distance used by the original
    std::int32_t plate = 0;
    std::int32_t hull = 0;
    std::uint32_t event = 0;
};

struct BlastResult {
    std::vector<BlastHit> hits;
    bool chain_reaction = false;   ///< plate > 0: op 75 also detonates type -4
};

/// Integer falloff used by `0x477280`: `plate * (radius-d) / radius`.
/// `plate < 0` is returned unchanged (status); `radius <= 0` returns 0 for
/// every distance except the centre, avoiding the original divide-by-zero.
std::int32_t blast_damage(const Blast& blast, int distance);
std::int32_t blast_hull_damage(const Blast& blast, int distance);

/// Pure spherical query from `0x477280`: no line-of-sight/geometry check.
/// `thing_alive` is indexed like `thing_positions`; its bit 0 is the original
/// target's "active" bit.  Damage is not applied here; callers use the hits.
BlastResult apply_blast(const Vec3d& center, const Blast& blast,
                        int source_player, int source_thing,
                        std::span<const Vec3d> player_positions,
                        std::span<const Vec3d> thing_positions,
                        std::span<const bool> thing_alive);

/// THING HP/shield block (`0x7ac418..+0x7ac430`), integer hundredths.
struct ThingVitals {
    std::int32_t hp = 0;
    std::int32_t max_hp = 0;
    std::array<std::int32_t, 4> shields{};
    std::array<std::int32_t, 4> shield_max{};
    bool alive = false;
    std::uint32_t flags = 0;
    std::uint32_t flags2 = 0;
    std::uint32_t flags3 = 0;
    std::int32_t burn_timer = 0;    ///< status -1 accumulator
    std::uint32_t status_flags = 0; ///< 0x18 frozen / 0x40 burning
};

struct ThingDamageResult {
    bool ok = false;
    const char* error = "";
    std::int32_t shield_absorbed = 0;
    std::int32_t hull_absorbed = 0;
    std::int32_t overflow = 0;
    std::array<bool, 4> broken{};
    bool dead = false;
    bool damaged_event = false; ///< target survived; caller posts +0x1f4
    bool low_health_event = false; ///< crossed half with `flags2 & 0x1000`
};

/// One hit plus the damage intake it produced.  Exactly one of
/// `player_damage` / `thing_damage` is filled.
struct AppliedBlastHit {
    BlastHit hit;
    GaugeDamageResult player_damage;
    ThingDamageResult thing_damage;
};

struct BlastDamageResult {
    std::vector<AppliedBlastHit> hits;
    bool chain_reaction = false;
};

/// Apply one `CAUSE_BLAST_RADIUS_DAMAGE` to both target arrays.  The spatial
/// query is `apply_blast`; player hits go through `apply_damage_gauge`
/// (`0x453b50`), THING hits through `apply_thing_damage` (`0x470500`).
BlastDamageResult apply_blast_damage(
    const Vec3d& center, const Blast& blast, int source_player,
    int source_thing, std::span<const Vec3d> player_positions,
    std::span<Vitals> player_vitals, std::span<const Vec3d> thing_positions,
    std::span<const bool> thing_alive, std::span<ThingVitals> thing_vitals);

/// Select `weapon` impact/expire blasts for the context and apply each one.
/// This is the runtime bridge for a hit (`expired=false`) or lifetime expiry
/// (`expired=true`); events are returned in the hits for the caller to post.
BlastDamageResult apply_weapon_blast(
    const Weapon& weapon, bool expired, const Vec3d& center, bool underwater,
    bool from_ground, bool deathmatch, int source_player, int source_thing,
    std::span<const Vec3d> player_positions, std::span<Vitals> player_vitals,
    std::span<const Vec3d> thing_positions,
    std::span<const bool> thing_alive, std::span<ThingVitals> thing_vitals,
    double power_up = -1.0);

/// Apply `weapon.inline_blasts` at the shot centre (EMP rings).  The caller is
/// responsible for the original `SLEEP 22` spacing between rings.
BlastDamageResult apply_weapon_inline(
    const Weapon& weapon, const Vec3d& center, bool underwater, bool from_ground,
    bool deathmatch, int source_player, int source_thing,
    std::span<const Vec3d> player_positions, std::span<Vitals> player_vitals,
    std::span<const Vec3d> thing_positions,
    std::span<const bool> thing_alive, std::span<ThingVitals> thing_vitals);


/// Exact `0x470500` intake for a THING: no plate/hull multipliers, shield
/// deficits spill into HP, and negative plate is a no-op status here.
ThingDamageResult apply_thing_damage(ThingVitals& vitals, std::int32_t plate,
                                     std::int32_t hull, std::uint32_t flags = 0,
                                     int side = -1);

struct Thing; // world/things.h
class Things; // world/things.h
class EventQueue; // game/events.h
/// Bridge the runtime `Thing` fields (+0x1d4/+0x1d8/+0x1c8) to `ThingVitals`
/// and back, so a blast can update the thing that AI/rendering read.
ThingVitals thing_vitals(const Thing& thing);
void store_thing_vitals(Thing& thing, const ThingVitals& vitals);
/// Convenience overload that updates the runtime `Thing` in place.
ThingDamageResult apply_thing_damage(Thing& thing, std::int32_t plate,
                                     std::int32_t hull,
                                     std::uint32_t flags = 0, int side = -1);
/// Find `id` in the runtime, damage it in place and optionally post the
/// `damaged`/`destroy` events through `events`.
ThingDamageResult apply_thing_damage(Things& things, int id,
                                     std::int32_t plate, std::int32_t hull,
                                     std::uint32_t flags = 0, int side = -1,
                                     EventQueue* events = nullptr);

/// Build `Vitals` from the parsed save state.
Vitals vitals_from_game(const GameState& game);

} // namespace gm
