// Money/shop state as a pure machine (`M6.2.6` slice 2,
// docs/ECONOMY_STATE.md).
//
// Pinned here:
//   * the shipped prices and masses (docs/ECONOMY.md §3/§12) as a catalog, and
//     their agreement with the real `level0.sbt`/`dm1.sbt` tables: equipment,
//     weapons, ammo, `DEFINE_SHIELDS (50, 0)` and the four top-level chassis;
//   * exact cash arithmetic: `buy()` debits `buy * count`, `sell()` credits
//     `sell * count`, `sell_all()` refunds every owned line and clears the
//     mass;
//   * the failed-funds rule of §4.2/§4.3: false, cash unchanged, one
//     `rejected_funds`;
//   * mass = `Σ mass * count`, the model limit (off by default, §6 says the
//     original never blocks a purchase), the exact boundary and
//     `rejected_mass`;
//   * negatives: an empty catalog, duplicate ids, negative prices/mass, an
//     unknown id, `count <= 0`, selling more than is owned, cash overflow and
//     a null `sell_all` total — every refusal with a text;
//   * determinism: two identical runs produce an identical snapshot.
//
// Real files are read through `gm::load_level` + `gm::decode_sbt_script`, the
// same seam `test_equipment_state.cpp` uses.  The test skips the file checks
// (and says so) when the installation is absent; the synthetic half always
// runs.
#include "game/economy_state.h"

#include "game/equipment.h"
#include "res/level.h"
#include "res/sbtvm.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
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

// --- data pinned by docs/ECONOMY.md §3/§12 ----------------------------------

/// Buyable `DEFINE_EQUIPMENT` records of the shipped levels (E9 price,
/// E10 weight, §3).  `SELF-DESTRUCT`, `FLIGHT PACK`, `COMBAT BLOCK`,
/// `AUTO SENTRY`, `DECOY` have `flags & 1 == 0` and are not buyable.
struct ItemDatum {
    const char* name;
    int price;
    int mass;
};

constexpr ItemDatum kEquipment[] = {
    {"HEADLIGHTS", 0, 0},        {"FORCE SHIELD", 800000, 650},
    {"MINE DETECTOR", 40000, 45}, {"TARGETING", 60000, 10},
    {"WARP DEVICE", 100000, 300}, {"RADAR", 25000, 30},
    {"MINE LAYER", 75000, 300},
};

/// `DEFINE_WEAPONS` price `+0x44` / weight `+0x48`, §3.
constexpr ItemDatum kWeapons[] = {
    {"MACHINE GUN", 0, 15},
    {"CANNON", 50000, 600},
    {"AUTOCANNON", 350000, 1300},
    {"RAILGUN", 500000, 900},
};

/// `DEFINE_AMMO` lot `+0x08`, price `+0x0c` per lot, weight `+0x10` per round
/// (§3; the shipped weight is 0 for every type).
struct AmmoDatum {
    const char* name;
    int lot;
    int price;
    int mass;
};

constexpr AmmoDatum kAmmo[] = {
    {"BULLETS", 100, 500, 0},
    {"CANNON SHELLS", 30, 4000, 0},
    {"POWER CELLS", 3000, 3000, 0},
};

/// The five chassis rows of §3/§12: base price `+0x168`, shields `+0x14c`
/// and the `0x479dd0` package `base + 4 * shields * shield_price`.
struct ChassisDatum {
    const char* name;
    int base;
    int shields;
    int mass_limit;
    int package;
};

constexpr ChassisDatum kChassis[] = {
    {"LIGHT CHASSIS", 10000, 80, 500, 26000},
    {"MEDIUM CHASSIS", 30000, 130, 1000, 56000},
    {"STEALTH CHASSIS", 350000, 90, 500, 368000},
    {"HEAVY CHASSIS", 80000, 200, 1600, 120000},
    {"57 CHEVY", 500000, 300, 9999, 560000},
};

