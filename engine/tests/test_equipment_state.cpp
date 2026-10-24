// Runtime equipment state (`M6.2.4` slice 2, docs/EQUIPMENT_STATE.md).
//
// Pinned here:
//   * the data fact: `level0` carries 13 `DEFINE_EQUIPMENT` records, `dm1` 12
//     (docs/EQUIPMENT.md §0/§3); `BALLAST` (flags 0x0c) is the only record
//     outside the selection cycle, so 12 slots are usable in both levels;
//   * the event map of every `level0` record (E1/E2/E3/E4/E6 → the numbers of
//     §3), returned verbatim as the u32 the VM posts;
//   * `activate()`: success (active bit + E3), empty pool (false + E6), a
//     running model cooldown (false), out of the cycle (`flags & 0x04`),
//     already active;
//   * `tick()` decrements cooldowns only; `consume()` spends the shared ammo
//     pool of the record's `ammo_type` and — per §2.1 — keeps the active bit;
//   * negatives: out-of-range slot, empty/oversized table, duplicate
//     `object_id`, out-of-range `object_id`/`ammo_type`, negative cooldown,
//     no-ammo-type record — every refusal carries a text.
//
// Real files are read through `gm::load_level` + `gm::decode_sbt_script`; the
// records are the ones `EquipmentTable::from_records` already validates.
#include "game/equipment_state.h"

#include "game/equipment.h"
#include "res/level.h"
#include "res/sbtvm.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
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

/// Event id as the u32 the VM posts (the shipped ids are negative).
constexpr std::uint32_t ev(int value) { return static_cast<std::uint32_t>(value); }

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

void print_state_dump(const char* level, const gm::equip::EquipmentState& state) {
    std::printf("equipment_state: %s records=%d usable=%d\n", level, state.slots(),
                state.usable());
    for (int slot = 0; slot < state.slots(); ++slot) {
        const gm::equip::EquipSlot& item = state.at(slot);
        const gm::EquipmentDef* def = state.record(slot);
        std::printf("equipment_state:   %2d %-13s flags=0x%02x obj=%2d E7=%d E8=%2d "
                    "E1=%d E2=%d E3=%d E4=%d E6=%d\n",
                    slot, def == nullptr ? "?" : def->name.c_str(),
                    def == nullptr ? 0 : static_cast<unsigned>(def->flags()),
                    def == nullptr ? 0 : def->object_id(),
                    def == nullptr ? 0 : def->ammo_per_tick(),
                    def == nullptr ? 0 : def->ammo_type(),
                    def == nullptr ? 0 : def->spawn_event(),
                    def == nullptr ? 0 : def->activate_event(),
                    def == nullptr ? 0 : def->event_on(),
                    def == nullptr ? 0 : def->event_off(),
                    def == nullptr ? 0 : def->event_no_ammo());
        CHECK_EQ(item.id, slot);
    }
}

/// §3 of docs/EQUIPMENT.md: index -> E1, E2, E3, E4, E6 of `level0`.
struct EventRow {
    int slot;
    const char* name;
    int spawn;
    int activate;
    int on;
    int off;
    int no_ammo;
};

constexpr EventRow kLevel0Events[] = {
    {0, "HEADLIGHTS", 0, 0, -30043, -30044, -30044},
    {1, "FORCE SHIELD", 0, 0, -30045, -30046, -30046},
    {2, "MINE DETECTOR", -30047, 0, -30047, -30048, -30048},
    {3, "TARGETING", -30049, 0, -30049, -30050, -30050},
    {4, "SELF-DESTRUCT", 0, 0, -30051, -30052, 0},
    {5, "WARP DEVICE", 0, 0, -30053, 0, -30054},
    {6, "RADAR", -30055, 0, -30055, -30056, 0},
    {7, "FLIGHT PACK", 0, 0, -30057, -30058, -30058},
    {8, "MINE LAYER", 0, 0, 1000018, 0, 0},
    {9, "COMBAT BLOCK", 0, 0, -30059, 0, 0},
    {10, "AUTO SENTRY", 0, 0, -30060, 0, 0},
    {11, "DECOY", 0, 0, -30061, 0, 0},
    {12, "BALLAST", 0, 0, 0, 0, 0},
};

