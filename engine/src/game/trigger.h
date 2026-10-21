#pragma once

// Player input -> weapon tick seam: the piece between `input/controls.*` and
// `game/weapons.*`.  One call is one fixed 91 Hz tick of the original weapon
// handler `0x4453d0`, driven by the action bits the controls layer produces.
//
// Proven original chain (see docs/WEAPONS_CPP.md §3.6):
//
//   action bitmask        `0x61f1e8[player]` (31 bits, action table 0x4bc558)
//   hold / edge words     `player+0x3340` / `player+0x3344` (0x4650f1/0x4650f8)
//   cooldown              `player+0x3318`, decremented by dt at 0x4453f8,
//                         gate at 0x445aa2
//   selected slot         `player+0x3294` (0x445ab2), mask `+0x3298`
//   fire mode             weapon `+0x24` (0x445ac6):
//                         0 -> 0x445d85 (shot on the trigger edge only)
//                         1 -> 0x445d64 (shot every tick while held)
//                         2..6 -> 0x445adb (charge accumulator)
//   charge                `player+0x2fa8` accumulated, `+0x2fa0` latched
//                         (0x445af6..0x445d49)
//   ammo / event / period `player+0x329c[ammo_type]`, `weapon+0x30`,
//                         `weapon+0x2c` (0x445dfb/0x445e98/0x445eee/0x445f08)
//
// The bit numbers are action indices, not device codes: action 5 is "Fire"
// (pointer table 0x4bc558 entry 5 = 0x4bc8ec = "Fire"), action 13/14 are
// "Next/Previous Weapon" (bits 0x2000/0x4000, consumed at 0x44553e/0x445644),
// action 16/17 are "Next/Previous Equipment" (bits 0x10000/0x20000,
// `FUN_00445fa0`).  `input/controls.*` exposes those actions by name; this
// module deliberately takes plain booleans so it stays free of SDL and of the
// controller backend.
//
// The slot switch itself is `FUN_00445300` (0x445300): it checks the mask bit
// `player+0x3298` (0x44532d..0x44533c), writes `+0x3294` (0x445343), posts the
// pending `player+0x35c` state event (0x4453a5) and zeroes the charge
// accumulator `+0x2fa8` (0x4453b7/0x4453c1).
//
// Hypotheses (no byte proof):
//   * the caller feeds `trigger_edge`/`slot_edge` as *one-tick* edges; the
//     original computes them from held counters in `FUN_004650a0`, while this
//     module trusts the input layer (the same convention as `Controls`);
//   * a slot request that the mask rejects keeps the old slot and is reported
//     (`slot_rejected`); the original instead scans forward/backward for the
//     next available slot (0x445555..0x445644), which needs the ammo-flag word
//     `player+0x3200 & 0x400` the C++ loadout does not carry yet;
//   * `Arsenal::cooldown` is per slot, while the original keeps one
//     `player+0x3318` shared by weapon switches.  The C++ model is the
//     existing one from `game/weapons.h`, not a byte-level port of that field.

#include <cstdint>

#include "game/weapons.h"
#include "render/collide.h"

namespace gm {

/// What the controls layer knows about the weapon controls in one tick.
/// `slot` is an absolute table index (`player+0x3294`), `slot_edge` says that
/// the request is a new "Next/Previous Weapon" press rather than a level.
struct PlayerWeaponInput {
    bool trigger_held = false; ///< action 5 held (`player+0x3340` bit 0x20)
    bool trigger_edge = false; ///< action 5 pressed this tick (`+0x3344` bit 0x20)
    int slot = -1;             ///< requested slot, -1 = no request
    bool slot_edge = false;    ///< apply `slot` this tick
    Vec3d aim_direction{0.0, 1.0, 0.0}; ///< fire direction, need not be unit
};

/// One tick of `0x4453d0` plus the shot it resolved.
struct WeaponActionResult {
    ChargeOutcome charge;      ///< charge branch result (`0x445adb..0x445d49`)
    ShotResult shot;           ///< `fire()` result; `shot.fired` says if it shot
    int slot = -1;             ///< selected slot after the update
    bool slot_changed = false; ///< the request switched the slot
    bool slot_rejected = false;///< the request was refused (mask/table/range)
    FireError slot_error = FireError::none;
    /// Pending `player+0x35c` state event posted by the slot switch
    /// (`FUN_00445300`); zero when there was none.
    std::uint32_t switched_state_event = 0;

    bool fired() const { return shot.ok && shot.fired; }
};

/// One fixed-tick player weapon update.
///
/// Order is the original's:
///   1. cooldown `arsenal.cooldown[slot]` is reduced by `ticks`
///      (`0x4453f8..0x445410`; `gm::tick()` also advances reloads, which the
///      original does not have -- reload is a hypothesis layer and is off for
///      loaded weapons);
///   2. a `slot_edge` request goes through `switch_slot()` (`FUN_00445300`);
///      on success the charge accumulator is zeroed and any pending state
///      event is returned in `switched_state_event`;
///   3. `update_charge()` runs for the *new* slot with `ready =
///      cooldown[slot] == 0` (`0x445aa2`) and `suppress_state_event =
///      trigger_held` (`0x445a4a`);
///   4. if the charge state machine asks for a shot, `fire()` runs with the
///      latched `charge.charge` (`player+0x2fa0` -> `0x46f0f0`).
///
/// A shot rejected by `fire()` (empty ammo, cooldown, bad direction) leaves
/// `shot.ok == false` with the reason; nothing is consumed in that case.
WeaponActionResult update_player_weapon(const Weapons& table, Arsenal& arsenal,
                                        ChargeState& charge,
                                        const PlayerWeaponInput& input,
                                        Vec3d origin,
                                        const CollisionWorld& world,
                                        std::uint32_t ticks = 1);

/// One player weapon tick as the runtime sees it: the action result plus the
/// `Projectiles::spawn()` index of the round it created (`-1` when the shot
/// was hitscan, rejected or no projectile template is installed).
struct PlayerWeaponTick {
    WeaponActionResult action;
    int spawned = -1;
};

} // namespace gm
