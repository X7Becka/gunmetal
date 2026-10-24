// Activation and ammo drain of the level's equipment records
// (`M6.2.4` slice 2, docs/EQUIPMENT_ACT.md).
//
// Pinned here:
//   * the data fact (docs/EQUIPMENT.md §0/§3): 36 shipped levels, 434
//     `DEFINE_EQUIPMENT` records, 12 per level plus `BALLAST` only in
//     `level0`/`level20`; the 13 names, `flags`, `ammo_per_tick` (E7),
//     `ammo_type` (E8) and `object_id` (E11) of every record;
//   * op 130 (§4.1): `activate()` sets `1 << id`, is idempotent, and does not
//     consult ammo (the guard is the level sub-script's);
//   * the tick `0x4463a9` (§2.1): `ammo[E8] -= E7 * ticks` clamped at 0, one
//     `no_ammo` event per activation→empty transition, the active bit stays,
//     `deactivate()` stops the drain, and two records with different
//     `ammo_type` spend their own pools;
//   * negatives: duplicate/missing/out-of-range id, negative
//     `ammo_per_tick`, `ammo_type` outside -1..15, empty/oversized table,
//     negative `ticks`, null ammo table — every refusal has a text;
//   * two identical runs produce identical masks, pools and event logs.
//
// Real files are read through `gm::load_level` + `gm::decode_sbt_script`; the
// corpus totals read the `.sbt` files directly through `gm::parse_sbt`.
#include "game/equipment_act.h"

#include "game/equipment.h"
#include "res/level.h"
#include "res/sbt.h"
#include "res/sbtvm.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

const fs::path kGame = "/mnt/d/games/Gunmetal 3dfx";

using gm::equip::EquipmentAct;
using gm::equip::EquipmentEvent;
using gm::equip::EquipmentRecord;

constexpr int kTypes = EquipmentAct::kAmmoTypes;

/// §3 of docs/EQUIPMENT.md: the 13 shipped names in table order.
const char* const kNames[13] = {
    "HEADLIGHTS",  "FORCE SHIELD", "MINE DETECTOR", "TARGETING",   "SELF-DESTRUCT",
    "WARP DEVICE", "RADAR",        "FLIGHT PACK",   "MINE LAYER",  "COMBAT BLOCK",
    "AUTO SENTRY", "DECOY",        "BALLAST",
};
/// E0 flags, E7 ammo_per_tick, E8 ammo_type, E11 object_id of `level0` (§3).
constexpr std::uint32_t kFlags[13] = {0x03, 0x03, 0x03, 0x03, 0x00, 0x03, 0x03,
                                      0x08, 0x03, 0x89, 0x89, 0x89, 0x0c};
constexpr int kPerTick[13] = {1, 10, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0};
constexpr int kAmmoType[13] = {10, 10, -1, -1, -1, -1, -1, -1, 11, 12, 13, 14, 0};
constexpr int kObjectId[13] = {1, 2, 3, 4, 0, 5, 6, 0, 7, 8, 9, 10, 0};
constexpr int kPrice[13] = {0, 800000, 40000, 60000, 0, 100000, 25000,
                            0, 75000,  0,     0,     0, 0};
constexpr int kWeight[13] = {0, 650, 45, 10, 0, 300, 30, 0, 300, 0, 0, 0, 850};

struct LevelEquip {
    bool present = false;
    std::string error;
    std::vector<gm::EquipmentDef> defs;
};

void find_equipment(const std::vector<gm::SbtInsn>& insns, LevelEquip& out) {
    for (const gm::SbtInsn& insn : insns) {
        if (insn.opcode == 175 && !insn.table.empty()) {
            std::string error;
            const auto table = gm::EquipmentTable::from_records(insn.table, &error);
            if (!table) {
                out.error = error;
                return;
            }
            out.defs = table->defs;
        }
        if (!insn.body.empty()) {
            find_equipment(insn.body, out);
        }
    }
}

/// Equipment of one level through the same reader the engine uses.
LevelEquip level_equipment(const char* name) {
    LevelEquip out;
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
    find_equipment(program->insns, out);
    out.present = out.error.empty();
    return out;
}

