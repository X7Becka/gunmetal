#include "game/state.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>

namespace gm {
namespace {

/// 100.00 points, the same limit `res/save.cpp` enforces on the gauges.
constexpr std::int32_t kMaxGauge = static_cast<std::int32_t>(kSaveGaugeScale * 100.0);

std::string normalize_level(std::string_view level) {
    const std::size_t slash = level.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        level.remove_prefix(slash + 1);
    }
    std::string out(level);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (out.size() >= 4 && out.compare(out.size() - 4, 4, ".sbt") == 0) {
        out.resize(out.size() - 4);
    }
    return out;
}

bool same_level(std::string_view a, std::string_view b) {
    return normalize_level(a) == normalize_level(b);
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool finite3(const std::array<double, 3>& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool check_door(std::size_t index, const SaveDoor& door, std::string& error) {
    const std::string who = "door " + std::to_string(index);
    if (door.sector < 0) {
        error = who + ": sector " + std::to_string(door.sector) + " is negative";
        return false;
    }
    if (door.object < 0) {
        error = who + ": object " + std::to_string(door.object) + " is negative";
        return false;
    }
    if (door.state < -1 || door.state > 1) {
        error = who + ": state " + std::to_string(door.state) + " outside -1..1";
        return false;
    }
    if (door.phase_prev > kDoorPhaseFull || door.phase_next > kDoorPhaseFull) {
        error = who + ": phase outside 0..65536 (" + std::to_string(door.phase_prev) +
                "/" + std::to_string(door.phase_next) + ")";
        return false;
    }
    if (door.step < 0) {
        error = who + ": step " + std::to_string(door.step) + " is negative";
        return false;
    }
    for (double v : door.box) {
        if (!std::isfinite(v)) {
            error = who + ": non-finite box value";
            return false;
        }
    }
    for (double v : door.delta) {
        if (!std::isfinite(v)) {
            error = who + ": non-finite delta value";
            return false;
        }
    }
    if (!std::isfinite(door.constant)) {
        error = who + ": non-finite delta constant";
        return false;
    }
    return true;
}

/// Index of the world door whose proven `(sector, object)` address matches.
/// The runtime identity `Door::id` is not usable here: several shipped doors
/// share one GENERATE id, while `(sector, object)` is unique in a level.
std::optional<std::size_t> find_door_index(const WorldState& world,
                                           const SaveDoor& door) {
    const std::vector<Door>& doors = world.doors();
    for (std::size_t i = 0; i < doors.size(); ++i) {
        if (doors[i].sector == door.sector && doors[i].object == door.object) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

bool GameState::validate(std::string* error) const {
    if (error != nullptr) {
        error->clear();
    }
    std::string message;

    if (level_name.empty()) {
        message = "level name is empty";
    } else if (level_name.size() > kSaveLevelNameLen) {
        message = "level name is longer than " + std::to_string(kSaveLevelNameLen) +
                  " bytes";
    } else if (!has_player) {
        message = "game state has no player";
    } else if (!finite3(position)) {
        message = "player position is not finite";
    } else if (!finite3(angles)) {
        message = "player angles are not finite";
    } else if (armor_max <= 0 || armor_max > kMaxGauge) {
        message = "armor_max " + std::to_string(armor_max) + " out of range [1, " +
                  std::to_string(kMaxGauge) + "]";
    } else if (armor < 0 || armor > armor_max) {
        message = "armor " + std::to_string(armor) + " outside [0, " +
                  std::to_string(armor_max) + "]";
    } else if (shield_max <= 0 || shield_max > kMaxGauge) {
        message = "shield_max " + std::to_string(shield_max) + " out of range [1, " +
                  std::to_string(kMaxGauge) + "]";
    } else if (weapon_slot < 0 ||
               weapon_slot >= static_cast<std::int32_t>(ammo.size())) {
        message = "weapon slot " + std::to_string(weapon_slot) + " outside 0.." +
                  std::to_string(ammo.size() - 1);
    } else if ((weapon_mask & ~0xfffu) != 0) {
        message = "weapon mask " + std::to_string(weapon_mask) +
                  " has bits above slot 11";
    } else if (doors.size() > kSaveMaxDoors) {
        message = "too many doors: " + std::to_string(doors.size()) + " > " +
                  std::to_string(kSaveMaxDoors);
    } else if (things.size() !=
               static_cast<std::size_t>(
                   std::count(thing_present.begin(), thing_present.end(), true))) {
        message = "thing records: " + std::to_string(things.size()) +
                  " for " +
                  std::to_string(std::count(thing_present.begin(), thing_present.end(),
                                            true)) +
                  " presence flags";
    }

    if (message.empty()) {
        for (std::size_t i = 0; i < shields.size(); ++i) {
            if (shields[i] < 0 || shields[i] > shield_max) {
                message = "shield " + std::to_string(i) + " value " +
                          std::to_string(shields[i]) + " outside [0, " +
                          std::to_string(shield_max) + "]";
                break;
            }
        }
    }
    if (message.empty()) {
        for (std::size_t i = 0; i < doors.size(); ++i) {
            if (!check_door(i, doors[i], message)) {
                break;
            }
        }
    }
    if (message.empty()) {
        for (std::size_t i = 0; i < things.size(); ++i) {
            const SaveThing& thing = things[i];
            if (thing.slot >= kSaveThingSlots) {
                message = "thing " + std::to_string(i) + ": slot " +
                          std::to_string(thing.slot) + " outside 0.." +
                          std::to_string(kSaveThingSlots - 1);
            } else if (!thing_present[thing.slot]) {
                message = "thing " + std::to_string(i) + ": slot " +
                          std::to_string(thing.slot) + " is not present";
            } else if (i > 0 && things[i - 1].slot >= thing.slot) {
                message = "thing " + std::to_string(i) + ": slots are not ascending";
            }
            if (!message.empty()) {
                break;
            }
        }
    }

    if (!message.empty()) {
        set_error(error, message);
        return false;
    }
    return true;
}

std::optional<GameState> GameState::from_save(Save save, std::string_view level,
                                              std::string& error) {
    error.clear();
    if (!same_level(level, save.header.level_name)) {
        error = "level mismatch: save is \"" + save.header.level_name +
                "\", requested \"" + std::string(level) + "\"";
        return std::nullopt;
    }
    if (!save.player) {
        error = "save has no player block (9)";
        return std::nullopt;
    }
    const SavePlayer& source = *save.player;
    if (!source.has_slot) {
        error = "player block has no present slot";
        return std::nullopt;
    }

    GameState state;
    state.level_name = save.header.level_name;
    state.fields = save.header.fields;
    state.level_clock = save.header.fields[1];

    state.has_player = true;
    state.position = source.position;
    state.angles = source.angles;
    state.history_index = source.history_index;
    state.exists = source.exists;
    state.status_present = source.status_present;
    state.object_id = source.object_id;
    state.armor_max = source.armor_max;
    state.armor = source.armor;
    state.shield_max = source.shield_max;
    state.shields = source.shields;
    state.weapon_slot = source.weapon_slot;
    state.weapon_mask = source.weapon_mask;
    state.ammo = source.ammo;

    state.doors = save.doors;
    state.thing_present = save.thing_present;
    state.things = save.things;

    state.template_ = std::move(save);
    state.has_template_ = true;

    if (!state.validate(&error)) {
        return std::nullopt;
    }
    return state;
}

bool GameState::apply_to_player(PlayerState& player, std::string* error) const {
    if (!validate(error)) {
        return false;
    }
    player.pos = Vec3d{position[0], position[1], position[2]};
    player.vel = Vec3d{0.0, 0.0, 0.0};
    player.yaw = angles[2];
    player.on_ground = false;
    return true;
}

std::size_t GameState::apply_to_doors(WorldState& world, std::string* error) const {
    if (!validate(error)) {
        return 0;
    }
    std::size_t matched = 0;
    for (const SaveDoor& saved : doors) {
        const std::optional<std::size_t> index = find_door_index(world, saved);
        if (!index) {
            continue;
        }
        // `doors()` exposes the vector as const.  This method takes the
        // WorldState by non-const reference, so the underlying objects are
        // mutable; the cast only works around the missing mutable accessor
        // (world/doors.h is out of scope for this change).
        Door& door = const_cast<Door&>(world.doors()[*index]);

        door.flags = saved.flags;
        door.kind = saved.flags & 0xf;
        door.ids = saved.ids;
        door.closed = {saved.box[0], saved.box[1], saved.box[2]};
        door.open = {saved.box[3], saved.box[4], saved.box[5]};
        door.size = {saved.box[6], saved.box[7], saved.box[8]};
        door.step = saved.step;
        door.phase = saved.phase_next;
        door.state = static_cast<DoorState>(saved.state);
        door.open_fraction = static_cast<double>(door.phase) * kDoorPhaseToFraction;
        door.sliding = false;
        for (std::size_t i = 0; i < 3; ++i) {
            if (std::fabs(door.open[i] - door.closed[i]) > 1e-6) {
                door.sliding = true;
            }
        }
        door.linked = door.ids[3] != 0;

        ++matched;
    }
    return matched;
}

bool GameState::update_save(Save& save, std::string* error) const {
    if (error != nullptr) {
        error->clear();
    }
    std::string message;
    if (!validate(&message)) {
        set_error(error, message);
        return false;
    }
    if (!same_level(save.header.level_name, level_name)) {
        set_error(error, "save level \"" + save.header.level_name +
                             "\" does not match state level \"" + level_name + "\"");
        return false;
    }
    if (save.find_block(9) == nullptr || !save.player) {
        set_error(error, "save has no player block (9)");
        return false;
    }
    if (!save.player->has_slot) {
        set_error(error, "player block has no present slot");
        return false;
    }
    if (!doors.empty() && save.find_block(2) == nullptr) {
        set_error(error, "save has no door block (2) but the state has " +
                             std::to_string(doors.size()) + " doors");
        return false;
    }
    const bool any_thing =
        !things.empty() ||
        std::any_of(thing_present.begin(), thing_present.end(),
                    [](bool present) { return present; });
    if (any_thing && save.find_block(3) == nullptr) {
        set_error(error, "save has no thing block (3) but the state has " +
                             std::to_string(things.size()) + " things");
        return false;
    }

    save.header.fields = fields;
    save.header.fields[1] = level_clock; // the counter API is authoritative

    SavePlayer& player = *save.player;
    player.position = position;
    player.angles = angles;
    player.history_index = history_index;
    player.exists = exists;
    player.status_present = status_present;
    player.object_id = object_id;
    player.armor_max = armor_max;
    player.armor = armor;
    player.shield_max = shield_max;
    player.shields = shields;
    player.weapon_slot = weapon_slot;
    player.weapon_mask = weapon_mask;
    player.ammo = ammo;

    if (save.find_block(2) != nullptr) {
        save.doors = doors;
    }
    if (save.find_block(3) != nullptr) {
        save.thing_present = thing_present;
        save.things = things;
    }
    return true;
}

std::optional<Save> GameState::to_save(std::string& error) const {
    error.clear();
    if (!has_template_) {
        error = "game state was not built from a save";
        return std::nullopt;
    }
    Save save = template_;
    if (!update_save(save, &error)) {
        return std::nullopt;
    }
    return save;
}

} // namespace gm
