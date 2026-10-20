#include "world/things.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <span>
#include <utility>

#include "core/bytes.h"
#include "res/scene.h"
#include "res/sbtvm.h"

namespace gm {
namespace {

constexpr std::size_t kThingTypeSize = 936;
constexpr std::size_t kThingModelOffset = 0x1A4;
constexpr std::size_t kThingTypeHeader = 4;

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

std::string fixed_name(const std::uint8_t* data, std::size_t size) {
    std::size_t n = 0;
    while (n < size && data[n] != 0) {
        ++n;
    }
    return std::string(reinterpret_cast<const char*>(data), n);
}

bool is_thing_kind(const std::string& name) {
    return name.rfind("ADD_THING", 0) == 0;
}

/// THING_TYPES (+0x1a4 model index) and SOB_TABLE names of one level.
struct TypeTable {
    std::vector<std::string> names;
    std::vector<int> model_indices;
    std::vector<std::string> models;
    std::vector<int> movement_types;
    std::vector<double> search_radii;
    std::vector<int> target_zone_thresholds;
    std::vector<int> target_history_offsets;
    std::vector<int> max_health;
    std::size_t valid_models = 0;
    std::size_t missing_models = 0;
};

TypeTable resolve_types(const SbtLevel& level) {
    TypeTable table;
    table.models.reserve(level.sobs.size());
    for (const SbtSob& sob : level.sobs) {
        table.models.push_back(sob.name);
    }
    for (const SbtRecord& record : level.records) {
        if (record.type != 4 || record.payload.size() < kThingTypeHeader) {
            continue;
        }
        const std::uint32_t count = rd32(record.payload.data());
        if (count > 0x80 ||
            kThingTypeHeader + static_cast<std::size_t>(count) * kThingTypeSize >
                record.payload.size()) {
            return table;
        }
        table.names.reserve(count);
        table.model_indices.reserve(count);
        table.movement_types.reserve(count);
        table.search_radii.reserve(count);
        table.target_zone_thresholds.reserve(count);
        table.target_history_offsets.reserve(count);
        table.max_health.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* entry =
                record.payload.data() + kThingTypeHeader +
                static_cast<std::size_t>(i) * kThingTypeSize;
            table.names.push_back(fixed_name(entry, 16));
            const std::uint32_t raw_model = rd32(entry + kThingModelOffset);
            if (raw_model < table.models.size()) {
                table.model_indices.push_back(static_cast<int>(raw_model));
                ++table.valid_models;
            } else {
                table.model_indices.push_back(-1);
                ++table.missing_models;
            }
            table.movement_types.push_back(static_cast<std::int32_t>(rd32(entry + 0x1b4)));
            table.search_radii.push_back(std::bit_cast<double>(rd64(entry + 0x58)));
            table.target_zone_thresholds.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x1cc)));
            table.target_history_offsets.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x1d0)));
            table.max_health.push_back(static_cast<std::int32_t>(rd32(entry + 0x1d4)));
        }
        return table;
    }
    return table;
}

void apply_type_fields(Thing& thing, const TypeTable& types) {
    if (thing.type < 0 ||
        static_cast<std::size_t>(thing.type) >= types.names.size()) {
        return;
    }
    const auto i = static_cast<std::size_t>(thing.type);
    thing.movement_type = types.movement_types[i];
    thing.search_radius = types.search_radii[i];
    thing.target_zone_threshold = types.target_zone_thresholds[i];
    thing.target_history_offset = types.target_history_offsets[i];
    thing.max_health = types.max_health[i];
}

/// Split an ADD_THING* operand list exactly like gm/scene.py's _thing_args:
/// ``G`` is the type, ``A``/``B``/``D`` are doubles, and integers before the
/// type overwrite ``owner`` (they are not positions).
struct ThingArgs {
    bool has_type = false;
    int type = -1;
    int owner = 0;
    std::vector<int> ints;
    std::vector<double> doubles;
};

