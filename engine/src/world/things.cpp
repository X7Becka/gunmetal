#include "world/things.h"

#include "game/events.h"

#include <algorithm>
#include <cmath>
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
/// The table itself is `gm::ThingTypeTable` (things.h); the helper fills it.
ThingTypeTable resolve_types(const SbtLevel& level) {
    ThingTypeTable table;
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
        table.speeds.reserve(count);
        table.turn_accels.reserve(count);
        table.turn_maxes.reserve(count);
        table.angle_rates.reserve(count);
        table.climb_speeds.reserve(count);
        table.flags_a.reserve(count);
        table.flags_b.reserve(count);
        table.flags_c.reserve(count);
        table.arrive_radii.reserve(count);
        table.leash_distances.reserve(count);
        table.gravities.reserve(count);
        table.dampings.reserve(count);
        table.collision_shapes.reserve(count);
        table.radii.reserve(count);
        table.boxes.reserve(count);
        table.proximities.reserve(count);
        table.path_locations.reserve(count);
        table.timer_periods.reserve(count);
        table.timer_steps.reserve(count);
        table.child_types.reserve(count);
        table.child_params.reserve(count);
        table.child_offsets.reserve(count);
        table.child_angles.reserve(count);
        table.search_radii.reserve(count);
        table.target_zone_thresholds.reserve(count);
        table.target_history_offsets.reserve(count);
        table.max_health.reserve(count);
        table.shields.reserve(count);
        table.hits.reserve(count);
        table.expire_ticks.reserve(count);
        table.collide_events.reserve(count);
        table.expire_events.reserve(count);
        table.damaged_events.reserve(count);
        table.destroy_events.reserve(count);
        table.create_events.reserve(count);
        table.timeout_events.reserve(count);
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
            table.speeds.push_back(std::bit_cast<double>(rd64(entry + 0x10)));
            table.turn_accels.push_back(std::bit_cast<double>(rd64(entry + 0x20)));
            table.turn_maxes.push_back(std::bit_cast<double>(rd64(entry + 0x28)));
            std::array<double, 3> angle_rate{};
            for (std::size_t a = 0; a < angle_rate.size(); ++a) {
                angle_rate[a] =
                    std::bit_cast<double>(rd64(entry + 0x90 + a * 8));
            }
            table.angle_rates.push_back(angle_rate);
            table.climb_speeds.push_back(std::bit_cast<double>(rd64(entry + 0x18)));
            table.flags_a.push_back(rd32(entry + 0x1a8));
            table.flags_b.push_back(rd32(entry + 0x1ac));
            table.flags_c.push_back(rd32(entry + 0x1b0));
            table.arrive_radii.push_back(std::bit_cast<double>(rd64(entry + 0x70)));
            table.leash_distances.push_back(std::bit_cast<double>(rd64(entry + 0xd0)));
            table.gravities.push_back(std::bit_cast<double>(rd64(entry + 0x78)));
            table.dampings.push_back(std::bit_cast<double>(rd64(entry + 0x80)));
            table.collision_shapes.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x1a0)));
            table.radii.push_back(std::bit_cast<double>(rd64(entry + 0x30)));
            std::array<double, 3> box{};
            for (std::size_t b = 0; b < box.size(); ++b) {
                box[b] = std::bit_cast<double>(rd64(entry + 0x38 + b * 8));
            }
            table.boxes.push_back(box);
            const std::int32_t proximity_count =
                static_cast<std::int32_t>(rd32(entry + 0x218));
            std::vector<ProximityEntry> proximity;
            const int clamped_count = std::max(0, std::min(8, proximity_count));
            proximity.reserve(static_cast<std::size_t>(clamped_count));
            for (int p = 0; p < clamped_count; ++p) {
                ProximityEntry item;
                item.event = rd32(entry + 0x21c + p * 4);
                item.low = static_cast<std::int32_t>(rd32(entry + 0x23c + p * 4));
                item.high = static_cast<std::int32_t>(rd32(entry + 0x25c + p * 4));
                item.cooldown = static_cast<std::int32_t>(rd32(entry + 0x27c + p * 4));
                item.offset.x = std::bit_cast<double>(rd64(entry + 0x29c + p * 8));
                item.offset.y = std::bit_cast<double>(rd64(entry + 0x2bc + p * 8));
                item.offset.z = std::bit_cast<double>(rd64(entry + 0x2dc + p * 8));
                proximity.push_back(item);
            }
            table.proximities.push_back(std::move(proximity));
            // +0x320 count (loader rejects >32, 0x47daea) and the +0x324 ids
            // resolved by 0x47db3d; the runtime copy keeps the ids only.
            const std::int32_t path_count =
                static_cast<std::int32_t>(rd32(entry + 0x320));
            std::vector<int> path_locations;
            const int clamped_paths = std::max(0, std::min(32, path_count));
            path_locations.reserve(static_cast<std::size_t>(clamped_paths));
            for (int p = 0; p < clamped_paths; ++p) {
                path_locations.push_back(
                    static_cast<std::int32_t>(rd32(entry + 0x324 + p * 4)));
            }
            table.path_locations.push_back(std::move(path_locations));
            // +0x210/+0x214 -> instance +0x3ec/+0x3f0 (0x46dd71/0x46dd77).
            table.timer_periods.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x210)));
            table.timer_steps.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x214)));
            // +0x2fc count (0x46d000 clamps at 4), then the type/param arrays.
            const std::int32_t child_count =
                static_cast<std::int32_t>(rd32(entry + 0x2fc));
            std::vector<int> kids;
            std::vector<int> kid_params;
            std::vector<std::array<double, 3>> kid_offsets;
            std::vector<std::array<double, 3>> kid_angles;
            const int clamped_children = std::max(0, std::min(4, child_count));
            kids.reserve(static_cast<std::size_t>(clamped_children));
            kid_params.reserve(static_cast<std::size_t>(clamped_children));
            kid_offsets.reserve(static_cast<std::size_t>(clamped_children));
            kid_angles.reserve(static_cast<std::size_t>(clamped_children));
            for (int c = 0; c < clamped_children; ++c) {
                const std::size_t child = static_cast<std::size_t>(c);
                kids.push_back(static_cast<std::int32_t>(rd32(entry + 0x300 + c * 4)));
                kid_params.push_back(
                    static_cast<std::int32_t>(rd32(entry + 0x310 + c * 4)));
                // +0xd8 + 0x18*i: the three doubles of the local offset the
                // spawn transforms by the parent matrix (0x46d000: the child
                // position is `origin + basis*offset`).
                std::array<double, 3> offset{};
                for (std::size_t o = 0; o < offset.size(); ++o) {
                    offset[o] = std::bit_cast<double>(
                        rd64(entry + 0xd8 + 0x18 * child + o * 8));
                }
                kid_offsets.push_back(offset);
                // +0x138/+0x158/+0x178 (+8*i): the child's own pitch/roll/yaw,
                // the input of the orientation composition (0x41e141).
                std::array<double, 3> angles{};
                for (std::size_t a = 0; a < angles.size(); ++a) {
                    angles[a] = std::bit_cast<double>(
                        rd64(entry + 0x138 + 8 * child + a * 0x20));
                }
                kid_angles.push_back(angles);
            }
            table.child_types.push_back(std::move(kids));
            table.child_params.push_back(std::move(kid_params));
            table.child_offsets.push_back(std::move(kid_offsets));
            table.child_angles.push_back(std::move(kid_angles));
            table.search_radii.push_back(std::bit_cast<double>(rd64(entry + 0x58)));
            table.target_zone_thresholds.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x1cc)));
            table.target_history_offsets.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x1d0)));
            table.max_health.push_back(static_cast<std::int32_t>(rd32(entry + 0x1d4)));
            std::array<int, 4> shields{};
            for (std::size_t s = 0; s < shields.size(); ++s) {
                shields[s] = static_cast<std::int32_t>(rd32(entry + 0x1d8 + s * 4));
            }
            table.shields.push_back(shields);
            table.hits.push_back(static_cast<std::int32_t>(rd32(entry + 0x1c8)));
            table.expire_ticks.push_back(
                static_cast<std::int32_t>(rd32(entry + 0x1e8)));
            table.collide_events.push_back(rd32(entry + 0x1c4));
            table.expire_events.push_back(rd32(entry + 0x1ec));
            table.damaged_events.push_back(rd32(entry + 0x1f4));
            table.destroy_events.push_back(rd32(entry + 0x1f8));
            table.create_events.push_back(rd32(entry + 0x1f0));
            table.timeout_events.push_back(rd32(entry + 0x1c0));
        }
        return table;
    }
    return table;
}

