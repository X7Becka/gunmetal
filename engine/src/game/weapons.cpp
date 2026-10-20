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
#include "world/things.h" // thing_type_events

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

std::uint32_t read_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
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

const ThingVariant* Weapon::variant_for_power(double power_up) const {
    const ThingVariant* best = nullptr;
    for (const ThingVariant& variant : thing_variants) {
        if (variant.min_power_up <= power_up &&
            (best == nullptr || variant.min_power_up > best->min_power_up)) {
            best = &variant;
        }
    }
    return best;
}

namespace {

std::vector<Blast> filter_blasts(const std::vector<Blast>& source,
                                 bool underwater, bool from_ground,
                                 bool deathmatch) {
    std::vector<Blast> out;
    out.reserve(source.size());
    for (const Blast& blast : source) {
        if (!(underwater ? blast.water : blast.air)) {
            continue;
        }
        if (!(from_ground ? blast.ground : blast.not_ground)) {
            continue;
        }
        if (!(deathmatch ? blast.deathmatch : blast.not_deathmatch)) {
            continue;
        }
        out.push_back(blast);
    }
    return out;
}

} // namespace

std::vector<Blast> Weapon::select_blasts(bool expired, bool underwater,
                                        bool from_ground,
                                        bool deathmatch) const {
    return filter_blasts(expired ? expire_blasts : impact_blasts, underwater,
                         from_ground, deathmatch);
}

std::vector<Blast> Weapon::select_blasts_at_power(double power_up, bool expired,
                                                  bool underwater,
                                                  bool from_ground,
                                                  bool deathmatch) const {
    const ThingVariant* variant = variant_for_power(power_up);
    if (variant != nullptr) {
        return filter_blasts(expired ? variant->expire_blasts
                                     : variant->impact_blasts,
                             underwater, from_ground, deathmatch);
    }
    return select_blasts(expired, underwater, from_ground, deathmatch);
}