/// Equipment of one `.sbt` read straight from disk (the corpus totals).
LevelEquip sbt_equipment(const fs::path& path) {
    LevelEquip out;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        out.error = "cannot open " + path.string();
        return out;
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};
    std::string error;
    const auto level = gm::parse_sbt(bytes, error);
    if (!level) {
        out.error = error;
        return out;
    }
    const auto program = gm::decode_sbt_script(level->script_code, error);
    if (!program) {
        out.error = error;
        return out;
    }
    find_equipment(program->insns, out);
    out.present = out.error.empty();
    return out;
}

/// The caller-side conversion this module expects (`docs/EQUIPMENT_ACT.md` §3):
/// `id` is the record index, the rest is copied from the level data.
std::vector<EquipmentRecord> to_records(const std::vector<gm::EquipmentDef>& defs) {
    std::vector<EquipmentRecord> records;
    records.reserve(defs.size());
    for (std::size_t n = 0; n < defs.size(); ++n) {
        const gm::EquipmentDef& def = defs[n];
        EquipmentRecord record;
        record.id = static_cast<int>(n);
        record.name = def.name;
        record.flags = static_cast<std::uint32_t>(def.flags());
        record.ammo_type = def.ammo_type();
        record.ammo_per_tick = def.ammo_per_tick();
        record.price = def.price();
        record.weight = def.weight();
        record.object_id = def.object_id();
        records.push_back(std::move(record));
    }
    return records;
}

std::vector<EquipmentRecord> level_records(const char* name) {
    const LevelEquip level = level_equipment(name);
    if (!level.present) {
        std::fprintf(stderr, "equipment_act: %s not readable: %s\n", name,
                     level.error.c_str());
        return {};
    }
    return to_records(level.defs);
}

int count_kind(const EquipmentAct& act, EquipmentEvent::Kind kind, int id = -1) {
    int count = 0;
    for (const EquipmentEvent& event : act.events()) {
        if (event.kind == kind && (id < 0 || event.id == id)) {
            ++count;
        }
    }
    return count;
}

/// Fresh 16-slot ammo pool with one value written (the rest stays 0).
void pool(int* ammo, int type, int value) {
    for (int i = 0; i < kTypes; ++i) {
        ammo[i] = 0;
    }
    ammo[type] = value;
}

// --- data ------------------------------------------------------------------

void test_corpus() {
    int files = 0;
    int total = 0;
    int names_seen = 0;
    bool names[13] = {};
    int ballast_files = 0;
    std::string ballast_names;

    for (const fs::directory_entry& entry : fs::directory_iterator(kGame)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sbt") {
            continue;
        }
        const LevelEquip level = sbt_equipment(entry.path());
        CHECK(level.present);
        if (!level.present) {
            std::fprintf(stderr, "equipment_act: %s: %s\n", entry.path().c_str(),
                         level.error.c_str());
            continue;
        }
        ++files;
        total += static_cast<int>(level.defs.size());
        CHECK(level.defs.size() == 12 || level.defs.size() == 13);
        bool has_ballast = false;
        for (const gm::EquipmentDef& def : level.defs) {
            for (int n = 0; n < 13; ++n) {
                if (def.name == kNames[n] && !names[n]) {
                    names[n] = true;
                    ++names_seen;
                }
            }
            if (def.name == "BALLAST") {
                has_ballast = true;
            }
        }
        if (has_ballast) {
            ++ballast_files;
            ballast_names += " " + entry.path().stem().string();
            CHECK(entry.path().stem() == "level0" || entry.path().stem() == "level20");
        }
    }

    std::printf("equipment_act: corpus files=%d records=%d names=%d ballast_in=%s\n", files,
                total, names_seen, ballast_names.c_str());
    CHECK_EQ(files, 36);
    CHECK_EQ(total, 434);
    CHECK_EQ(names_seen, 13);
    CHECK_EQ(ballast_files, 2);
}

