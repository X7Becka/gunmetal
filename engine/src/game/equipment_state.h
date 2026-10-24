#pragma once

// Runtime equipment state (`M6.2.4` slice 2): the per-player view of a level's
// `DEFINE_EQUIPMENT` records, kept next to — not instead of — the read-only
// data model in `game/equipment.h`.
//
// What the original proves (`docs/EQUIPMENT.md`):
//   * the record is static data (§2): name, `flags`, five event ids (E1/E2/E3/
//     E4/E6), `ammo_per_tick`, `ammo_type`, `price`, `weight`, `object_id`;
//     the store takes at most 31 records (`0x450e65`);
//   * the player keeps the state in three words (§5): `+0x3320` index,
//     `+0x3324` owned, `+0x3328` active;
//   * input never flips the active bit (§4.1/§4.2): `FUN_00445fa0` posts E3
//     ("turn on") or E4 ("turn off"), the level sub-script checks the ammo and
//     calls `SOURCEPLAYER_EQUIPMENT_ACTIVATE` (op 130, `0x45a816`), and *that*
//     sets the bit.  An "activation" here is therefore the script's decision
//     over data, not a physical effect: the effects (headlight bit, damage
//     modifier, fly block, sprites) are opcodes the sub-script runs (§3.1,
//     §6.1) and are not modelled by this class;
//   * the "energy" of an item is a shared ammo pool per `ammo_type`
//     (`0x81c2dc + 4*E8`, §2.1).  The per-tick spend `E7*ticks` already lives
//     in `gm::equipment_tick`; this class owns the pool and mirrors it into
//     every slot that uses the same type;
//   * the tick that spends ammo never clears the active bit (§2.1), and the
//     consumables (8..11) never set it at all (§3.1) — so `consume()` does not
//     deactivate either.
//
// Model-only (no RE backing, spelled out in `docs/EQUIPMENT_STATE.md`):
//   * `EquipSlot::cooldown`: neither the record nor the player block has a
//     cooldown counter in 1.36.  It is a caller-driven rate limit in ticks
//     (0 = free), refused by `activate()` and decremented by `tick()`; it lets
//     a game loop rate-limit consumables without inventing a data field.
//   * the event reported by `activate()` is `event_on` (E3) on success and
//     `event_no_ammo` (E6) when the pool is empty; the original's sub-script
//     instead prints `NO POWER FOR …`/`AMMUNITION DEPLETED` (§3.1, §9.4).
//
// This class is deliberately not wired into `GameRuntime`/`VmExecutor` yet:
// the VM half (`equipment_bits`/`equipment_active`/`equipment_index` in
// `res/vm_exec.h`) stays authoritative for scripts, and the seam is the next
// slice.

#include "game/equipment.h" // EquipmentDef, AmmoPool

#include <cstdint>
#include <string>
#include <vector>

namespace gm::equip {

/// Event slots of one `DEFINE_EQUIPMENT` record (`docs/EQUIPMENT.md` §2).
/// The numeric value is the file dword index of the field, so `Event` maps
/// straight to `EquipmentDef::fields` (E5 is the only gap, §2/§9.1).
enum class Event : int {
    Spawn = 1,    ///< E1 `+0x04`, posted when the item spawns (§2)
    Activate = 2, ///< E2 `+0x08`, posted on selection; 0 in shipped data (§4.1)
    On = 3,       ///< E3 `+0x0c`, "toggle on" request (§4.2)
    Off = 4,      ///< E4 `+0x10`, "toggle off" request (§4.2)
    NoAmmo = 6,   ///< E6 `+0x18`, "no ammo" event (`0x446445`, §2.1)
};

/// One runtime slot.  `id` is the record index (`0..slots()-1`); the active
/// bit is `1 << id` of `player+0x3328` (§5).
struct EquipSlot {
    int id = -1;        ///< record index, -1 = empty sentinel
    bool active = false;///< `+0x3328` bit `id` (set by op 130, §4.1)
    int ammo = 0;       ///< mirror of `ammo[ammo_type]` (`0x81c2dc`, §2.1)
    int cooldown = 0;   ///< model-only rate limit in ticks (0 = free)
};

/// Equipment state of one player over one level's item table.
class EquipmentState {
public:
    /// `0x450e65`: `cmp eax,0x1f; jl` — the store rejects record 31, so the
    /// shipped maximum is 31 slots, indexes 0..30 (§2).
    static constexpr int kMaxSlots = 31;
    /// `ammo_type` of a record without a pool: E8 = -1 (§2, §3).
    static constexpr int kNoAmmoType = -1;
    /// Ammo words reachable through `ammo_type`: `0x81c2dc + 4*E8`, shipped
    /// types are 0..14 (§2.1, §3), the `AmmoPool` is 16 wide.
    static constexpr int kAmmoTypes = 16;
    /// `flags & 0x04` = "not in the selection cycle" (§3): set for `BALLAST`
    /// (and it is the bit the next/prev walk skips, `0x445ff1`/`0x4460f5`).
    static constexpr std::int32_t kFlagNotInCycle = 0x04;
    /// `flags & 0x08` = hidden from the shop list (§3); kept for callers.
    static constexpr std::int32_t kFlagHidden = 0x08;