std::vector<Blast> Weapon::select_inline_blasts(bool underwater,
                                                bool from_ground,
                                                bool deathmatch) const {
    return filter_blasts(inline_blasts, underwater, from_ground, deathmatch);
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

struct FireScriptInfo {
    bool found = false;
    bool hitscan = false;
    int damage_gauge = 0;
    int thing_type = -1;
    int spawn_type = -1; ///< ADD_THING* G operand outside SOURCE_UNDERWATER
    std::uint32_t impact_event = 0; ///< BULLET args[1] / LASER args[3]
    std::vector<SbtInsn> inline_body; ///< body of the chosen IF FIRE_*
    double projectile_charge = 0.0;
    bool underwater = false;
    struct PowerVariant {
        double min_power_up = 1.0;
        int thing_type = -1;
    };
    std::vector<PowerVariant> power_variants;
    bool saw_power_up = false;
};

/// Type operand of an ADD_THING* condition (`G` in its signature).  RGM uses
/// `ADD_THING_AT_SOURCE_WEAPON_POINT ... 32` instead of FIRE_THING.
int add_thing_type(const SbtInsn& insn) {
    if (insn.condition.rfind("ADD_THING", 0) != 0) {
        return -1;
    }
    const std::string signature = sbt_condition_signature(insn.condition);
    for (std::size_t k = 0; k < signature.size() && k < insn.args.size(); ++k) {
        if (signature[k] == 'G') {
            return static_cast<int>(as_uint(insn.args[k]));
        }
    }
    return -1;
}

bool scan_fire_script(const std::vector<SbtInsn>& insns, FireScriptInfo& info,
                      bool underwater = false, double power_up = 1.0) {
    for (const SbtInsn& insn : insns) {
        double child_power = power_up;
        if (insn.condition.rfind("SOURCEPLAYER_WEAPON_POWER_UP", 0) == 0) {
            info.saw_power_up = true;
            if (!insn.args.empty()) {
                child_power = std::min(power_up, as_real(insn.args[0]));
            }
        }
        if (!insn.condition.empty()) {
            if (!underwater) {
                const int spawn = add_thing_type(insn);
                if (spawn >= 0) {
                    info.spawn_type = spawn;
                }
            }
            const bool is_fire =
                insn.condition == "FIRE_SOURCEPLAYER_BULLET" ||
                insn.condition == "FIRE_SOURCEPLAYER_LASER" ||
                insn.condition == "FIRE_SOURCEPLAYER_THING";
            if (is_fire) {
                if (insn.condition == "FIRE_SOURCEPLAYER_THING" &&
                    insn.args.size() > 2) {
                    const int type = static_cast<int>(as_uint(insn.args[2]));
                    const double threshold =
                        (info.saw_power_up && power_up >= 1.0) ? 0.0 : power_up;
                    if (threshold < 1.0 || info.saw_power_up) {
                        bool seen = false;
                        for (const auto& variant : info.power_variants) {
                            seen = seen || variant.thing_type == type;
                        }
                        if (!seen) {
                            info.power_variants.push_back(
                                {threshold, type});
                        }
                    }
                }
                // Prefer the air branch over SOURCE_UNDERWATER; this fixes the
                // old first-leaf DFS (LASER 1.00 instead of 3.00, etc.).
                const bool prefer = !info.found ||
                                    (info.underwater && !underwater);
                if (prefer) {
                    info.hitscan = insn.condition != "FIRE_SOURCEPLAYER_THING";
                    info.damage_gauge = 0;
                    info.impact_event = 0;
                    info.inline_body.clear();
                    if (info.hitscan) {
                        const std::size_t damage_arg =
                            insn.condition == "FIRE_SOURCEPLAYER_LASER" ? 4 : 2;
                        if (insn.args.size() > damage_arg) {
                            info.damage_gauge = static_cast<int>(
                                as_uint(insn.args[damage_arg]));
                        }
                        const std::size_t event_arg =
                            insn.condition == "FIRE_SOURCEPLAYER_LASER" ? 3 : 1;
                        if (insn.args.size() > event_arg) {
                            info.impact_event = as_uint(insn.args[event_arg]);
                        }
                        info.inline_body = insn.body; // EMP rings
                    } else if (insn.args.size() > 9) {
                        info.thing_type =
                            static_cast<int>(as_uint(insn.args[2]));
                        info.projectile_charge = as_real(insn.args[9]);
                    }
                    info.underwater = underwater;
                    info.found = true;
                }
            }
        }
        if (!insn.body.empty()) {
            scan_fire_script(insn.body, info,
                             underwater ||
                                 insn.condition == "SOURCE_UNDERWATER",
                             child_power);
        }
    }
    return info.found;
}

/// Decode exactly the entity block at ``entity_id``.  A handle offset points at
/// one block; decoding to EOF would walk every following entity and mix its
/// FIRE/blast instructions into this one.
std::optional<SbtProgram> entity_block(const SbtLevel& level,
                                       std::uint32_t entity_id) {
    const std::size_t n = level.script.handles;
    if (n == 0 || level.script_handles_data.size() < n * 12) {
        return std::nullopt;
    }
    const auto* data = level.script_handles_data.data();
    for (std::size_t i = 0; i < n; ++i) {
        const auto id = static_cast<std::int32_t>(read_u32(data + (n + i) * 4));
        if (id != static_cast<std::int32_t>(entity_id)) {
            continue;
        }
        const std::uint32_t offset = read_u32(data + (2 * n + i) * 4);
        if (offset >= level.script_code.size()) {
            return std::nullopt;
        }
        std::uint32_t block_end =
            static_cast<std::uint32_t>(level.script_code.size());
        for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t other = read_u32(data + (2 * n + j) * 4);
            if (other > offset && other < block_end) {
                block_end = other;
            }
        }
        std::string error;
        const std::span<const std::uint8_t> sub(level.script_code.data() + offset,
                                                block_end - offset);
        auto program = decode_sbt_script(sub, error);
        if (!program) {
            return std::nullopt;
        }
        return program;
    }
    return std::nullopt;
}

bool resolve_fire_script(const SbtLevel& level, std::uint32_t entity_id,
                         FireScriptInfo& info) {
    const auto program = entity_block(level, entity_id);
    if (!program) {
        return false;
    }
    return scan_fire_script(program->insns, info);
}