ThingArgs split_args(const SbtInsn& insn) {
    ThingArgs out;
    const std::string signature = sbt_condition_signature(insn.condition);
    for (std::size_t k = 0; k < signature.size() && k < insn.args.size(); ++k) {
        const char letter = signature[k];
        const SbtValue& value = insn.args[k];
        if (letter == 'G') {
            out.type = as_i32(value);
            out.has_type = true;
        } else if (letter == 'A' || letter == 'B' || letter == 'D') {
            out.doubles.push_back(as_f64(value));
        } else if (value.kind != SbtValue::Kind::String) {
            if (!out.has_type && out.ints.empty()) {
                out.owner = as_i32(value);
            } else {
                out.ints.push_back(as_i32(value));
            }
        }
    }
    return out;
}

/// Fill the shared fields of a Thing from an ADD_THING* EVAL instruction.
Thing make_thing(const SbtInsn& insn, int parent, std::uint32_t flags,
                 std::uint32_t script, const TypeTable& types, bool executed,
                 const std::vector<std::string>& path, int depth,
                 std::uint32_t base_offset) {
    const ThingArgs args = split_args(insn);
    Thing thing;
    thing.type = args.type;
    thing.kind = insn.condition;
    thing.owner = args.owner;
    thing.flags = flags;
    thing.parent = parent;
    thing.script = script;
    thing.site = base_offset + insn.offset;
    thing.depth = depth;
    thing.path = path;
    thing.executed = executed;
    if (args.has_type && args.type >= 0 &&
        static_cast<std::size_t>(args.type) < types.names.size()) {
        const auto index = static_cast<std::size_t>(args.type);
        thing.type_name = types.names[index];
        thing.model_index = types.model_indices[index];
        if (thing.model_index >= 0 &&
            static_cast<std::size_t>(thing.model_index) < types.models.size()) {
            thing.model = types.models[static_cast<std::size_t>(thing.model_index)];
        }
    }
    const bool absolute =
        insn.condition == "ADD_THING" || insn.condition == "ADD_THING_EX";
    if (absolute && args.ints.size() >= 3) {
        thing.position = std::array<int, 3>{args.ints[0], args.ints[1], args.ints[2]};
    }
    thing.angles = args.doubles;
    return thing;
}

std::string label(const SbtInsn& insn) {
    if (insn.name == "IF") {
        return "IF " + insn.condition;
    }
    if (insn.name == "ELSE") {
        return "ELSE";
    }
    return insn.name;
}

enum class Walk {
    Continue,
    Stop, // END: the whole sub-script stops (not only the current body)
    Error,
};

struct RecursiveWalk {
    const TypeTable& types;
    int parent = 0;
    std::uint32_t flags = 0;
    std::uint32_t script = 0;
    std::uint32_t base_offset = 0;
    std::vector<Thing> things;
    std::set<std::uint32_t> sites;
    std::size_t duplicates = 0;
    std::string error;

    explicit RecursiveWalk(const TypeTable& table) : types(table) {}

    Walk visit(const std::vector<SbtInsn>& insns,
               const std::vector<std::string>& path, int depth) {
        for (const SbtInsn& insn : insns) {
            if (insn.name == "END") {
                return Walk::Stop;
            }
            if (insn.opcode == 0xC9 && is_thing_kind(insn.condition)) {
                const std::uint32_t site = base_offset + insn.offset;
                if (sites.insert(site).second) {
                    things.push_back(make_thing(insn, parent, flags, script, types,
                                                false, path, depth, base_offset));
                } else {
                    ++duplicates;
                }
            }
            if (!insn.body.empty()) {
                if (depth + 1 > kThingMaxDepth) {
                    error = "ADD_THING nesting deeper than " +
                            std::to_string(kThingMaxDepth) + " at 0x" +
                            std::to_string(base_offset + insn.offset);
                    return Walk::Error;
                }
                std::vector<std::string> inner = path;
                inner.push_back(label(insn));
                const Walk result = visit(insn.body, inner, depth + 1);
                if (result != Walk::Continue) {
                    return result;
                }
            }
        }
        return Walk::Continue;
    }
};

std::vector<Thing> top_level_things(const SbtProgram& program,
                                    const TypeTable& types, int parent,
                                    std::uint32_t flags, std::uint32_t script,
                                    std::uint32_t base_offset) {
    std::vector<Thing> out;
    for (const SbtInsn& insn : program.insns) {
        if (insn.name == "END") {
            break;
        }
        if (insn.opcode != 0xC9 || !is_thing_kind(insn.condition)) {
            continue;
        }
        out.push_back(make_thing(insn, parent, flags, script, types, false, {}, 0,
                                 base_offset));
    }
    return out;
}

