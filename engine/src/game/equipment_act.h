#pragma once

// Equipment activation and per-tick ammo drain (`M6.2.4` slice 2 of
// `docs/EQUIPMENT.md`, spelled out in `docs/EQUIPMENT_ACT.md`).
//
// The caller fills `EquipmentRecord` from the level's `DEFINE_EQUIPMENT`
// records (`gm::EquipmentTable`, op 175); this module does not parse `.sbt`.
//
// Proven by the original (`docs/EQUIPMENT.md`):
//   * op 130 `SOURCEPLAYER_EQUIPMENT_ACTIVATE` (`0x45a816`) is a pure bit
//     operation on `player+0x3328`: on = `active |= 1 << bit`, off =
//     `active &= ~(1 << bit)` (§4.1).  626 calls in the 36 shipped levels,
//     bits 0..7, none outside `0..count-1`.  There is no ammo check in the
//     opcode: the `IF SOURCEPLAYER_AMMO_AT_LEAST` guard lives in the E3/E4
//     level sub-scripts (§3.1/§4.2), which this module does not execute.
//   * the tick `0x4463a9` of `FUN_00445fa0` walks every record whose bit is
//     set and, when `E7 > 0` and `E8 >= 0`, does
//     `ammo[E8] -= E7 * ticks` (`0x4463eb`, pool at `0x81c2dc + 4*E8`),
//     clamps at 0 and posts the record's E6 (§2.1).  It never clears the
//     active bit — a drained item stays on until a script turns it off.
//   * the "energy" of an item is a shared ammo pool per `ammo_type`: E7 > 0
//     only for `HEADLIGHTS` (1), `FORCE SHIELD` (10) and the four deployables
//     (1 each) in the shipped data (§2.1/§3).
//
// Model-only (no RE backing; recorded in `docs/EQUIPMENT_ACT.md`):
//   * `EquipmentEvent`/`events()`: the original posts the level events E3/E4/E6
//     through `event_post`; here the class keeps a log of what a game loop
//     should forward.
//   * E6 is logged **once** per activation→empty transition.  The original
//     calls `event_post(E6)` on every drained tick (§2.1); logging it every
//     tick would spam the log, so the latch `exhausted_` deduplicates it and
//     `activate()` on a non-active record clears the latch.
//   * `activate()`/`deactivate()` are idempotent and never consult ammo: that
//     is exactly the proven op 130 behaviour, not a simulation of the script
//     guard (so turning a drained item on succeeds and the next tick reports
//     `no_ammo` once).
//   * `tick(0, …)` is a no-op: the original computes `E7 * 0 = 0` and would
//     still post E6 for a pool that is already 0.
//
// Deliberately not wired into `GameRuntime`, `VmExecutor`, input, HUD or the
// save format — like `game/equipment_state.*`, this is a standalone seam.

#include <cstdint>
#include <string>
#include <vector>

namespace gm::equip {

/// One caller-supplied `DEFINE_EQUIPMENT` row (`docs/EQUIPMENT.md` §2/§3).
/// `id` is the record index in the level table: the active bit is `1 << id`
/// of `player+0x3328` (§5).  The remaining fields are copied verbatim from the
/// level data; `ammo_type = -1` means "no ammo pool" (§3).
struct EquipmentRecord {
    int id = -1;                 ///< record index, 0..kMaxRecords-1
    std::string name;            ///< `+0x2c`, 16-byte name in the original
    std::uint32_t flags = 0;     ///< `+0x00` (E0)
    int ammo_type = -1;          ///< `+0x20` (E8), -1 = no pool
    int ammo_per_tick = 0;       ///< `+0x1c` (E7), units per 91 Hz tick
    int price = 0;               ///< `+0x24` (E9)
    int weight = 0;              ///< `+0x28` (E10)
    int object_id = 0;           ///< `+0x3c` (E11), hotkey id, 0 = none
};

/// A transition a game loop should translate into the level event of the same
/// record: `activated`/`deactivated` accompany the bit change, `no_ammo` is
/// the drained-pool report (§2.1).  The original's numeric event ids are E3/
/// E4/E6 of the record (`gm::EquipmentDef::event_on/event_off/event_no_ammo`).
struct EquipmentEvent {
    enum Kind { activated, deactivated, no_ammo };