/// VM op 75 operands: flags, no_direction, plate, hull, radius, event.
std::optional<Blast> blast_from_insn(const SbtInsn& insn) {
    if (insn.name != "CAUSE_BLAST_RADIUS_DAMAGE" || insn.args.size() < 6) {
        return std::nullopt;
    }
    Blast blast;
    blast.flags = as_uint(insn.args[0]);
    blast.no_direction = as_uint(insn.args[1]) != 0;
    blast.plate = static_cast<std::int32_t>(as_uint(insn.args[2]));
    blast.hull = static_cast<std::int32_t>(as_uint(insn.args[3]));
    blast.radius = static_cast<int>(as_uint(insn.args[4]));
    blast.event = as_uint(insn.args[5]);
    return blast;
}

/// Guards for the mutually exclusive IF branches we can prove in the shipped
/// data: medium, ground contact and deathmatch.  `true` means the blast runs
/// in that context; an op outside a guarded IF has every flag true.
struct BlastGuard {
    bool air = true;
    bool water = true;
    bool ground = true;
    bool not_ground = true;
    bool deathmatch = true;
    bool not_deathmatch = true;
};

bool guarded_condition(const std::string& condition) {
    return condition == "SOURCE_UNDERWATER" ||
           condition.rfind("SOURCE_FROM_GROUND", 0) == 0 ||
           condition == "DEATHMATCH_GAME";
}

BlastGuard branch_guard(BlastGuard guard, const std::string& condition,
                        bool truth) {
    if (condition == "SOURCE_UNDERWATER") {
        (truth ? guard.air : guard.water) = false;
    } else if (condition.rfind("SOURCE_FROM_GROUND", 0) == 0) {
        (truth ? guard.not_ground : guard.ground) = false;
    } else if (condition == "DEATHMATCH_GAME") {
        (truth ? guard.not_deathmatch : guard.deathmatch) = false;
    }
    return guard;
}

void scan_blasts(const std::vector<SbtInsn>& insns, std::vector<Blast>& out,
                 BlastGuard guard = {});

void scan_blast_insn(const SbtInsn& insn, std::vector<Blast>& out,
                     BlastGuard guard) {
    if (auto blast = blast_from_insn(insn)) {
        blast->air = guard.air;
        blast->water = guard.water;
        blast->ground = guard.ground;
        blast->not_ground = guard.not_ground;
        blast->deathmatch = guard.deathmatch;
        blast->not_deathmatch = guard.not_deathmatch;
        out.push_back(*blast);
    }
    if (!insn.body.empty()) {
        scan_blasts(insn.body, out, guard);
    }
}

void scan_blasts(const std::vector<SbtInsn>& insns, std::vector<Blast>& out,
                 BlastGuard guard) {
    for (std::size_t i = 0; i < insns.size(); ++i) {
        const SbtInsn& insn = insns[i];
        if (insn.name == "IF" && guarded_condition(insn.condition)) {
            const BlastGuard taken =
                branch_guard(guard, insn.condition, true);
            const BlastGuard fallthrough =
                branch_guard(guard, insn.condition, false);
            std::uint32_t else_target = 0;
            std::vector<SbtInsn> body;
            for (const SbtInsn& child : insn.body) {
                if (child.name == "ELSE") {
                    else_target = child.target;
                } else {
                    body.push_back(child);
                }
            }
            scan_blasts(body, out, taken);
            if (else_target != 0) {
                std::size_t j = i + 1;
                while (j < insns.size() && insns[j].offset < else_target) {
                    ++j;
                }
                if (j > i + 1) {
                    std::vector<SbtInsn> fall(insns.begin() + i + 1,
                                              insns.begin() + j);
                    scan_blasts(fall, out, fallthrough);
                }
                i = j - 1;
            }
            continue;
        }
        scan_blast_insn(insn, out, guard);
    }
}

void collect_event_blasts(const SbtLevel& level, std::uint32_t event_id,
                          std::vector<Blast>& out, int depth = 0,
                          BlastGuard guard = {});

void scan_generates(const SbtLevel& level, const std::vector<SbtInsn>& insns,
                    std::vector<Blast>& out, int depth, BlastGuard guard);

