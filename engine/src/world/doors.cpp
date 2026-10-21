#include "world/doors.h"

#include "game/events.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "res/scene.h"
#include "res/sbtvm.h"

namespace gm {
namespace {

// --- little-endian readers over a byte payload -----------------------------
bool read_u32(const std::vector<std::uint8_t>& data, std::size_t offset,
              std::uint32_t& value) {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, 4);
    return true;
}

bool read_i32(const std::vector<std::uint8_t>& data, std::size_t offset, int& value) {
    std::uint32_t raw = 0;
    if (!read_u32(data, offset, raw)) {
        return false;
    }
    std::memcpy(&value, &raw, 4);
    return true;
}

bool read_f64(const std::vector<std::uint8_t>& data, std::size_t offset, double& value) {
    if (offset > data.size() || data.size() - offset < 8) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, 8);
    return std::isfinite(value);
}

int as_i32(std::uint32_t value) {
    int out = 0;
    std::memcpy(&out, &value, 4);
    return out;
}

std::uint32_t clamp_phase(std::int64_t phase) {
    if (phase <= 0) {
        return 0;
    }
    if (phase >= static_cast<std::int64_t>(kDoorPhaseFull)) {
        return kDoorPhaseFull;
    }
    return static_cast<std::uint32_t>(phase);
}