VmState context_state(const SbtLevel& level, const VmState& base,
                      const ThingContext& context) {
    VmContext config;
    config.game_mode = context.game_mode;
    config.difficulty = base.difficulty;
    config.player_count = std::max(1, base.player_count);
    config.players = config.player_count;
    config.level_number = base.level_number;
    config.god_mode = base.god_mode;
    config.seed = base.rng;
    config.playertype = context.playertype;
    config.source_player = base.source_player;
    config.mp_flags = base.mp_flags;
    return vm_state_from_level(level, config);
}

std::uint64_t site_occ_key(std::uint32_t site, int occ) {
    return (static_cast<std::uint64_t>(site) << 32) |
           static_cast<std::uint32_t>(occ);
}

struct PathInfo {
    std::vector<std::string> path;
    int depth = 0;
};

/// The decoder walks the whole SCRIPT record (every entity block is top-level
/// code separated by 0xFF).  Decoding one sub-span per entity would re-parse
/// all following blocks; slice the one full decode instead.  The slice ends at
/// END, which is exactly where the static walker and the VM stop.
std::optional<SbtProgram> block_program(
    const SbtProgram& full, const std::map<std::uint32_t, std::size_t>& index,
    std::uint32_t offset) {
    const auto found = index.find(offset);
    if (found == index.end()) {
        return std::nullopt;
    }
    SbtProgram block;
    for (std::size_t i = found->second; i < full.insns.size(); ++i) {
        block.insns.push_back(full.insns[i]);
        if (full.insns[i].name == "END") {
            block.end = i + 1;
            break;
        }
    }
    if (block.insns.empty()) {
        return std::nullopt;
    }
    return block;
}

} // namespace

std::vector<ThingContext> default_thing_contexts() {
    std::vector<ThingContext> out;
    for (int mode = 0; mode <= 1; ++mode) {
        for (int playertype = 0; playertype <= 3; ++playertype) {
            out.push_back(ThingContext{mode, playertype});
        }
    }
    return out;
}

