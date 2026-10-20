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
//     ms, ammo type, ammo per shot, GENERATE/event ids, range, spread tangents,
//     muzzle node, price, weight, flags, class, charge rate);
//   * `player+0x3294` = selected slot, `+0x3298` = `1 << slot` mask,
//     `+0x329c[12]` = ammo per slot (docs/SAVE_FIELDS.md §9.2);
//   * the original fire path sets `player[0x3318] = weapon[+0x2c]` and
//     decrements it by `dt` in milliseconds (`0x4453f8`..`0x44540c`), so the
//     table period is milliseconds, not ticks.  `fire_ticks` is the fixed-tick
//     quantisation of that period.
//
// Hypotheses (no field exists in the shipped data, see §3 of the doc):
//   * `magazine` / `reload_ticks` / `damage` / `hitscan` / `projectile_speed`.
//     `load()` keeps a conservative runtime: magazine 0 = direct feed, no
//     reload, damage 1.0 (placeholder, not balance).  The original uses
//     hitscan for BULLET/LASER (0x478520/0x478be0) and a real THING for
//     THING weapons (0x45e19d -> 0x46ea40 -> 0x46d8b0); the current runtime
//     still treats every weapon as a projectile and uses
//     `kDefaultProjectileSpeed`.
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
#include <string>
#include <vector>

#include "player/controller.h" // PlayerState
#include "render/collide.h"    // Vec3d, CollisionWorld

namespace gm {

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
/// Seconds in one tick.
constexpr double kWeaponTickSeconds = 1.0 / kWeaponTickHz;
/// Placeholder projectile speed in world units/second.  `DEFINE_WEAPONS` has
/// no speed field; the real THING speed is `THING_TYPES[+0x10]` plus
/// `charge * operand[9]` (Ghidra 0x46dd11/0x46f0f0), and the original moves
/// it by `speed * dt`.  This constant only fills `ShotResult::time` and never
/// changes the hit point (the runtime resolves a straight segment).
constexpr double kDefaultProjectileSpeed = 12000.0;
/// Placeholder damage in points.  There is no damage field in the weapon
/// table; this value is deliberately conservative and is not game balance.
constexpr double kPlaceholderDamage = 1.0;
/// Default deterministic spread seed (SplitMix64); any value works.
constexpr std::uint64_t kWeaponSeed = 0x243f6a8885a308d3ULL;

/// One `DEFINE_WEAPONS` record mapped to the runtime fields.
///
/// Field provenance: `docs/FORMATS.md` §12.3 (consumer `0x450be0`) and
/// `docs/SHAKE.md` §7.1.  `fire_period_ms` is the raw file value; `fire_ticks`
/// is derived.  `magazine`, `reload_ticks`, `damage`, `hitscan` and
/// `projectile_speed` have no shipped-data source and are marked in the doc.
struct Weapon {
    std::string name;       ///< strings[0], e.g. "MACHINE GUN"
    std::string icon;       ///< strings[2], e.g. "WICON1"
    int slot = -1;          ///< index in the level table, 0..n-1
    std::uint32_t id = 0;   ///< d7 -> +0x4c (0xffff8ab5 in the shipped data)

    int fire_mode = 0;      ///< d0 -> +0x24, switch at 0x445ac0 (0/1/4/5)
    int fire_period_ms = 0; ///< d1 -> +0x2c, cooldown in milliseconds
    int fire_ticks = 0;     ///< ceil(fire_period_ms * 91 / 1000), fixed tick
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
    double damage = 0.0;        ///< points, caller supplied; load() = 1.0
    int magazine = 0;           ///< capacity; 0 = direct feed (original)
    int reload_ticks = 0;       ///< 0 = instant (original has no reload)
    bool hitscan = false;       ///< original: BULLET/LASER hitscan, THING spawns an entity; runtime TODO
    double projectile_speed = 0.0; ///< units/s when !hitscan; 0 = invalid
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
    double spread_x = 0.0; ///< sampled tangent offset, axis 1 (unitless)
    double spread_y = 0.0; ///< sampled tangent offset, axis 2 (unitless)

    std::uint32_t consumed = 0;
    std::uint32_t ammo_left = 0; ///< magazine when capacity > 0, else ammo
    int cooldown_left = 0;       ///< fire_ticks of the shot
};

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
                const CollisionWorld& world);

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

enum class ShieldSide : std::uint8_t { front = 0, back = 1, left = 2, right = 3 };

/// Player gauges in the same units as the save (`+0x3244..+0x3260`).
struct Vitals {
    std::int32_t armor = 0;
    std::int32_t armor_max = 0;
    std::int32_t shield_max = 0;
    std::array<std::int32_t, 4> shields{}; ///< front/back/left/right
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
/// quadrant (front/back/left/right); `hull` is the armor.  Values are integer
/// hundredths (400 = 4.00).  `side` is -1 for the non-directional path; flags:
/// 1 = no direction, 2 = full damage per plate, 4 = already scaled.
/// Negative `plate` is a status: -1 burn, -2 freeze/electro.
GaugeDamageResult apply_damage_gauge(Vitals& vitals, std::int32_t plate,
                                     std::int32_t hull, std::uint32_t flags = 0,
                                     int side = -1);

/// Build `Vitals` from the parsed save state.
Vitals vitals_from_game(const GameState& game);

} // namespace gm