    int id = -1;              ///< record index (`1 << id` is the active bit)
    Kind kind = activated;
};

/// Activation mask and sanity-checked record table of one level's equipment.
class EquipmentAct {
public:
    /// `0x450e65`: the store takes at most 31 records, indexes 0..30 (§2).
    static constexpr int kMaxRecords = 31;
    /// Ammo pools reachable through `ammo_type`: `0x81c2dc + 4*E8`; shipped
    /// types are 0..14 and `AmmoPool` is 16 wide (§2.1).  `tick()` reads and
    /// writes exactly this many `int`s through its pointer.
    static constexpr int kAmmoTypes = 16;
    /// `ammo_type` of a record without a pool: E8 = -1 (§2, §3).
    static constexpr int kNoAmmoType = -1;

    /// Adopt a level's records.  Refused with a text in `*error` and
    /// `last_error()` (the object is left empty) on: an empty set, more than
    /// `kMaxRecords` records, an `id` outside `0..kMaxRecords-1`, a duplicate
    /// `id`, `ammo_per_tick < 0`, or `ammo_type` outside -1..kAmmoTypes-1.
    bool set_records(std::vector<EquipmentRecord> records, std::string* error = nullptr);

    bool has_records() const { return !records_.empty(); }
    /// Number of adopted records (12 in every shipped level, 13 with
    /// `BALLAST` — §0/§3).
    int count() const { return static_cast<int>(records_.size()); }
    const std::vector<EquipmentRecord>& records() const { return records_; }
    /// Record by id, nullptr when absent.
    const EquipmentRecord* record(int id) const;

    /// `SOURCEPLAYER_EQUIPMENT_ACTIVATE id, 1` (op 130, §4.1): sets bit `id`
    /// and logs `activated`.  An already-active record is a successful no-op
    /// (the opcode is an unconditional OR) with no second event.  Ammo is not
    /// consulted — that check is the level script's (§3.1/§4.2).
    bool activate(int id);
    /// `SOURCEPLAYER_EQUIPMENT_ACTIVATE id, 0` (op 130, §4.1): clears bit
    /// `id` and logs `deactivated`.  Already-clear and out-of-range ids are
    /// handled like `activate()`.
    bool deactivate(int id);
    bool is_active(int id) const;
    /// `player+0x3328` (`0x81c368`) as one word; bits 0..7 are the ones the
    /// shipped scripts touch, the mask itself is 32-bit (§4.1/§5).
    std::uint32_t active_mask() const { return active_; }
    /// Model latch: true when the last drain hit 0 while the record was on
    /// (E6 already logged for this activation).
    bool is_exhausted(int id) const;

    /// One tile of `0x4463a9`: for every active record with
    /// `ammo_per_tick > 0` and a pool (`ammo_type >= 0`) spends
    /// `ammo_per_tick * ticks` from `ammo[ammo_type]`, clamps at 0 and — on the
    /// first transition to 0 for this activation — logs `no_ammo`.
    /// `ammo` must address `kAmmoTypes` ints.  A negative `ticks` and a null
    /// `ammo` are refused with a text and change nothing (§2.1 for the spend,
    /// the refusal is a model guard).
    void tick(int ticks, int* ammo);

    /// Transitions since the last `clear_events()`, in operation order.
    const std::vector<EquipmentEvent>& events() const { return events_; }
    void clear_events() { events_.clear(); }

    /// Text of the last refusal; cleared by a successful call.
    const std::string& last_error() const { return error_; }

private:
    const EquipmentRecord* find(int id) const;
    bool refuse(std::string message);
    bool set_bit(int id, bool on);

    std::vector<EquipmentRecord> records_;
    std::uint32_t active_ = 0;
    std::uint32_t exhausted_ = 0;
    std::vector<EquipmentEvent> events_;
    std::string error_;
};

} // namespace gm::equip