Things Things::load(const SbtLevel& level, const VmState& vm,
                    const ThingsOptions& options) {
    Things out;

    const TypeTable types = resolve_types(level);
    out.stats_.thing_types = types.names.size();
    out.stats_.valid_model_refs = types.valid_models;
    out.stats_.missing_model_refs = types.missing_models;

    std::string scene_error;
    const auto scene = extract_scene(level, scene_error, false);
    if (!scene) {
        out.error_ = scene_error.empty() ? "cannot decode SCRIPT" : scene_error;
        out.stats_.decode_error = true;
        return out;
    }
    out.stats_.spawns_static = scene->spawns.size();

    std::map<int, std::uint32_t> entity_flags;
    std::map<int, std::uint32_t> entity_offsets;
    for (const SceneEntity& entity : scene->entities) {
        entity_flags[entity.id] = entity.flags;
        entity_offsets[entity.id] = entity.offset;
    }

    const std::vector<std::uint8_t>& code = level.script_code;
    SbtProgram full_program;
    std::map<std::uint32_t, std::size_t> full_index;
    {
        std::string error;
        const auto decoded = decode_sbt_script(code, error);
        if (decoded) {
            full_program = *decoded;
            for (std::size_t i = 0; i < full_program.insns.size(); ++i) {
                full_index.emplace(full_program.insns[i].offset, i);
            }
        }
    }
    if (options.build_static) {
        std::set<std::uint32_t> sites;
        for (const SceneEntity& entity : scene->entities) {
            if (entity.offset >= code.size()) {
                continue;
            }
            std::string error;
            std::optional<SbtProgram> sliced =
                block_program(full_program, full_index, entity.offset);
            std::optional<SbtProgram> decoded;
            if (!sliced) {
                const std::span<const std::uint8_t> block(
                    code.data() + entity.offset, code.size() - entity.offset);
                decoded = decode_sbt_script(block, error);
            }
            if (!sliced && !decoded) {
                out.stats_.decode_error = true;
                if (out.error_.empty()) {
                    out.error_ = "thing block 0x" + std::to_string(entity.offset) +
                                 ": " + error;
                }
                continue;
            }
            const SbtProgram* program = sliced ? &*sliced : &*decoded;

            // A sliced block keeps the absolute offsets of the full decode;
            // a fallback sub-span decode is relative to the block.
            const std::uint32_t site_base = sliced ? 0u : entity.offset;
            std::vector<Thing> top = top_level_things(
                *program, types, entity.id, entity.flags, entity.offset, site_base);

            RecursiveWalk walk{types};
            walk.parent = entity.id;
            walk.flags = entity.flags;
            walk.script = entity.offset;
            walk.base_offset = site_base;
            walk.sites = sites;
            const Walk result = walk.visit(program->insns, {}, 0);
            if (result == Walk::Error) {
                out.stats_.nesting_limit = true;
                if (out.error_.empty()) {
                    out.error_ = walk.error;
                }
                if (!options.strict) {
                    // Keep the historical top-level view, skip only the
                    // recursive walk of this block.
                    out.top_level_.insert(out.top_level_.end(),
                                          std::make_move_iterator(top.begin()),
                                          std::make_move_iterator(top.end()));
                }
                continue;
            }

            out.top_level_.insert(out.top_level_.end(),
                                  std::make_move_iterator(top.begin()),
                                  std::make_move_iterator(top.end()));
            out.stats_.static_site_duplicates += walk.duplicates;
            sites = std::move(walk.sites);
            out.recursive_.insert(out.recursive_.end(),
                                  std::make_move_iterator(walk.things.begin()),
                                  std::make_move_iterator(walk.things.end()));
        }
        out.stats_.recursive = out.recursive_.size();
        out.stats_.top_level = out.top_level_.size();
        out.stats_.nested = out.stats_.recursive - out.stats_.top_level;
    }

    // Assign ids to the static views (separate namespaces; by_id()/children()
    // operate on the runtime list).
    for (std::size_t i = 0; i < out.top_level_.size(); ++i) {
        out.top_level_[i].id = static_cast<int>(i);
    }
    for (std::size_t i = 0; i < out.recursive_.size(); ++i) {
        out.recursive_[i].id = static_cast<int>(i);
        out.stats_.max_depth = std::max<std::size_t>(
            out.stats_.max_depth, static_cast<std::size_t>(out.recursive_[i].depth));
        if (out.recursive_[i].type < 0) {
            ++out.stats_.null_type_static;
        }
    }
    for (Thing& thing : out.top_level_) {
        apply_type_fields(thing, types);
    }
    for (Thing& thing : out.recursive_) {
        apply_type_fields(thing, types);
    }

    std::map<std::uint32_t, PathInfo> paths;
    for (const Thing& thing : out.recursive_) {
        paths.emplace(thing.site, PathInfo{thing.path, thing.depth});
    }

    if (!options.execute) {
        return out;
    }

    std::vector<ThingContext> contexts = options.contexts;
    if (contexts.empty()) {
        contexts = default_thing_contexts();
    }

    VmOptions vm_options;
    vm_options.max_steps = options.max_steps;
    vm_options.max_paths = options.max_paths;
    vm_options.max_time = options.max_time;
    vm_options.on_unknown = VmUnknownPolicy::Both;
    vm_options.follow_generate = options.follow_generate;
    vm_options.random_unknown = options.random_unknown;

    std::set<std::uint64_t> seen_things;
    std::set<std::uint64_t> seen_spawns;

    // Python _block_offsets: entity-table order, first row wins.
    std::vector<std::pair<int, std::uint32_t>> blocks;
    std::set<std::uint32_t> seen_offsets;
    for (const SceneEntity& entity : scene->entities) {
        if (entity.offset >= code.size()) {
            continue;
        }
        if (seen_offsets.insert(entity.offset).second) {
            blocks.emplace_back(entity.id, entity.offset);
        }
    }
    out.stats_.blocks = blocks.size();

    for (const ThingContext& context : contexts) {
        VmState state = context_state(level, vm, context);
        VmExecutor executor(std::move(state), vm_options);
        for (const auto& [entity_id, offset] : blocks) {
            const auto results = executor.run(offset, entity_id);
            out.stats_.paths += results.size();
            for (const VmPathResult& result : results) {
                ++out.stats_.reasons[std::string(vm_reason_name(result.reason))];
            }
            const VmScene merged = vm_merge_scenes(results);
            for (const VmSpawn& spawn : merged.spawns) {
                if (seen_spawns.insert(site_occ_key(spawn.site, spawn.occ)).second) {
                    ++out.stats_.spawns_executed;
                }
            }
            for (const VmThing& source : merged.things) {
                const std::uint64_t key = site_occ_key(source.site, source.occ);
                if (!seen_things.insert(key).second) {
                    continue;
                }
                const int parent = source.entity.value_or(entity_id);
                const auto info = paths.find(source.site);
                const std::vector<std::string> path =
                    info == paths.end() ? std::vector<std::string>{} : info->second.path;
                const int depth = info == paths.end() ? 0 : info->second.depth;

                Thing thing;
                thing.type = source.type_index;
                thing.kind = source.kind;
                thing.owner = source.owner;
                thing.position = source.pos;
                thing.angles = source.angles;
                thing.parent = parent;
                const auto script = entity_offsets.find(parent);
                thing.script =
                    script == entity_offsets.end() ? offset : script->second;
                thing.site = source.site;
                thing.occ = source.occ;
                thing.depth = depth;
                thing.path = path;
                thing.executed = true;
                const auto flags = entity_flags.find(parent);
                thing.flags = flags == entity_flags.end() ? 0u : flags->second;
                if (source.type_index >= 0 &&
                    static_cast<std::size_t>(source.type_index) < types.names.size()) {
                    const auto index = static_cast<std::size_t>(source.type_index);
                    thing.type_name = types.names[index];
                    thing.model_index = types.model_indices[index];
                    if (thing.model_index >= 0 &&
                        static_cast<std::size_t>(thing.model_index) < types.models.size()) {
                        thing.model =
                            types.models[static_cast<std::size_t>(thing.model_index)];
                    }
                }
                apply_type_fields(thing, types);
                if (thing.type < 0) {
                    ++out.stats_.null_type_things;
                } else if (static_cast<std::size_t>(thing.type) >= types.names.size()) {
                    ++out.stats_.invalid_type_things;
                } else if (thing.model.empty()) {
                    ++out.stats_.missing_model_things;
                }
                out.stats_.max_depth = std::max<std::size_t>(
                    out.stats_.max_depth, static_cast<std::size_t>(depth));
                if (depth == 0) {
                    ++out.stats_.executed_top_level;
                }
                out.things_.push_back(std::move(thing));
            }
        }
    }

    out.stats_.executed = out.things_.size();
    out.stats_.executed_nested =
        out.stats_.executed - out.stats_.executed_top_level;

    for (std::size_t i = 0; i < out.things_.size(); ++i) {
        Thing& thing = out.things_[i];
        thing.id = static_cast<int>(i);
        if (!out.by_id_.emplace(thing.id, i).second) {
            ++out.stats_.duplicate_ids;
        }
    }
    return out;
}