void test_levels(const std::vector<EquipmentRecord>& level0,
                 const std::vector<EquipmentRecord>& level20,
                 const std::vector<EquipmentRecord>& dm1) {
    CHECK_EQ(level0.size(), std::size_t{13});
    CHECK_EQ(level20.size(), std::size_t{13});
    CHECK_EQ(dm1.size(), std::size_t{12});

    std::printf("equipment_act: records level0=%zu level20=%zu dm1=%zu\n", level0.size(),
                level20.size(), dm1.size());
    for (std::size_t n = 0; n < level0.size(); ++n) {
        const EquipmentRecord& r = level0[n];
        std::printf("equipment_act:   %2d %-13s flags=0x%02x E7=%2d E8=%2d obj=%2d "
                    "price=%6d weight=%4d\n",
                    r.id, r.name.c_str(), r.flags, r.ammo_per_tick, r.ammo_type, r.object_id,
                    r.price, r.weight);
        CHECK_EQ(r.id, static_cast<int>(n));
        CHECK_EQ(r.name, std::string(kNames[n]));
        CHECK_EQ(r.flags, kFlags[n]);
        CHECK_EQ(r.ammo_per_tick, kPerTick[n]);
        CHECK_EQ(r.ammo_type, kAmmoType[n]);
        CHECK_EQ(r.object_id, kObjectId[n]);
        CHECK_EQ(r.price, kPrice[n]);
        if (n < 12) {
            // BALLAST is the only record whose weight differs between the two
            // files that carry it (docs/EQUIPMENT_ACT.md §2).
            CHECK_EQ(r.weight, kWeight[n]);
        }
    }

    // `BALLAST`: only `level0`/`level20`, flags 0x0c ("out of the cycle" +
    // "hidden" — §3), no ammo pool, and the one data difference between the
    // two files: weight 850 vs 2000.
    if (level0.size() == 13 && level20.size() == 13) {
        const EquipmentRecord& a = level0[12];
        const EquipmentRecord& b = level20[12];
        CHECK_EQ(a.name, std::string("BALLAST"));
        CHECK_EQ(b.name, std::string("BALLAST"));
        CHECK_EQ(a.flags, 0x0cu);
        CHECK_EQ(b.flags, 0x0cu);
        CHECK_EQ(a.ammo_per_tick, 0);
        CHECK_EQ(b.ammo_per_tick, 0);
        CHECK_EQ(a.weight, 850);
        CHECK_EQ(b.weight, 2000);
        std::printf("equipment_act: BALLAST weight level0=%d level20=%d\n", a.weight, b.weight);
    }
    // dm1 has no BALLAST and ends at DECOY.
    if (dm1.size() == 12) {
        CHECK_EQ(dm1[11].name, std::string("DECOY"));
        for (const EquipmentRecord& r : dm1) {
            CHECK(r.name != "BALLAST");
        }
    }

    // All three tables are accepted by the module.
    for (const auto* table : {&level0, &level20, &dm1}) {
        EquipmentAct act;
        std::string error;
        CHECK(act.set_records(*table, &error));
        CHECK(error.empty());
        CHECK_EQ(act.count(), static_cast<int>(table->size()));
        CHECK(act.has_records());
    }
}

// --- activation (op 130) ---------------------------------------------------

void test_activation(const std::vector<EquipmentRecord>& level0) {
    EquipmentAct act;
    CHECK(act.set_records(level0));
    CHECK_EQ(act.active_mask(), 0u);

    for (int id = 0; id < 12; ++id) {
        const EquipmentRecord* item = act.record(id);
        CHECK(item != nullptr);
        act.clear_events();
        CHECK(act.activate(id));
        CHECK(act.last_error().empty());
        CHECK(act.is_active(id));
        CHECK_EQ(act.active_mask(), 1u << id);
        CHECK_EQ(count_kind(act, EquipmentEvent::activated, id), 1);

        // Repeat: op 130 is `active |= 1 << bit` (§4.1) — no-op, no event.
        CHECK(act.activate(id));
        CHECK_EQ(act.active_mask(), 1u << id);
        CHECK_EQ(act.events().size(), std::size_t{1});

        CHECK(act.deactivate(id));
        CHECK(!act.is_active(id));
        CHECK_EQ(act.active_mask(), 0u);
        CHECK_EQ(count_kind(act, EquipmentEvent::deactivated, id), 1);
        std::printf("equipment_act: id=%2d %-13s mask=0x%08x active=1 repeat=noop "
                    "events=%zu\n",
                    id, item == nullptr ? "?" : item->name.c_str(), 1u << id, act.events().size());
    }

    // All 12 at once: bits 0..11, the mask the shipped scripts never fully
    // reach (they only call bits 0..7) but op 130 would accept (§4.1).
    act.clear_events();
    for (int id = 0; id < 12; ++id) {
        CHECK(act.activate(id));
    }
    CHECK_EQ(act.active_mask(), 0x0fffu);
    CHECK_EQ(act.events().size(), std::size_t{12});
    std::printf("equipment_act: all 12 active mask=0x%08x events=%zu BALLAST(id 12) "
                "not activated (flags 0x%02x)\n",
                act.active_mask(), act.events().size(), level0[12].flags);
    CHECK(!act.is_active(12));
    CHECK(!act.is_active(99));
    CHECK(!act.is_active(-1));
}

