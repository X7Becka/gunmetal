// Pickup rules as a pure machine (`M6.2.6` slice 3, docs/PICKUP_STATE.md).
//
// Pinned here:
//   * armor: `HEAL_SOURCEPLAYER` (op 112) adds and clamps at `armor_max`
//     (10000 hundredths = chassis `op3 100 * 100`, docs/DAMAGE.md §0.1); over
//     the cap is a proven no-op (`already_full`);
//   * shields: `REPAIR_SOURCEPLAYER` (op 113) rotation branch fills the four
//     plates one point at a time 0→1→2→3 (`0x45b177`), exactly 100 points on
//     four empty plates is 25/25/25/25; the cap is `shield_max`
//     (LIGHT 8000 = `op2 80 * 100`, §0.2);
//   * weapon/equipment/key: the pickups OR one ownership bit (ops 108/111/106)
//     so a repeat is an idempotent no-op — classified `rejected_duplicate`;
//   * ammo: op 109 adds and clamps `[0, max]`, so a repeat stacks and is *not*
//     a duplicate;
//   * cash: op 114 adds, clamp at 0 (docs/ECONOMY.md §2.1);
//   * secret: op 95 adds exactly one to the tally and has no cap
//     (docs/CAMPAIGN_FLOW.md §5);
//   * mass: the limit is a model (docs/ECONOMY.md §6 proves the original never
//     gates on mass), off by default; an over-limit item is refused atomically;
//   * negatives: unknown kind, `id < 0`, `amount <= 0`, `mass < 0`, cash
//     overflow — refusal with a text, inventory untouched, no crash.
//
// Real data (`level0.sbt`/`dm1.sbt`) is read through `gm::load_level` +
// `gm::decode_sbt_script`, the seam `test_economy_state.cpp` uses: item-op
// counts, trigger/radius split, `DEFINE_SHIELDS (50, 0)`,
// `DEFINE_REGENERATION_MARKER (19, 3000, 50)`, the virus numbers and the four
// chassis rows.  Without the installation the file half prints a reason and
// skips; the synthetic half always runs.
#include "game/pickup.h"

#include "res/level.h"
#include "res/sbtvm.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

// --- numbers of docs/PICKUP.md §2/§6/§8/§9 --------------------------------

/// Item slots of the original table (`0x12c` = 300, §2).
constexpr int kItemSlots = 300;
/// Direct item records in the 36 shipped levels (§6): 401 + 17 + 636 + 84.
constexpr int kDirectItems = 1138;
constexpr int kOp52 = 401;
constexpr int kOp53 = 17;
constexpr int kOp54 = 636;
constexpr int kOp55 = 84;
constexpr int kOp177 = 440;
constexpr int kOp185 = 53;
constexpr int kOp186 = 4252;
/// §6: 448 decorative items (`trigger = 0`, radius 0) and 690 with a trigger.
constexpr int kTriggerZero = 448;
constexpr int kTriggerHit = 690;
/// Reward opcodes reached from the triggers (§6).
constexpr int kRewardCash = 310;
constexpr int kRewardSalvage = 306;
constexpr int kRewardHeal = 219;
constexpr int kRewardRemove = 408;
constexpr int kRewardScreenText = 317;
constexpr int kRewardSample = 441;
constexpr int kRewardInfect = 37;
/// §8: L0 = 16 (drain per tick), V = 28, P = 10, D = 0.03 on every level.
constexpr int kVirusL0 = 16;
constexpr int kVirusVolume = 28;
constexpr int kVirusPriority = 10;
constexpr double kVirusDecay = 0.03;
/// §9: `DEFINE_SHIELDS` is `(50, 0)` in all 36 levels — price 50 per 100
/// points, weight 0.
constexpr int kShieldPrice = 50;
constexpr int kShieldWeight = 0;
/// §7: `DEFINE_REGENERATION_MARKER` is `(19, 3000.0, 50.0)` in all 36 levels.
constexpr int kRegenSob = 19;
constexpr double kRegenNear = 3000.0;
constexpr double kRegenFar = 50.0;

/// Chassis rows of `DEFINE_PLAYER_TYPES`: `op2` (shields) and `op3`
/// (armor_max) are stored ×100 (docs/CHASSIS.md §3, §4).  All 36 levels carry
/// the same values.
struct ChassisDatum {
    const char* name;
    int shields;
    int armor_max;
};

constexpr ChassisDatum kChassis[] = {
    {"LIGHT CHASSIS", 80, 100},
    {"MEDIUM CHASSIS", 130, 100},
    {"STEALTH CHASSIS", 90, 100},
    {"HEAVY CHASSIS", 200, 100},
};

