#include "game/weapons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>

#include "game/state.h"   // GameState
#include "res/sbt.h"      // parse_sbt, SbtLevel
#include "res/sbtvm.h"    // decode_sbt_script, SbtValue

namespace gm {
namespace {

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
               std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            *error = "cannot open level script: " + path.string();
        }
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        if (error != nullptr) {
            *error = "cannot size level script: " + path.string();
        }
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), size);
        if (!in) {
            if (error != nullptr) {
                *error = "short read of level script: " + path.string();
            }
            return false;
        }
    }
    return true;
}

std::uint32_t as_uint(const SbtValue& value) {
    if (value.kind == SbtValue::Kind::UInt) {
        return value.u;
    }
    if (value.kind == SbtValue::Kind::Real && std::isfinite(value.d) &&
        value.d >= 0.0 && value.d <= 4294967295.0) {
        return static_cast<std::uint32_t>(value.d);
    }
    return 0;
}

double as_real(const SbtValue& value) {
    if (value.kind == SbtValue::Kind::Real) {
        return value.d;
    }
    return static_cast<double>(value.u);
}

// SplitMix64 finalizer; fixed, platform-independent and seedable.
std::uint64_t mix64(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Uniform in [-1, 1); the top 53 bits give exactly representable doubles.
double signed_unit(std::uint64_t hash) {
    const double unit = static_cast<double>(hash >> 11) * 0x1.0p-53;
    return unit * 2.0 - 1.0;
}

void finish_reload(const Weapon& weapon, Arsenal& arsenal, int slot) {
    const std::size_t index = static_cast<std::size_t>(slot);
    arsenal.reloading[index] = false;
    arsenal.reload_left[index] = 0;
    const int capacity = weapon.magazine;
    if (capacity <= 0) {
        return;
    }
    const std::uint32_t room =
        static_cast<std::uint32_t>(capacity) > arsenal.magazine[index]
            ? static_cast<std::uint32_t>(capacity) - arsenal.magazine[index]
            : 0;
    const std::uint32_t take = std::min(room, arsenal.ammo[index]);
    arsenal.magazine[index] += take;
    arsenal.ammo[index] -= take;
}

} // namespace

int fire_ticks_from_ms(int period_ms) {
    if (period_ms <= 0) {
        return 0;
    }
    const std::int64_t ticks =
        (static_cast<std::int64_t>(period_ms) * 91 + 999) / 1000;
    return static_cast<int>(std::max<std::int64_t>(1, ticks));
}

const char* fire_error_name(FireError error) {
    switch (error) {
    case FireError::none:
        return "none";
    case FireError::slot_out_of_range:
        return "slot out of range";
    case FireError::no_weapon:
        return "no weapon in slot";
    case FireError::not_available:
        return "weapon not in the level mask";
    case FireError::cooling_down:
        return "cooldown";
    case FireError::reloading:
        return "reloading";
    case FireError::empty_magazine:
        return "magazine empty";
    case FireError::empty_ammo:
        return "ammo empty";
    case FireError::no_magazine:
        return "weapon has no magazine";
    case FireError::magazine_full:
        return "magazine full";
    case FireError::already_reloading:
        return "already reloading";
    case FireError::bad_direction:
        return "bad direction";
    case FireError::bad_origin:
        return "bad origin";
    case FireError::bad_projectile_speed:
        return "bad projectile speed";
    }
    return "unknown";
}

Weapons::Weapons(std::vector<Weapon> slots, std::vector<AmmoType> ammo_types)
    : slots_(std::move(slots)), ammo_types_(std::move(ammo_types)) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].slot < 0) {
            slots_[i].slot = static_cast<int>(i);
        }
    }
}

const Weapon* Weapons::at(int slot) const {
    if (slot < 0 || static_cast<std::size_t>(slot) >= slots_.size()) {
        return nullptr;
    }
    return &slots_[static_cast<std::size_t>(slot)];
}

std::uint32_t Weapons::ammo_max_for(int ammo_type) const {
    if (ammo_type < 0 || static_cast<std::size_t>(ammo_type) >= ammo_types_.size()) {
        return 0;
    }
    return ammo_types_[static_cast<std::size_t>(ammo_type)].max;
}