// --- tick: E7 * ticks from ammo[E8] ----------------------------------------

void test_spend(const std::vector<EquipmentRecord>& level0) {
    EquipmentAct act;
    CHECK(act.set_records(level0));

    int ammo[kTypes];
    pool(ammo, 10, 100);

    // HEADLIGHTS: E7 = 1, E8 = 10 (§3).
    act.clear_events();
    CHECK(act.activate(0));
    act.tick(10, ammo);
    CHECK_EQ(ammo[10], 90);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo), 0);
    std::printf("equipment_act: HEADLIGHTS E7=1 ammo10=100 tick(10) -> %d events=%zu\n",
                ammo[10], act.events().size());

    // Zero ticks spend nothing.
    act.tick(0, ammo);
    CHECK_EQ(ammo[10], 90);

    // Deactivation stops the drain.
    CHECK(act.deactivate(0));
    act.tick(10, ammo);
    CHECK_EQ(ammo[10], 90);
    std::printf("equipment_act: HEADLIGHTS deactivate + tick(10) -> ammo10=%d\n", ammo[10]);

    // Boundary: 5 units, 10 ticks -> 0 and exactly one E6, not one per tick.
    ammo[10] = 5;
    act.clear_events();
    CHECK(act.activate(0));
    act.tick(10, ammo);
    CHECK_EQ(ammo[10], 0);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 0), 1);
    CHECK(act.is_active(0)); // §2.1: the bit stays set
    CHECK(act.is_exhausted(0));
    std::printf("equipment_act: HEADLIGHTS ammo10=5 tick(10) -> %d no_ammo=%d active=%d\n",
                ammo[10], count_kind(act, EquipmentEvent::no_ammo, 0),
                act.is_active(0) ? 1 : 0);

    // Further ticks while drained: pool stays 0, no second event (MODEL).
    act.tick(7, ammo);
    CHECK_EQ(ammo[10], 0);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 0), 1);

    // Re-activating an already-active drained item is the proven op-130 no-op:
    // no bit change, no event; the latch is not cleared.
    act.clear_events();
    CHECK(act.activate(0));
    CHECK(act.is_active(0));
    CHECK_EQ(act.events().size(), std::size_t{0});
    act.tick(1, ammo);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 0), 0);
    std::printf("equipment_act: re-activate at 0 -> no event, tick(1) no_ammo=%d\n",
                count_kind(act, EquipmentEvent::no_ammo, 0));

    // A fresh activation clears the latch, so the drained pool reports E6 once
    // more — the model's only deliberate deviation from "E6 every tick" (§2.1).
    CHECK(act.deactivate(0));
    act.clear_events();
    CHECK(act.activate(0));
    act.tick(1, ammo);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 0), 1);
    std::printf("equipment_act: off/on at 0 + tick(1) -> no_ammo=%d (once per "
                "activation)\n",
                count_kind(act, EquipmentEvent::no_ammo, 0));

    // FORCE SHIELD: E7 = 10, E8 = 10 (§3): 95 - 10*3 = 65.
    EquipmentAct shield;
    CHECK(shield.set_records(level0));
    int pool_b[kTypes];
    pool(pool_b, 10, 95);
    CHECK(shield.activate(1));
    shield.tick(3, pool_b);
    CHECK_EQ(pool_b[10], 65);
    std::printf("equipment_act: FORCE SHIELD E7=10 ammo10=95 tick(3) -> %d\n", pool_b[10]);
}