void apply_type_fields(Thing& thing, const ThingTypeTable& types) {
    if (thing.type < 0 ||
        static_cast<std::size_t>(thing.type) >= types.names.size()) {
        return;
    }
    const auto i = static_cast<std::size_t>(thing.type);
    thing.movement_type = types.movement_types[i];
    thing.speed = types.speeds[i];
    thing.turn_accel = types.turn_accels[i];
    thing.turn_max = types.turn_maxes[i];
    thing.angle_rate = types.angle_rates[i];
    thing.climb_speed = types.climb_speeds[i];
    thing.flags_a = types.flags_a[i];
    thing.flags_b = types.flags_b[i];
    thing.flags_c = types.flags_c[i];
    thing.arrive_radius = types.arrive_radii[i];
    thing.leash_distance = types.leash_distances[i];
    thing.gravity = types.gravities[i];
    thing.damping = types.dampings[i];
    thing.collision_shape = types.collision_shapes[i];
    thing.radius = types.radii[i];
    thing.box = types.boxes[i];
    thing.proximity = types.proximities[i];
    thing.path_locations = types.path_locations[i];
    thing.timer_period = types.timer_periods[i];
    thing.timer_step = types.timer_steps[i];
    thing.child_types = types.child_types[i];
    thing.child_params = types.child_params[i];
    thing.child_offsets = types.child_offsets[i];
    thing.child_angles = types.child_angles[i];
    thing.search_radius = types.search_radii[i];
    thing.target_zone_threshold = types.target_zone_thresholds[i];
    thing.target_history_offset = types.target_history_offsets[i];
    thing.max_health = types.max_health[i];
    thing.health = thing.max_health;
    thing.shields = types.shields[i];
    thing.shield_max = thing.shields;
    thing.hits_left = types.hits[i];
    thing.expire_ticks = types.expire_ticks[i];
    thing.timeout_event = types.timeout_events[i];
    thing.collide_event = types.collide_events[i];
    thing.expire_event = types.expire_events[i];
    thing.create_event = types.create_events[i];
    thing.damaged_event = types.damaged_events[i];
    thing.destroy_event = types.destroy_events[i];
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
                 std::uint32_t script, const ThingTypeTable& types, bool executed,
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
    const ThingTypeTable& types;
    int parent = 0;
    std::uint32_t flags = 0;
    std::uint32_t script = 0;
    std::uint32_t base_offset = 0;
    std::vector<Thing> things;
    std::set<std::uint32_t> sites;
    std::size_t duplicates = 0;
    std::string error;

    explicit RecursiveWalk(const ThingTypeTable& table) : types(table) {}

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
                                    const ThingTypeTable& types, int parent,
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

/// The three matrix rows of a thing object: right, forward and up.
///
/// `0x41e025` builds them from (roll, pitch, yaw); the `ADD_THING*`
/// operands are (pitch, roll, yaw) -- `0x46d8b0` copies the first double to
/// the instance +0x18 (pitch), the second to +0x20 (roll) and the third to
/// +0x28 (yaw).
std::array<Vec3d, 3> thing_matrix(const std::vector<double>& angles) {
    const double pitch = angles.size() > 0 ? angles[0] : 0.0;
    const double roll = angles.size() > 1 ? angles[1] : 0.0;
    const double yaw = angles.size() > 2 ? angles[2] : 0.0;
    return thing_basis(roll, pitch, yaw);
}

double component(const Vec3d& v, std::size_t index) {
    return index == 0 ? v.x : (index == 1 ? v.y : v.z);
}

/// `0x41e141`: the child's matrix in the parent's frame.
///
/// The original writes `result[i] = parent[0]*child[i][0] +
/// parent[1]*child[i][1] + parent[2]*child[i][2]` component wise (the child
/// vector is expressed in the parent's frame); the summation order below is
/// the x87 order of the original.
std::array<Vec3d, 3> compose_matrix(const std::array<Vec3d, 3>& parent,
                                     const std::array<Vec3d, 3>& child) {
    std::array<Vec3d, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            const double value =
                ((component(parent[0], j) * component(child[i], 0)) +
                 (component(parent[1], j) * component(child[i], 1))) +
                (component(parent[2], j) * component(child[i], 2));
            switch (j) {
            case 0: out[i].x = value; break;
            case 1: out[i].y = value; break;
            default: out[i].z = value; break;
            }
        }
    }
    return out;
}