void scan_generate_insn(const SbtLevel& level, const SbtInsn& insn,
                        std::vector<Blast>& out, int depth,
                        BlastGuard guard) {
    if (insn.name == "GENERATE" && !insn.args.empty()) {
        collect_event_blasts(level, as_uint(insn.args[0]), out, depth + 1,
                             guard);
    }
    if (!insn.body.empty()) {
        scan_generates(level, insn.body, out, depth, guard);
    }
}

void scan_generates(const SbtLevel& level, const std::vector<SbtInsn>& insns,
                    std::vector<Blast>& out, int depth, BlastGuard guard) {
    for (std::size_t i = 0; i < insns.size(); ++i) {
        const SbtInsn& insn = insns[i];
        if (insn.name == "IF" && guarded_condition(insn.condition)) {
            const BlastGuard taken =
                branch_guard(guard, insn.condition, true);
            const BlastGuard fallthrough =
                branch_guard(guard, insn.condition, false);
            std::uint32_t else_target = 0;
            std::vector<SbtInsn> body;
            for (const SbtInsn& child : insn.body) {
                if (child.name == "ELSE") {
                    else_target = child.target;
                } else {
                    body.push_back(child);
                }
            }
            scan_generates(level, body, out, depth, taken);
            if (else_target != 0) {
                std::size_t j = i + 1;
                while (j < insns.size() && insns[j].offset < else_target) {
                    ++j;
                }
                if (j > i + 1) {
                    std::vector<SbtInsn> fall(insns.begin() + i + 1,
                                              insns.begin() + j);
                    scan_generates(level, fall, out, depth, fallthrough);
                }
                i = j - 1;
            }
            continue;
        }
        scan_generate_insn(level, insn, out, depth, guard);
    }
}

/// Follow ``GENERATE`` once.  Grenade and plasma damage live in the generated
/// child block (-30093 -> -30096, -30116 -> -30118, ...), while rocket and
/// bullet impacts already carry their own blast.  Going deeper would pull in
/// secondary debris effects, so the limit is one child.
void collect_event_blasts(const SbtLevel& level, std::uint32_t event_id,
                          std::vector<Blast>& out, int depth,
                          BlastGuard guard) {
    if (event_id == 0 || event_id == 0xffffffffU || depth > 1) {
        return;
    }
    const auto program = entity_block(level, event_id);
    if (!program) {
        return;
    }
    scan_blasts(program->insns, out, guard);
    scan_generates(level, program->insns, out, depth, guard);
}