void test_real_tables(const LevelEquip& level0, const LevelEquip& dm1) {
    // --- the data fact -----------------------------------------------------
    CHECK_EQ(level0.defs.size(), std::size_t{13});
    CHECK_EQ(dm1.defs.size(), std::size_t{12});

    gm::equip::EquipmentState st;
    std::string error;
    CHECK(st.load(level0.defs, &error));
    CHECK(error.empty());
    CHECK(st.loaded());
    CHECK_EQ(st.slots(), 13);
    CHECK_EQ(st.usable(), 12);
    CHECK_EQ(st.has(12), true);
    CHECK_EQ(st.has(13), false);
    int zeros = 0;
    for (const gm::EquipmentDef& def : level0.defs) {
        if (def.object_id() == 0) {
            ++zeros;
        }
    }
    // SELF-DESTRUCT, FLIGHT PACK, BALLAST: object_id 0 is not "a duplicate
    // hotkey", it means "no hotkey" (§4.1) — so the load above must succeed.
    CHECK_EQ(zeros, 3);
    print_state_dump("level0", st);

    gm::equip::EquipmentState dm;
    error.clear();
    CHECK(dm.load(dm1.defs, &error));
    CHECK_EQ(dm.slots(), 12);
    CHECK_EQ(dm.usable(), 12);
    CHECK(dm.record(11) != nullptr);
    if (dm.record(11) != nullptr) {
        CHECK_EQ(dm.record(11)->name, std::string("DECOY"));
    }
    std::printf("equipment_state: dm1 records=%d usable=%d last=%s\n", dm.slots(),
                dm.usable(), dm.record(11) == nullptr ? "?" : dm.record(11)->name.c_str());

    // --- flags / object_id / ammo type of the whole level0 table (§3) ------
    for (const EventRow& row : kLevel0Events) {
        const gm::EquipmentDef* def = st.record(row.slot);
        CHECK(def != nullptr);
        if (def == nullptr) {
            continue;
        }
        CHECK_EQ(def->name, std::string(row.name));
        CHECK_EQ(st.event_for(row.slot, gm::equip::Event::Spawn), ev(row.spawn));
        CHECK_EQ(st.event_for(row.slot, gm::equip::Event::Activate), ev(row.activate));
        CHECK_EQ(st.event_for(row.slot, gm::equip::Event::On), ev(row.on));
        CHECK_EQ(st.event_for(row.slot, gm::equip::Event::Off), ev(row.off));
        CHECK_EQ(st.event_for(row.slot, gm::equip::Event::NoAmmo), ev(row.no_ammo));
        const int cooldown_free = st.at(row.slot).cooldown;
        CHECK_EQ(cooldown_free, 0);
        const bool in_cycle = (def->flags() & gm::equip::EquipmentState::kFlagNotInCycle) == 0;
        CHECK_EQ(in_cycle, std::string(row.name) != "BALLAST");
    }
    if (const gm::EquipmentDef* def = st.record(0)) {
        CHECK_EQ(def->flags(), 3);
        CHECK_EQ(def->object_id(), 1);
        CHECK_EQ(def->ammo_per_tick(), 1);
        CHECK_EQ(def->ammo_type(), 10);
    }
    if (const gm::EquipmentDef* def = st.record(7)) {
        CHECK_EQ(def->flags(), 8);
        CHECK_EQ(def->ammo_type(), -1);
    }
    if (const gm::EquipmentDef* def = st.record(12)) {
        CHECK_EQ(def->flags(), 0x0c);
        CHECK_EQ(def->ammo_type(), 0);
    }
    // Out-of-range event queries are 0, never a stray field.
    CHECK_EQ(st.event_for(-1, gm::equip::Event::On), 0u);
    CHECK_EQ(st.event_for(13, gm::equip::Event::NoAmmo), 0u);
}