/// The three angles of a thing: pitch, roll and yaw.
struct ThingAngles {
    double pitch = 0.0;
    double roll = 0.0;
    double yaw = 0.0;
};

/// `0x41e2f9`: the angles of a matrix.
///
///   * yaw   = atan2(forward.x, forward.y)  (`0x4809f0`)
///   * pitch = -asin(forward.z)             (`0x480a24`)
///   * roll  = atan2(-right.z, up.z)        (`0x4809f0`)
ThingAngles matrix_angles(const std::array<Vec3d, 3>& matrix) {
    ThingAngles out;
    out.yaw = std::atan2(matrix[1].x, matrix[1].y);
    out.pitch = -std::asin(matrix[1].z);
    out.roll = std::atan2(-matrix[0].z, matrix[2].z);
    return out;
}

/// `0x46d000`: the world position of a type child.
///
/// The original transforms the child's local offset (`THING_TYPES+0xd8`) by
/// the parent's matrix and adds the parent's position.  The summation order
/// is the x87 order of the original (the `right`/`up` pair first for x and
/// z, `right`/`forward` for y).
std::array<double, 3> child_position(const std::array<double, 3>& origin,
                                     const std::array<Vec3d, 3>& basis,
                                     const std::array<double, 3>& offset) {
    const double lx = offset[0];
    const double ly = offset[1];
    const double lz = offset[2];
    const Vec3d& right = basis[0];
    const Vec3d& forward = basis[1];
    const Vec3d& up = basis[2];
    const double x = (((right.x * lx) + (up.x * lz)) + (forward.x * ly)) + origin[0];
    const double y = (((right.y * lx) + (forward.y * ly)) + (up.y * lz)) + origin[1];
    const double z = (((right.z * lx) + (up.z * lz)) + (forward.z * ly)) + origin[2];
    return {x, y, z};
}

} // namespace

