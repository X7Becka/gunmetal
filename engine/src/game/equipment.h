// Equipment model (M6.2.4): 12 DEFINE_EQUIPMENT records + the per-tick spend.
//
// The level script defines the items (`DEFINE_EQUIPMENT`, opcode 175, VM store
// 0x450e60, docs/EQUIPMENT.md): 12 dwords + a 16-byte name, at most 31 records
// (0x450e65).  The *effects* are data too: E3/E4 point at sub-scripts, so a
// renderer/game loop only has to keep the state and spend the "energy" ammo.
//
// State lives in the player block (docs/EQUIPMENT.md §5):
//   +0x3320 index   (int, -1 = nothing selected)
//   +0x3324 owned   (bit i = record i bought)
//   +0x3328 active  (bit i = record i on)
// and the tick is 0x4463a9: for every active record with `ammo_per_tick > 0`
// it spends `ammo_per_tick * ticks` from `ammo[ammo_type]`, posting the
// record's `event_no_ammo` when the pool reaches zero.
#pragma once

#include "res/sbtvm.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gm {

/// One `DEFINE_EQUIPMENT` record: name plus E0..E11.
struct EquipmentDef {
    std::string name;
    std::array<std::int32_t, 12> fields{};

    std::int32_t flags() const { return fields[0]; }           ///< +0x00
    std::int32_t spawn_event() const { return fields[1]; }     ///< +0x04 (E1)
    std::int32_t activate_event() const { return fields[2]; }  ///< +0x08 (E2)
    std::int32_t event_on() const { return fields[3]; }        ///< +0x0c (E3)
    std::int32_t event_off() const { return fields[4]; }       ///< +0x10 (E4)
    std::int32_t event_no_ammo() const { return fields[6]; }   ///< +0x18 (E6)
    std::int32_t ammo_per_tick() const { return fields[7]; }   ///< +0x1c (E7)
    std::int32_t ammo_type() const { return fields[8]; }       ///< +0x20 (E8)
    std::int32_t price() const { return fields[9]; }           ///< +0x24
    std::int32_t weight() const { return fields[10]; }         ///< +0x28
    std::int32_t object_id() const { return fields[11]; }      ///< +0x2c
};

/// The item table of one level (in `DEFINE_EQUIPMENT` order).
struct EquipmentTable {
    /// 0x450e65: `cmp eax,0x1f; jl` — the store takes 31 records.
    static constexpr std::size_t kMaxRecords = 31;

    std::vector<EquipmentDef> defs;

    /// Build from the VM's decoded records.  Fails (with *error*) on more than
    /// `kMaxRecords` rows or a row without the 12 dwords.
    static std::optional<EquipmentTable> from_records(
        const std::vector<SbtTableRecord>& records, std::string* error = nullptr);

    /// Record by index, or nullptr when out of range.
    const EquipmentDef* find(int index) const;
    std::size_t size() const { return defs.size(); }
    bool empty() const { return defs.empty(); }
};

/// Player-side equipment state (`+0x3320`/`+0x3324`/`+0x3328`).
struct EquipmentState {
    int index = -1;             ///< +0x3320, -1 = nothing selected
    std::uint32_t owned = 0;    ///< +0x3324
    std::uint32_t active = 0;   ///< +0x3328

    bool is_owned(int record) const {
        return record >= 0 && record < 32 && (owned & (1u << record)) != 0u;
    }
    bool is_active(int record) const {
        return record >= 0 && record < 32 && (active & (1u << record)) != 0u;
    }
    /// `SOURCEPLAYER_EQUIPMENT_ACTIVATE bit, on` (op 130, 0x45a816).
    void set_active(int record, bool on);
};

/// Ammo arrays are per ammo type (`0x81c2dc + type*4`); 16 covers the shipped
/// maximum (types 0..14, docs/EQUIPMENT.md §2).
using AmmoPool = std::array<std::int32_t, 16>;

/// One tick of `0x4463a9`.  Spends `ammo_per_tick * ticks` for every active
/// record and returns the *record indexes* whose pool reached zero (the caller
/// posts `event_no_ammo`; the script decides what happens).
///
/// The original spends from `ammo[ammo_type]`, never below zero, and does not
/// clear the active bit — a script may keep a drained item on.
std::vector<int> equipment_tick(const EquipmentTable& table,
                                const EquipmentState& state, AmmoPool& ammo,
                                int ticks);

} // namespace gm