void test_activation(const LevelEquip& level0) {
    gm::equip::EquipmentState st;
    CHECK(st.load(level0.defs));

    // Shared pool (§2.1): HEADLIGHTS and FORCE SHIELD both read type 10.
    CHECK(st.set_ammo(0, 25));
    CHECK_EQ(st.ammo_for_type(10), 25);
    CHECK_EQ(st.at(1).ammo, 25);

    CHECK(st.activate(0));
    CHECK_EQ(st.at(0).active, true);
    CHECK_EQ(st.last_event(), ev(-30043)); // HEADLIGHTS E3
    CHECK_EQ(st.last_event_slot(), 0);
    std::printf("equipment_state: activate HEADLIGHTS ammo10=%d -> active=%d event=%d\n",
                st.ammo_for_type(10), st.at(0).active ? 1 : 0,
                static_cast<int>(st.last_event()));

    // Already on: refused with a text, state unchanged.
    CHECK(!st.activate(0));
    CHECK(st.last_error().find("already active") != std::string::npos);
    CHECK_EQ(st.last_event(), 0u);

    st.deactivate(0);
    CHECK_EQ(st.at(0).active, false);
    CHECK_EQ(st.last_event(), ev(-30044)); // HEADLIGHTS E4
    std::printf("equipment_state: deactivate HEADLIGHTS -> active=%d event=%d\n",
                st.at(0).active ? 1 : 0, static_cast<int>(st.last_event()));

    // Empty pool: false + the record's E6, no bit.
    CHECK(st.set_ammo(1, 0));
    CHECK(!st.activate(1));
    CHECK_EQ(st.at(1).active, false);
    CHECK_EQ(st.last_event(), ev(-30046)); // FORCE SHIELD E6
    CHECK_EQ(st.last_event_slot(), 1);
    CHECK(st.last_error().find("no ammo") != std::string::npos);
    std::printf("equipment_state: activate FORCE SHIELD ammo10=0 -> false event=%d err=%s\n",
                static_cast<int>(st.last_event()), st.last_error().c_str());

    // Model cooldown: refused, tick() drains it one tick at a time, then the
    // activation succeeds.  MINE DETECTOR has E8 = -1, so no ammo is needed.
    CHECK(st.set_cooldown(2, 3));
    CHECK(!st.activate(2));
    CHECK(st.last_error().find("cooling") != std::string::npos);
    for (int left = 2; left >= 0; --left) {
        st.tick();
        CHECK_EQ(st.at(2).cooldown, left);
        std::printf("equipment_state: tick -> MINE DETECTOR cooldown=%d\n",
                    st.at(2).cooldown);
    }
    CHECK(st.activate(2));
    CHECK_EQ(st.at(2).active, true);
    CHECK_EQ(st.last_event(), ev(-30047)); // MINE DETECTOR E3
    st.deactivate(2);
    CHECK_EQ(st.last_event(), ev(-30048));

    // flags & 0x04 = out of the cycle: BALLAST is never switched on.
    CHECK(!st.activate(12));
    CHECK(st.last_error().find("flags") != std::string::npos);
    CHECK_EQ(st.at(12).active, false);
    std::printf("equipment_state: activate BALLAST flags=0x0c -> false err=%s\n",
                st.last_error().c_str());
}

void test_tick_and_consume(const LevelEquip& level0) {
    gm::equip::EquipmentState st;
    CHECK(st.load(level0.defs));

    // --- consume: shared pool, one unit per press (§3.1) -------------------
    CHECK(st.set_ammo(8, 2)); // MINE LAYER, type 11
    CHECK_EQ(st.at(8).ammo, 2);
    CHECK(st.consume(8));
    CHECK_EQ(st.at(8).ammo, 1);
    CHECK_EQ(st.last_event(), 0u); // MINE LAYER E6 = 0
    CHECK(st.consume(8));
    CHECK_EQ(st.at(8).ammo, 0);
    CHECK_EQ(st.last_event(), 0u);
    CHECK(!st.consume(8));
    CHECK(st.last_error().find("no ammo") != std::string::npos);
    // §3.1: consumables never set the active bit; consume does not set it.
    CHECK_EQ(st.at(8).active, false);
    std::printf("equipment_state: MINE LAYER consume 2x -> ammo=%d active=%d\n", st.at(8).ammo,
                st.at(8).active ? 1 : 0);

    // --- consume on an active item: the bit stays (§2.1) -------------------
    CHECK(st.set_ammo(0, 2));
    CHECK(st.activate(0));
    CHECK_EQ(st.last_event(), ev(-30043));
    CHECK(st.consume(0));
    CHECK_EQ(st.at(0).ammo, 1);
    CHECK_EQ(st.at(0).active, true);
    CHECK(st.consume(0));
    CHECK_EQ(st.at(0).ammo, 0);
    CHECK_EQ(st.at(0).active, true); // the original leaves the bit alone
    CHECK_EQ(st.last_event(), ev(-30044)); // HEADLIGHTS E6 = event_off
    std::printf("equipment_state: HEADLIGHTS consume to 0 -> ammo=%d active=%d event=%d\n",
                st.at(0).ammo, st.at(0).active ? 1 : 0, static_cast<int>(st.last_event()));

    // --- tick(): cooldowns only, ammo/active untouched ---------------------
    CHECK(st.set_cooldown(3, 2));
    const int ammo_before = st.at(0).ammo;
    st.tick();
    st.tick();
    st.tick();
    CHECK_EQ(st.at(3).cooldown, 0);
    CHECK_EQ(st.at(0).ammo, ammo_before);
    CHECK_EQ(st.at(0).active, true);
    std::printf("equipment_state: 3 ticks -> cooldown=%d ammo10=%d active=%d\n",
                st.at(3).cooldown, st.at(0).ammo, st.at(0).active ? 1 : 0);

    // --- refusals around ammo types ---------------------------------------
    CHECK(!st.set_ammo(2, 5)); // MINE DETECTOR: E8 = -1
    CHECK(st.last_error().find("no ammo type") != std::string::npos);
    CHECK(!st.consume(2));
    CHECK(st.last_error().find("no ammo type") != std::string::npos);
    CHECK(!st.set_ammo_for_type(16, 0));
    CHECK(!st.set_ammo_for_type(0, -1));
    CHECK(!st.set_cooldown(0, -1));
    CHECK_EQ(st.ammo_for_type(-1), 0);
    CHECK_EQ(st.ammo_for_type(99), 0);
}

