// Pickup rules (`M6.2.6` slice 3).  Proven/model split and section references
// are in `game/pickup.h` and `docs/PICKUP_STATE.md`; this file is the machine.

#include "game/pickup.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

namespace gm {
namespace {

const char* const kKindNames[] = {
    "armor", "shield", "ammo", "weapon", "equipment",
    "health", "cash", "secret", "key", "unknown",
};

const char* const kResultNames[] = {
    "taken", "already_full", "rejected_mass", "rejected_duplicate", "rejected_unknown",
};

int kind_index(PickupKind kind) {
    const int index = static_cast<int>(kind);
    return index >= 0 && index < static_cast<int>(std::size(kKindNames)) ? index : 9;
}

/// `mass_limit < 0` is "no limit" (E §6: the original only uses `+0x3338` for
/// the speed penalty `0x4525e7`).
bool fits_mass(const PlayerInventoryModel& inventory, int item_mass) {
    if (inventory.mass_limit < 0) {
        return true;
    }
    const std::int64_t total =
        static_cast<std::int64_t>(inventory.mass) + static_cast<std::int64_t>(item_mass);
    return total <= inventory.mass_limit;
}

bool owns_weapon(const PlayerInventoryModel& inventory, int id) {
    return std::find(inventory.weapons.begin(), inventory.weapons.end(), id) !=
           inventory.weapons.end();
}

int ammo_at(const PlayerInventoryModel& inventory, int type) {
    if (type < 0 || type >= static_cast<int>(inventory.ammo.size())) {
        return 0;
    }
    return inventory.ammo[type];
}

std::string armor_text(const PlayerInventoryModel& inventory, int amount) {
    return "armor +" + std::to_string(amount) + " -> " + std::to_string(inventory.armor) +
           "/" + std::to_string(inventory.armor_max) + " (HEAL_SOURCEPLAYER, op 112)";
}

} // namespace

const char* pickup_kind_name(PickupKind kind) { return kKindNames[kind_index(kind)]; }

const char* pickup_result_name(PickupResult result) {
    const int index = static_cast<int>(result);
    return index >= 0 && index < static_cast<int>(std::size(kResultNames)) ? kResultNames[index]
                                                                          : "?";
}

int distribute_shields(std::array<int, 4>& plates, int max, int amount) {
    // Proven rotation `0x45b177..0x45b1bf`: one point per plate in the order
    // 0→1→2→3, full plates are skipped, and the loop stops when four plates in
    // a row are full (`cmp ebx,4`).  Points that do not fit are lost, exactly
    // like the original's `dec edi` only on a successful `inc`.
    if (max <= 0 || amount <= 0) {
        return 0;
    }
    int capacity = 0;
    for (int plate : plates) {
        if (plate < max) {
            capacity += max - std::max(plate, 0);
        }
    }
    const int points = std::min(amount, capacity);
    int added = 0;
    while (added < points) {
        int active = 0;
        for (int plate : plates) {
            if (plate < max) {
                ++active;
            }
        }
        if (active == 0) {
            break;
        }
        const int left = points - added;
        const int rounds = left / active;
        if (rounds == 0) {
            // Partial round: the first `left` non-full plates in index order.
            for (int i = 0; i < 4 && left > 0; ++i) {
                if (plates[i] < max && added < points) {
                    ++plates[i];
                    ++added;
                }
            }
            break;
        }
        // Full rounds: every non-full plate gets `rounds`, capped by its own
        // room.  If a plate caps early, the next iteration recomputes the
        // active set (that preserves the sequential one-point result).
        for (int& plate : plates) {
            if (plate < max) {
                const int room = max - std::max(plate, 0);
                const int give = std::min(rounds, room);
                plate = std::max(plate, 0) + give;
                added += give;
            }
        }
    }
    return added;
}

PickupResult PickupRules::take(const PickupItem& item, PlayerInventoryModel& inventory,
                               std::string* message) {
    std::string text;
    PickupResult result = PickupResult::rejected_unknown;

    const auto refuse = [&](PickupResult value, std::string why) {
        text = std::move(why);
        result = value;
    };

    // --- validation (before any mutation) ---------------------------------
    if (item.kind == PickupKind::unknown) {
        refuse(PickupResult::rejected_unknown, "unknown pickup kind");
    } else if (item.id < 0) {
        refuse(PickupResult::rejected_unknown,
               std::string(pickup_kind_name(item.kind)) + ": negative id " +
                   std::to_string(item.id));
    } else if (item.amount <= 0) {
        refuse(PickupResult::rejected_unknown,
               std::string(pickup_kind_name(item.kind)) + ": amount " +
                   std::to_string(item.amount) + " <= 0");
    } else if (item.mass < 0) {
        refuse(PickupResult::rejected_unknown,
               std::string(pickup_kind_name(item.kind)) + ": negative mass " +
                   std::to_string(item.mass));
    } else {
        switch (item.kind) {
        case PickupKind::armor:
        case PickupKind::health: {
            // One gauge: armor is the health pool (D §0.1).  `HEAL_SOURCEPLAYER`
            // clamps at `armor_max` (`0x45b02a`).
            if (inventory.armor >= inventory.armor_max) {
                refuse(PickupResult::already_full,
                       "armor already at max " + std::to_string(inventory.armor_max) +
                           " (HEAL_SOURCEPLAYER would clamp, op 112)");
            } else if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else {
                const int room = inventory.armor_max - inventory.armor;
                const int add = std::min(item.amount, room);
                inventory.armor += add;
                inventory.mass += item.mass;
                text = armor_text(inventory, add);
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::shield: {
            // Four plates, one `shield_max` (D §0.2).  The rotation branch of
            // `REPAIR_SOURCEPLAYER` (flag bit 1, `0x45b177`).
            const bool room_left =
                inventory.shield_max > 0 &&
                std::any_of(inventory.shields.begin(), inventory.shields.end(),
                            [&](int plate) { return plate < inventory.shield_max; });
            if (!room_left) {
                refuse(PickupResult::already_full,
                       "all 4 shields at max " + std::to_string(inventory.shield_max) +
                           " (REPAIR_SOURCEPLAYER would clamp, op 113)");
            } else if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else {
                const int added =
                    distribute_shields(inventory.shields, inventory.shield_max, item.amount);
                inventory.mass += item.mass;
                text = "shield +" + std::to_string(added) + " -> " +
                       std::to_string(inventory.shields[0]) + "/" +
                       std::to_string(inventory.shields[1]) + "/" +
                       std::to_string(inventory.shields[2]) + "/" +
                       std::to_string(inventory.shields[3]) + " of " +
                       std::to_string(inventory.shield_max) +
                       " (rotation 0→1→2→3, 0x45b177)";
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::ammo: {
            // `ammo[type] += delta`, clamp `[0, max]` (op 109, `0x459ca6`).
            const int current = ammo_at(inventory, item.id);
            const bool capped = inventory.ammo_max > 0 && current >= inventory.ammo_max;
            if (capped) {
                refuse(PickupResult::already_full,
                       "ammo type " + std::to_string(item.id) + " already at max " +
                           std::to_string(inventory.ammo_max) + " (op 109 clamp)");
            } else if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else {
                const int cap = inventory.ammo_max > 0 ? inventory.ammo_max
                                                       : std::numeric_limits<int>::max();
                const int add = static_cast<int>(
                    std::min<std::int64_t>(item.amount, static_cast<std::int64_t>(cap) - current));
                if (item.id >= static_cast<int>(inventory.ammo.size())) {
                    inventory.ammo.resize(static_cast<std::size_t>(item.id) + 1, 0);
                }
                inventory.ammo[item.id] = current + add;
                inventory.mass += item.mass;
                text = "ammo type " + std::to_string(item.id) + " +" + std::to_string(add) +
                       " -> " + std::to_string(inventory.ammo[item.id]) + "/" +
                       (inventory.ammo_max > 0 ? std::to_string(inventory.ammo_max)
                                               : std::string("uncapped")) +
                       " (op 109)";
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::weapon: {
            // op 108 `OR_SOURCEPLAYER_WEAPON_BITS`: OR of an already set bit is
            // a no-op, so a repeat is classified, not rejected by the original.
            if (owns_weapon(inventory, item.id)) {
                refuse(PickupResult::rejected_duplicate,
                       "weapon " + std::to_string(item.id) +
                           " already owned (op 108 OR of a set bit is a no-op)");
            } else if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else {
                inventory.weapons.push_back(item.id);
                std::sort(inventory.weapons.begin(), inventory.weapons.end());
                inventory.mass += item.mass;
                text = "weapon " + std::to_string(item.id) + " added (op 108), owned " +
                       std::to_string(inventory.weapons.size());
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::equipment: {
            // op 111 `OR_SOURCEPLAYER_EQUIPMENT_BITS`, same idempotent OR.
            if (inventory.equipment.count(item.id) != 0) {
                refuse(PickupResult::rejected_duplicate,
                       "equipment " + std::to_string(item.id) +
                           " already owned (op 111 OR of a set bit is a no-op)");
            } else if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else {
                inventory.equipment.insert(item.id);
                inventory.mass += item.mass;
                text = "equipment " + std::to_string(item.id) + " added (op 111)";
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::key: {
            // op 106 `OR_SOURCEPLAYER_KEY_BITS`, bit per key.
            if (inventory.keys.count(item.id) != 0) {
                refuse(PickupResult::rejected_duplicate,
                       "key " + std::to_string(item.id) +
                           " already owned (op 106 OR of a set bit is a no-op)");
            } else if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else {
                inventory.keys.insert(item.id);
                inventory.mass += item.mass;
                text = "key " + std::to_string(item.id) + " added (op 106)";
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::cash: {
            // op 114: `cash += delta`, lower clamp at 0 (E §2.1).  Overflow is
            // a model guard; the original field is a plain i32.
            if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else if (item.amount > std::numeric_limits<int>::max() - inventory.cash) {
                refuse(PickupResult::rejected_unknown,
                       "cash " + std::to_string(inventory.cash) + " + " +
                           std::to_string(item.amount) + " overflows int (model)");
            } else {
                inventory.cash += item.amount;
                inventory.mass += item.mass;
                text = "cash +" + std::to_string(item.amount) + " -> " +
                       std::to_string(inventory.cash) + " (op 114)";
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::secret: {
            // op 95 increments the tally by exactly one; `amount` is not a
            // count (CF §5).  There is no cap — the tally may exceed the
            // declared `SET_LEVEL_SECRETS` value.
            if (!fits_mass(inventory, item.mass)) {
                refuse(PickupResult::rejected_mass,
                       "mass " + std::to_string(inventory.mass) + " + " +
                           std::to_string(item.mass) + " > limit " +
                           std::to_string(inventory.mass_limit) + " (model, E §6)");
            } else if (inventory.secrets == std::numeric_limits<int>::max()) {
                refuse(PickupResult::rejected_unknown, "secret tally overflows int (model)");
            } else {
                ++inventory.secrets;
                inventory.mass += item.mass;
                text = "secret tally +1 -> " + std::to_string(inventory.secrets) + " of " +
                       std::to_string(inventory.secrets_declared) +
                       " declared (op 95, no cap)";
                result = PickupResult::taken;
            }
            break;
        }
        case PickupKind::unknown:
        default:
            refuse(PickupResult::rejected_unknown, "unknown pickup kind");
            break;
        }
    }

    switch (result) {
    case PickupResult::taken:
        ++stats_.taken;
        ++stats_.taken_by_kind[static_cast<std::size_t>(kind_index(item.kind))];
        stats_.mass_taken += item.mass;
        break;
    case PickupResult::already_full:
        ++stats_.already_full;
        break;
    case PickupResult::rejected_mass:
        ++stats_.rejected_mass;
        break;
    case PickupResult::rejected_duplicate:
        ++stats_.rejected_duplicate;
        break;
    case PickupResult::rejected_unknown:
        ++stats_.rejected_unknown;
        break;
    }

    if (message != nullptr) {
        *message = text;
    }
    last_message_ = text;
    return result;
}

int PickupRules::pickup_mass_total(const std::vector<PickupItem>& items) {
    std::int64_t total = 0;
    for (const PickupItem& item : items) {
        if (item.mass > 0) {
            total += item.mass;
        }
    }
    return static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max()));
}

} // namespace gm