std::optional<ThingTypeEvents> thing_type_events(const SbtLevel& level,
                                                int type) {
    if (type < 0) {
        return std::nullopt;
    }
    const ThingTypeTable types = resolve_types(level);
    const auto index = static_cast<std::size_t>(type);
    if (index >= types.names.size()) {
        return std::nullopt;
    }
    ThingTypeEvents out;
    out.type = type;
    out.movement_type = types.movement_types[index];
    out.speed = types.speeds[index];
    out.turn_accel = types.turn_accels[index];
    out.turn_max = types.turn_maxes[index];
    out.angle_rate = types.angle_rates[index];
    out.climb_speed = types.climb_speeds[index];
    out.flags_a = types.flags_a[index];
    out.flags_b = types.flags_b[index];
    out.flags_c = types.flags_c[index];
    out.arrive_radius = types.arrive_radii[index];
    out.leash_distance = types.leash_distances[index];
    out.gravity = types.gravities[index];
    out.damping = types.dampings[index];
    out.collision_shape = types.collision_shapes[index];
    out.radius = types.radii[index];
    out.box = types.boxes[index];
    out.proximity = types.proximities[index];
    out.path_locations = types.path_locations[index];
    out.timer_period = types.timer_periods[index];
    out.timer_step = types.timer_steps[index];
    out.child_types = types.child_types[index];
    out.child_params = types.child_params[index];
    out.child_offsets = types.child_offsets[index];
    out.child_angles = types.child_angles[index];
    out.expire_ticks = types.expire_ticks[index];
    out.collide_event = types.collide_events[index];
    out.expire_event = types.expire_events[index];
    out.damaged_event = types.damaged_events[index];
    out.destroy_event = types.destroy_events[index];
    out.create_event = types.create_events[index];
    out.timeout_event = types.timeout_events[index];
    return out;
}

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

    out.types_ = std::make_shared<const ThingTypeTable>(resolve_types(level));
    const ThingTypeTable& types = *out.types_;
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
    std::set<std::uint64_t> seen_locations;

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
            // The runtime location table 0x80d8e0 is a global array of the
            // executed ADD_LOCATION commands (0x45bea6); the AI resolves
            // THING_TYPES +0x324[] against it (0x476012).  Keep the same
            // per-site dedup as things/spawns so the sweep stays deterministic.
            for (const VmLocation& location : merged.locations) {
                if (seen_locations
                        .insert(site_occ_key(location.site, location.occ))
                        .second) {
                    out.locations_.push_back(location);
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

    // The type children of `0x46d000` are opt-in: the original spawns them
    // while the parent is created, but the C++ view counts the instances (and
    // their AI) per thing, so the spawn changes the golden counters of the
    // corpus sweep (docs/THINGS_CPP.md §2.3a).  One level only: the
    // children of a child are not spawned.
    if (options.spawn_type_children) {
        const std::size_t instances = out.things_.size();
        for (std::size_t i = 0; i < instances; ++i) {
            out.spawn_children(out.things_[i].id, nullptr);
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

const VmLocation* Things::location(int id) const {
    for (const VmLocation& entry : locations_) {
        if (entry.thing_id == id) {
            return &entry;
        }
    }
    return nullptr;
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

void Things::set_motion_enabled(bool enabled, const CollisionWorld* world) {
    motion_enabled_ = enabled;
    motion_world_ = enabled ? world : nullptr;
    motion_last_.resize(things_.size());
    if (motion_.size() < things_.size()) {
        motion_.resize(things_.size());
    }
    if (motion_targets_.size() < things_.size()) {
        motion_targets_.resize(things_.size());
        motion_target_seen_.resize(things_.size(), false);
    }
}

void Things::set_motion_target(std::size_t index, ThingMotionTarget target) {
    if (index >= things_.size()) {
        return;
    }
    if (motion_targets_.size() <= index) {
        motion_targets_.resize(index + 1);
        motion_target_seen_.resize(index + 1, false);
    }
    motion_targets_[index] = target;
    motion_target_seen_[index] = true;
}

void Things::set_push_targets(std::vector<ThingPushTarget> targets) {
    push_targets_ = std::move(targets);
}

bool Things::aim_motion_at(std::size_t index, const Vec3d& target, bool visible) {
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) {
        return false;
    }
    if (!motion_enabled_ || index >= things_.size() || index >= motion_.size() ||
        !motion_[index].has_value()) {
        return false;
    }
    ThingMotionState& state = *motion_[index];
    const Vec3d delta = target - state.position;
    // 0x437909: local coordinates are the projections onto the three basis
    // rows.  Row 1 is forward, so yaw = atan2(x, y) with the engine's
    // (sin yaw, cos yaw) forward.
    const std::array<Vec3d, 3> basis = thing_basis(state.roll, state.pitch, state.yaw);
    const double local_x = dot(basis[0], delta);
    const double local_y = dot(basis[1], delta);
    double yaw_error = std::atan2(local_x, local_y);
    // Keep the error in (-pi, pi]; atan2 already returns [-pi, pi].
    if (yaw_error <= -3.14159265358979323846) {
        yaw_error += 2.0 * 3.14159265358979323846;
    }
    ThingMotionTarget aim;
    aim.position = target;
    aim.yaw_error = yaw_error;
    aim.pitch_error = 0.0; // 0x41e2b8 writes the zero pitch error of this build
    aim.vertical_error = delta.z;
    aim.visible = visible;
    aim.valid = true;
    if (motion_targets_.size() <= index) {
        motion_targets_.resize(index + 1);
        motion_target_seen_.resize(index + 1, false);
    }
    motion_targets_[index] = aim;
    motion_target_seen_[index] = true;
    return true;
}

void Things::clear_motion_aim(std::size_t index) {
    if (index >= motion_targets_.size()) {
        return;
    }
    motion_targets_[index] = ThingMotionTarget{};
    if (motion_target_seen_.size() <= index) {
        motion_target_seen_.resize(index + 1, false);
    }
    motion_target_seen_[index] = true;
}

std::optional<std::size_t> Things::index_of(int id) const {
    const auto it = by_id_.find(id);
    if (it == by_id_.end() || it->second >= things_.size()) {
        return std::nullopt;
    }
    return it->second;
}

const ThingMotionState* Things::motion_state(std::size_t index) const {
    if (!motion_enabled_ || index >= motion_.size() || !motion_[index].has_value()) {
        return nullptr;
    }
    return &*motion_[index];
}

const ThingMotionStep* Things::motion_last(std::size_t index) const {
    if (!motion_enabled_ || index >= motion_last_.size()) {
        return nullptr;
    }
    return &motion_last_[index];
}

const ThingMotionTarget* Things::motion_target(std::size_t index) const {
    if (!motion_enabled_ || index >= motion_targets_.size() ||
        index >= motion_target_seen_.size() || !motion_target_seen_[index]) {
        return nullptr;
    }
    return &motion_targets_[index];
}

void Things::tick(std::uint64_t ticks, EventQueue* events) {
    if (ticks == 0) {
        return;
    }
    clock_ += ticks;
    stats_.ticks = clock_;
    if (motion_enabled_) {
        if (motion_.size() < things_.size()) {
            motion_.resize(things_.size());
            motion_targets_.resize(things_.size());
            motion_last_.resize(things_.size());
        }
    }
    for (std::size_t index = 0; index < things_.size(); ++index) {
        Thing& thing = things_[index];
        if (!thing.alive) {
            continue;
        }
        thing.age += ticks;
        if (!thing.expired && thing.expire_ticks > 0 &&
            thing.age >= static_cast<std::uint64_t>(thing.expire_ticks)) {
            thing.expired = true;
            if (events != nullptr) {
                post_thing_event(*events, thing, ThingEventKind::expire);
            }
        }
        if (!motion_enabled_ || thing.expired || thing.movement_type < 0 ||
            thing.movement_type >= 19) {
            continue;
        }
        std::optional<ThingMotionState>& slot = motion_[index];
        if (!slot.has_value()) {
            slot = thing_motion_spawn(thing_motion_config(thing));
            ThingMotionState& state = *slot;
            // The spawn view carries the script position where it exists; the
            // source-relative variants start at the origin like `0x46dfe9`.
            if (thing.position.has_value()) {
                state.position = {static_cast<double>((*thing.position)[0]),
                                  static_cast<double>((*thing.position)[1]),
                                  static_cast<double>((*thing.position)[2])};
            }
        }
        ThingMotionInput input;
        input.dt = static_cast<double>(ticks);
        input.world = motion_world_;
        if (index < motion_targets_.size()) {
            input.target = motion_targets_[index];
        }
        input.push_targets = push_targets_;
        input.rng = motion_rng_;
        ThingMotionStep step = thing_motion_step(*slot, thing_motion_config(thing), input);
        motion_last_[index] = step;
        ++stats_.motion_steps;
        if (step.moved || step.pushed >= 0) {
            ++stats_.motion_moved;
        }
        if (step.pushed >= 0) {
            ++stats_.motion_pushed;
        }
        // Weapon/event ids stay in the step record: posting them belongs to the
        // game layer (`step.fired_event` is a `THING_TYPES+0x21c` id, not the
        // +0x1c4 collide id this module owns).
    }
}

int Things::spawn(Thing thing, EventQueue* events) {
    int id = thing.id;
    if (id < 0) {
        id = 0;
        while (true) {
            const auto it = by_id_.find(id);
            if (it == by_id_.end() || !things_[it->second].alive) {
                break;
            }
            ++id;
        }
    }
    thing.id = id;
    thing.alive = true;
    thing.executed = true;
    thing.age = 0;
    thing.expired = false;

    std::size_t index = 0;
    const auto existing = by_id_.find(id);
    if (existing != by_id_.end()) {
        Thing& slot = things_[existing->second];
        if (slot.alive) {
            return -1;
        }
        // A removed runtime instance may be reused by a later spawn (projectiles
        // and scripted things do this); keep the vector index stable.
        index = existing->second;
        slot = std::move(thing);
    } else {
        index = things_.size();
        things_.push_back(std::move(thing));
        by_id_.emplace(id, index);
    }
    Thing& spawned = things_[index];
    if (motion_.size() < things_.size()) {
        motion_.resize(things_.size());
        motion_targets_.resize(things_.size());
        motion_target_seen_.resize(things_.size(), false);
        motion_last_.resize(things_.size());
    }
    motion_[index].reset();
    if (index < motion_target_seen_.size()) {
        motion_target_seen_[index] = false;
    } // a reused slot starts with the fresh-instance state
    motion_last_[index] = ThingMotionStep{};
    ++stats_.executed;
    if (spawned.depth > 0) {
        ++stats_.executed_nested;
    } else {
        ++stats_.executed_top_level;
    }
    if (events != nullptr) {
        post_thing_event(*events, spawned, ThingEventKind::create);
    }
    return id;
}

std::vector<int> Things::spawn_children(int parent_id, EventQueue* events) {
    std::vector<int> created;
    Thing* parent_slot = by_id(parent_id);
    if (parent_slot == nullptr || types_ == nullptr) {
        return created;
    }
    // The original spawns the children while the parent is created, so a
    // second call must not duplicate them.
    if (!parent_slot->type_children.empty()) {
        return parent_slot->type_children;
    }
    // A copy: `spawn()` may reallocate `things_`.
    const Thing parent = *parent_slot;
    if (parent.child_types.empty()) {
        return created;
    }

    // The origin and the matrix of the parent (0x46d000 uses the position
    // arguments of the thing add and the object matrix of 0x41e025).
    std::array<double, 3> origin{0.0, 0.0, 0.0};
    if (parent.position.has_value()) {
        origin = {static_cast<double>((*parent.position)[0]),
                  static_cast<double>((*parent.position)[1]),
                  static_cast<double>((*parent.position)[2])};
    }
    const std::array<Vec3d, 3> parent_matrix = thing_matrix(parent.angles);

    for (std::size_t i = 0; i < parent.child_types.size(); ++i) {
        const int child_type = parent.child_types[i];
        const int child_param =
            i < parent.child_params.size() ? parent.child_params[i] : 0;
        const std::array<double, 3> offset =
            i < parent.child_offsets.size()
                ? parent.child_offsets[i]
                : std::array<double, 3>{0.0, 0.0, 0.0};
        const std::array<double, 3> own_angles =
            i < parent.child_angles.size()
                ? parent.child_angles[i]
                : std::array<double, 3>{0.0, 0.0, 0.0};

        Thing child;
        child.id = -1; // let `spawn()` pick the free id
        child.type = child_type;
        child.parent = parent_id;
        child.depth = parent.depth + 1;
        child.path = parent.path;
        child.executed = true;
        child.alive = true;
        if (child_type >= 0 &&
            static_cast<std::size_t>(child_type) < types_->names.size()) {
            const auto index = static_cast<std::size_t>(child_type);
            child.type_name = types_->names[index];
            child.model_index = types_->model_indices[index];
            if (child.model_index >= 0 &&
                static_cast<std::size_t>(child.model_index) < types_->models.size()) {
                child.model = types_->models[static_cast<std::size_t>(child.model_index)];
            }
        }
        apply_type_fields(child, *types_);
        // The runtime flag word of the child (0x46d000): the flags of its own
        // type, the child param of the table and the flags of the parent.
        child.flags_a = child.flags_a | static_cast<std::uint32_t>(child_param) |
                        parent.flags;
        child.flags = child.flags_a;

        // The world position: `origin + parent_matrix * offset`.
        const std::array<double, 3> world =
            child_position(origin, parent_matrix, offset);
        child.world_position = world;
        child.position = std::array<int, 3>{
            static_cast<int>(world[0]), static_cast<int>(world[1]),
            static_cast<int>(world[2])};

        // The orientation: the parent's matrix composed with the child's own
        // matrix (0x41e141) and the angles re-derived from the result
        // (0x41e2f9).  The flag 0x2000 of the child keeps the raw angles.
        const std::array<Vec3d, 3> own_matrix =
            thing_basis(own_angles[1], own_angles[0], own_angles[2]);
        const std::array<Vec3d, 3> matrix =
            compose_matrix(parent_matrix, own_matrix);
        if ((child.flags_b & 0x2000u) == 0u) {
            const ThingAngles angles = matrix_angles(matrix);
            child.angles = {angles.pitch, angles.roll, angles.yaw};
        } else {
            child.angles = {own_angles[0], own_angles[1], own_angles[2]};
        }

        const int id = spawn(std::move(child), events);
        if (id >= 0) {
            created.push_back(id);
        }
    }
    // The parent keeps the ids of its children (the original: instance
    // +0x314 count, +0x318 ids).
    if (Thing* slot = by_id(parent_id)) {
        slot->type_children = created;
    }
    return created;
}

const std::vector<int>& Things::type_children(int parent_id) const {
    static const std::vector<int> empty;
    const Thing* parent = by_id(parent_id);
    return parent == nullptr ? empty : parent->type_children;
}

const ThingTypeTable& Things::type_table() const {
    static const ThingTypeTable empty;
    return types_ == nullptr ? empty : *types_;
}

bool Things::remove(int id, EventQueue* events) {
    Thing* thing = by_id(id);
    if (thing == nullptr || !thing->alive) {
        return false;
    }
    if (events != nullptr) {
        post_thing_event(*events, *thing, ThingEventKind::destroy);
    }
    thing->alive = false;
    return true;
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
