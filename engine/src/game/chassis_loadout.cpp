#include "game/chassis_loadout.h"

#include "res/sbt.h"
#include "res/sbtvm.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

namespace gm {
namespace {

void fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool value_at(const PlayerTypeDef& def, std::size_t index, double& out) {
    if (index >= def.values.size()) {
        return false;
    }
    out = def.values[index];
    return std::isfinite(out);
}

bool int_at(const PlayerTypeDef& def, std::size_t index, int& out) {
    double value = 0.0;
    if (!value_at(def, index, value) || value != std::floor(value)) {
        return false;
    }
    if (value < -1.0 || value > 255.0) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

/// The sound operand is a u32 in the record: `-1` (no sound) is stored as
/// `0xFFFFFFFF`, so both spellings map to -1.
bool sound_at(const PlayerTypeDef& def, int& out) {
    double value = 0.0;
    if (!value_at(def, kChassisSound, value) || value != std::floor(value)) {
        return false;
    }
    if (value == 4294967295.0) {
        out = -1;
        return true;
    }
    if (value < -1.0 || value > 255.0) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

void walk(const std::vector<SbtInsn>& insns, std::vector<ChassisSpec>& out) {
    for (const SbtInsn& insn : insns) {
        if (insn.name == "DEFINE_PLAYER_TYPES" && !insn.table.empty()) {
            for (const SbtTableRecord& record : insn.table) {
                PlayerTypeDef def;
                def.strings = record.strings;
                if (!def.strings.empty()) {
                    def.name = def.strings.front();
                }
                def.values.reserve(record.values.size());
                for (const SbtValue& value : record.values) {
                    def.values.push_back(value.kind == SbtValue::Kind::Real
                                             ? value.d
                                             : static_cast<double>(value.u));
                }
                if (auto spec = chassis_spec(def); spec) {
                    out.push_back(std::move(*spec));
                }
            }
        }
        if (!insn.body.empty()) {
            walk(insn.body, out);
        }
    }
}

} // namespace

std::optional<ChassisSpec> chassis_spec(const PlayerTypeDef& def,
                                        std::string* error) {
    if (def.values.size() < kChassisFieldsNeeded) {
        fail(error, "chassis '" + def.name + "': " +
                        std::to_string(def.values.size()) +
                        " numeric fields, need at least " +
                        std::to_string(kChassisFieldsNeeded));
        return std::nullopt;
    }

    ChassisSpec spec;
    spec.name = def.name;
    const std::pair<std::size_t, double*> reals[] = {
        {kChassisMass, &spec.mass},
        {kChassisWeightPenalty, &spec.weight_penalty},
        {kChassisArmour, &spec.armour},
        {kChassisArmourLimit, &spec.armour_limit},
        {kChassisMaxSpeed, &spec.max_speed},
        {kChassisEyeHeight, &spec.eye_height},
        {kChassisPrice, &spec.price},
    };
    for (const auto& [index, target] : reals) {
        if (!value_at(def, index, *target)) {
            fail(error, "chassis '" + spec.name + "': field " +
                            std::to_string(index) + " is not a finite number");
            return std::nullopt;
        }
        if (*target < 0.0) {
            fail(error, "chassis '" + spec.name + "': field " +
                            std::to_string(index) + " is negative");
            return std::nullopt;
        }
    }
    // The two armour operands are stored `*100` (`0x47ac2d`/`0x47ac41`).
    spec.armour *= kChassisArmourScale;
    spec.armour_limit *= kChassisArmourScale;

    if (!int_at(def, kChassisModel, spec.model_index)) {
        fail(error, "chassis '" + spec.name + "': model index is not 0..255");
        return std::nullopt;
    }
    if (!sound_at(def, spec.sound_index)) {
        fail(error, "chassis '" + spec.name + "': sound index is not -1..255");
        return std::nullopt;
    }
    return spec;
}

ChassisMotion chassis_motion(const ChassisSpec& spec) {
    ChassisMotion motion;
    motion.max_speed = spec.max_speed;
    motion.weight_penalty = spec.weight_penalty;
    motion.weight = 0.0;
    motion.weight_limit = spec.mass;
    motion.health_factor = 1.0;
    motion.retention = 0.9;
    return motion;
}

std::vector<ChassisSpec> collect_chassis(const std::vector<SbtInsn>& insns) {
    std::vector<ChassisSpec> out;
    walk(insns, out);
    return out;
}

std::optional<std::vector<ChassisSpec>> chassis_from_level(
    const std::filesystem::path& level_path, std::string* error) {
    std::ifstream in(level_path, std::ios::binary);
    if (!in) {
        fail(error, "cannot open " + level_path.string());
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    std::string parse_error;
    std::optional<SbtLevel> level = parse_sbt(bytes, parse_error);
    if (!level) {
        fail(error, level_path.string() + ": " + parse_error);
        return std::nullopt;
    }
    if (level->script_code.empty()) {
        fail(error, level_path.string() + ": no SCRIPT bytecode");
        return std::nullopt;
    }
    std::optional<SbtProgram> program =
        decode_sbt_script(level->script_code, parse_error);
    if (!program) {
        fail(error, level_path.string() + ": script: " + parse_error);
        return std::nullopt;
    }
    return collect_chassis(program->insns);
}

} // namespace gm