void test_negatives(const LevelEquip& level0) {
    gm::equip::EquipmentState st;
    CHECK(st.load(level0.defs));

    // Out-of-range slot: sentinel, text, no crash.
    CHECK_EQ(st.at(-1).id, -1);
    CHECK_EQ(st.at(-1).ammo, 0);
    CHECK_EQ(st.at(13).id, -1);
    CHECK(!st.activate(13));
    CHECK(st.last_error().find("outside") != std::string::npos);
    st.deactivate(13);
    CHECK(st.last_error().find("outside") != std::string::npos);
    CHECK(!st.consume(-1));
    CHECK(!st.set_ammo(13, 1));
    CHECK(!st.set_cooldown(13, 1));
    CHECK(st.record(-1) == nullptr);
    CHECK(st.record(13) == nullptr);
    std::printf("equipment_state: slot -1/13 -> at.id=%d err=%s\n", st.at(13).id,
                st.last_error().c_str());

    // Empty table.
    gm::equip::EquipmentState empty;
    std::string error;
    CHECK(!empty.load({}, &error));
    CHECK(!error.empty());
    CHECK(error.find("empty") != std::string::npos);
    CHECK_EQ(empty.slots(), 0);
    CHECK_EQ(empty.usable(), 0);
    CHECK(!empty.loaded());
    std::printf("equipment_state: load empty -> false err=%s\n", error.c_str());

    // More than 31 records (0x450e65).
    std::vector<gm::EquipmentDef> too_many(32, level0.defs.front());
    for (std::size_t n = 0; n < too_many.size(); ++n) {
        too_many[n].fields[11] = static_cast<std::int32_t>(n % 11); // object_id
    }
    error.clear();
    CHECK(!empty.load(too_many, &error));
    CHECK(error.find("31") != std::string::npos);
    std::printf("equipment_state: load %zu records -> false err=%s\n", too_many.size(),
                error.c_str());

    // Duplicate hotkey id: two records with object_id 5.
    std::vector<gm::EquipmentDef> duplicate(2, level0.defs.front());
    duplicate[0].fields[11] = 5;
    duplicate[1].fields[11] = 5;
    duplicate[1].name = "TWIN";
    error.clear();
    CHECK(!empty.load(duplicate, &error));
    CHECK(error.find("duplicate object_id 5") != std::string::npos);
    std::printf("equipment_state: duplicate object_id 5 -> false err=%s\n", error.c_str());

    // object_id / ammo_type outside the documented ranges.
    std::vector<gm::EquipmentDef> bad_obj(1, level0.defs.front());
    bad_obj[0].fields[11] = 11;
    error.clear();
    CHECK(!empty.load(bad_obj, &error));
    CHECK(error.find("object_id 11") != std::string::npos);
    std::printf("equipment_state: object_id 11 -> false err=%s\n", error.c_str());

    std::vector<gm::EquipmentDef> bad_ammo(1, level0.defs.front());
    bad_ammo[0].fields[8] = 16;
    error.clear();
    CHECK(!empty.load(bad_ammo, &error));
    CHECK(error.find("ammo_type 16") != std::string::npos);
    std::printf("equipment_state: ammo_type 16 -> false err=%s\n", error.c_str());

    // The three shipped object_id 0 records are *not* duplicates.
    error.clear();
    gm::equip::EquipmentState again;
    CHECK(again.load(level0.defs, &error));
    CHECK(error.empty());
    CHECK_EQ(again.slots(), 13);
}

} // namespace

int main() {
    const LevelEquip level0 = level_equipment("level0");
    const LevelEquip dm1 = level_equipment("dm1");
    if (!level0.present || !dm1.present) {
        std::fprintf(stderr, "equipment_state: level0/dm1 not readable: %s %s\n",
                     level0.error.c_str(), dm1.error.c_str());
        return 1;
    }
    std::printf("equipment_state: level0=%zu dm1=%zu records\n", level0.defs.size(),
                dm1.defs.size());

    test_real_tables(level0, dm1);
    test_activation(level0);
    test_tick_and_consume(level0);
    test_negatives(level0);

    if (g_failures != 0) {
        std::fprintf(stderr, "equipment_state: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("equipment_state: ok\n");
    return 0;
}