    /// Build the slots from a level's decoded records.
    ///
    /// Refused with a text in `*error` (and `last_error()`) on: an empty set,
    /// more than `kMaxSlots` records, a negative or > 10 `object_id` (§4.1
    /// hotkey ids are 1..10, 0 = no hotkey), a repeated non-zero `object_id`
    /// (a hotkey must address exactly one record), or `ammo_type` outside
    /// -1..15.  A failed load leaves the object empty.
    bool load(const std::vector<EquipmentDef>& defs, std::string* error = nullptr);

    bool loaded() const { return !defs_.empty(); }
    /// Number of records (`0x815420`, §2), 0 after a failed/absent load.
    int slots() const { return static_cast<int>(defs_.size()); }
    /// Records a player can switch on: `!(flags & 0x04)` (§3).  `level0`
    /// has 13 records and 12 usable ones (`BALLAST` is out of the cycle).
    int usable() const;

    bool has(int slot) const {
        return slot >= 0 && slot < static_cast<int>(slots_.size());
    }
    /// Slot by index; an out-of-range request returns a static empty slot
    /// (`id = -1`) instead of dereferencing garbage.
    const EquipSlot& at(int slot) const;
    /// The record behind a slot, or nullptr when out of range.
    const EquipmentDef* record(int slot) const;

    /// Script decision "turn slot on" (`SOURCEPLAYER_EQUIPMENT_ACTIVATE`, op
    /// 130, §4.1).  True and `active = true` only when the record is usable
    /// (`!(flags & 4)`), is not already on, its `cooldown` is 0 and — when it
    /// has an `ammo_type` — the shared pool is above zero.
    ///
    /// On an empty pool it returns false and sets `last_event()` to the
    /// record's E6 (`event_no_ammo`, §2.1); on success it sets it to E3
    /// (`event_on`, the request the script answers, §4.2).
    bool activate(int slot);
    /// Script decision "turn slot off" (op 130 with `on = 0`, §4.1).  Sets
    /// `last_event()` to E4 (`event_off`) when the slot was on.
    void deactivate(int slot);
    /// Spend one unit of the slot's ammo pool (the `ADD_AMMO type,-1` of the
    /// consumable sub-scripts, §3.1).  False when the record has no ammo type
    /// (E8 = -1) or the pool is already 0; on the transition to 0
    /// `last_event()` becomes the record's E6 (0 for most consumables, §3).
    /// The active bit is *not* cleared — §2.1 says the original keeps it.
    bool consume(int slot);
    /// One 1/91 s tick: decrements every positive `cooldown` by one.  The
    /// ammo spend is `gm::equipment_tick` (§2.1); this method does not touch
    /// ammo or the active bit.
    void tick();

    /// Set the model-only cooldown of a slot (ticks, >= 0).
    bool set_cooldown(int slot, int ticks);
    /// Write the shared pool of `ammo_type` (`0x81c2dc + 4*type`, §2.1) and
    /// refresh every slot that mirrors it.
    bool set_ammo_for_type(int type, int value);
    /// Same for the pool of the slot's own `ammo_type`; false when the record
    /// has none (E8 = -1) or the slot is out of range.
    bool set_ammo(int slot, int value);
    /// Current shared pool of an ammo type, 0 when out of range.
    int ammo_for_type(int type) const;
    /// Adopt an external pool (e.g. after `gm::equipment_tick`) and refresh
    /// the per-slot mirrors.
    void sync_ammo(const AmmoPool& ammo);
    const AmmoPool& ammo_pools() const { return ammo_; }

    /// Event id of a record slot by kind (E1/E2/E3/E4/E6, §2), as the u32 the
    /// VM posts (`event_post`, §4.1); the shipped ids are negative, so the
    /// value is the two's-complement bit pattern.  0 when out of range.
    std::uint32_t event_for(int slot, Event kind) const;

    /// Event produced by the last `activate()`/`deactivate()`/`consume()`:
    /// E3/E4/E6 or 0 for "nothing".  Kept until the next operation.
    std::uint32_t last_event() const { return last_event_; }
    int last_event_slot() const { return last_slot_; }
    void clear_event() {
        last_event_ = 0;
        last_slot_ = -1;
    }

    /// Text of the last refusal (bad load, slot out of range, empty pool,
    /// cooldown, record without ammo type).
    const std::string& last_error() const { return error_; }

private:
    bool refuse(std::string message);
    void sync_slot_ammo();
    const EquipmentDef* def_at(int slot) const;

    std::vector<EquipmentDef> defs_;
    std::vector<EquipSlot> slots_;
    AmmoPool ammo_{};
    std::uint32_t last_event_ = 0;
    int last_slot_ = -1;
    std::string error_;
};

} // namespace gm::equip