void test_two_types(const std::vector<EquipmentRecord>& level0) {
    EquipmentAct act;
    CHECK(act.set_records(level0));

    int ammo[kTypes];
    for (int i = 0; i < kTypes; ++i) {
        ammo[i] = 0;
    }
    ammo[10] = 7; // sentinel: no active record uses type 10
    ammo[11] = 10; // MINE LAYER
    ammo[12] = 20; // COMBAT BLOCK

    act.clear_events();
    CHECK(act.activate(8)); // MINE LAYER, E7 = 1, E8 = 11
    CHECK(act.activate(9)); // COMBAT BLOCK, E7 = 1, E8 = 12
    CHECK_EQ(act.active_mask(), (1u << 8) | (1u << 9));
    act.tick(3, ammo);
    CHECK_EQ(ammo[11], 7);
    CHECK_EQ(ammo[12], 17);
    CHECK_EQ(ammo[10], 7);
    std::printf("equipment_act: MINE LAYER+COMBAT BLOCK tick(3) -> ammo11=%d ammo12=%d "
                "ammo10=%d\n",
                ammo[11], ammo[12], ammo[10]);

    act.tick(7, ammo);
    CHECK_EQ(ammo[11], 0);
    CHECK_EQ(ammo[12], 10);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 8), 1);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 9), 0);

    act.tick(10, ammo);
    CHECK_EQ(ammo[12], 0);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 8), 1);
    CHECK_EQ(count_kind(act, EquipmentEvent::no_ammo, 9), 1);
    // No other pool was touched.
    for (int type = 0; type < kTypes; ++type) {
        if (type != 11 && type != 12) {
            CHECK_EQ(ammo[type], type == 10 ? 7 : 0);
        }
    }
    std::printf("equipment_act: tick(7)+tick(10) -> ammo11=%d ammo12=%d no_ammo8=%d "
                "no_ammo9=%d\n",
                ammo[11], ammo[12], count_kind(act, EquipmentEvent::no_ammo, 8),
                count_kind(act, EquipmentEvent::no_ammo, 9));

    // Deactivating both stops all spending.
    CHECK(act.deactivate(8));
    CHECK(act.deactivate(9));
    CHECK_EQ(act.active_mask(), 0u);
    ammo[11] = 5;
    ammo[12] = 6;
    act.tick(4, ammo);
    CHECK_EQ(ammo[11], 5);
    CHECK_EQ(ammo[12], 6);
    std::printf("equipment_act: both off + tick(4) -> ammo11=%d ammo12=%d\n", ammo[11],
                ammo[12]);
}

// --- determinism -----------------------------------------------------------

struct Run {
    std::uint32_t mask = 0;
    int ammo[kTypes] = {};
    std::vector<std::pair<int, int>> events;
};

Run run_script(const std::vector<EquipmentRecord>& level0) {
    EquipmentAct act;
    CHECK(act.set_records(level0));
    Run run;
    for (int i = 0; i < kTypes; ++i) {
        run.ammo[i] = 0;
    }
    run.ammo[10] = 20;
    run.ammo[11] = 5;
    run.ammo[13] = 4;
    for (int id : {0, 1, 8, 10}) {
        CHECK(act.activate(id));
    }
    act.tick(3, run.ammo);
    CHECK(act.deactivate(1));
    act.tick(4, run.ammo);
    CHECK(act.activate(1));
    act.tick(2, run.ammo);
    // `tick()` writes into `run.ammo` directly, so the pool is already final.
    run.mask = act.active_mask();
    for (const EquipmentEvent& event : act.events()) {
        run.events.emplace_back(event.id, static_cast<int>(event.kind));
    }
    return run;
}

void test_determinism(const std::vector<EquipmentRecord>& level0) {
    const Run a = run_script(level0);
    const Run b = run_script(level0);
    CHECK_EQ(a.mask, b.mask);
    CHECK_EQ(a.events.size(), b.events.size());
    for (int i = 0; i < kTypes; ++i) {
        CHECK_EQ(a.ammo[i], b.ammo[i]);
    }
    CHECK(a.events == b.events);
    std::printf("equipment_act: determinism mask=0x%08x events=%zu ammo10=%d ammo11=%d "
                "ammo13=%d\n",
                a.mask, a.events.size(), a.ammo[10], a.ammo[11], a.ammo[13]);
}

// --- negatives -------------------------------------------------------------

void expect_refused(const std::vector<EquipmentRecord>& records, const char* needle,
                    const char* label) {
    EquipmentAct act;
    std::string error;
    const bool ok = act.set_records(records, &error);
    CHECK(!ok);
    CHECK(!error.empty());
    CHECK(error.find(needle) != std::string::npos);
    CHECK(!act.has_records());
    CHECK_EQ(act.count(), 0);
    CHECK_EQ(act.active_mask(), 0u);
    std::printf("equipment_act: %s -> refused: %s\n", label, error.c_str());
}

