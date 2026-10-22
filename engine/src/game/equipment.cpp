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