constexpr int kArmorMax = 10000;        // 100 * 100, every chassis
constexpr int kLightShieldMax = 8000;   // 80 * 100

// --- real files ------------------------------------------------------------

std::int64_t as_int(const gm::SbtValue& value) {
    return value.kind == gm::SbtValue::Kind::Real
               ? static_cast<std::int64_t>(std::llround(value.d))
               : static_cast<std::int64_t>(value.u);
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

double as_double(const gm::SbtValue& value) {
    return value.kind == gm::SbtValue::Kind::Real ? value.d : static_cast<double>(value.u);
}

template <typename Fn>
void walk_insns(const std::vector<gm::SbtInsn>& insns, Fn&& fn) {
    for (const gm::SbtInsn& insn : insns) {
        fn(insn);
        walk_insns(insn.body, fn);
    }
}

struct ChassisRow {
    std::string name;
    int shields = 0;
    int armor_max = 0;
};

struct LevelFacts {
    bool present = false;
    std::string error;
    int items = 0;
    std::array<int, 7> ops{}; ///< 52, 53, 54, 55, 177, 185, 186
    int direct = 0;           ///< ops 52..55 only
    int trigger_zero = 0;
    int trigger_nonzero = 0;
    int radius_zero = 0;
    int radius_nonzero = 0;
    int shield_price = -1;
    int shield_weight = -1;
    bool has_regen = false;
    int regen_sob = -1;
    double regen_near = 0.0;
    double regen_far = 0.0;
    bool has_virus = false;
    std::array<int, 5> virus{}; ///< L0, T, N, V, P
    double virus_decay = 0.0;
    std::vector<ChassisRow> chassis;
};

int op_index(int opcode) {
    switch (opcode) {
    case 52:
        return 0;
    case 53:
        return 1;
    case 54:
        return 2;
    case 55:
        return 3;
    case 177:
        return 4;
    case 185:
        return 5;
    case 186:
        return 6;
    default:
        return -1;
    }
}

LevelFacts load_level_facts(const char* name) {
    LevelFacts out;
    std::string error;
    const auto level = gm::load_level(kGame, name, error);
    if (!level) {
        out.error = error;
        return out;
    }
    if (!level->entities) {
        out.error = "no SCRIPT/SBT record";
        return out;
    }
    const auto program = gm::decode_sbt_script(level->entities->script_code, error);
    if (!program) {
        out.error = error;
        return out;
    }

    walk_insns(program->insns, [&](const gm::SbtInsn& insn) {
        const int index = op_index(insn.opcode);
        if (index >= 0) {
            ++out.items;
            ++out.ops[static_cast<std::size_t>(index)];
            // `OPPEELP`/`OPPEELLLLAAAP`: operands 3 = trigger, 5 = radius.
            if (index <= 3) {
                ++out.direct;
                const int trigger = insn.args.size() > 3 ? static_cast<int>(as_int(insn.args[3])) : 0;
                if (trigger == 0) {
                    ++out.trigger_zero;
                } else {
                    ++out.trigger_nonzero;
                }
                if (insn.args.size() > 5) {
                    const int radius = static_cast<int>(as_int(insn.args[5]));
                    if (radius == 0) {
                        ++out.radius_zero;
                    } else {
                        ++out.radius_nonzero;
                    }
                }
            }
        } else if (insn.opcode == 176 && insn.args.size() >= 2) {
            out.shield_price = static_cast<int>(as_int(insn.args[0]));
            out.shield_weight = static_cast<int>(as_int(insn.args[1]));
        } else if (insn.opcode == 174 && insn.args.size() >= 3) {
            out.has_regen = true;
            out.regen_sob = static_cast<int>(as_int(insn.args[0]));
            out.regen_near = as_double(insn.args[1]);
            out.regen_far = as_double(insn.args[2]);
        } else if (insn.opcode == 173 && insn.args.size() >= 6) {
            out.has_virus = true;
            for (int i = 0; i < 5; ++i) {
                out.virus[static_cast<std::size_t>(i)] = static_cast<int>(as_int(insn.args[i]));
            }
            out.virus_decay = as_double(insn.args[5]);
        } else if (insn.name == "DEFINE_PLAYER_TYPES") {
            // Several branches carry their own table (campaign / DM / `/god`);
            // the last one is the campaign table, like the economy test reads.
            out.chassis.clear();
            for (const gm::SbtTableRecord& record : insn.table) {
                if (record.values.size() < 4 || record.strings.empty()) {
                    continue;
                }
                ChassisRow row;
                row.name = record.strings[0];
                row.shields = static_cast<int>(as_int(record.values[2]));    // +0x14c
                row.armor_max = static_cast<int>(as_int(record.values[3]));  // +0x150
                out.chassis.push_back(std::move(row));
            }
        }
    });

    out.present = true;
    return out;
}

// --- synthetic cases -------------------------------------------------------

using gm::PickupItem;
using gm::PickupKind;
using gm::PickupResult;
using gm::PickupRules;
using gm::PlayerInventoryModel;

PickupItem item(PickupKind kind, int id, int amount, int mass = 0) {
    PickupItem out;
    out.kind = kind;
    out.id = id;
    out.amount = amount;
    out.mass = mass;
    return out;
}

PlayerInventoryModel model() {
    PlayerInventoryModel out;
    out.armor_max = kArmorMax;
    out.shield_max = kLightShieldMax;
    return out;
}

void test_armor() {
    PickupRules rules;
    PlayerInventoryModel inv = model();
    std::string msg;

    CHECK(rules.take(item(PickupKind::armor, 0, 400), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.armor, 400);
    CHECK_EQ(inv.mass, 0);
    CHECK(contains(msg, "op 112"));
    std::printf("pickup: armor +400 -> %d/%d (%s)\n", inv.armor, inv.armor_max, msg.c_str());

    CHECK(rules.take(item(PickupKind::armor, 0, 9700), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.armor, kArmorMax);
    std::printf("pickup: armor +9700 clamps -> %d/%d\n", inv.armor, inv.armor_max);

    CHECK(rules.take(item(PickupKind::armor, 0, 100), inv, &msg) ==
          PickupResult::already_full);
    CHECK_EQ(inv.armor, kArmorMax);
    CHECK(contains(msg, "already at max 10000"));
    std::printf("pickup: armor +100 at cap -> already_full, armor=%d\n", inv.armor);

    // `health` is the same gauge: the player has no separate HP (DAMAGE §0.1).
    PlayerInventoryModel hp;
    hp.armor_max = kArmorMax;
    hp.armor = 9500;
    CHECK(rules.take(item(PickupKind::health, 0, 900), hp, &msg) == PickupResult::taken);
    CHECK_EQ(hp.armor, kArmorMax);
    CHECK(rules.take(item(PickupKind::health, 0, 1), hp, &msg) == PickupResult::already_full);
    CHECK_EQ(rules.stats().taken, 3);
    CHECK_EQ(rules.stats().already_full, 2);
    CHECK_EQ(rules.stats().taken_by_kind[static_cast<int>(PickupKind::health)], 1);
    std::printf("pickup: health 900 heals the armor gauge 9500 -> %d, next -> already_full\n",
                hp.armor);
}

void test_shields() {
    PickupRules rules;
    PlayerInventoryModel inv = model();
    std::string msg;

    CHECK(rules.take(item(PickupKind::shield, 0, 100), inv, &msg) == PickupResult::taken);
    for (int plate : inv.shields) {
        CHECK_EQ(plate, 25);
    }
    std::printf("pickup: shield +100 on 0/0/0/0 (max %d) -> %d/%d/%d/%d\n", inv.shield_max,
                inv.shields[0], inv.shields[1], inv.shields[2], inv.shields[3]);

    CHECK(rules.take(item(PickupKind::shield, 0, 100), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.shields[0], 50);
    CHECK_EQ(inv.shields[3], 50);
    std::printf("pickup: shield +100 again -> %d/%d/%d/%d\n", inv.shields[0], inv.shields[1],
                inv.shields[2], inv.shields[3]);

    CHECK(rules.take(item(PickupKind::shield, 0, 31800), inv, &msg) == PickupResult::taken);
    for (int plate : inv.shields) {
        CHECK_EQ(plate, kLightShieldMax);
    }
    std::printf("pickup: shield +31800 fills the remaining capacity -> %d each\n",
                inv.shields[0]);

    CHECK(rules.take(item(PickupKind::shield, 0, 100), inv, &msg) ==
          PickupResult::already_full);
    CHECK_EQ(inv.shields[0], kLightShieldMax);
    std::printf("pickup: shield +100 at cap -> already_full, max=%d\n", inv.shield_max);

    // Rotation skips the full plate: 8000/7900/0/0 and 100 points →
    // 33 full rounds (99 points) + 1 to the first non-full plate (plate 1).
    PlayerInventoryModel uneven = model();
    uneven.shields = {8000, 7900, 0, 0};
    CHECK(rules.take(item(PickupKind::shield, 0, 100), uneven, &msg) == PickupResult::taken);
    CHECK_EQ(uneven.shields[0], 8000);
    CHECK_EQ(uneven.shields[1], 7934);
    CHECK_EQ(uneven.shields[2], 33);
    CHECK_EQ(uneven.shields[3], 33);
    std::printf("pickup: shield +100 on 8000/7900/0/0 -> %d/%d/%d/%d (rotation skips full)\n",
                uneven.shields[0], uneven.shields[1], uneven.shields[2], uneven.shields[3]);

    // capacity-limited: 8000 room among 8000/7999/8000/8000 → 1 point only.
    PlayerInventoryModel tight = model();
    tight.shields = {8000, 7999, 8000, 8000};
    CHECK(rules.take(item(PickupKind::shield, 0, 100), tight, &msg) == PickupResult::taken);
    CHECK_EQ(tight.shields[1], 8000);
    std::printf("pickup: shield +100 with 1 free point -> stored 1, now %d/%d/%d/%d\n",
                tight.shields[0], tight.shields[1], tight.shields[2], tight.shields[3]);

    PlayerInventoryModel none = model();
    none.shield_max = 0;
    CHECK(rules.take(item(PickupKind::shield, 0, 100), none, &msg) ==
          PickupResult::already_full);
    std::printf("pickup: shield_max 0 -> already_full, no crash\n");

    CHECK_EQ(gm::distribute_shields(none.shields, 0, 100), 0);
    CHECK_EQ(gm::distribute_shields(none.shields, 8000, 0), 0);
}

void test_weapons() {
    PickupRules rules;
    PlayerInventoryModel inv = model();
    std::string msg;

    CHECK(rules.take(item(PickupKind::weapon, 7, 1), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.weapons.size(), std::size_t{1});
    CHECK_EQ(inv.weapons[0], 7);
    CHECK(contains(msg, "op 108"));
    std::printf("pickup: weapon 7 -> %zu owned (%s)\n", inv.weapons.size(), msg.c_str());

    CHECK(rules.take(item(PickupKind::weapon, 7, 1), inv, &msg) ==
          PickupResult::rejected_duplicate);
    CHECK_EQ(inv.weapons.size(), std::size_t{1});
    CHECK(contains(msg, "no-op"));
    std::printf("pickup: weapon 7 again -> rejected_duplicate, owned %zu (OR is idempotent)\n",
                inv.weapons.size());

    CHECK(rules.take(item(PickupKind::weapon, 3, 1), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.weapons.size(), std::size_t{2});
    CHECK_EQ(inv.weapons[0], 3); // ascending
    CHECK_EQ(inv.weapons[1], 7);
    std::printf("pickup: weapon 3 -> %d/%d sorted\n", inv.weapons[0], inv.weapons[1]);

    CHECK(rules.take(item(PickupKind::equipment, 4, 1), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.equipment.count(4), std::size_t{1});
    CHECK(rules.take(item(PickupKind::equipment, 4, 1), inv, &msg) ==
          PickupResult::rejected_duplicate);
    CHECK_EQ(inv.equipment.size(), std::size_t{1});
    std::printf("pickup: equipment 4 added, repeat -> rejected_duplicate (op 111)\n");

    CHECK(rules.take(item(PickupKind::key, 2, 1), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::key, 2, 1), inv, &msg) ==
          PickupResult::rejected_duplicate);
    CHECK_EQ(inv.keys.size(), std::size_t{1});
    std::printf("pickup: key 2 added, repeat -> rejected_duplicate (op 106)\n");

    CHECK_EQ(rules.stats().taken, 4);
    CHECK_EQ(rules.stats().rejected_duplicate, 3);
}

void test_ammo() {
    PickupRules rules;
    PlayerInventoryModel inv = model();
    inv.ammo_max = 5000;
    std::string msg;

    CHECK(rules.take(item(PickupKind::ammo, 0, 100), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.ammo.size(), std::size_t{1});
    CHECK_EQ(inv.ammo[0], 100);
    std::printf("pickup: ammo type 0 +100 -> %d/%d (op 109)\n", inv.ammo[0], inv.ammo_max);

    // A repeat is a stack, not a duplicate: op 109 adds, it does not OR a bit.
    CHECK(rules.take(item(PickupKind::ammo, 0, 100), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.ammo[0], 200);
    CHECK_EQ(rules.stats().rejected_duplicate, 0);
    std::printf("pickup: ammo type 0 +100 again -> %d (stacks, not rejected_duplicate)\n",
                inv.ammo[0]);

    CHECK(rules.take(item(PickupKind::ammo, 0, 4900), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.ammo[0], 5000);
    CHECK(rules.take(item(PickupKind::ammo, 0, 1), inv, &msg) == PickupResult::already_full);
    CHECK_EQ(inv.ammo[0], 5000);
    std::printf("pickup: ammo +4900 clamps -> %d/%d, next -> already_full\n", inv.ammo[0],
                inv.ammo_max);

    CHECK(rules.take(item(PickupKind::ammo, 3, 1000), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.ammo.size(), std::size_t{4});
    CHECK_EQ(inv.ammo[1], 0);
    CHECK_EQ(inv.ammo[3], 1000);
    std::printf("pickup: ammo type 3 +1000 -> %d, types 0..3 = %zu (gap filled with 0)\n",
                inv.ammo[3], inv.ammo.size());

    PlayerInventoryModel uncapped = model();
    CHECK(rules.take(item(PickupKind::ammo, 1, 1000), uncapped, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::ammo, 1, 1000), uncapped, &msg) == PickupResult::taken);
    CHECK_EQ(uncapped.ammo[1], 2000);
    std::printf("pickup: ammo_max 0 (uncapped) -> %d after two +1000\n", uncapped.ammo[1]);
}

void test_mass() {
    // Default: no limit.  §6 proves the original does not gate on mass.
    PickupRules rules;
    PlayerInventoryModel inv = model();
    std::string msg;
    CHECK_EQ(inv.mass_limit, PickupRules::kNoMassLimit);
    CHECK(rules.take(item(PickupKind::weapon, 11, 1, 600), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.mass, 600);
    std::printf("pickup: mass: no limit (shipped rule, E §6) -> weapon mass 600, mass=%d\n",
                inv.mass);

    // Model limit: exact boundary is allowed, one unit more is refused and the
    // inventory does not move.
    PickupRules limited_rules;
    PlayerInventoryModel limited = model();
    limited.mass = 400;
    limited.mass_limit = 500;
    CHECK(limited_rules.take(item(PickupKind::equipment, 1, 1, 200), limited, &msg) ==
          PickupResult::rejected_mass);
    CHECK_EQ(limited.mass, 400);
    CHECK_EQ(limited.equipment.size(), std::size_t{0});
    CHECK(contains(msg, "> limit 500"));
    std::printf("pickup: mass 400 + 200 > 500 -> rejected_mass, mass=%d, equipment=%zu\n",
                limited.mass, limited.equipment.size());

    CHECK(limited_rules.take(item(PickupKind::equipment, 1, 1, 100), limited, &msg) ==
          PickupResult::taken);
    CHECK_EQ(limited.mass, 500); // exact boundary
    CHECK(limited_rules.take(item(PickupKind::equipment, 3, 1, 1), limited, &msg) ==
          PickupResult::rejected_mass);
    CHECK_EQ(limited.mass, 500);
    CHECK(limited_rules.take(item(PickupKind::equipment, 2, 1, 0), limited, &msg) ==
          PickupResult::taken);
    CHECK_EQ(limited.mass, 500);
    CHECK_EQ(limited_rules.stats().rejected_mass, 2);
    CHECK_EQ(limited_rules.stats().taken, 2);
    std::printf("pickup: mass exact boundary 400 + 100 -> %d == limit; +1 -> rejected_mass\n",
                limited.mass);

    // Order: a duplicate is classified before the mass check (no mass spent).
    PlayerInventoryModel order = model();
    order.mass_limit = 10;
    order.weapons = {5};
    CHECK(rules.take(item(PickupKind::weapon, 5, 1, 1000), order, &msg) ==
          PickupResult::rejected_duplicate);
    CHECK_EQ(order.mass, 0);
    std::printf("pickup: duplicate weapon with mass 1000 > limit 10 -> rejected_duplicate "
                "(checked before mass)\n");
}

void test_cash_secret() {
    PickupRules rules;
    PlayerInventoryModel inv = model();
    inv.cash = 2000; // saved0 (docs/ECONOMY.md §9)
    std::string msg;

    CHECK(rules.take(item(PickupKind::cash, 0, 500), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.cash, 2500);
    std::printf("pickup: cash 2000 + 500 -> %d (op 114)\n", inv.cash);

    // A secret adds exactly one to the tally whatever `amount` says, and there
    // is no cap: the tally may exceed `SET_LEVEL_SECRETS` (§5).
    inv.secrets = 0;
    inv.secrets_declared = 3;
    CHECK(rules.take(item(PickupKind::secret, 0, 5), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.secrets, 1);
    CHECK(contains(msg, "op 95"));
    CHECK(rules.take(item(PickupKind::secret, 0, 1), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::secret, 0, 1), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.secrets, 3);
    CHECK(rules.take(item(PickupKind::secret, 0, 1), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.secrets, 4); // 4 > 3 declared, still taken
    std::printf("pickup: secret amount 5 -> tally 1; total %d of %d declared (op 95, no cap)\n",
                inv.secrets, inv.secrets_declared);
}

std::string snapshot(const PlayerInventoryModel& inv, const gm::PickupStats& stats) {
    std::string out = std::to_string(inv.armor) + "/" + std::to_string(inv.armor_max) + "/" +
                      std::to_string(inv.shield_max);
    for (int plate : inv.shields) {
        out += "," + std::to_string(plate);
    }
    out += "|" + std::to_string(inv.cash) + "|" + std::to_string(inv.mass) + "/" +
           std::to_string(inv.mass_limit) + "|";
    for (int id : inv.weapons) {
        out += std::to_string(id) + ",";
    }
    out += "|";
    for (int id : inv.equipment) {
        out += std::to_string(id) + ",";
    }
    out += "|";
    for (int id : inv.keys) {
        out += std::to_string(id) + ",";
    }
    out += "|";
    for (int rounds : inv.ammo) {
        out += std::to_string(rounds) + ",";
    }
    out += "|" + std::to_string(inv.secrets) + "/" + std::to_string(inv.secrets_declared) + "|" +
           std::to_string(stats.taken) + "/" + std::to_string(stats.already_full) + "/" +
           std::to_string(stats.rejected_mass) + "/" +
           std::to_string(stats.rejected_duplicate) + "/" +
           std::to_string(stats.rejected_unknown) + "/" + std::to_string(stats.mass_taken);
    return out;
}

void test_negatives() {
    PickupRules rules;
    PlayerInventoryModel inv = model();
    inv.armor = 100;
    inv.cash = 50;
    const std::string before = snapshot(inv, rules.stats());
    std::string msg;
    // Empty item: default kind is `unknown`.
    CHECK(rules.take(PickupItem{}, inv, &msg) == PickupResult::rejected_unknown);
    CHECK(contains(msg, "unknown pickup kind"));
    std::printf("pickup: empty item -> rejected_unknown (%s)\n", msg.c_str());

    CHECK(rules.take(item(PickupKind::unknown, 0, 1), inv, &msg) ==
          PickupResult::rejected_unknown);
    CHECK(rules.take(item(PickupKind::armor, -1, 100), inv, &msg) ==
          PickupResult::rejected_unknown);
    CHECK(contains(msg, "negative id -1"));
    CHECK(rules.take(item(PickupKind::armor, 0, 0), inv, &msg) == PickupResult::rejected_unknown);
    CHECK(contains(msg, "amount 0 <= 0"));
    CHECK(rules.take(item(PickupKind::armor, 0, -5), inv, &msg) ==
          PickupResult::rejected_unknown);
    CHECK(rules.take(item(PickupKind::weapon, 1, 1, -1), inv, &msg) ==
          PickupResult::rejected_unknown);
    CHECK(contains(msg, "negative mass -1"));
    CHECK_EQ(rules.take(PickupItem{}, inv, nullptr), PickupResult::rejected_unknown);

    // Cash overflow is refused before the field moves.
    PlayerInventoryModel rich = model();
    rich.cash = std::numeric_limits<int>::max();
    CHECK(rules.take(item(PickupKind::cash, 0, 1), rich, &msg) ==
          PickupResult::rejected_unknown);
    CHECK_EQ(rich.cash, std::numeric_limits<int>::max());
    CHECK(contains(msg, "overflow"));
    std::printf("pickup: cash INT_MAX + 1 -> rejected_unknown, cash unchanged\n");

    // Huge amount is capacity-limited, not a crash.
    PlayerInventoryModel big = model();
    CHECK(rules.take(item(PickupKind::shield, 0, std::numeric_limits<int>::max()), big, &msg) ==
          PickupResult::taken);
    for (int plate : big.shields) {
        CHECK_EQ(plate, kLightShieldMax);
    }
    std::printf("pickup: shield INT_MAX -> %d each (capacity-limited)\n", big.shields[0]);

    CHECK_EQ(inv.armor, 100);
    CHECK_EQ(inv.cash, 50);
    CHECK(inv.weapons.empty());
    CHECK(inv.equipment.empty());
    CHECK_EQ(snapshot(inv, gm::PickupStats{}), before);
    CHECK_EQ(rules.stats().rejected_unknown, 8);
    std::printf("pickup: negatives -> %d refusals, inventory untouched\n",
                rules.stats().rejected_unknown);
}

std::string run_sequence() {
    PickupRules rules;
    PlayerInventoryModel inv = model();
    std::string msg;
    CHECK(rules.take(item(PickupKind::armor, 0, 400), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::shield, 0, 100), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::weapon, 7, 1, 15), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::weapon, 7, 1, 15), inv, &msg) ==
          PickupResult::rejected_duplicate);
    CHECK(rules.take(item(PickupKind::ammo, 0, 100), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::cash, 0, 500), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::secret, 0, 5), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::armor, 0, 100), inv, &msg) == PickupResult::taken);
    return snapshot(inv, rules.stats());
}

void test_determinism() {
    const std::string first = run_sequence();
    const std::string second = run_sequence();
    CHECK(first == second);
    CHECK(!first.empty());
    std::printf("pickup: determinism: two runs %s (%zu bytes)\n",
                first == second ? "identical" : "DIFFER", first.size());
}

void test_mass_total() {
    const std::vector<PickupItem> batch = {
        item(PickupKind::weapon, 1, 1, 600),
        item(PickupKind::equipment, 2, 1, 45),
        item(PickupKind::cash, 0, 1000),
        item(PickupKind::armor, 0, 100, -5), // invalid mass ignored by the total
    };
    CHECK_EQ(PickupRules::pickup_mass_total(batch), 645);
    PickupRules rules;
    PlayerInventoryModel inv = model();
    std::string msg;
    for (const PickupItem& entry : batch) {
        (void)rules.take(entry, inv, &msg);
    }
    CHECK_EQ(inv.mass, 645);
    CHECK_EQ(rules.pickup_mass_total(), 645);
    std::printf("pickup: mass_total(batch)=%d, after takes: model=%d rules=%d\n",
                PickupRules::pickup_mass_total(batch), inv.mass, rules.pickup_mass_total());

    CHECK_EQ(PickupRules::kItemSlots, kItemSlots);
}

void test_data(const char* name, const LevelFacts& level) {
    if (!level.present) {
        std::printf("pickup: %s not installed, file checks skipped (%s)\n", name,
                    level.error.c_str());
        return;
    }

    // --- item table (§2/§6) -------------------------------------------------
    CHECK_EQ(level.items, level.ops[0] + level.ops[1] + level.ops[2] + level.ops[3] +
                              level.ops[4] + level.ops[5] + level.ops[6]);
    CHECK_EQ(level.direct,
             level.ops[0] + level.ops[1] + level.ops[2] + level.ops[3]);
    CHECK_EQ(level.trigger_zero + level.trigger_nonzero, level.direct);
    CHECK_EQ(level.radius_zero + level.radius_nonzero, level.direct);
    // P §0/§6: every decorative item has trigger 0 *and* radius 0.
    CHECK_EQ(level.trigger_zero, level.radius_zero);
    std::printf("pickup: %s items=%d (52:%d 53:%d 54:%d 55:%d 177:%d 185:%d 186:%d) "
                "trigger 0/non0=%d/%d radius 0/non0=%d/%d\n",
                name, level.items, level.ops[0], level.ops[1], level.ops[2], level.ops[3],
                level.ops[4], level.ops[5], level.ops[6], level.trigger_zero,
                level.trigger_nonzero, level.radius_zero, level.radius_nonzero);

    // Numbers pinned from the corpus summary of docs/PICKUP.md §6.
    if (std::string(name) == "level0") {
        CHECK_EQ(level.items, 41);
        CHECK_EQ(level.ops[0], 10);
        CHECK_EQ(level.ops[1], 0);
        CHECK_EQ(level.ops[2], 16);
        CHECK_EQ(level.ops[3], 2);
        CHECK_EQ(level.ops[4], 12);
        CHECK_EQ(level.ops[5], 0);
        CHECK_EQ(level.ops[6], 1);
        CHECK_EQ(level.trigger_zero, 9);
        CHECK_EQ(level.trigger_nonzero, 19);
        CHECK_EQ(level.radius_zero, 9);
    } else if (std::string(name) == "dm1") {
        CHECK_EQ(level.items, 170);
        CHECK_EQ(level.ops[0], 10);
        CHECK_EQ(level.ops[1], 1);
        CHECK_EQ(level.ops[2], 22);
        CHECK_EQ(level.ops[3], 0);
        CHECK_EQ(level.ops[4], 12);
        CHECK_EQ(level.ops[5], 1);
        CHECK_EQ(level.ops[6], 124);
        CHECK_EQ(level.trigger_zero, 15);
        CHECK_EQ(level.trigger_nonzero, 18);
        CHECK_EQ(level.radius_zero, 15);
    }

    // --- DEFINE_SHIELDS / marker / virus (§7–§9) ---------------------------
    CHECK_EQ(level.shield_price, kShieldPrice);
    CHECK_EQ(level.shield_weight, kShieldWeight);
    CHECK(level.has_regen);
    CHECK_EQ(level.regen_sob, kRegenSob);
    CHECK(std::fabs(level.regen_near - kRegenNear) < 1e-9);
    CHECK(std::fabs(level.regen_far - kRegenFar) < 1e-9);
    CHECK(level.has_virus);
    CHECK_EQ(level.virus[0], kVirusL0);
    CHECK_EQ(level.virus[2], 12); // N
    CHECK_EQ(level.virus[3], kVirusVolume);
    CHECK_EQ(level.virus[4], kVirusPriority);
    CHECK(std::fabs(level.virus_decay - kVirusDecay) < 1e-9);
    const int expected_t = std::string(name) == "level0" ? 105 : 199;
    CHECK_EQ(level.virus[1], expected_t);
    std::printf("pickup: %s shields=%d/%d regen=(%d, %.1f, %.1f) virus=(%d, %d, %d, %d, %d, "
                "%.2f)\n",
                name, level.shield_price, level.shield_weight, level.regen_sob,
                level.regen_near, level.regen_far, level.virus[0], level.virus[1],
                level.virus[2], level.virus[3], level.virus[4], level.virus_decay);

    // --- chassis: caps for armor/shield pickups (§9, CHASSIS §3) -----------
    int matched = 0;
    for (const ChassisDatum& datum : kChassis) {
        for (const ChassisRow& row : level.chassis) {
            if (row.name != datum.name) {
                continue;
            }
            CHECK_EQ(row.shields, datum.shields);
            CHECK_EQ(row.armor_max, datum.armor_max);
            CHECK_EQ(row.armor_max * 100, kArmorMax);
            CHECK_EQ(row.shields * 100, datum.shields * 100);
            ++matched;
            std::printf("pickup: %s %-14s armor_max=%d shield_max=%d\n", name,
                        row.name.c_str(), row.armor_max * 100, row.shields * 100);
        }
    }
    CHECK_EQ(matched, 4);

    // --- the same rules on the real caps (LIGHT: armor 10000, shields 8000) -
    PickupRules rules;
    PlayerInventoryModel inv;
    inv.armor_max = kArmorMax;
    inv.shield_max = kLightShieldMax;
    std::string msg;
    CHECK(rules.take(item(PickupKind::armor, 0, 2500), inv, &msg) == PickupResult::taken);
    CHECK(rules.take(item(PickupKind::shield, 0, 400), inv, &msg) == PickupResult::taken);
    CHECK_EQ(inv.armor, 2500);
    CHECK_EQ(inv.shields[0], 100);
    std::printf("pickup: %s LIGHT caps: armor +2500 -> %d/%d, shield +400 -> %d/%d/%d/%d\n", name,
                inv.armor, inv.armor_max, inv.shields[0], inv.shields[1], inv.shields[2],
                inv.shields[3]);
}

void test_corpus_numbers() {
    // docs/PICKUP.md §6 — constants only (no file is read here).
    CHECK_EQ(kItemSlots, 300);
    CHECK_EQ(kDirectItems, kOp52 + kOp53 + kOp54 + kOp55);
    CHECK_EQ(kTriggerZero + kTriggerHit, kDirectItems);
    CHECK_EQ(kOp177, 440);
    CHECK_EQ(kOp185 + kOp186, 53 + 4252);
    std::printf("pickup: corpus (36 levels): direct=%d (52:%d 53:%d 54:%d 55:%d) "
                "trigger 0/hit=%d/%d radius0=%d; named 185:%d 186:%d random-ammo:%d; "
                "slots=%d\n",
                kDirectItems, kOp52, kOp53, kOp54, kOp55, kTriggerZero, kTriggerHit,
                kTriggerZero, kOp185, kOp186, kOp177, kItemSlots);
    std::printf("pickup: corpus rewards: cash=%d salvage=%d heal=%d remove=%d screen=%d "
                "sample=%d infect=%d\n",
                kRewardCash, kRewardSalvage, kRewardHeal, kRewardRemove, kRewardScreenText,
                kRewardSample, kRewardInfect);
    std::printf("pickup: corpus defs: shields (%d, %d) 36/36, marker (%d, %.1f, %.1f) 36/36, "
                "virus L0=%d V=%d P=%d D=%.2f 36/36\n",
                kShieldPrice, kShieldWeight, kRegenSob, kRegenNear, kRegenFar, kVirusL0,
                kVirusVolume, kVirusPriority, kVirusDecay);
}

} // namespace

int main() {
    test_armor();
    test_shields();
    test_weapons();
    test_ammo();
    test_mass();
    test_cash_secret();
    test_negatives();
    test_mass_total();
    test_determinism();
    test_corpus_numbers();

    const LevelFacts level0 = load_level_facts("level0");
    const LevelFacts dm1 = load_level_facts("dm1");
    test_data("level0", level0);
    test_data("dm1", dm1);

    if (g_failures != 0) {
        std::fprintf(stderr, "pickup: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("pickup: %d checks, ok\n", g_checks);
    return 0;
}