/// Follow a hitscan event to its nested `EVAL FIRE_SOURCEPLAYER_BULLET`
/// (DISINTEGRATOR 900013 -> GENERATE -30112).  The value is a real hitscan
/// range in world units, unlike the THING `+0x10` speed.
void scan_secondary_insns(const SbtLevel& level,
                          const std::vector<SbtInsn>& insns, double& damage,
                          double& range, int depth) {
    for (const SbtInsn& insn : insns) {
        if (insn.condition == "FIRE_SOURCEPLAYER_BULLET" &&
            insn.args.size() > 2) {
            const double value =
                static_cast<double>(as_uint(insn.args[2])) / kGaugeScale;
            if (value > damage) {
                damage = value;
                range = as_real(insn.args[0]);
            }
        }
        if (insn.name == "GENERATE" && !insn.args.empty() && depth < 2) {
            const auto child = entity_block(level, as_uint(insn.args[0]));
            if (child) {
                scan_secondary_insns(level, child->insns, damage, range,
                                     depth + 1);
            }
        }
        if (!insn.body.empty()) {
            scan_secondary_insns(level, insn.body, damage, range, depth);
        }
    }
}

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
        // d1 -> weapon[+0x2c] is already a cooldown in fixed ticks: 0x445f02
        // stores it into player[0x3318] and 0x4453d0 subtracts dt directly.
        weapon.fire_ticks = static_cast<int>(as_uint(v[1]));
        weapon.fire_period_ms = weapon.fire_ticks > 0
                                    ? (weapon.fire_ticks * 1000 + 45) / 91
                                    : 0;
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
        if (weapon.fire_ticks < 0) {
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

        // The original directly feeds from ammo[slot].  BULLET/LASER use
        // hitscan (0x478520/0x478be0); THING weapons spawn an .soe entity
        // (0x45e19d -> 0x46ea40).  Resolve the mode and the direct-hit gauge
        // from the entity sub-script; level10.sbt's GENERATE chain and all
        // impact/blast damage remain fallback/placeholder for now.
        weapon.magazine = 0;
        weapon.reload_ticks = 0;
        weapon.damage = kPlaceholderDamage;
        weapon.hitscan = false;
        weapon.projectile_speed = kDefaultProjectileSpeed;

        FireScriptInfo fire;
        if (resolve_fire_script(*level, weapon.projectile_id, fire) && fire.found) {
            weapon.hitscan = fire.hitscan;
            if (fire.hitscan) {
                // A real zero gauge is zero damage (EMP/DISRUPTOR/MICROWAVE);
                // the old fallback to 1.00 was a loader shortcut.
                weapon.damage =
                    static_cast<double>(fire.damage_gauge) / kGaugeScale;
                weapon.impact_event = fire.impact_event;
                collect_event_blasts(*level, fire.impact_event,
                                     weapon.impact_blasts);
                scan_blasts(fire.inline_body, weapon.inline_blasts);
            }
            weapon.thing_type = fire.thing_type;
            weapon.projectile_charge = fire.projectile_charge;
            if (weapon.thing_type < 0 && fire.spawn_type >= 0) {
                // RGM: the real projectile is ADD_THING_AT_SOURCE_WEAPON_POINT
                // type 32; the following FIRE_BULLET is only a fallback.
                weapon.thing_type = fire.spawn_type;
                weapon.hitscan = false;
                // The 400-unit FIRE_BULLET is only an obstruction fallback;
                // the projectile's own collide/expire events own the damage.
                weapon.impact_event = 0;
                weapon.impact_blasts.clear();
                weapon.inline_blasts.clear();
            }
        }

        if (weapon.thing_type >= 0) {
            if (const auto events = thing_type_events(*level, weapon.thing_type)) {
                // 0x46f0f0 reads THING_TYPES[type]+0x10 (double) and 0x4752d0
                // advances pos by `speed * dt`; the same units/tick as op4.
                if (events->speed > 0.0) {
                    weapon.projectile_speed =
                        events->speed * kWeaponTickHz;
                }
                collect_event_blasts(*level, events->collide_event,
                                     weapon.impact_blasts);
                collect_event_blasts(*level, events->expire_event,
                                     weapon.expire_blasts);
                if (const auto impact =
                        entity_block(*level, events->collide_event)) {
                    scan_secondary_insns(*level, impact->insns,
                                         weapon.secondary_damage,
                                         weapon.secondary_range, 0);
                }
            }
        }

        for (const FireScriptInfo::PowerVariant& source :
             fire.power_variants) {
            if (source.thing_type < 0) {
                continue;
            }
            ThingVariant variant;
            variant.min_power_up = source.min_power_up;
            variant.thing_type = source.thing_type;
            if (const auto events =
                    thing_type_events(*level, variant.thing_type)) {
                collect_event_blasts(*level, events->collide_event,
                                     variant.impact_blasts);
                collect_event_blasts(*level, events->expire_event,
                                     variant.expire_blasts);
            }
            weapon.thing_variants.push_back(std::move(variant));
        }

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

namespace {

std::int32_t blast_damage_impl(std::int32_t value, int radius, int distance) {
    if (radius <= 0) {
        return 0; // original divides by zero; the runtime guard drops damage
    }
    if (distance < 0 || distance > radius) {
        return 0;
    }
    const std::int64_t scaled =
        static_cast<std::int64_t>(value) * (radius - distance) / radius;
    return static_cast<std::int32_t>(scaled);
}

std::int32_t gauge_trunc(double value) {
    if (!std::isfinite(value)) {
        return value < 0.0 ? std::numeric_limits<std::int32_t>::min()
                           : std::numeric_limits<std::int32_t>::max();
    }
    if (value <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(value); // truncation toward zero
}

} // namespace

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

GaugeDamageResult apply_damage_gauge(Vitals& vitals, std::int32_t plate,
                                     std::int32_t hull, std::uint32_t flags,
                                     int side) {
    GaugeDamageResult result;
    if (side < -1 || side > 3) {
        result.error = "shield side outside -1..3";
        return result;
    }
    if ((flags & 1U) != 0U) {
        side = -1;
    }

    if (plate < 0) {
        if (plate == -1) {
            vitals.burn_timer += (hull * 91) / 100;
            if (vitals.burn_timer >= 1) {
                vitals.status_flags |= 0x20U;
            }
        } else if (plate == -2) {
            vitals.status_flags |= 0xC0U;
        }
        result.ok = true;
        result.stun = vitals.hitstun;
        return result;
    }

    if ((flags & 4U) == 0U) {
        plate = gauge_trunc(static_cast<double>(plate) * vitals.mult_plate);
        hull = gauge_trunc(static_cast<double>(hull) * vitals.mult_hull);
        if (plate == 0 && hull == 0) {
            result.ok = true;
            result.stun = vitals.hitstun;
            return result;
        }
    }

    const std::int64_t pool =
        static_cast<std::int64_t>(vitals.armor) + static_cast<std::int64_t>(vitals.shield_max);
    const double denom = std::max(1.0, static_cast<double>(pool) * (1.0 / 64.0));
    std::int32_t stun = gauge_trunc(524288.0 * static_cast<double>(plate + hull) / denom);
    stun = std::clamp<std::int32_t>(stun, 0x10000, 0x80000);
    if (vitals.hitstun < stun) {
        vitals.hitstun = stun;
    }
    result.stun = vitals.hitstun;

    std::int32_t spill = hull;
    std::array<bool, 4> broken{};

    if (side >= 0) {
        const auto index = static_cast<std::size_t>(side);
        if (vitals.shields[index] > 0) {
            vitals.shields[index] -= plate;
            result.plate_absorbed = plate;
        } else {
            spill = plate + hull;
        }
    } else {
        if ((flags & 2U) == 0U) {
            plate >>= 2;
        }
        for (std::size_t i = 0; i < vitals.shields.size(); ++i) {
            if (vitals.shields[i] > 0) {
                vitals.shields[i] -= plate;
                result.plate_absorbed += plate;
            } else {
                spill += ((flags & 2U) != 0U) ? (plate >> 2) : plate;
            }
        }
    }

    for (std::size_t i = 0; i < vitals.shields.size(); ++i) {
        if (vitals.shields[i] < 0) {
            const std::int32_t negative =
                ((flags & 2U) != 0U) ? (vitals.shields[i] >> 2) : vitals.shields[i];
            spill -= negative;
            vitals.shields[i] = 0;
            broken[i] = true;
        }
    }

    const std::int32_t old_armor = vitals.armor;
    vitals.armor -= spill;
    result.hull_absorbed = spill;
    result.broken = broken;
    if (vitals.armor < 0) {
        result.overflow = -vitals.armor;
        vitals.armor = 0;
    }
    result.dead = vitals.armor <= 0;

    if (!result.dead && vitals.armor_max > 0) {
        const std::int32_t half = vitals.armor_max / 2;
        result.low_health = old_armor >= half && vitals.armor < half;
        result.recovered = old_armor < half && vitals.armor >= half;
    }
    result.ok = true;
    return result;
}

std::int32_t blast_damage(const Blast& blast, int distance) {
    if (blast.plate < 0) {
        return blast.plate;
    }
    return blast_damage_impl(blast.plate, blast.radius, distance);
}

std::int32_t blast_hull_damage(const Blast& blast, int distance) {
    if (blast.plate < 0) {
        return blast.hull;
    }
    return blast_damage_impl(blast.hull, blast.radius, distance);
}

BlastResult apply_blast(const Vec3d& center, const Blast& blast,
                        int source_player, int source_thing,
                        std::span<const Vec3d> player_positions,
                        std::span<const Vec3d> thing_positions,
                        std::span<const bool> thing_alive) {
    BlastResult result;
    result.chain_reaction = blast.plate > 0;
    const bool skip_players = (blast.flags & 4U) != 0U;
    const bool skip_things = (blast.flags & 8U) != 0U;
    const auto distance_to = [&center](const Vec3d& target) {
        const double dx = center.x - target.x;
        const double dy = center.y - target.y;
        const double dz = center.z - target.z;
        return static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz));
    };
    const auto add_hit = [&](bool is_player, int index, int distance) {
        BlastHit hit;
        hit.is_player = is_player;
        hit.index = index;
        hit.distance = distance;
        hit.plate = blast_damage(blast, distance);
        hit.hull = blast_hull_damage(blast, distance);
        hit.event = blast.event;
        result.hits.push_back(hit);
    };

    if (!skip_players) {
        for (std::size_t i = 0; i < player_positions.size() && i < 8; ++i) {
            if (blast.flags & 1U) {
                if (source_player >= 0 && static_cast<int>(i) == source_player) {
                    continue;
                }
            }
            const int distance = distance_to(player_positions[i]);
            if (distance <= blast.radius) {
                add_hit(true, static_cast<int>(i), distance);
            }
        }
    }
    if (!skip_things) {
        for (std::size_t i = 0; i < thing_positions.size(); ++i) {
            if (i < thing_alive.size() && !thing_alive[i]) {
                continue;
            }
            if ((blast.flags & 1U) != 0U && source_thing >= 0 &&
                static_cast<int>(i) == source_thing) {
                continue;
            }
            const int distance = distance_to(thing_positions[i]);
            if (distance <= blast.radius) {
                add_hit(false, static_cast<int>(i), distance);
            }
        }
    }
    return result;
}