const Thing* Things::by_id(int id) const {
    const auto found = by_id_.find(id);
    return found == by_id_.end() ? nullptr : &things_[found->second];
}

Thing* Things::by_id(int id) {
    const auto found = by_id_.find(id);
    return found == by_id_.end() ? nullptr : &things_[found->second];
}

std::vector<const Thing*> Things::children(int parent) const {
    std::vector<const Thing*> out;
    for (const Thing& thing : things_) {
        if (thing.parent == parent) {
            out.push_back(&thing);
        }
    }
    return out;
}

std::vector<int> Things::child_ids(int parent) const {
    std::vector<int> out;
    for (const Thing& thing : things_) {
        if (thing.parent == parent) {
            out.push_back(thing.id);
        }
    }
    return out;
}

void Things::tick(std::uint64_t ticks) {
    if (ticks == 0) {
        return;
    }
    clock_ += ticks;
    stats_.ticks = clock_;
    for (Thing& thing : things_) {
        if (thing.alive) {
            thing.age += ticks;
        }
    }
}

bool Things::kill(int id) {
    return set_alive(id, false);
}

bool Things::revive(int id) {
    return set_alive(id, true);
}

bool Things::set_alive(int id, bool alive) {
    Thing* thing = by_id(id);
    if (thing == nullptr) {
        return false;
    }
    thing->alive = alive;
    return true;
}

} // namespace gm