bool all_finite(const std::array<double, 3>& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

/// How many ticks a door needs to travel from its phase to *target*.
std::uint64_t ticks_to_phase(std::uint32_t phase, std::uint32_t target, std::uint32_t step) {
    const std::uint32_t distance = target > phase ? target - phase : phase - target;
    if (distance == 0) {
        return 0;
    }
    return (static_cast<std::uint64_t>(distance) + step - 1) / step;
}

// --- record -> Door ---------------------------------------------------------

bool parse_door_record(const std::vector<std::uint8_t>& payload, std::size_t offset,
                       Door& door) {
    if (offset > payload.size() || payload.size() - offset < 0xB8) {
        return false;
    }
    int flags = 0;
    if (!read_i32(payload, offset + 0x68, flags)) {
        return false;
    }
    door.flags = flags;
    door.kind = flags & 0xF;
    for (int k = 0; k < 5; ++k) {
        if (!read_i32(payload, offset + 0x6C + static_cast<std::size_t>(k) * 4,
                      door.ids[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    int state = 0;
    if (!read_i32(payload, offset + 0x80, state)) {
        return false;
    }
    if (state < -1 || state > 1) {
        return false;
    }
    door.state = static_cast<DoorState>(state);
    if (!read_i32(payload, offset + 0x84, door.sector) ||
        !read_i32(payload, offset + 0x88, door.object)) {
        return false;
    }
    std::uint32_t phase = 0;
    if (!read_u32(payload, offset + 0x8C, phase)) {
        return false;
    }
    door.phase = std::min(phase, kDoorPhaseFull);
    if (!read_i32(payload, offset + 0x94, door.step)) {
        return false;
    }
    if (door.step < 0) {
        door.step = 0;
    }
    for (int k = 0; k < 3; ++k) {
        if (!read_f64(payload, offset + static_cast<std::size_t>(k) * 8,
                      door.closed[static_cast<std::size_t>(k)]) ||
            !read_f64(payload, offset + 0x18 + static_cast<std::size_t>(k) * 8,
                      door.open[static_cast<std::size_t>(k)]) ||
            !read_f64(payload, offset + 0x30 + static_cast<std::size_t>(k) * 8,
                      door.size[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    door.sliding = false;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(door.open[static_cast<std::size_t>(k)] -
                      door.closed[static_cast<std::size_t>(k)]) > 1e-6) {
            door.sliding = true;
        }
    }
    door.linked = door.ids[3] != 0;
    return true;
}

// --- SCRIPT -> Door / Switch ------------------------------------------------

Door door_from_scene(const SceneDoor& src) {
    Door door;
    door.flags = src.flags;
    door.kind = src.flags & 0xF;
    door.ids = src.ids;
    door.closed = {src.values[0], src.values[1], src.values[2]};
    door.open = {src.values[3], src.values[4], src.values[5]};
    door.size = {src.values[6], src.values[7], src.values[8]};
    door.sector = src.params[0];
    door.object = src.params[1];
    door.step = src.params[2];
    if (door.step < 0) {
        door.step = 0;
    }
    // The scene layer's ``state`` tuple is (state word, phase); +0x80/+0x8c.
    door.state = static_cast<DoorState>(std::clamp(src.params[4], -1, 1));
    door.phase = clamp_phase(src.params[3]);
    door.sliding = false;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(door.open[static_cast<std::size_t>(k)] -
                      door.closed[static_cast<std::size_t>(k)]) > 1e-6) {
            door.sliding = true;
        }
    }
    door.linked = door.ids[3] != 0;
    return door;
}

int effective_door_id(const std::array<int, 5>& ids) {
    for (int k = 0; k < 3; ++k) {
        if (ids[static_cast<std::size_t>(k)] != 0) {
            return ids[static_cast<std::size_t>(k)];
        }
    }
    return ids[0];
}

void finalise_door(Door& door) {
    door.id = effective_door_id(door.ids);
    door.phase = std::min(door.phase, kDoorPhaseFull);
    door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
    door.timer = 0;
    if (!all_finite(door.closed) || !all_finite(door.open) || !all_finite(door.size)) {
        door.closed = {0.0, 0.0, 0.0};
        door.open = {0.0, 0.0, 0.0};
        door.size = {0.0, 0.0, 0.0};
    }
}

// --- ADD_LIFT ---------------------------------------------------------------

void collect_lifts(const std::vector<SbtInsn>& insns, const std::vector<std::uint8_t>& code,
                   std::vector<Lift>& lifts) {
    for (const SbtInsn& insn : insns) {
        if (insn.name == "ADD_LIFT") {
            Lift lift;
            lift.script = true;
            lift.id = static_cast<int>(lifts.size());
            const std::size_t head = static_cast<std::size_t>(insn.offset) + 1;
            if (head <= code.size() && code.size() - head >= 0x94) {
                for (std::size_t k = 0; k < lift.head.size(); ++k) {
                    std::memcpy(&lift.head[k], code.data() + head + k * 4, 4);
                }
                lift.kind = as_i32(lift.head[0]);
                // Hypothesis (documented): word 1 is the entity id, words 2..4
                // the position and word 7 the initial height.
                lift.id = as_i32(lift.head[1]);
                lift.x = as_i32(lift.head[2]);
                lift.y = as_i32(lift.head[3]);
                lift.z = as_i32(lift.head[4]);
                lift.height = as_i32(lift.head[7]);
                lift.target = lift.height;
            }
            const std::size_t count = std::min<std::size_t>(insn.count,
                                                           insn.ints.size() / 3);
            lift.stops.reserve(count);
            for (std::size_t k = 0; k < count; ++k) {
                LiftStop stop;
                stop.floor_id = as_i32(insn.ints[k * 3]);
                stop.height = as_i32(insn.ints[k * 3 + 1]);
                stop.sound = as_i32(insn.ints[k * 3 + 2]);
                lift.stops.push_back(stop);
            }
            lifts.push_back(std::move(lift));
        }
        collect_lifts(insn.body, code, lifts);
    }
}

// --- record 18 -> Lift ------------------------------------------------------

bool parse_lift_record(const std::vector<std::uint8_t>& payload, std::size_t offset,
                       std::size_t index, Lift& lift) {
    if (offset > payload.size() || payload.size() - offset < 0xA8) {
        return false;
    }
    lift.id = static_cast<int>(index);
    if (!read_i32(payload, offset + 0x34, lift.kind) ||
        !read_i32(payload, offset + 0x3C, lift.x) ||
        !read_i32(payload, offset + 0x40, lift.y) ||
        !read_i32(payload, offset + 0x44, lift.z) ||
        !read_i32(payload, offset + 0x54, lift.speed) ||
        !read_i32(payload, offset + 0x5C, lift.height) ||
        !read_i32(payload, offset + 0x60, lift.target) ||
        !read_i32(payload, offset + 0x6C, lift.stop_index)) {
        return false;
    }
    int moving = 0;
    if (!read_i32(payload, offset + 0x68, moving)) {
        return false;
    }
    lift.moving = moving != 0;
    for (int k = 0; k < 3; ++k) {
        if (!read_i32(payload, offset + 0x70 + static_cast<std::size_t>(k) * 4,
                      lift.call_ids[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    if (lift.speed < 0) {
        lift.speed = 0;
    }
    return true;
}

} // namespace

const char* door_state_name(DoorState state) {
    switch (state) {
    case DoorState::Closing:
        return "closing";
    case DoorState::Opening:
        return "opening";
    case DoorState::Closed:
    default:
        return "closed";
    }
}

// --- WorldState -------------------------------------------------------------

void WorldState::clear() {
    doors_.clear();
    switches_.clear();
    lifts_.clear();
    events_.clear();
    switch_event_cursor_ = 0;
    door_targets_.clear();
    lift_targets_.clear();
    stats_ = WorldStats{};
}

void WorldState::add_door(Door door) {
    finalise_door(door);
    doors_.push_back(std::move(door));
}

void WorldState::rebuild_indexes() {
    door_targets_.clear();
    lift_targets_.clear();
    for (std::size_t k = 0; k < doors_.size(); ++k) {
        const Door& door = doors_[k];
        for (int i = 0; i < 3; ++i) {
            const int alias = door.ids[static_cast<std::size_t>(i)];
            if (alias == 0) {
                continue;
            }
            auto it = std::find_if(door_targets_.begin(), door_targets_.end(),
                                   [alias](const auto& row) { return row.first == alias; });
            if (it == door_targets_.end()) {
                door_targets_.push_back({alias, {k}});
            } else if (std::find(it->second.begin(), it->second.end(), k) == it->second.end()) {
                it->second.push_back(k);
            }
        }
    }
    for (std::size_t k = 0; k < lifts_.size(); ++k) {
        const Lift& lift = lifts_[k];
        if (lift.id != 0) {
            lift_targets_.push_back({lift.id, k});
        }
        for (int call : lift.call_ids) {
            if (call != 0) {
                lift_targets_.push_back({call, k});
            }
        }
    }
    std::set<std::pair<int, int>> distinct;
    stats_.doors_bound = 0;
    for (const Door& door : doors_) {
        if (door.bound) {
            ++stats_.doors_bound;
            distinct.insert({door.sector, door.object});
        }
    }
    stats_.doors_distinct_objects = distinct.size();
    stats_.doors = doors_.size();
    stats_.doors_sliding = static_cast<std::size_t>(
        std::count_if(doors_.begin(), doors_.end(), [](const Door& d) { return d.sliding; }));
    stats_.doors_linked = static_cast<std::size_t>(
        std::count_if(doors_.begin(), doors_.end(), [](const Door& d) { return d.linked; }));
    stats_.switches = switches_.size();
    stats_.lifts = lifts_.size();
}

bool WorldState::build(const SbtLevel& level, const SbwLevel& world, std::string* error) {
    clear();
    if (error != nullptr) {
        error->clear();
    }

    const std::vector<std::uint8_t>* door_record = nullptr;
    const std::vector<std::uint8_t>* switch_record = nullptr;
    const std::vector<std::uint8_t>* lift_record = nullptr;
    for (const SbtRecord& record : level.records) {
        if (record.type == 2 && door_record == nullptr) {
            door_record = &record.payload;
        } else if (record.type == 1 && switch_record == nullptr) {
            switch_record = &record.payload;
        } else if (record.type == 18 && lift_record == nullptr) {
            lift_record = &record.payload;
        }
    }

    // Shipped levels keep the command stream in SCRIPT (record 11); save-like
    // files carry the runtime arrays directly.  Both are accepted.
    std::optional<Scene> scene;
    if (!level.script_code.empty()) {
        std::string scene_error;
        scene = extract_scene(level, scene_error, false);
        if (!scene && error != nullptr && door_record == nullptr && switch_record == nullptr) {
            *error = scene_error;
        }
    }

    if (door_record != nullptr) {
        std::uint32_t count = 0;
        if (!read_u32(*door_record, 0, count)) {
            ++stats_.bad_records;
        } else {
            const std::size_t available = (door_record->size() - 4) / 0xB8;
            if (count > available || count > 100) {
                ++stats_.bad_records;
            }
            const std::size_t n = std::min<std::size_t>(
                std::min<std::uint32_t>(count, 100), available);
            for (std::size_t k = 0; k < n; ++k) {
                Door door;
                if (parse_door_record(*door_record, 4 + k * 0xB8, door)) {
                    add_door(std::move(door));
                } else {
                    ++stats_.bad_records;
                }
            }
            stats_.record_doors = doors_.size();
        }
    } else if (scene) {
        for (const SceneDoor& src : scene->doors) {
            Door door = door_from_scene(src);
            add_door(std::move(door));
            ++stats_.script_doors;
        }
    }

    if (switch_record != nullptr) {
        std::uint32_t count = 0;
        if (!read_u32(*switch_record, 0, count)) {
            ++stats_.bad_records;
        } else {
            const std::size_t available = (switch_record->size() - 4) / 0x20;
            if (count > available || count > 512) {
                ++stats_.bad_records;
            }
            const std::size_t n = std::min<std::size_t>(
                std::min<std::uint32_t>(count, 512), available);
            for (std::size_t k = 0; k < n; ++k) {
                Switch sw;
                sw.id = static_cast<int>(k);
                const std::size_t offset = 4 + k * 0x20;
                if (!read_i32(*switch_record, offset, sw.kind) ||
                    !read_i32(*switch_record, offset + 4, sw.target) ||
                    !read_i32(*switch_record, offset + 8, sw.radius)) {
                    ++stats_.bad_records;
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    if (!read_i32(*switch_record, offset + 12 +
                                      static_cast<std::size_t>(axis) * 4,
                                  sw.pos[static_cast<std::size_t>(axis)])) {
                        ++stats_.bad_records;
                        break;
                    }
                }
                switches_.push_back(std::move(sw));
            }
            stats_.record_switches = switches_.size();
        }
    } else if (scene) {
        for (const SceneSwitch& src : scene->switches) {
            Switch sw;
            sw.id = static_cast<int>(switches_.size());
            sw.target = src.id;
            sw.kind = src.kind;
            sw.radius = src.radius;
            sw.pos = src.pos;
            switches_.push_back(std::move(sw));
            ++stats_.script_switches;
        }
    }

    if (lift_record != nullptr) {
        std::uint32_t count = 0;
        if (!read_u32(*lift_record, 0, count)) {
            ++stats_.bad_records;
        } else {
            const std::size_t available = (lift_record->size() - 4) / 0xA8;
            if (count > available || count > 30) {
                ++stats_.bad_records;
            }
            const std::size_t n = std::min<std::size_t>(
                std::min<std::uint32_t>(count, 30), available);
            const std::size_t columns_offset = 4 + n * 0xA8;
            std::uint32_t column_count = 0;
            const bool has_columns =
                read_u32(*lift_record, columns_offset, column_count) &&
                column_count <= 200 &&
                lift_record->size() - columns_offset - 4 >=
                    static_cast<std::size_t>(column_count) * 12;
            if (!has_columns) {
                ++stats_.bad_records;
            }
            for (std::size_t k = 0; k < n; ++k) {
                Lift lift;
                // parse_lift_record needs the record offset; store it in the
                // otherwise-unused tail word so the helper stays simple.
                if (!parse_lift_record(*lift_record, 4 + k * 0xA8, k, lift)) {
                    ++stats_.bad_records;
                    continue;
                }
                if (has_columns) {
                    const std::size_t col = columns_offset + 4;
                    const std::size_t count_col = column_count;
                    int stop_count = 0;
                    int stop_start = 0;
                    if (read_i32(*lift_record, 4 + k * 0xA8 + 0x9C, stop_count) &&
                        read_i32(*lift_record, 4 + k * 0xA8 + 0xA0, stop_start) &&
                        stop_count >= 0 && stop_start >= 0 &&
                        static_cast<std::size_t>(stop_start + stop_count) <= count_col) {
                        for (int s = 0; s < stop_count; ++s) {
                            const std::size_t index = static_cast<std::size_t>(stop_start + s);
                            LiftStop stop;
                            read_i32(*lift_record, col + index * 4, stop.floor_id);
                            read_i32(*lift_record, col + count_col * 4 + index * 4,
                                     stop.height);
                            read_i32(*lift_record, col + count_col * 8 + index * 4,
                                     stop.sound);
                            lift.stops.push_back(stop);
                        }
                    } else {
                        ++stats_.bad_records;
                    }
                }
                if (!lift.stops.empty()) {
                    lift.stop_index = std::clamp(
                        lift.stop_index, 0, static_cast<int>(lift.stops.size()) - 1);
                } else {
                    lift.stop_index = 0;
                }
                lifts_.push_back(std::move(lift));
            }
            stats_.record_lifts = lifts_.size();
        }
    } else if (!level.script_code.empty()) {
        std::string decode_error;
        const std::optional<SbtProgram> program = decode_sbt_script(level.script_code,
                                                                    decode_error);
        if (program) {
            collect_lifts(program->insns, level.script_code, lifts_);
            stats_.script_lifts = lifts_.size();
        } else {
            ++stats_.bad_records;
            if (error != nullptr && error->empty()) {
                *error = decode_error;
            }
        }
    }

    // Bind doors to the .sbw object table.  This is the proven 0x434419
    // (sector, object) lookup, done once at build time.
    const auto& sectors = world.header.sectors;
    for (Door& door : doors_) {
        const bool sector_ok = door.sector >= 0 &&
                               static_cast<std::size_t>(door.sector) < sectors.size();
        const bool object_ok =
            sector_ok && door.object >= 0 &&
            static_cast<std::size_t>(door.object) <
                sectors[static_cast<std::size_t>(door.sector)].objects.size();
        door.bound = sector_ok && object_ok;
        if (!door.bound) {
            ++stats_.bad_bindings;
        }
        door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
    }

    rebuild_indexes();

    if (doors_.empty() && switches_.empty() && lifts_.empty() && !scene &&
        door_record == nullptr && switch_record == nullptr && lift_record == nullptr) {
        if (error != nullptr && error->empty()) {
            *error = "no DOOR/SWITCH/LIFT records and no SCRIPT bytecode";
        }
        return false;
    }
    return true;
}

void WorldState::start_lift(Lift& lift) {
    if (lift.stops.size() < 2 || lift.speed <= 0) {
        return;
    }
    std::size_t next = static_cast<std::size_t>(std::max(lift.stop_index, 0)) + 1;
    if (next >= lift.stops.size()) {
        next = 0;
    }
    lift.stop_index = static_cast<int>(next);
    lift.target = lift.stops[next].height;
    lift.moving = lift.target != lift.height;
}

std::size_t WorldState::post_switch_events(EventQueue& queue,
                                           std::int16_t source) const {
    if (switch_event_cursor_ > events_.size()) {
        switch_event_cursor_ = events_.size();
    }
    std::size_t posted = 0;
    while (switch_event_cursor_ < events_.size()) {
        const WorldEvent& event = events_[switch_event_cursor_];
        if (event.kind != WorldEvent::Kind::SwitchPressed || event.switch_id < 0 ||
            event.switch_id >= 0x200) {
            // Invalid ids are never postable: advance instead of treating the
            // permanent rejection as a full queue and stalling the cursor.
            ++switch_event_cursor_;
            continue;
        }
        if (!queue.post_switch(static_cast<std::uint32_t>(event.switch_id), source)) {
            break; // full queue: keep the cursor on the pending record
        }
        ++posted;
        ++switch_event_cursor_;
    }
    return posted;
}

bool WorldState::interact(int switch_id) {
    if (switch_id < 0 || static_cast<std::size_t>(switch_id) >= switches_.size()) {
        return false;
    }
    Switch& sw = switches_[static_cast<std::size_t>(switch_id)];
    sw.pressed = true;
    ++stats_.switch_interactions;
    events_.push_back({WorldEvent::Kind::SwitchPressed, sw.id, sw.target, 0, 0});

    bool acted = false;
    for (const auto& row : door_targets_) {
        if (row.first != sw.target) {
            continue;
        }
        for (std::size_t index : row.second) {
            Door& door = doors_[index];
            if (door.locked) {
                ++stats_.locked_hits;
                events_.push_back(
                    {WorldEvent::Kind::DoorLocked, sw.id, sw.target, door.id, 0});
                acted = true;
                continue;
            }
            acted = true;
            door.timer = 0;
            bool opening = true;
            if (door.state == DoorState::Opening) {
                opening = false;
            } else if (door.state == DoorState::Closing) {
                opening = true;
            } else {
                opening = door.phase < kDoorPhaseFull;
            }
            door.state = opening ? DoorState::Opening : DoorState::Closing;
            events_.push_back({opening ? WorldEvent::Kind::DoorOpening
                                       : WorldEvent::Kind::DoorClosing,
                               sw.id, sw.target, door.id, 0});
        }
    }
    for (const auto& row : lift_targets_) {
        if (row.first != sw.target) {
            continue;
        }
        Lift& lift = lifts_[row.second];
        if (!lift.moving) {
            start_lift(lift);
            ++stats_.lift_calls;
            events_.push_back({WorldEvent::Kind::LiftCalled, sw.id, sw.target, 0, lift.id});
            acted = true;
        }
    }
    if (!acted) {
        ++stats_.unmatched_targets;
        events_.push_back({WorldEvent::Kind::NoTarget, sw.id, sw.target, 0, 0});
    }
    return true;
}

void WorldState::advance_door(Door& door, std::uint64_t ticks) {
    std::uint64_t left = ticks;
    std::uint32_t step = static_cast<std::uint32_t>(
        std::max<std::int32_t>(door.step, 0));
    const std::uint32_t max_step = 1u << 20; // defensive cap, still > one phase
    if (step > max_step) {
        step = max_step;
    }
    int guard = 0;
    while (left > 0 && guard++ < 4) {
        if (door.state == DoorState::Opening || door.state == DoorState::Closing) {
            if (step == 0) {
                break;
            }
            const bool opening = door.state == DoorState::Opening;
            const std::uint32_t target = opening ? kDoorPhaseFull : 0;
            const std::uint64_t need = ticks_to_phase(door.phase, target, step);
            if (left < need) {
                const std::uint64_t delta = left * step;
                door.phase = opening
                                 ? clamp_phase(static_cast<std::int64_t>(door.phase) +
                                               static_cast<std::int64_t>(delta))
                                 : clamp_phase(static_cast<std::int64_t>(door.phase) -
                                               static_cast<std::int64_t>(delta));
                left = 0;
            } else {
                door.phase = target;
                left -= need;
                door.state = DoorState::Closed;
                events_.push_back({opening ? WorldEvent::Kind::DoorOpened
                                           : WorldEvent::Kind::DoorClosed,
                                   -1, door.id, door.id, 0});
                if (opening && door.hold_ticks > 0) {
                    door.timer = door.hold_ticks;
                }
            }
        } else if (door.timer > 0) {
            if (left < door.timer) {
                door.timer -= static_cast<std::uint32_t>(left);
                left = 0;
            } else {
                left -= door.timer;
                door.timer = 0;
                door.state = DoorState::Closing;
                events_.push_back(
                    {WorldEvent::Kind::DoorClosing, -1, door.id, door.id, 0});
            }
        } else {
            break;
        }
    }
    door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
}

void WorldState::advance_lift(Lift& lift, std::uint64_t ticks) {
    if (!lift.moving || lift.speed <= 0 || lift.stops.empty()) {
        return;
    }
    std::uint64_t left = ticks;
    int guard = 0;
    while (left > 0 && lift.moving && guard++ < 2) {
        const std::int64_t distance =
            static_cast<std::int64_t>(lift.target) - lift.height;
        const std::int64_t speed = lift.speed;
        const std::uint64_t need = distance == 0
                                       ? 0
                                       : static_cast<std::uint64_t>(
                                             (std::llabs(distance) + speed - 1) / speed);
        if (left < need) {
            const std::int64_t delta = static_cast<std::int64_t>(left) * speed;
            lift.height = static_cast<int>(
                static_cast<std::int64_t>(lift.height) +
                (distance > 0 ? delta : -delta));
            left = 0;
        } else {
            lift.height = lift.target;
            left -= need;
            lift.moving = false;
            if (static_cast<std::size_t>(lift.stop_index) < lift.stops.size() &&
                lift.stops[static_cast<std::size_t>(lift.stop_index)].height ==
                    lift.height) {
                // already at the requested stop
            }
        }
    }
}

void WorldState::tick(std::uint64_t ticks) {
    events_.clear();
    switch_event_cursor_ = 0;
    if (ticks == 0) {
        return;
    }
    stats_.ticks += ticks;
    for (Door& door : doors_) {
        advance_door(door, ticks);
    }
    for (Lift& lift : lifts_) {
        advance_lift(lift, ticks);
    }
    for (Switch& sw : switches_) {
        sw.pressed = false;
    }
}

std::vector<ObjectDoorState> WorldState::apply_to_objects() const {
    std::vector<ObjectDoorState> out;
    out.reserve(doors_.size());
    for (const Door& door : doors_) {
        ObjectDoorState state;
        state.door_id = door.id;
        state.sector = door.sector;
        state.object = door.object;
        state.state = door.state;
        state.open_fraction = door.open_fraction;
        state.sliding = door.sliding;
        state.linked = door.linked;
        state.bound = door.bound;
        out.push_back(state);
    }
    return out;
}

Door* WorldState::find_door(int id) {
    for (Door& door : doors_) {
        if (door.id == id) {
            return &door;
        }
    }
    return nullptr;
}

const Door* WorldState::find_door(int id) const {
    for (const Door& door : doors_) {
        if (door.id == id) {
            return &door;
        }
    }
    return nullptr;
}

Lift* WorldState::find_lift(int id) {
    for (Lift& lift : lifts_) {
        if (lift.id == id) {
            return &lift;
        }
    }
    return nullptr;
}

const Lift* WorldState::find_lift(int id) const {
    for (const Lift& lift : lifts_) {
        if (lift.id == id) {
            return &lift;
        }
    }
    return nullptr;
}

void WorldState::set_locked(int door_id, bool locked) {
    for (Door& door : doors_) {
        if (door.id == door_id) {
            door.locked = locked;
        }
    }
}

void WorldState::set_hold_ticks(int door_id, std::uint32_t ticks) {
    for (Door& door : doors_) {
        if (door.id == door_id) {
            door.hold_ticks = ticks;
            if (door.state == DoorState::Closed && door.timer > 0) {
                door.timer = ticks;
            }
        }
    }
}

void WorldState::set_step(int door_id, std::int32_t phase_per_tick) {
    for (Door& door : doors_) {
        if (door.id == door_id) {
            door.step = std::max<std::int32_t>(phase_per_tick, 0);
        }
    }
}

} // namespace gm
