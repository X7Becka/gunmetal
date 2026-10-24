#include "game/equipment_act.h"

#include <utility>

namespace gm::equip {
namespace {

std::uint32_t bit_of(int id) {
    return 1u << static_cast<unsigned>(id);
}

} // namespace

bool EquipmentAct::refuse(std::string message) {
    error_ = std::move(message);
    return false;
}

const EquipmentRecord* EquipmentAct::find(int id) const {
    for (const EquipmentRecord& record : records_) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

const EquipmentRecord* EquipmentAct::record(int id) const { return find(id); }

bool EquipmentAct::set_records(std::vector<EquipmentRecord> records, std::string* error) {
    records_.clear();
    active_ = 0;
    exhausted_ = 0;
    events_.clear();
    error_.clear();

    auto fail = [&](std::string message) {
        if (error != nullptr) {
            *error = message;
        }
        refuse(std::move(message));
        return false;
    };

    if (records.empty()) {
        return fail("set_records: empty table, a level defines its equipment with "
                    "DEFINE_EQUIPMENT (docs/EQUIPMENT.md §2)");
    }
    if (records.size() > static_cast<std::size_t>(kMaxRecords)) {
        return fail("set_records: " + std::to_string(records.size()) +
                    " records, the store takes at most " + std::to_string(kMaxRecords) +
                    " (0x450e65, docs/EQUIPMENT.md §2)");
    }

    std::vector<int> seen;
    seen.reserve(records.size());
    for (std::size_t n = 0; n < records.size(); ++n) {
        const EquipmentRecord& item = records[n];
        const std::string where = "set_records[" + std::to_string(n) + "] '" + item.name +
                                  "' (id " + std::to_string(item.id) + "): ";
        if (item.id < 0 || item.id >= kMaxRecords) {
            return fail(where + "id outside 0.." + std::to_string(kMaxRecords - 1) +
                        ", the active bit is 1 << id (0x450e65, docs/EQUIPMENT.md §2/§5)");
        }
        for (int other : seen) {
            if (other == item.id) {
                return fail(where + "duplicate id " + std::to_string(item.id) +
                            ", the active bit is 1 << id and must address one record "
                            "(docs/EQUIPMENT.md §5)");
            }
        }
        seen.push_back(item.id);
        if (item.ammo_per_tick < 0) {
            return fail(where + "ammo_per_tick " + std::to_string(item.ammo_per_tick) +
                        ", expected >= 0 (E7, 0x4463c0, docs/EQUIPMENT.md §2.1)");
        }
        if (item.ammo_type < kNoAmmoType || item.ammo_type >= kAmmoTypes) {
            return fail(where + "ammo_type " + std::to_string(item.ammo_type) +
                        " outside -1.." + std::to_string(kAmmoTypes - 1) +
                        " (E8, 0x81c2dc + 4*E8, docs/EQUIPMENT.md §2.1)");
        }
    }

    records_ = std::move(records);
    error_.clear();
    return true;
}

bool EquipmentAct::is_active(int id) const {
    if (find(id) == nullptr) {
        return false;
    }
    return (active_ & bit_of(id)) != 0u;
}

bool EquipmentAct::is_exhausted(int id) const {
    if (find(id) == nullptr) {
        return false;
    }
    return (exhausted_ & bit_of(id)) != 0u;
}

bool EquipmentAct::activate(int id) {
    error_.clear();
    const EquipmentRecord* item = find(id);
    if (item == nullptr) {
        return refuse("activate: id " + std::to_string(id) + " is not in the records (" +
                      std::to_string(count()) + " adopted, docs/EQUIPMENT_ACT.md §3)");
    }
    if ((active_ & bit_of(id)) != 0u) {
        // op 130 is `active |= 1 << bit` (§4.1): idempotent, and the script
        // would not post a second "on" event either.
        return true;
    }
    active_ |= bit_of(id);
    exhausted_ &= ~bit_of(id);
    events_.push_back(EquipmentEvent{id, EquipmentEvent::activated});
    return true;
}

bool EquipmentAct::deactivate(int id) {
    error_.clear();
    const EquipmentRecord* item = find(id);
    if (item == nullptr) {
        return refuse("deactivate: id " + std::to_string(id) + " is not in the records (" +
                      std::to_string(count()) + " adopted, docs/EQUIPMENT_ACT.md §3)");
    }
    if ((active_ & bit_of(id)) == 0u) {
        // op 130 with `on = 0` is `active &= ~(1 << bit)` (§4.1): idempotent.
        return true;
    }
    active_ &= ~bit_of(id);
    events_.push_back(EquipmentEvent{id, EquipmentEvent::deactivated});
    return true;
}

void EquipmentAct::tick(int ticks, int* ammo) {
    error_.clear();
    if (ticks < 0) {
        refuse("tick: got " + std::to_string(ticks) + " ticks, expected >= 0");
        return;
    }
    if (ammo == nullptr) {
        refuse("tick: null ammo table, expected " + std::to_string(kAmmoTypes) +
               " ints (0x81c2dc + 4*E8, docs/EQUIPMENT.md §2.1)");
        return;
    }
    if (ticks == 0) {
        // `E7 * 0` spends nothing; the original would still post E6 for an
        // already-empty pool, which this model deliberately does not do.
        return;
    }

    for (const EquipmentRecord& item : records_) {
        const std::uint32_t bit = bit_of(item.id);
        if ((active_ & bit) == 0u) {
            continue;
        }
        if (item.ammo_per_tick <= 0) {
            continue;
        }
        if (item.ammo_type < 0 || item.ammo_type >= kAmmoTypes) {
            // set_records() already rejects this; keep the pool access safe.
            continue;
        }
        const long long spend = static_cast<long long>(item.ammo_per_tick) *
                                static_cast<long long>(ticks);
        long long pool = static_cast<long long>(ammo[item.ammo_type]) - spend;
        if (pool > 0) {
            ammo[item.ammo_type] = static_cast<int>(pool);
            continue;
        }
        // §2.1: clamp at 0 and report E6.  The active bit stays set.
        ammo[item.ammo_type] = 0;
        if ((exhausted_ & bit) == 0u) {
            exhausted_ |= bit;
            events_.push_back(EquipmentEvent{item.id, EquipmentEvent::no_ammo});
        }
    }
}

} // namespace gm::equip