BlastDamageResult apply_blast_damage(
    const Vec3d& center, const Blast& blast, int source_player,
    int source_thing, std::span<const Vec3d> player_positions,
    std::span<Vitals> player_vitals, std::span<const Vec3d> thing_positions,
    std::span<const bool> thing_alive, std::span<ThingVitals> thing_vitals) {
    BlastDamageResult result;
    const BlastResult spatial = apply_blast(
        center, blast, source_player, source_thing, player_positions,
        thing_positions, thing_alive);
    result.chain_reaction = spatial.chain_reaction;
    result.hits.reserve(spatial.hits.size());
    for (const BlastHit& hit : spatial.hits) {
        AppliedBlastHit applied;
        applied.hit = hit;
        if (hit.is_player) {
            const std::size_t index = static_cast<std::size_t>(hit.index);
            if (index < player_vitals.size()) {
                applied.player_damage = apply_damage_gauge(
                    player_vitals[index], hit.plate, hit.hull, blast.flags, -1);
            }
        } else {
            const std::size_t index = static_cast<std::size_t>(hit.index);
            if (index < thing_vitals.size()) {
                applied.thing_damage = apply_thing_damage(
                    thing_vitals[index], hit.plate, hit.hull, blast.flags, -1);
            }
        }
        result.hits.push_back(applied);
    }
    return result;
}