/// The catalog the tests transact with: the seven buyable items, four
/// weapons, three ammo lots (one unit = one lot) and 100 shield points.
/// Ids are stable and sorted; buy == sell because the original reads the same
/// E9 in both directions (§4.2).
std::vector<gm::PriceEntry> shipped_catalog() {
    return {
        {1, 0, 0, 0},            // HEADLIGHTS
        {2, 800000, 800000, 650},   // FORCE SHIELD
        {3, 40000, 40000, 45},      // MINE DETECTOR
        {4, 60000, 60000, 10},      // TARGETING
        {5, 100000, 100000, 300},   // WARP DEVICE
        {6, 25000, 25000, 30},      // RADAR
        {7, 75000, 75000, 300},     // MINE LAYER
        {10, 0, 0, 15},             // MACHINE GUN
        {11, 50000, 50000, 600},    // CANNON
        {12, 350000, 350000, 1300}, // AUTOCANNON
        {13, 500000, 500000, 900},  // RAILGUN
        {20, 500, 500, 0},          // BULLETS, one lot (100)
        {21, 4000, 4000, 0},        // CANNON SHELLS, one lot (30)
        {22, 3000, 3000, 0},        // POWER CELLS, one lot (3000)
        {30, 50, 50, 0},            // 100 shield points
    };
}

constexpr int kEquipmentBuySum = 800000 + 40000 + 60000 + 100000 + 25000 + 75000;

// --- real files -------------------------------------------------------------

struct TableRow {
    std::string name;
    std::int64_t price = 0;
    std::int64_t weight = 0;
    std::int64_t lot = 0;
};

struct ChassisRow {
    std::string name;
    std::int64_t price = 0;
    std::int64_t shields = 0;
    std::int64_t mass_limit = 0;
};

struct LevelData {
    bool present = false;
    std::string error;
    std::vector<gm::EquipmentDef> equipment;
    std::vector<TableRow> weapons;
    std::vector<TableRow> ammo;
    std::vector<ChassisRow> chassis;
    int shield_price = -1;
    int shield_weight = -1;
};

std::int64_t as_int(const gm::SbtValue& value) {
    return value.kind == gm::SbtValue::Kind::Real
               ? static_cast<std::int64_t>(std::llround(value.d))
               : static_cast<std::int64_t>(value.u);
}

LevelData load_level_data(const char* name) {
    LevelData out;
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

    // Top-level tables are the single-player branch (the DEATHMATCH copy is
    // nested in an IF body and carries the same numbers, WEAPONS_CPP §2).
    for (const gm::SbtInsn& insn : program->insns) {
        if (insn.opcode == 175 && !insn.table.empty()) {
            std::string table_error;
            const auto table = gm::EquipmentTable::from_records(insn.table, &table_error);
            if (!table) {
                out.error = table_error;
                return out;
            }
            out.equipment = table->defs;
        } else if (insn.name == "DEFINE_WEAPONS") {
            for (const gm::SbtTableRecord& record : insn.table) {
                if (record.values.size() < 10 || record.strings.empty()) {
                    out.error = "DEFINE_WEAPONS record is too short";
                    return out;
                }
                TableRow row;
                row.name = record.strings[0];
                row.price = as_int(record.values[8]);  // +0x44
                row.weight = as_int(record.values[9]); // +0x48
                out.weapons.push_back(std::move(row));
            }
        } else if (insn.name == "DEFINE_AMMO") {
            for (const gm::SbtTableRecord& record : insn.table) {
                if (record.values.size() < 7 || record.strings.empty()) {
                    out.error = "DEFINE_AMMO record is too short";
                    return out;
                }
                TableRow row;
                row.name = record.strings[0];
                row.lot = as_int(record.values[4]);    // +0x08
                row.price = as_int(record.values[5]);  // +0x0c
                row.weight = as_int(record.values[6]); // +0x10
                out.ammo.push_back(std::move(row));
            }
        } else if (insn.name == "DEFINE_PLAYER_TYPES") {
            for (const gm::SbtTableRecord& record : insn.table) {
                if (record.values.size() < 11 || record.strings.empty()) {
                    out.error = "DEFINE_PLAYER_TYPES record is too short";
                    return out;
                }
                ChassisRow row;
                row.name = record.strings[0];
                row.mass_limit = as_int(record.values[0]); // +0x148
                row.shields = as_int(record.values[2]);    // +0x14c
                row.price = as_int(record.values[10]);     // +0x168
                out.chassis.push_back(std::move(row));
            }
        } else if (insn.opcode == 176 && insn.args.size() >= 2) {
            out.shield_price = static_cast<int>(as_int(insn.args[0]));
            out.shield_weight = static_cast<int>(as_int(insn.args[1]));
        }
    }
    out.present = out.error.empty();
    return out;
}