void test_negatives(const std::vector<EquipmentRecord>& level0) {
    EquipmentAct act;
    CHECK(act.set_records(level0));
    CHECK(act.last_error().empty());

    // Unknown ids: activate/deactivate refuse with a text, is_active is false.
    CHECK(!act.activate(13));
    CHECK(act.last_error().find("not in the records") != std::string::npos);
    std::printf("equipment_act: activate(13) -> %s\n", act.last_error().c_str());
    CHECK(!act.deactivate(-1));
    CHECK(act.last_error().find("not in the records") != std::string::npos);
    CHECK(!act.activate(99));
    std::printf("equipment_act: activate(99) -> %s\n", act.last_error().c_str());

    // Negative ticks and a null pool: refused, nothing changes.
    int ammo[kTypes];
    pool(ammo, 10, 9);
    CHECK(act.activate(0));
    act.tick(-1, ammo);
    CHECK(act.last_error().find("ticks") != std::string::npos);
    CHECK_EQ(ammo[10], 9);
    std::printf("equipment_act: tick(-1) -> %s\n", act.last_error().c_str());
    act.tick(1, nullptr);
    CHECK(act.last_error().find("null ammo") != std::string::npos);
    CHECK_EQ(ammo[10], 9);
    std::printf("equipment_act: tick(null) -> %s\n", act.last_error().c_str());

    // Duplicate id.
    std::vector<EquipmentRecord> dup = level0;
    dup[1].id = dup[0].id;
    expect_refused(dup, "duplicate id 0", "duplicate id 0");

    // Negative ammo_per_tick.
    std::vector<EquipmentRecord> negative = level0;
    negative[0].ammo_per_tick = -1;
    expect_refused(negative, "ammo_per_tick -1", "ammo_per_tick -1");

    // ammo_type outside -1..15.
    std::vector<EquipmentRecord> bad_type = level0;
    bad_type[4].ammo_type = 16;
    expect_refused(bad_type, "ammo_type 16", "ammo_type 16");
    std::vector<EquipmentRecord> bad_low = level0;
    bad_low[4].ammo_type = -2;
    expect_refused(bad_low, "ammo_type -2", "ammo_type -2");

    // id outside 0..30.
    std::vector<EquipmentRecord> bad_id = level0;
    bad_id[0].id = 31;
    expect_refused(bad_id, "id outside 0..30", "id 31");

    // Empty table.
    expect_refused({}, "empty table", "empty table");

    // More than 31 records (0x450e65).
    std::vector<EquipmentRecord> too_many;
    for (int n = 0; n < 32; ++n) {
        EquipmentRecord record = level0.front();
        record.id = n;
        record.name = "R" + std::to_string(n);
        too_many.push_back(std::move(record));
    }
    expect_refused(too_many, "31", "32 records");

    // A failed load leaves the object empty, and a good one clears the error.
    EquipmentAct again;
    std::string error;
    CHECK(!again.set_records(dup, &error));
    CHECK_EQ(again.count(), 0);
    CHECK(!again.last_error().empty());
    error.clear();
    CHECK(again.set_records(level0, &error));
    CHECK(error.empty());
    CHECK(again.last_error().empty());
    CHECK_EQ(again.count(), 13);
}

} // namespace

int main() {
    const LevelEquip level0_defs = level_equipment("level0");
    const LevelEquip level20_defs = level_equipment("level20");
    const LevelEquip dm1_defs = level_equipment("dm1");
    if (!level0_defs.present || !level20_defs.present || !dm1_defs.present) {
        std::fprintf(stderr, "equipment_act: level0/level20/dm1 not readable: %s %s %s\n",
                     level0_defs.error.c_str(), level20_defs.error.c_str(),
                     dm1_defs.error.c_str());
        return 1;
    }
    const std::vector<EquipmentRecord> level0 = to_records(level0_defs.defs);
    const std::vector<EquipmentRecord> level20 = to_records(level20_defs.defs);
    const std::vector<EquipmentRecord> dm1 = to_records(dm1_defs.defs);

    test_corpus();
    test_levels(level0, level20, dm1);
    test_activation(level0);
    test_spend(level0);
    test_two_types(level0);
    test_determinism(level0);
    test_negatives(level0);

    if (g_failures != 0) {
        std::fprintf(stderr, "equipment_act: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("equipment_act: ok\n");
    return 0;
}