Arsenal arsenal_from_game(const GameState& game) {
    Arsenal arsenal;
    arsenal.mask = game.weapon_mask;
    arsenal.current = game.weapon_slot;
    for (int i = 0; i < kWeaponSlots; ++i) {
        arsenal.ammo[static_cast<std::size_t>(i)] =
            game.ammo[static_cast<std::size_t>(i)];
    }
    return arsenal;
}

namespace {

bool parse_tables(const std::filesystem::path& level_path, std::string* error,
                  Weapons* out) {
    auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    std::vector<std::uint8_t> bytes;
    if (!read_file(level_path, bytes, error)) {
        return false;
    }

    std::string parse_error;
    std::optional<SbtLevel> level = parse_sbt(bytes, parse_error);
    if (!level) {
        return fail("sbt: " + parse_error);
    }
    if (level->script_code.empty()) {
        return fail("sbt has no SCRIPT bytecode");
    }

    std::optional<SbtProgram> program =
        decode_sbt_script(level->script_code, parse_error);
    if (!program) {
        return fail("sbt script: " + parse_error);
    }

    // Top-level tables only; the DEATHMATCH copy is nested inside an IF block and
    // is identical for the shipped data (tools/re/savefile.py does the same).
    const SbtInsn* weapons_insn = nullptr;
    const SbtInsn* ammo_insn = nullptr;
    for (const SbtInsn& insn : program->insns) {
        if (insn.name == "DEFINE_WEAPONS" && !insn.table.empty()) {
            weapons_insn = &insn;
        } else if (insn.name == "DEFINE_AMMO" && !insn.table.empty()) {
            ammo_insn = &insn;
        }
    }
    if (weapons_insn == nullptr) {
        return fail("level has no top-level DEFINE_WEAPONS table");
    }
    if (weapons_insn->table.size() > static_cast<std::size_t>(kWeaponTableMax)) {
        return fail("DEFINE_WEAPONS has more than " +
                    std::to_string(kWeaponTableMax) + " records");
    }

    std::vector<AmmoType> ammo_types;
    if (ammo_insn != nullptr) {
        ammo_types.reserve(ammo_insn->table.size());
        for (const SbtTableRecord& record : ammo_insn->table) {
            if (record.values.size() < 10) {
                return fail("DEFINE_AMMO record has fewer than 10 operands");
            }
            AmmoType type;
            type.name = record.strings.empty() ? std::string() : record.strings[0];
            type.max = as_uint(record.values[0]);
            type.pickup = as_uint(record.values[2]);
            type.flags = as_uint(record.values[7]);
            type.lot = as_uint(record.values[4]);
            type.price = as_uint(record.values[5]);
            type.weight = as_uint(record.values[6]);
            type.rounds_per_unit = as_uint(record.values[8]);
            ammo_types.push_back(std::move(type));
        }
    }

    std::vector<Weapon> slots;
    slots.reserve(weapons_insn->table.size());
    for (const SbtTableRecord& record : weapons_insn->table) {
        if (record.values.size() != 17) {
            return fail("DEFINE_WEAPONS record has " +
                        std::to_string(record.values.size()) +
                        " operands, expected 17");
        }
        if (record.strings.empty()) {
            return fail("DEFINE_WEAPONS record has no name");
        }
        const std::vector<SbtValue>& v = record.values;

        Weapon weapon;
        weapon.name = record.strings[0];
        weapon.icon = record.strings.size() > 2 ? record.strings[2] : std::string();
        weapon.slot = static_cast<int>(slots.size());
        weapon.id = as_uint(v[7]);

        weapon.fire_mode = static_cast<int>(as_uint(v[0]));
        weapon.fire_period_ms = static_cast<int>(as_uint(v[1]));
        weapon.fire_ticks = fire_ticks_from_ms(weapon.fire_period_ms);
        weapon.ammo_type = static_cast<int>(as_uint(v[2]));
        weapon.ammo_per_shot = static_cast<int>(as_uint(v[3]));

        weapon.projectile_id = as_uint(v[4]);
        weapon.projectile_alt = as_uint(v[5]);
        weapon.state = as_uint(v[6]);
        weapon.price = as_uint(v[8]);
        weapon.weight = as_uint(v[9]);
        weapon.flags = as_uint(v[10]);
        weapon.class_index = static_cast<int>(as_uint(v[11]));
        weapon.charge_rate = as_real(v[12]);
        weapon.spread = as_real(v[13]);
        weapon.spread_y = as_real(v[14]);
        weapon.range = as_real(v[15]);
        weapon.muzzle = static_cast<int>(as_uint(v[16]));

        if (weapon.fire_mode < 0 || weapon.fire_mode > 5) {
            return fail(weapon.name + ": fire mode " +
                        std::to_string(weapon.fire_mode) + " is outside 0..5");
        }
        if (weapon.fire_period_ms < 0) {
            return fail(weapon.name + ": negative fire period");
        }
        if (weapon.ammo_per_shot <= 0) {
            return fail(weapon.name + ": ammo per shot must be positive");
        }
        if (!std::isfinite(weapon.charge_rate) || weapon.charge_rate < 0.0) {
            return fail(weapon.name + ": bad charge rate");
        }
        if (!std::isfinite(weapon.spread) || weapon.spread < 0.0 ||
            !std::isfinite(weapon.spread_y) || weapon.spread_y < 0.0) {
            return fail(weapon.name + ": bad spread cone");
        }
        if (!std::isfinite(weapon.range) || weapon.range < 0.0) {
            return fail(weapon.name + ": bad range");
        }
        if (weapon.ammo_type >= 0 &&
            static_cast<std::size_t>(weapon.ammo_type) >= ammo_types.size()) {
            return fail(weapon.name + ": ammo type " +
                        std::to_string(weapon.ammo_type) +
                        " is past the DEFINE_AMMO table");
        }

        // Hypothesis layer: the original directly feeds from ammo[slot] and
        // spawns an .soe projectile, so keep it as close to that as possible.
        weapon.magazine = 0;
        weapon.reload_ticks = 0;
        weapon.damage = kPlaceholderDamage;
        weapon.hitscan = false;
        weapon.projectile_speed = kDefaultProjectileSpeed;

        slots.push_back(std::move(weapon));
    }

    *out = Weapons(std::move(slots), std::move(ammo_types));
    return true;
}

} // namespace