BlastDamageResult apply_weapon_blast(
    const Weapon& weapon, bool expired, const Vec3d& center, bool underwater,
    bool from_ground, bool deathmatch, int source_player, int source_thing,
    std::span<const Vec3d> player_positions, std::span<Vitals> player_vitals,
    std::span<const Vec3d> thing_positions,
    std::span<const bool> thing_alive, std::span<ThingVitals> thing_vitals,
    double power_up) {
    BlastDamageResult result;
    const std::vector<Blast> selected =
        power_up >= 0.0
            ? weapon.select_blasts_at_power(power_up, expired, underwater,
                                            from_ground, deathmatch)
            : weapon.select_blasts(expired, underwater, from_ground,
                                   deathmatch);
    for (const Blast& blast : selected) {
        BlastDamageResult one = apply_blast_damage(
            center, blast, source_player, source_thing, player_positions,
            player_vitals, thing_positions, thing_alive, thing_vitals);
        result.chain_reaction = result.chain_reaction | one.chain_reaction;
        result.hits.insert(result.hits.end(), one.hits.begin(), one.hits.end());
    }
    return result;
}

BlastDamageResult apply_weapon_inline(
    const Weapon& weapon, const Vec3d& center, bool underwater, bool from_ground,
    bool deathmatch, int source_player, int source_thing,
    std::span<const Vec3d> player_positions, std::span<Vitals> player_vitals,
    std::span<const Vec3d> thing_positions,
    std::span<const bool> thing_alive, std::span<ThingVitals> thing_vitals) {
    BlastDamageResult result;
    for (const Blast& blast :
         weapon.select_inline_blasts(underwater, from_ground, deathmatch)) {
        BlastDamageResult one = apply_blast_damage(
            center, blast, source_player, source_thing, player_positions,
            player_vitals, thing_positions, thing_alive, thing_vitals);
        result.chain_reaction = result.chain_reaction | one.chain_reaction;
        result.hits.insert(result.hits.end(), one.hits.begin(), one.hits.end());
    }
    return result;
}

