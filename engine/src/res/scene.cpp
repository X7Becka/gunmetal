#include "res/scene.h"

#include "core/bytes.h"
#include "res/sbtvm.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace gm {
namespace {

int32_t as_i32(const SbtValue& value) {
    if (value.kind == SbtValue::Kind::Real) {
        return static_cast<std::int32_t>(value.d);
    }
    if (value.kind == SbtValue::Kind::String) {
        return 0;
    }
    return static_cast<std::int32_t>(value.u);
}

double as_f64(const SbtValue& value) {
    if (value.kind == SbtValue::Kind::Real) {
        return value.d;
    }
    if (value.kind == SbtValue::Kind::String) {
        return 0.0;
    }
    return static_cast<double>(value.u);
}

int channel(const SbtValue& value) {
    return static_cast<int>(as_i32(value) & 0xFF);
}

const SbtValue* arg(const SbtInsn& insn, std::size_t index) {
    return index < insn.args.size() ? &insn.args[index] : nullptr;
}

void walk(const std::vector<SbtInsn>& insns, Scene& scene) {
    for (const auto& insn : insns) {
        if (insn.name == "ADD_SKY_LIGHT_SOURCE" && insn.args.size() >= 7) {
            SceneLight light;
            light.x = as_i32(insn.args[0]);
            light.y = as_i32(insn.args[1]);
            light.z = as_i32(insn.args[2]);
            light.param = as_i32(insn.args[3]);
            light.r = channel(insn.args[4]);
            light.g = channel(insn.args[5]);
            light.b = channel(insn.args[6]);
            light.kind = "sky";
            scene.lights.push_back(std::move(light));
        } else if (insn.name == "ADD_LIGHT_SOURCE" && insn.args.size() >= 3) {
            SceneLight light;
            light.x = as_i32(insn.args[0]);
            light.y = as_i32(insn.args[1]);
            light.z = as_i32(insn.args[2]);
            light.kind = "local";
            light.param = insn.args.size() > 3 ? as_i32(insn.args[3]) : 0;
            if (insn.args.size() >= 7) {
                light.r = channel(insn.args[4]);
                light.g = channel(insn.args[5]);
                light.b = channel(insn.args[6]);
            }
            if (insn.args.size() >= 12) {
                for (std::size_t k = 9; k < 12; ++k) {
                    light.extra.push_back(as_i32(insn.args[k]));
                }
            }
            scene.lights.push_back(std::move(light));
        } else if (insn.name == "ADD_LOCATION" && insn.args.size() >= 7) {
            SceneLocation location;
            location.id = as_i32(insn.args[0]);
            location.x = as_i32(insn.args[1]);
            location.y = as_i32(insn.args[2]);
            location.z = as_i32(insn.args[3]);
            for (int k = 0; k < 3; ++k) {
                location.orientation[static_cast<std::size_t>(k)] =
                    as_f64(insn.args[static_cast<std::size_t>(4 + k)]);
            }
            scene.locations.push_back(std::move(location));
        } else if ((insn.name == "ADD_ITEM" || insn.name == "ADD_ITEM_FIXED") &&
                   insn.args.size() >= 8) {
            SceneItem item;
            item.id = as_i32(insn.args[1]);
            item.x = as_i32(insn.args[5]);
            item.y = as_i32(insn.args[6]);
            item.z = as_i32(insn.args[7]);
            item.fixed = insn.name == "ADD_ITEM_FIXED";
            for (int k = 0; k < 5; ++k) {
                item.extra[static_cast<std::size_t>(k)] =
                    as_i32(insn.args[static_cast<std::size_t>(k)]);
            }
            scene.items.push_back(std::move(item));
        } else if (insn.name == "ADD_SOUND_SOURCE" && insn.args.size() >= 3) {
            SceneSound sound;
            sound.x = as_i32(insn.args[0]);
            sound.y = as_i32(insn.args[1]);
            sound.z = as_i32(insn.args[2]);
            sound.sample = insn.args.size() > 3 ? as_i32(insn.args[3]) : 0;
            sound.volume = insn.args.size() > 4 ? as_i32(insn.args[4]) : 0;
            sound.pitch = insn.args.size() > 5 ? as_f64(insn.args[5]) : 0.0;
            scene.sounds.push_back(std::move(sound));
        } else if (insn.name == "ADD_SPRITE" && insn.args.size() >= 9) {
            SceneSprite sprite;
            sprite.x = as_f64(insn.args[1]);
            sprite.y = as_f64(insn.args[2]);
            sprite.z = as_f64(insn.args[3]);
            sprite.jitter = as_i32(insn.args[8]);
            sprite.id = as_i32(insn.args[0]);
            for (int k = 0; k < 5; ++k) {
                sprite.extra[static_cast<std::size_t>(k)] =
                    as_i32(insn.args[static_cast<std::size_t>(4 + k)]);
            }
            scene.sprites.push_back(std::move(sprite));
        } else if (insn.name == "ADD_DOOR" && insn.args.size() >= 7) {
            SceneDoor door;
            door.flags = as_i32(insn.args[0]);
            for (int k = 0; k < 5; ++k) {
                door.ids[static_cast<std::size_t>(k)] =
                    as_i32(insn.args[static_cast<std::size_t>(1 + k)]);
            }
            for (int k = 0; k < 3; ++k) {
                door.angles[static_cast<std::size_t>(k)] =
                    as_f64(insn.args[static_cast<std::size_t>(6 + k)]);
            }
            for (int k = 0; k < 7; ++k) {
                door.params[static_cast<std::size_t>(k)] =
                    as_i32(insn.args[static_cast<std::size_t>(9 + k)]);
            }
            for (int k = 0; k < 9; ++k) {
                door.values[static_cast<std::size_t>(k)] =
                    as_f64(insn.args[static_cast<std::size_t>(16 + k)]);
            }
            for (int k = 0; k < 6; ++k) {
                door.tail[static_cast<std::size_t>(k)] =
                    as_i32(insn.args[static_cast<std::size_t>(25 + k)]);
            }
            scene.doors.push_back(std::move(door));
        } else if (insn.name == "ADD_SWITCH" && insn.args.size() >= 6) {
            SceneSwitch sw;
            sw.kind = as_i32(insn.args[0]);
            sw.id = as_i32(insn.args[1]);
            sw.radius = as_i32(insn.args[2]);
            for (int k = 0; k < 3; ++k) {
                sw.pos[static_cast<std::size_t>(k)] =
                    as_i32(insn.args[static_cast<std::size_t>(3 + k)]);
            }
            for (int k = 0; k < 2; ++k) {
                sw.extra[static_cast<std::size_t>(k)] =
                    as_i32(insn.args[static_cast<std::size_t>(6 + k)]);
            }
            scene.switches.push_back(std::move(sw));
        } else if (insn.name == "ADD_NAMED_ITEM" || insn.name == "ADD_NAMED_ITEM_EX" ||
                   insn.name == "ADD_SPRITE_AT_SOURCE" ||
                   insn.name == "ADD_SPRITE_MOVING" ||
                   insn.name == "ADD_SPRITE_MOVING_AT_SOURCE" ||
                   insn.name == "ADD_ITEM_AT_SOURCE" ||
                   insn.name == "ADD_ITEM_AT_SOURCE_FIXED" ||
                   insn.name == "ADD_WEAPON_AT_SOURCE") {
            ++scene.other[insn.name];
        }
        if (!insn.body.empty()) {
            walk(insn.body, scene);
        }
    }
}

void extract_things(const SbtLevel& level, Scene& scene) {
    const auto& code = level.script_code;
    for (const auto& entity : scene.entities) {
        if (entity.offset >= code.size()) {
            continue;
        }
        std::string error;
        const std::span<const std::uint8_t> sub(code.data() + entity.offset,
                                                code.size() - entity.offset);
        const auto program = decode_sbt_script(sub, error);
        if (!program) {
            continue;
        }
        for (const auto& insn : program->insns) {
            if (insn.name == "END") {
                break;
            }
            if (insn.opcode != 0xC9 || insn.condition.empty()) {
                continue;
            }
            if (insn.condition.rfind("ADD_THING", 0) != 0) {
                continue;
            }
            const std::string signature = sbt_condition_signature(insn.condition);
            int type_index = -1;
            bool has_type = false;
            int owner = 0;
            std::vector<int> ints;
            std::vector<double> doubles;
            for (std::size_t k = 0; k < signature.size() && k < insn.args.size(); ++k) {
                const char kind = signature[k];
                const SbtValue& value = insn.args[k];
                if (kind == 'G') {
                    type_index = as_i32(value);
                    has_type = true;
                } else if (kind == 'A' || kind == 'B' || kind == 'D') {
                    doubles.push_back(as_f64(value));
                } else if (value.kind != SbtValue::Kind::String) {
                    if (!has_type && ints.empty()) {
                        owner = as_i32(value);
                    } else {
                        ints.push_back(as_i32(value));
                    }
                }
            }
            if (!has_type) {
                continue;
            }
            SceneThing thing;
            thing.entity = entity.id;
            thing.kind = insn.condition;
            thing.type_index = type_index;
            thing.absolute = (insn.condition == "ADD_THING" ||
                              insn.condition == "ADD_THING_EX") &&
                             ints.size() >= 3;
            if (thing.absolute) {
                thing.x = ints[0];
                thing.y = ints[1];
                thing.z = ints[2];
            }
            thing.angles = std::move(doubles);
            thing.owner = owner;
            scene.things.push_back(std::move(thing));
        }
    }
}

} // namespace