std::optional<Weapons> Weapons::load(const GameState& game,
                                     const std::filesystem::path& level_path,
                                     std::string* error) {
    Weapons table;
    if (!parse_tables(level_path, error, &table)) {
        return std::nullopt;
    }
    table.initial_ = arsenal_from_game(game);
    return table;
}

std::optional<Weapons> Weapons::load(const std::filesystem::path& level_path,
                                     std::string* error) {
    Weapons table;
    if (!parse_tables(level_path, error, &table)) {
        return std::nullopt;
    }
    return table;
}

bool can_fire(const Weapons& table, const Arsenal& arsenal, int slot,
              FireError* why) {
    auto fail = [why](FireError error) {
        if (why != nullptr) {
            *why = error;
        }
        return false;
    };
    if (slot < 0 || slot >= kWeaponSlots) {
        return fail(FireError::slot_out_of_range);
    }
    const Weapon* weapon = table.at(slot);
    if (weapon == nullptr) {
        return fail(FireError::no_weapon);
    }
    const std::size_t index = static_cast<std::size_t>(slot);
    if ((arsenal.mask & (1u << slot)) == 0) {
        return fail(FireError::not_available);
    }
    if (arsenal.reloading[index]) {
        return fail(FireError::reloading);
    }
    if (arsenal.cooldown[index] > 0) {
        return fail(FireError::cooling_down);
    }
    const std::uint32_t cost =
        static_cast<std::uint32_t>(weapon->ammo_per_shot);
    if (weapon->magazine > 0) {
        if (arsenal.magazine[index] < cost) {
            return fail(FireError::empty_magazine);
        }
    } else if (arsenal.ammo[index] < cost) {
        return fail(FireError::empty_ammo);
    }
    return true;
}