ThingDamageResult apply_thing_damage(ThingVitals& vitals, std::int32_t plate,
                                     std::int32_t hull, std::uint32_t flags,
                                     int side) {
    ThingDamageResult result;
    if (side < -1 || side > 3) {
        result.error = "shield side outside -1..3";
        return result;
    }
    if (plate < 0) {
        // 0x470500 status path: -1 adds (hull * 91)/100 to the burn timer and
        // sets 0x40 once it reaches one; -2 sets 0x18 (frozen) unless 0x40 is
        // already present.  `hull` is the operand, not scaled by falloff.
        if (plate == -1) {
            vitals.burn_timer += (hull * 91) / 100;
            if (vitals.burn_timer < 1) {
                result.ok = true;
                return result;
            }
            vitals.status_flags |= 0x40U;
        } else if (plate == -2) {
            if ((vitals.status_flags & 0x40U) != 0U) {
                result.ok = true;
                return result;
            }
            vitals.status_flags |= 0x18U;
        } else {
            result.ok = true;
            return result;
        }
        result.ok = true;
        return result;
    }
    if ((flags & 1U) != 0U) {
        side = -1;
    }

    std::int64_t spill = hull;
    if (side >= 0) {
        auto& shield = vitals.shields[static_cast<std::size_t>(side)];
        const std::int32_t old = shield;
        shield = static_cast<std::int32_t>(
            static_cast<std::int64_t>(shield) - plate);
        result.shield_absorbed = std::min<std::int32_t>(old, plate);
        if (shield < 0) {
            spill -= shield; // deficit, not the whole plate, reaches HP
            shield = 0;
            result.broken[static_cast<std::size_t>(side)] = true;
        }
    } else {
        if ((flags & 2U) == 0U) {
            plate >>= 2;
        }
        for (std::size_t i = 0; i < vitals.shields.size(); ++i) {
            auto& shield = vitals.shields[i];
            const std::int32_t old = shield;
            shield = static_cast<std::int32_t>(
                static_cast<std::int64_t>(shield) - plate);
            result.shield_absorbed += std::min<std::int32_t>(old, plate);
            if (shield < 0) {
                spill -= shield;
                shield = 0;
                result.broken[i] = true;
            }
        }
    }

    const std::int32_t old_hp = vitals.hp;
    vitals.hp = static_cast<std::int32_t>(
        static_cast<std::int64_t>(vitals.hp) - spill);
    if (vitals.hp < 0) {
        result.overflow = -vitals.hp;
        vitals.hp = 0;
    }
    result.hull_absorbed = static_cast<std::int32_t>(spill);
    result.dead = vitals.hp <= 0;
    if (!result.dead && vitals.max_hp > 0 &&
        (vitals.flags2 & 0x1000U) != 0U) {
        const std::int32_t half = vitals.max_hp / 2;
        result.low_health_event = old_hp >= half && vitals.hp < half;
    }
    result.damaged_event = !result.dead;
    result.ok = true;
    return result;
}

ThingVitals thing_vitals(const Thing& thing) {
    ThingVitals vitals;
    vitals.hp = thing.health;
    vitals.max_hp = thing.max_health;
    vitals.shields = thing.shields;
    vitals.shield_max = thing.shield_max;
    vitals.alive = thing.alive;
    vitals.burn_timer = thing.burn_timer;
    vitals.status_flags = thing.status_flags;
    return vitals;
}

void store_thing_vitals(Thing& thing, const ThingVitals& vitals) {
    thing.health = vitals.hp;
    thing.max_health = vitals.max_hp;
    thing.shields = vitals.shields;
    thing.shield_max = vitals.shield_max;
    thing.burn_timer = vitals.burn_timer;
    thing.status_flags = vitals.status_flags;
    thing.alive = vitals.alive && vitals.hp > 0;
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
