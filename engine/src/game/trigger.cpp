#include "game/trigger.h"

#include <cstddef>
#include <limits>

namespace gm {

WeaponActionResult update_player_weapon(const Weapons& table, Arsenal& arsenal,
                                        ChargeState& charge,
                                        const PlayerWeaponInput& input,
                                        Vec3d origin,
                                        const CollisionWorld& world,
                                        std::uint32_t ticks) {
    WeaponActionResult result;
    result.slot = arsenal.current;

    // 0x4453f8..0x445410: the handler decrements `player+0x3318` by dt before
    // the cooldown gate.  `gm::tick()` also advances reload counters; the
    // original has no reload, and loaded weapons keep `reload_ticks == 0`.
    gm::tick(table, arsenal, ticks);
    const int tick_count =
        ticks > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(ticks);

    // --- slot request (`FUN_00445300`, 0x445300) ---------------------------
    // The original scans for the next/previous available slot when it sees the
    // 0x2000/0x4000 edge bits (0x44553e..0x445644) and then calls 0x445300.
    // This seam takes the already resolved absolute slot from the input layer.
    if (input.slot_edge) {
        const int previous = arsenal.current;
        if (switch_slot(table, arsenal, input.slot, &result.slot_error)) {
            if (arsenal.current != previous) {
                result.slot_changed = true;
                // 0x4453b7/0x4453c1: `+0x2fa8` (accumulated) is zeroed, and
                // 0x4453ad clears the pending `+0x35c` after posting it.
                charge.accumulated = 0.0;
                if (charge.state_event != 0) {
                    result.switched_state_event = charge.state_event;
                    charge.state_event = 0;
                }
            }
        } else {
            result.slot_rejected = true;
        }
    }
    result.slot = arsenal.current;

    const Weapon* weapon = table.at(result.slot);
    if (weapon == nullptr) {
        result.shot.error = FireError::no_weapon;
        return result;
    }

    // 0x445aa2: the cooldown gate runs before the charge branch, so a cooling
    // weapon neither accumulates nor latches.  0x445a4a: the hold bit
    // (`player+0x3340 & 0x20`) suppresses the pending state event.
    const bool ready =
        arsenal.cooldown[static_cast<std::size_t>(result.slot)] == 0;
    result.charge =
        update_charge(*weapon, charge, ready, input.trigger_held,
                      input.trigger_edge, tick_count, input.trigger_held);

    if (result.charge.fire) {
        // `fire()` consumes `charge.charge` (`player+0x2fa0`): the projectile
        // spawner reads it as `speed += charge * operand[9]` (0x46f0f0).
        result.shot = fire(table, arsenal, origin, input.aim_direction, world,
                           charge.charge);
    }
    return result;
}

} // namespace gm