ShotResult fire(const Weapons& table, Arsenal& arsenal, Vec3d origin, Vec3d dir,
                const CollisionWorld& world) {
    ShotResult result;
    const int slot = arsenal.current;
    result.slot = slot;

    FireError why = FireError::none;
    if (!can_fire(table, arsenal, slot, &why)) {
        result.error = why;
        return result;
    }
    const Weapon* weapon = table.at(slot);
    if (weapon == nullptr) {
        result.error = FireError::no_weapon;
        return result;
    }
    if (!finite(origin)) {
        result.error = FireError::bad_origin;
        return result;
    }
    const double length = gm::length(dir);
    if (!finite(dir) || !(length > 0.0)) {
        result.error = FireError::bad_direction;
        return result;
    }
    if (!weapon->hitscan &&
        (!std::isfinite(weapon->projectile_speed) ||
         !(weapon->projectile_speed > 0.0))) {
        result.error = FireError::bad_projectile_speed;
        return result;
    }

    const std::size_t index = static_cast<std::size_t>(slot);
    const Vec3d forward = dir * (1.0 / length);

    // Deterministic spread: one stream step per successful shot.  Zero spread
    // keeps the exact input direction (no basis construction, no rounding).
    double spread_x = 0.0;
    double spread_y = 0.0;
    Vec3d shot_dir = forward;
    if (weapon->spread > 0.0 || weapon->spread_y > 0.0) {
        const std::uint64_t hash =
            mix64(arsenal.seed ^
                  (static_cast<std::uint64_t>(slot + 1) << 32) ^
                  (arsenal.shot_index * 0x9e3779b97f4a7c15ULL));
        spread_x = signed_unit(hash);
        spread_y = signed_unit(mix64(hash));

        const Vec3d up_ref =
            std::fabs(forward.z) > 0.999 ? Vec3d{0.0, 1.0, 0.0} : Vec3d{0.0, 0.0, 1.0};
        const Vec3d right = normalized(cross(up_ref, forward));
        const Vec3d up = normalized(cross(forward, right));
        shot_dir = normalized(forward + right * (spread_x * weapon->spread) +
                              up * (spread_y * weapon->spread_y));
    }

    const std::uint32_t cost = static_cast<std::uint32_t>(weapon->ammo_per_shot);
    if (weapon->magazine > 0) {
        arsenal.magazine[index] -= cost;
    } else {
        arsenal.ammo[index] -= cost;
    }
    arsenal.cooldown[index] = weapon->fire_ticks;
    ++arsenal.shot_index;

    const double max_t = weapon->range > 0.0 ? weapon->range : kCollideInfinity;
    const std::optional<RayHit> hit = world.raycast(origin, shot_dir, max_t);

    result.ok = true;
    result.fired = true;
    result.weapon_id = weapon->id;
    result.projectile = !weapon->hitscan;
    result.origin = origin;
    result.direction = shot_dir;
    result.hit = hit.has_value();
    if (hit) {
        result.distance = hit->t;
        result.point = hit->point;
        result.normal = hit->normal;
    } else {
        result.distance = max_t;
        result.point = origin + shot_dir * max_t;
        result.normal = Vec3d{};
    }
    result.time = weapon->hitscan ? 0.0 : result.distance / weapon->projectile_speed;
    result.damage = weapon->damage;
    result.spread_x = spread_x;
    result.spread_y = spread_y;
    result.consumed = cost;
    result.ammo_left =
        weapon->magazine > 0 ? arsenal.magazine[index] : arsenal.ammo[index];
    result.cooldown_left = weapon->fire_ticks;
    return result;
}

bool reload(const Weapons& table, Arsenal& arsenal, int slot, FireError* why) {
    auto fail = [why](FireError error) {
        if (why != nullptr) {
            *why = error;
        }
        return false;
    };
    if (slot < 0 || slot >= kWeaponSlots) {
        return fail(FireError::slot_out_of_range);
    }
    const Weapon* weapon = table.at(slot);
    if (weapon == nullptr) {
        return fail(FireError::no_weapon);
    }
    const std::size_t index = static_cast<std::size_t>(slot);
    if ((arsenal.mask & (1u << slot)) == 0) {
        return fail(FireError::not_available);
    }
    if (weapon->magazine <= 0) {
        return fail(FireError::no_magazine);
    }
    if (arsenal.reloading[index]) {
        return fail(FireError::already_reloading);
    }
    if (arsenal.magazine[index] >= static_cast<std::uint32_t>(weapon->magazine)) {
        return fail(FireError::magazine_full);
    }
    if (arsenal.ammo[index] == 0) {
        return fail(FireError::empty_ammo);
    }
    if (weapon->reload_ticks <= 0) {
        finish_reload(*weapon, arsenal, slot);
        return true;
    }
    arsenal.reloading[index] = true;
    arsenal.reload_left[index] = weapon->reload_ticks;
    return true;
}

