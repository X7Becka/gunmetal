#include "game/equipment.h"

#include <algorithm>

namespace gm {
namespace {

bool to_i32(const SbtValue& value, std::int32_t& out) {
    switch (value.kind) {
    case SbtValue::Kind::UInt:
        out = static_cast<std::int32_t>(value.u);
        return true;
    case SbtValue::Kind::Real:
        if (value.d != value.d) { // NaN
            return false;
        }
        out = static_cast<std::int32_t>(value.d);
        return true;
    case SbtValue::Kind::String:
        return false;
    }
    return false;
}

void fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

} // namespace

std::optional<EquipmentTable> EquipmentTable::from_records(
    const std::vector<SbtTableRecord>& records, std::string* error) {
    if (records.size() > kMaxRecords) {
        fail(error, "DEFINE_EQUIPMENT: " + std::to_string(records.size()) +
                        " records, the store takes at most " +
                        std::to_string(kMaxRecords));
        return std::nullopt;
    }
    EquipmentTable table;
    table.defs.reserve(records.size());
    for (std::size_t n = 0; n < records.size(); ++n) {
        const SbtTableRecord& record = records[n];
        if (record.values.size() != 12) {
            fail(error, "DEFINE_EQUIPMENT[" + std::to_string(n) + "]: " +
                            std::to_string(record.values.size()) +
                            " values, expected 12");
            return std::nullopt;
        }
        EquipmentDef def;
        if (!record.strings.empty()) {
            def.name = record.strings.front();
        }
        for (std::size_t k = 0; k < def.fields.size(); ++k) {
            if (!to_i32(record.values[k], def.fields[k])) {
                fail(error, "DEFINE_EQUIPMENT[" + std::to_string(n) + "]: field " +
                                std::to_string(k) + " is not a number");
                return std::nullopt;
            }
        }
        table.defs.push_back(std::move(def));
    }
    return table;
}

const EquipmentDef* EquipmentTable::find(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= defs.size()) {
        return nullptr;
    }
    return &defs[static_cast<std::size_t>(index)];
}

void EquipmentState::set_active(int record, bool on) {
    if (record < 0 || record >= 32) {
        return;
    }
    if (on) {
        active |= 1u << record;
    } else {
        active &= ~(1u << record);
    }
}

namespace {

/// `owned & !(flags & 4)`: bought and selectable by cycling.
bool cycle_selectable(const EquipmentTable& table, const EquipmentState& state,
                      int record) {
    const EquipmentDef* def = table.find(record);
    return def != nullptr && state.is_owned(record) && (def->flags() & 4) == 0;
}

} // namespace

EquipmentInput equipment_input(const EquipmentTable& table, EquipmentState& state,
                               std::uint32_t actions) {
    EquipmentInput out;
    const int count = static_cast<int>(table.size());
    if (count == 0) {
        return out;
    }

    // 0x4460ab / 0x4461a5: next and previous owned item, wrapping.
    const int dir = (actions & kEquipNext) ? 1 : ((actions & kEquipPrev) ? -1 : 0);
    if (dir != 0) {
        const int from = state.index;
        for (int step = 1; step <= count; ++step) {
            const int record = ((from + dir * step) % count + count) % count;
            if (!cycle_selectable(table, state, record)) {
                continue;
            }
            state.index = record;
            out.record = record;
            // E2 is the "selection changed" event; in the shipped data it is
            // always 0 (docs/EQUIPMENT.md §4.1).
            out.event = table.defs[static_cast<std::size_t>(record)].activate_event();
            return out;
        }
        return out;
    }

    // 0x4461b1..0x446234: toggle the current item; the script flips the bit.
    if (actions & kEquipToggle) {
        const EquipmentDef* def = table.find(state.index);
        if (def != nullptr) {
            out.record = state.index;
            out.event = state.is_active(state.index) ? def->event_off()
                                                     : def->event_on();
        }
        return out;
    }

    // 0x44623d..0x44638d: hotkey `1 << 10 << object_id` (ids 1..10).
    for (int id = 1; id <= 10; ++id) {
        const std::uint32_t bit = kEquipHotkeyBase << (id - 1);
        if ((actions & bit) == 0) {
            continue;
        }
        for (int record = 0; record < count; ++record) {
            const EquipmentDef& def = table.defs[static_cast<std::size_t>(record)];
            if (def.object_id() != id || !state.is_owned(record)) {
                continue;
            }
            out.record = record;
            out.event = state.is_active(record) ? def.event_off() : def.event_on();
            return out;
        }
        return out;
    }
    return out;
}

std::vector<int> equipment_tick(const EquipmentTable& table,
                                const EquipmentState& state, AmmoPool& ammo,
                                int ticks) {
    std::vector<int> drained;
    if (ticks <= 0) {
        return drained;
    }
    for (std::size_t n = 0; n < table.defs.size(); ++n) {
        const int record = static_cast<int>(n);
        if (!state.is_active(record)) {
            continue;
        }
        const EquipmentDef& def = table.defs[n];
        const std::int32_t per_tick = def.ammo_per_tick();
        if (per_tick <= 0) {
            continue;
        }
        const std::int32_t type = def.ammo_type();
        if (type < 0 || static_cast<std::size_t>(type) >= ammo.size()) {
            continue;
        }
        std::int32_t& pool = ammo[static_cast<std::size_t>(type)];
        const std::int64_t spend = static_cast<std::int64_t>(per_tick) * ticks;
        if (pool <= 0) {
            // Already empty: the tick has nothing to spend but the item is
            // still on, so the empty event fires only on the transition.
            continue;
        }
        const std::int64_t left = static_cast<std::int64_t>(pool) - spend;
        if (left <= 0) {
            pool = 0;
            drained.push_back(record);
        } else {
            pool = static_cast<std::int32_t>(left);
        }
    }
    return drained;
}

} // namespace gm
