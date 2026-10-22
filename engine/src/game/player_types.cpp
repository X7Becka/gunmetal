#include "game/player_types.h"

#include <utility>

namespace gm {
namespace {

bool field_value(const SbtValue& value, bool real, double& out) {
    if (real) {
        if (value.kind != SbtValue::Kind::Real || value.d != value.d) {
            return false;
        }
        out = value.d;
        return true;
    }
    if (value.kind != SbtValue::Kind::UInt) {
        return false;
    }
    out = static_cast<double>(value.u);
    return true;
}

void fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

} // namespace

std::optional<PlayerTypeTable> PlayerTypeTable::from_records(
    const std::vector<SbtTableRecord>& records, std::string* error) {
    if (records.size() > kMaxRecords) {
        fail(error, "DEFINE_PLAYER_TYPES: " + std::to_string(records.size()) +
                        " records, expected at most " + std::to_string(kMaxRecords));
        return std::nullopt;
    }
    PlayerTypeTable table;
    table.defs.reserve(records.size());
    for (std::size_t n = 0; n < records.size(); ++n) {
        const SbtTableRecord& record = records[n];
        if (record.values.size() != kFieldCount) {
            fail(error, "DEFINE_PLAYER_TYPES[" + std::to_string(n) + "]: " +
                            std::to_string(record.values.size()) + " fields, expected " +
                            std::to_string(kFieldCount));
            return std::nullopt;
        }
        PlayerTypeDef def;
        def.strings = record.strings;
        if (!def.strings.empty()) {
            def.name = def.strings.front();
        }
        def.values.resize(kFieldCount);
        for (std::size_t k = 0; k < kFieldCount; ++k) {
            if (!field_value(record.values[k], player_type_field_is_real(k), def.values[k])) {
                fail(error, "DEFINE_PLAYER_TYPES[" + std::to_string(n) + "]: field " +
                                std::to_string(k) + " has the wrong type");
                return std::nullopt;
            }
        }
        table.defs.push_back(std::move(def));
    }
    return table;
}

const PlayerTypeDef* PlayerTypeTable::find(std::string_view name) const {
    for (const PlayerTypeDef& def : defs) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

} // namespace gm