void tick(const Weapons& table, Arsenal& arsenal, std::uint32_t ticks) {
    if (ticks == 0) {
        return;
    }
    for (int slot = 0; slot < kWeaponSlots; ++slot) {
        const std::size_t index = static_cast<std::size_t>(slot);
        if (arsenal.cooldown[index] > 0) {
            const std::int64_t left =
                static_cast<std::int64_t>(arsenal.cooldown[index]) - ticks;
            arsenal.cooldown[index] = static_cast<int>(std::max<std::int64_t>(0, left));
        }
        if (arsenal.reloading[index]) {
            const std::int64_t left =
                static_cast<std::int64_t>(arsenal.reload_left[index]) - ticks;
            if (left <= 0) {
                const Weapon* weapon = table.at(slot);
                if (weapon != nullptr) {
                    finish_reload(*weapon, arsenal, slot);
                } else {
                    arsenal.reloading[index] = false;
                    arsenal.reload_left[index] = 0;
                }
            } else {
                arsenal.reload_left[index] = static_cast<int>(left);
            }
        }
    }
}

bool switch_slot(const Weapons& table, Arsenal& arsenal, int slot, FireError* why) {
    auto fail = [why](FireError error) {
        if (why != nullptr) {
            *why = error;
        }
        return false;
    };
    if (slot < 0 || slot >= kWeaponSlots) {
        return fail(FireError::slot_out_of_range);
    }
    if ((arsenal.mask & (1u << slot)) == 0) {
        return fail(FireError::not_available);
    }
    if (table.at(slot) == nullptr) {
        return fail(FireError::no_weapon);
    }
    arsenal.current = slot;
    return true;
}

Vec3d recoil_velocity_delta(double yaw, double view_pitch, double impulse) {
    if (!std::isfinite(yaw) || !std::isfinite(view_pitch) || !std::isfinite(impulse)) {
        return Vec3d{};
    }
    const double scale = std::fabs(std::cos(view_pitch)) * impulse;
    return Vec3d{-std::sin(yaw) * scale, -std::cos(yaw) * scale, 0.0};
}

void apply_recoil(PlayerState& player, double view_pitch, double impulse) {
    player.vel = player.vel + recoil_velocity_delta(player.yaw, view_pitch, impulse);
}

DamageResult apply_damage(Vitals& vitals, double points, ShieldSide side) {
    DamageResult result;
    if (!std::isfinite(points) || points < 0.0) {
        result.error = "damage must be finite and non-negative";
        return result;
    }
    const int side_index = static_cast<int>(side);
    if (side_index < 0 || side_index >= 4) {
        result.error = "shield side outside 0..3";
        return result;
    }

    const double hundredths = points * kGaugeScale;
    std::int64_t amount = 0;
    if (hundredths >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        amount = std::numeric_limits<std::int32_t>::max();
    } else {
        amount = static_cast<std::int64_t>(std::llround(hundredths));
    }

    const std::int32_t shield =
        std::max<std::int32_t>(0, vitals.shields[static_cast<std::size_t>(side_index)]);
    const std::int64_t shield_take = std::min<std::int64_t>(amount, shield);
    vitals.shields[static_cast<std::size_t>(side_index)] =
        static_cast<std::int32_t>(shield - shield_take);
    std::int64_t rest = amount - shield_take;

    const std::int32_t armor = std::max<std::int32_t>(0, vitals.armor);
    const std::int64_t armor_take = std::min<std::int64_t>(rest, armor);
    vitals.armor = static_cast<std::int32_t>(armor - armor_take);
    rest -= armor_take;

    result.ok = true;
    result.shield_absorbed = static_cast<std::int32_t>(shield_take);
    result.armor_absorbed = static_cast<std::int32_t>(armor_take);
    result.overflow = static_cast<std::int32_t>(rest);
    result.dead = vitals.armor <= 0;
    return result;
}

Vitals vitals_from_game(const GameState& game) {
    Vitals vitals;
    vitals.armor = game.armor;
    vitals.armor_max = game.armor_max;
    vitals.shield_max = game.shield_max;
    vitals.shields = game.shields;
    return vitals;
}

} // namespace gm