bool SceneDoor::moving() const {
    for (int k = 0; k < 3; ++k) {
        if (std::abs(values[static_cast<std::size_t>(k)] -
                     values[static_cast<std::size_t>(k + 3)]) > 1e-6) {
            return true;
        }
    }
    return false;
}

std::optional<Scene> extract_scene(const SbtLevel& level, std::string& error,
                                  bool with_things) {
    const std::vector<std::uint8_t>* code = nullptr;
    for (const auto& record : level.records) {
        if (record.type == 11) {
            code = &level.script_code;
            break;
        }
    }
    if (code == nullptr || code->empty()) {
        error = "no SCRIPT record with bytecode";
        return std::nullopt;
    }
    const auto program = decode_sbt_script(*code, error);
    if (!program) {
        return std::nullopt;
    }

    Scene scene;
    // Entity table: n, then three columns of n u32 (flags, ids, sub-script
    // offsets); gm/scene.py's entities().
    const std::size_t n = level.script.handles;
    if (n > 0 && level.script_handles_data.size() >= n * 12) {
        for (std::size_t i = 0; i < n; ++i) {
            SceneEntity entity;
            entity.flags = rd32(level.script_handles_data.data() + i * 4);
            entity.id = static_cast<std::int32_t>(rd32(level.script_handles_data.data() +
                                                       (n + i) * 4));
            entity.offset = rd32(level.script_handles_data.data() + (2 * n + i) * 4);
            scene.entities.push_back(entity);
        }
    }
    walk(program->insns, scene);
    if (with_things) {
        extract_things(level, scene);
    }
    return scene;
}

} // namespace gm
