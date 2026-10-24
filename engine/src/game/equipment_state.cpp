#include "game/equipment_state.h"

#include <array>
#include <utility>

namespace gm::equip {
namespace {

constexpr EquipSlot kEmptySlot{};

int field_index(Event kind) {
    return static_cast<int>(kind);
}

} // namespace

bool EquipmentState::refuse(std::string message) {
    error_ = std::move(message);
    return false;
}

void EquipmentState::sync_slot_ammo() {
    for (EquipSlot& slot : slots_) {
        const EquipmentDef* def = this->def_at(slot.id);
        if (def == nullptr) {
            continue;
        }
        const int type = def->ammo_type();
        slot.ammo = (type >= 0 && type < kAmmoTypes) ? ammo_[static_cast<std::size_t>(type)] : 0;
    }
}

const EquipmentDef* EquipmentState::def_at(int slot) const {
    if (slot < 0 || static_cast<std::size_t>(slot) >= defs_.size()) {
        return nullptr;
    }
    return &defs_[static_cast<std::size_t>(slot)];
}

const EquipmentDef* EquipmentState::record(int slot) const { return def_at(slot); }

bool EquipmentState::load(const std::vector<EquipmentDef>& defs, std::string* error) {
    defs_.clear();
    slots_.clear();
    ammo_.fill(0);
    clear_event();
    error_.clear();

    auto fail = [&](std::string message) {
        if (error != nullptr) {
            *error = message;
        }
        refuse(std::move(message));
        return false;
    };

    if (defs.empty()) {
        return fail("DEFINE_EQUIPMENT: empty table, a level must define its items "
                    "(docs/EQUIPMENT.md §2)");
    }
    if (defs.size() > static_cast<std::size_t>(kMaxSlots)) {
        return fail("DEFINE_EQUIPMENT: " + std::to_string(defs.size()) +
                    " records, the store takes at most " + std::to_string(kMaxSlots) +
                    " (0x450e65, docs/EQUIPMENT.md §2)");
    }

    // §4.1: hotkeys are `0x40000 << object_id` for ids 1..10; id 0 has none
    // (SELF-DESTRUCT, FLIGHT PACK, BALLAST) and repeats in level0.
    std::array<bool, 11> hotkey_used{};
    for (std::size_t n = 0; n < defs.size(); ++n) {
        const EquipmentDef& def = defs[n];
        const std::string where = "DEFINE_EQUIPMENT[" + std::to_string(n) + "] '" +
                                  def.name + "': ";
        const int object_id = def.object_id();
        if (object_id < 0 || object_id > 10) {
            return fail(where + "object_id " + std::to_string(object_id) +
                        " outside 0..10 (docs/EQUIPMENT.md §4.1)");
        }
        if (object_id != 0 && hotkey_used[static_cast<std::size_t>(object_id)]) {
            return fail(where + "duplicate object_id " + std::to_string(object_id) +
                        ", a hotkey addresses exactly one record (§4.1)");
        }
        if (object_id != 0) {
            hotkey_used[static_cast<std::size_t>(object_id)] = true;
        }
        const int type = def.ammo_type();
        if (type < kNoAmmoType || type >= kAmmoTypes) {
            return fail(where + "ammo_type " + std::to_string(type) +
                        " outside -1.." + std::to_string(kAmmoTypes - 1) +
                        " (0x81c2dc + 4*E8, docs/EQUIPMENT.md §2.1)");
        }
    }

    defs_ = defs;
    slots_.reserve(defs.size());
    for (std::size_t n = 0; n < defs.size(); ++n) {
        EquipSlot slot;
        slot.id = static_cast<int>(n);
        slots_.push_back(slot);
    }
    sync_slot_ammo();
    return true;
}

int EquipmentState::usable() const {
    int count = 0;
    for (const EquipmentDef& def : defs_) {
        if ((def.flags() & kFlagNotInCycle) == 0) {
            ++count;
        }
    }
    return count;
}

const EquipSlot& EquipmentState::at(int slot) const {
    if (!has(slot)) {
        return kEmptySlot;
    }
    return slots_[static_cast<std::size_t>(slot)];
}

bool EquipmentState::activate(int slot) {
    clear_event();
    const EquipmentDef* def = this->def_at(slot);
    if (def == nullptr) {
        return refuse("activate: slot " + std::to_string(slot) + " outside 0.." +
                      std::to_string(slots() - 1));
    }
    EquipSlot& state = slots_[static_cast<std::size_t>(slot)];
    if (state.active) {
        return refuse("activate: slot " + std::to_string(slot) + " (" + def->name +
                      ") is already active");
    }
    if ((def->flags() & kFlagNotInCycle) != 0) {
        return refuse("activate: slot " + std::to_string(slot) + " (" + def->name +
                      ") has flags & 0x04 — out of the selection cycle, "
                      "docs/EQUIPMENT.md §3");
    }
    const int type = def->ammo_type();
    if (type != kNoAmmoType) {
        if (type < 0 || type >= kAmmoTypes) {
            return refuse("activate: slot " + std::to_string(slot) + " (" + def->name +
                          ") has ammo_type " + std::to_string(type) + " outside the pool");
        }
        if (ammo_[static_cast<std::size_t>(type)] <= 0) {
            last_event_ = event_for(slot, Event::NoAmmo);
            last_slot_ = slot;
            return refuse("activate: slot " + std::to_string(slot) + " (" + def->name +
                          ") has no ammo of type " + std::to_string(type) + " (E6)");
        }
    }
    if (state.cooldown > 0) {
        return refuse("activate: slot " + std::to_string(slot) + " (" + def->name +
                      ") is cooling down, " + std::to_string(state.cooldown) + " tick(s)");
    }
    state.active = true;
    last_event_ = event_for(slot, Event::On);
    last_slot_ = slot;
    return true;
}

void EquipmentState::deactivate(int slot) {
    clear_event();
    const EquipmentDef* def = this->def_at(slot);
    if (def == nullptr) {
        refuse("deactivate: slot " + std::to_string(slot) + " outside 0.." +
               std::to_string(slots() - 1));
        return;
    }
    EquipSlot& state = slots_[static_cast<std::size_t>(slot)];
    if (!state.active) {
        refuse("deactivate: slot " + std::to_string(slot) + " (" + def->name +
               ") is not active");
        return;
    }
    state.active = false;
    last_event_ = event_for(slot, Event::Off);
    last_slot_ = slot;
}

bool EquipmentState::consume(int slot) {
    clear_event();
    const EquipmentDef* def = this->def_at(slot);
    if (def == nullptr) {
        return refuse("consume: slot " + std::to_string(slot) + " outside 0.." +
                      std::to_string(slots() - 1));
    }
    const int type = def->ammo_type();
    if (type == kNoAmmoType) {
        return refuse("consume: slot " + std::to_string(slot) + " (" + def->name +
                      ") has no ammo type (E8 = -1, docs/EQUIPMENT.md §3)");
    }
    if (type < 0 || type >= kAmmoTypes) {
        return refuse("consume: slot " + std::to_string(slot) + " (" + def->name +
                      ") has ammo_type " + std::to_string(type) + " outside the pool");
    }
    std::int32_t& pool = ammo_[static_cast<std::size_t>(type)];
    if (pool <= 0) {
        last_event_ = event_for(slot, Event::NoAmmo);
        last_slot_ = slot;
        return refuse("consume: slot " + std::to_string(slot) + " (" + def->name +
                      ") has no ammo of type " + std::to_string(type));
    }
    --pool;
    sync_slot_ammo();
    last_slot_ = slot;
    if (pool == 0) {
        // §2.1: reaching zero posts E6; the active bit is left alone (the
        // shipped consumables have E6 = 0, §3).
        last_event_ = event_for(slot, Event::NoAmmo);
    }
    return true;
}

void EquipmentState::tick() {
    for (EquipSlot& slot : slots_) {
        if (slot.cooldown > 0) {
            --slot.cooldown;
        }
    }
}

bool EquipmentState::set_cooldown(int slot, int ticks) {
    if (!has(slot)) {
        return refuse("set_cooldown: slot " + std::to_string(slot) + " outside 0.." +
                      std::to_string(slots() - 1));
    }
    if (ticks < 0) {
        return refuse("set_cooldown: slot " + std::to_string(slot) + " got " +
                      std::to_string(ticks) + " ticks, expected >= 0");
    }
    slots_[static_cast<std::size_t>(slot)].cooldown = ticks;
    return true;
}

bool EquipmentState::set_ammo_for_type(int type, int value) {
    if (type < 0 || type >= kAmmoTypes) {
        return refuse("set_ammo_for_type: type " + std::to_string(type) + " outside 0.." +
                      std::to_string(kAmmoTypes - 1));
    }
    if (value < 0) {
        return refuse("set_ammo_for_type: type " + std::to_string(type) + " got " +
                      std::to_string(value) + ", expected >= 0");
    }
    ammo_[static_cast<std::size_t>(type)] = value;
    sync_slot_ammo();
    return true;
}

bool EquipmentState::set_ammo(int slot, int value) {
    const EquipmentDef* def = this->def_at(slot);
    if (def == nullptr) {
        return refuse("set_ammo: slot " + std::to_string(slot) + " outside 0.." +
                      std::to_string(slots() - 1));
    }
    if (def->ammo_type() == kNoAmmoType) {
        return refuse("set_ammo: slot " + std::to_string(slot) + " (" + def->name +
                      ") has no ammo type (E8 = -1)");
    }
    return set_ammo_for_type(def->ammo_type(), value);
}

int EquipmentState::ammo_for_type(int type) const {
    if (type < 0 || type >= kAmmoTypes) {
        return 0;
    }
    return ammo_[static_cast<std::size_t>(type)];
}

void EquipmentState::sync_ammo(const AmmoPool& ammo) {
    ammo_ = ammo;
    sync_slot_ammo();
}

std::uint32_t EquipmentState::event_for(int slot, Event kind) const {
    const EquipmentDef* def = this->def_at(slot);
    if (def == nullptr) {
        return 0;
    }
    const int index = field_index(kind);
    if (index < 0 || static_cast<std::size_t>(index) >= def->fields.size()) {
        return 0;
    }
    // The record stores signed ids (-30043 …); the VM posts the u32 bit
    // pattern, so keep it verbatim (§2).
    return static_cast<std::uint32_t>(def->fields[static_cast<std::size_t>(index)]);
}

} // namespace gm::equip