const TableRow* by_name(const std::vector<TableRow>& rows, const std::string& name) {
    for (const TableRow& row : rows) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

const gm::EquipmentDef* equipment_by_name(const std::vector<gm::EquipmentDef>& defs,
                                          const std::string& name) {
    for (const gm::EquipmentDef& def : defs) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

const ChassisRow* chassis_by_name(const std::vector<ChassisRow>& rows, const std::string& name) {
    for (const ChassisRow& row : rows) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// --- synthetic cases --------------------------------------------------------

void test_catalog() {
    gm::EconomyState state;
    std::string error;

    CHECK(!state.set_catalog({}, &error));
    CHECK(!error.empty());
    CHECK(contains(error, "empty"));
    CHECK_EQ(state.catalog().size(), std::size_t{0});
    std::printf("economy_state: catalog empty -> false err=%s\n", error.c_str());

    error.clear();
    CHECK(!state.set_catalog({{1, 10, 10, 1}, {1, 20, 20, 2}}, &error));
    CHECK(contains(error, "duplicate id 1"));
    std::printf("economy_state: catalog duplicate id 1 -> false err=%s\n", error.c_str());

    std::vector<gm::PriceEntry> bad = {{-1, 10, 10, 1}};
    error.clear();
    CHECK(!state.set_catalog(bad, &error));
    CHECK(contains(error, "negative id -1"));
    std::printf("economy_state: catalog id -1 -> false err=%s\n", error.c_str());

    bad = {{1, -5, 10, 1}};
    error.clear();
    CHECK(!state.set_catalog(bad, &error));
    CHECK(contains(error, "negative buy price for id 1"));
    std::printf("economy_state: catalog buy -5 -> false err=%s\n", error.c_str());

    bad = {{1, 10, -5, 1}};
    error.clear();
    CHECK(!state.set_catalog(bad, &error));
    CHECK(contains(error, "negative sell price for id 1"));
    std::printf("economy_state: catalog sell -5 -> false err=%s\n", error.c_str());

    bad = {{1, 10, 10, -5}};
    error.clear();
    CHECK(!state.set_catalog(bad, &error));
    CHECK(contains(error, "negative mass for id 1"));
    std::printf("economy_state: catalog mass -5 -> false err=%s\n", error.c_str());

    // A valid catalog is sorted by id and replaces the owned counts only.
    error = "stale";
    CHECK(state.set_catalog(shipped_catalog(), &error));
    CHECK(error.empty());
    CHECK(state.last_error().empty());
    CHECK_EQ(state.catalog().size(), std::size_t{15});
    for (std::size_t i = 1; i < state.catalog().size(); ++i) {
        CHECK(state.catalog()[i - 1].id < state.catalog()[i].id);
    }
    CHECK(state.find(2) != nullptr);
    CHECK_EQ(state.find(2)->buy, 800000);
    CHECK_EQ(state.find(2)->mass, 650);
    CHECK(state.find(999) == nullptr);

    // A failing reload keeps the installed catalog.
    error.clear();
    CHECK(!state.set_catalog({{1, 1, 1, 1}, {1, 2, 2, 2}}, &error));
    CHECK_EQ(state.catalog().size(), std::size_t{15});
    std::printf("economy_state: catalog 15 entries, ids sorted, failed reload kept it\n");
}

void test_money() {
    gm::EconomyState state;
    CHECK(state.set_catalog(shipped_catalog()));

    CHECK_EQ(state.cash(), 0);
    CHECK(!state.set_cash(-1));
    CHECK(contains(state.last_error(), "negative cash"));
    CHECK_EQ(state.cash(), 0);
    std::printf("economy_state: set_cash(-1) -> false err=%s\n", state.last_error().c_str());

    // saved0 holds 2000 (docs/ECONOMY.md §9).
    CHECK(state.set_cash(2000));
    CHECK_EQ(state.cash(), 2000);
    CHECK(!state.buy(2)); // FORCE SHIELD 800000
    CHECK_EQ(state.cash(), 2000);
    CHECK_EQ(state.stats().purchases, 0);
    CHECK_EQ(state.stats().rejected_funds, 1);
    CHECK(contains(state.last_error(), "not enough cash"));
    std::printf("economy_state: buy FORCE SHIELD 800000 with cash 2000 -> false, cash=2000, "
                "rejected_funds=%d err=%s\n",
                state.stats().rejected_funds, state.last_error().c_str());

    CHECK(state.set_cash(1000000));
    const int before = state.cash();
    CHECK(state.buy(2));
    CHECK_EQ(state.cash(), before - 800000);
    CHECK_EQ(state.mass(), 650);
    CHECK_EQ(state.stats().purchases, 1);
    CHECK_EQ(state.owned(2), 1);
    std::printf("economy_state: buy FORCE SHIELD x1: cash %d -> %d mass=%d purchases=%d\n",
                before, state.cash(), state.mass(), state.stats().purchases);

    const int before_two = state.cash();
    CHECK(state.buy(7, 2)); // MINE LAYER 75000 x2
    CHECK_EQ(state.cash(), before_two - 2 * 75000);
    CHECK_EQ(state.mass(), 650 + 2 * 300);
    CHECK_EQ(state.owned(7), 2);
    CHECK_EQ(state.stats().purchases, 2);
    std::printf("economy_state: buy MINE LAYER x2: cash %d -> %d mass=%d purchases=%d\n",
                before_two, state.cash(), state.mass(), state.stats().purchases);

    // Atomic failure: 3 x TARGETING (180000) does not fit in 50000.
    const int cash_now = state.cash();
    const int mass_now = state.mass();
    CHECK(!state.buy(4, 3));
    CHECK_EQ(state.cash(), cash_now);
    CHECK_EQ(state.mass(), mass_now);
    CHECK_EQ(state.owned(4), 0);
    CHECK_EQ(state.stats().rejected_funds, 2);
    CHECK(contains(state.last_error(), "need 180000"));
    std::printf("economy_state: buy TARGETING x3 (needs 180000, have %d) -> false, cash=%d "
                "rejected_funds=%d\n",
                cash_now, state.cash(), state.stats().rejected_funds);

    CHECK(!state.buy(2, 0));
    CHECK(contains(state.last_error(), "positive"));
    CHECK(!state.buy(2, -3));
    CHECK(contains(state.last_error(), "positive"));
    CHECK(!state.buy(999));
    CHECK(contains(state.last_error(), "unknown id 999"));
    CHECK(!state.sell(999));
    CHECK(contains(state.last_error(), "unknown id 999"));
    std::printf("economy_state: count 0/-3, unknown id -> false with texts\n");

    // op 114: cash += delta, clamp at 0 (§2.1).
    const int cash_before_add = state.cash();
    CHECK_EQ(state.add_cash(-1000000), 0);
    CHECK_EQ(state.cash(), 0);
    CHECK_EQ(state.add_cash(2000), 2000);
    std::printf("economy_state: add_cash -1000000 from %d -> 0, then +2000 -> %d\n",
                cash_before_add, state.cash());
}

void test_mass() {
    // Default: no limit — §6 says the original never blocks a purchase on
    // mass (the only reader of +0x3338 is the speed penalty 0x4525e7).
    gm::EconomyState shipped;
    CHECK(shipped.set_catalog(shipped_catalog()));
    CHECK(shipped.set_cash(1000000));
    CHECK_EQ(shipped.mass_limit(), gm::EconomyState::kNoMassLimit);
    CHECK(shipped.buy(11)); // CANNON, weight 600 > LIGHT limit 500
    CHECK_EQ(shipped.mass(), 600);
    CHECK_EQ(shipped.cash(), 950000);
    std::printf("economy_state: mass: no limit (shipped rule), CANNON 600 -> cash=%d mass=%d\n",
                shipped.cash(), shipped.mass());

    // Model limit: off by default, opt-in via set_mass_limit().
    gm::EconomyState limited;
    CHECK(limited.set_catalog(shipped_catalog()));
    CHECK(limited.set_cash(1000000));
    CHECK(limited.set_mass_limit(500));
    CHECK(limited.buy(10)); // MACHINE GUN, 15
    CHECK_EQ(limited.mass(), 15);
    const int cash_limited = limited.cash();
    CHECK(!limited.buy(11)); // 15 + 600 > 500
    CHECK_EQ(limited.cash(), cash_limited);
    CHECK_EQ(limited.mass(), 15);
    CHECK_EQ(limited.owned(11), 0);
    CHECK_EQ(limited.stats().rejected_mass, 1);
    CHECK(contains(limited.last_error(), "mass limit 500"));
    std::printf("economy_state: mass: limit 500, MG 15 + CANNON 600 -> false, cash=%d mass=%d "
                "rejected_mass=%d err=%s\n",
                limited.cash(), limited.mass(), limited.stats().rejected_mass,
                limited.last_error().c_str());

    // Exact boundary: mass == limit is allowed; one more unit is not.
    CHECK(limited.set_mass_limit(615)); // saved0 mass = 615 at limit 500 (§6)
    CHECK(limited.buy(11));
    CHECK_EQ(limited.mass(), 615);
    CHECK(!limited.buy(11));
    CHECK_EQ(limited.mass(), 615);
    CHECK_EQ(limited.stats().rejected_mass, 2);
    std::printf("economy_state: mass: limit 615, %d == boundary ok, next CANNON -> false "
                "mass=%d rejected_mass=%d\n",
                limited.mass(), limited.mass(), limited.stats().rejected_mass);

    CHECK(limited.set_mass_limit(gm::EconomyState::kNoMassLimit));
    CHECK(limited.buy(11));
    CHECK_EQ(limited.mass(), 1215);
    CHECK(!limited.set_mass_limit(-2));
    CHECK(contains(limited.last_error(), "below -1"));
    std::printf("economy_state: mass: limit off -> CANNON ok, mass=%d; set -2 -> false err=%s\n",
                limited.mass(), limited.last_error().c_str());
}

void test_sell() {
    gm::EconomyState state;
    CHECK(state.set_catalog(shipped_catalog()));
    CHECK(state.set_cash(1000000));
    CHECK(state.buy(2)); // FORCE SHIELD
    CHECK(state.sell(2));
    CHECK_EQ(state.cash(), 1000000);
    CHECK_EQ(state.mass(), 0);
    CHECK_EQ(state.owned(2), 0);
    CHECK_EQ(state.stats().sales, 1);
    std::printf("economy_state: sell FORCE SHIELD x1: cash 200000 -> %d mass=%d sales=%d\n",
                state.cash(), state.mass(), state.stats().sales);

    CHECK(!state.sell(2));
    CHECK(contains(state.last_error(), "owned 0, requested 1"));
    CHECK(!state.sell(2, 0));
    CHECK(contains(state.last_error(), "positive"));
    std::printf("economy_state: sell not owned / count 0 -> false with texts\n");

    // Count and the same-price rule: 2 x MINE LAYER out, 1 back.
    CHECK(state.set_cash(200000));
    CHECK(state.buy(7, 2));
    CHECK_EQ(state.cash(), 50000);
    CHECK(state.sell(7, 1));
    CHECK_EQ(state.cash(), 125000);
    CHECK_EQ(state.mass(), 300);
    CHECK_EQ(state.owned(7), 1);
    CHECK_EQ(state.stats().sales, 2);
    const int cash_one_left = state.cash();
    CHECK(!state.sell(7, 2));
    CHECK_EQ(state.cash(), cash_one_left);
    CHECK_EQ(state.owned(7), 1);
    std::printf("economy_state: sell MINE LAYER 1 of 2: cash %d -> %d mass=%d owned=%d; "
                "sell 2 -> false\n",
                50000, state.cash(), state.mass(), state.owned(7));

    // sell_all: RADAR (25000) + 2 x MINE LAYER (150000) = 175000, mass back
    // to 0, one sales counter per cleared line.
    gm::EconomyState all;
    CHECK(all.set_catalog(shipped_catalog()));
    CHECK(all.set_cash(175000));
    CHECK(all.buy(6));
    CHECK(all.buy(7, 2));
    CHECK_EQ(all.cash(), 0);
    CHECK_EQ(all.mass(), 630);
    int total = 0;
    const int sales_before = all.stats().sales;
    CHECK(all.sell_all(&total));
    CHECK_EQ(total, 175000);
    CHECK_EQ(all.cash(), 175000);
    CHECK_EQ(all.mass(), 0);
    CHECK_EQ(all.owned(6), 0);
    CHECK_EQ(all.owned(7), 0);
    CHECK_EQ(all.stats().sales, sales_before + 2);
    std::printf("economy_state: sell_all: mass 630 -> 0, total=%d, cash 0 -> %d, sales=%d\n",
                total, all.cash(), all.stats().sales);

    CHECK_EQ(all.sell_all(), 0);
    CHECK_EQ(all.cash(), 175000);
    CHECK_EQ(all.stats().sales, sales_before + 2);
    CHECK(!all.sell_all(nullptr));
    CHECK(contains(all.last_error(), "null total"));
    std::printf("economy_state: sell_all on an empty inventory -> 0, null total -> false err=%s\n",
                all.last_error().c_str());

    // Cash overflow is refused before the state moves.
    gm::EconomyState overflow;
    CHECK(overflow.set_catalog(shipped_catalog()));
    CHECK(overflow.set_cash(std::numeric_limits<int>::max()));
    CHECK(overflow.buy(2));
    CHECK(overflow.set_cash(std::numeric_limits<int>::max()));
    const int max_cash = overflow.cash();
    CHECK(!overflow.sell(2));
    CHECK_EQ(overflow.cash(), max_cash);
    CHECK_EQ(overflow.owned(2), 1);
    CHECK(contains(overflow.last_error(), "cash overflow"));
    std::printf("economy_state: sell with cash INT_MAX -> false err=%s, owned=%d\n",
                overflow.last_error().c_str(), overflow.owned(2));
}

void test_empty() {
    gm::EconomyState empty;
    CHECK_EQ(empty.cash(), 0);
    CHECK_EQ(empty.mass(), 0);
    CHECK(!empty.buy(1));
    CHECK(contains(empty.last_error(), "unknown id 1"));
    CHECK(!empty.sell(1));
    CHECK(contains(empty.last_error(), "unknown id 1"));
    CHECK_EQ(empty.sell_all(), 0);
    CHECK_EQ(empty.owned(1), 0);
    CHECK_EQ(empty.stats().purchases, 0);
    CHECK_EQ(empty.stats().sales, 0);
    CHECK_EQ(empty.stats().rejected_funds, 0);
    CHECK_EQ(empty.stats().rejected_mass, 0);
    std::printf("economy_state: no catalog: buy/sell -> false, sell_all=%d, cash=%d\n",
                empty.sell_all(), empty.cash());
}

std::string snapshot(const gm::EconomyState& state) {
    std::string out = std::to_string(state.cash()) + "|" + std::to_string(state.mass()) + "|" +
                      std::to_string(state.mass_limit());
    for (const gm::PriceEntry& entry : state.catalog()) {
        out += "," + std::to_string(entry.id) + ":" + std::to_string(state.owned(entry.id));
    }
    const gm::EconomyStats& stats = state.stats();
    out += "|" + std::to_string(stats.purchases) + "/" + std::to_string(stats.sales) + "/" +
           std::to_string(stats.rejected_funds) + "/" + std::to_string(stats.rejected_mass) +
           "|" + state.last_error();
    return out;
}

std::string run_sequence() {
    gm::EconomyState state;
    CHECK(state.set_catalog(shipped_catalog()));
    CHECK(state.set_cash(1000000));
    CHECK(state.set_mass_limit(2000));
    CHECK(state.buy(2));
    CHECK(state.buy(7, 2));
    CHECK(!state.buy(13)); // RAILGUN 500000 with 150000 left
    CHECK(state.sell(7, 1));
    int total = 0;
    CHECK(state.sell_all(&total));
    return snapshot(state);
}

void test_determinism() {
    const std::string first = run_sequence();
    const std::string second = run_sequence();
    CHECK(first == second);
    CHECK(!first.empty());
    std::printf("economy_state: determinism: two runs %s (%zu bytes)\n",
                first == second ? "identical" : "DIFFER", first.size());
}

void test_data(const char* name, const LevelData& level) {
    if (!level.present) {
        std::printf("economy_state: %s not installed, file checks skipped (%s)\n", name,
                    level.error.c_str());
        return;
    }

    // Table sizes of §12 and the corpus constants.
    CHECK_EQ(level.shield_price, 50);
    CHECK_EQ(level.shield_weight, 0);
    CHECK_EQ(level.weapons.size(), std::size_t{19});
    CHECK_EQ(level.ammo.size(), std::size_t{15});
    CHECK_EQ(level.chassis.size(), std::size_t{4});

    int equipment_price_sum = 0;
    int equipment_mass_sum = 0;
    int buyable_mass_sum = 0;
    for (const gm::EquipmentDef& def : level.equipment) {
        equipment_mass_sum += def.weight();
        if ((def.flags() & 0x01) != 0) {
            equipment_price_sum += def.price();
            buyable_mass_sum += def.weight();
        }
    }
    CHECK_EQ(equipment_price_sum, kEquipmentBuySum);
    CHECK_EQ(equipment_price_sum, 1100000); // §12, Σ «купить»
    CHECK_EQ(equipment_mass_sum, std::string(name) == "level0" ? 2185 : 1335);
    std::printf("economy_state: %s equipment=%zu buy_sum=%d all_mass=%d buy_mass=%d\n", name,
                level.equipment.size(), equipment_price_sum, equipment_mass_sum,
                buyable_mass_sum);

    // Every pinned equipment price/weight must exist in the file.
    int equipment_ok = 0;
    for (const ItemDatum& item : kEquipment) {
        const gm::EquipmentDef* def = equipment_by_name(level.equipment, item.name);
        CHECK(def != nullptr);
        if (def == nullptr) {
            continue;
        }
        CHECK_EQ(def->price(), item.price);
        CHECK_EQ(def->weight(), item.mass);
        ++equipment_ok;
    }
    CHECK_EQ(equipment_ok, static_cast<int>(std::size(kEquipment)));

    int weapons_ok = 0;
    for (const ItemDatum& item : kWeapons) {
        const TableRow* row = by_name(level.weapons, item.name);
        CHECK(row != nullptr);
        if (row == nullptr) {
            continue;
        }
        CHECK_EQ(static_cast<int>(row->price), item.price);
        CHECK_EQ(static_cast<int>(row->weight), item.mass);
        ++weapons_ok;
    }
    CHECK_EQ(weapons_ok, static_cast<int>(std::size(kWeapons)));

    int ammo_ok = 0;
    for (const AmmoDatum& item : kAmmo) {
        const TableRow* row = by_name(level.ammo, item.name);
        CHECK(row != nullptr);
        if (row == nullptr) {
            continue;
        }
        CHECK_EQ(static_cast<int>(row->lot), item.lot);
        CHECK_EQ(static_cast<int>(row->price), item.price);
        CHECK_EQ(static_cast<int>(row->weight), item.mass);
        ++ammo_ok;
    }
    CHECK_EQ(ammo_ok, static_cast<int>(std::size(kAmmo)));

    // The four top-level chassis rows (57 CHEVY is in another branch).
    int chassis_ok = 0;
    for (const ChassisDatum& item : kChassis) {
        const ChassisRow* row = chassis_by_name(level.chassis, item.name);
        if (row == nullptr) {
            continue;
        }
        CHECK_EQ(static_cast<int>(row->price), item.base);
        CHECK_EQ(static_cast<int>(row->shields), item.shields);
        CHECK_EQ(static_cast<int>(row->mass_limit), item.mass_limit);
        ++chassis_ok;
    }
    CHECK_EQ(chassis_ok, 4);

    std::printf("economy_state: %s matched: equipment %d/%zu weapons %d/%zu ammo %d/%zu "
                "chassis %d/4\n",
                name, equipment_ok, std::size(kEquipment), weapons_ok, std::size(kWeapons),
                ammo_ok, std::size(kAmmo), chassis_ok);
}

void test_prices() {
    // §3: shields 50/0, the `0x479dd0` package formula and the five chassis
    // rows of §12.  The catalog entries are the same numbers.
    const std::vector<gm::PriceEntry> catalog = shipped_catalog();
    int item_buy_sum = 0;
    for (const ItemDatum& item : kEquipment) {
        item_buy_sum += item.price;
    }
    CHECK_EQ(item_buy_sum, 1100000);
    CHECK_EQ(kEquipment[1].price, 800000); // FORCE SHIELD
    CHECK_EQ(kEquipment[6].price, 75000);  // MINE LAYER
    CHECK_EQ(kEquipment[0].price, 0);      // HEADLIGHTS

    int ids = 0;
    for (const gm::PriceEntry& entry : catalog) {
        if (entry.id < 10) {
            ++ids;
        }
        CHECK_EQ(entry.buy, entry.sell); // the same E9 both ways (§4.2)
    }
    CHECK_EQ(ids, static_cast<int>(std::size(kEquipment)));

    for (const ChassisDatum& chassis : kChassis) {
        CHECK_EQ(chassis.base + 4 * chassis.shields * 50, chassis.package);
    }
    CHECK_EQ(kChassis[0].package, 26000);  // LIGHT
    CHECK_EQ(kChassis[1].package, 56000);  // MEDIUM
    CHECK_EQ(kChassis[2].package, 368000); // STEALTH
    CHECK_EQ(kChassis[3].package, 120000); // HEAVY
    CHECK_EQ(kChassis[4].package, 560000); // 57 CHEVY

    std::printf("economy_state: prices: %zu catalog lines, item buy_sum=%d, shield 50/100, "
                "packages 26000/56000/368000/120000/560000\n",
                catalog.size(), item_buy_sum);
}

} // namespace

int main() {
    test_catalog();
    test_prices();
    test_money();
    test_mass();
    test_sell();
    test_empty();
    test_determinism();

    const LevelData level0 = load_level_data("level0");
    const LevelData dm1 = load_level_data("dm1");
    test_data("level0", level0);
    test_data("dm1", dm1);

    if (g_failures != 0) {
        std::fprintf(stderr, "economy_state: %d failure(s) in %d checks\n", g_failures,
                     g_checks);
        return 1;
    }
    std::printf("economy_state: %d checks, ok\n", g_checks);
    return 0;
}
