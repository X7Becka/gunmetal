#include "game/save_load.h"

#include "core/path.h"      // iequals_ascii, resolve_ci
#include "game/chassis_loadout.h" // ChassisSpec
#include "player/controller.h"    // ControllerConfig, kPlayerTickHz

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <system_error>
#include <utility>

namespace gm {
namespace {

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

bool fail(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
    return false;
}

std::string trim_name(std::string name) {
    auto space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!name.empty() && space(name.back())) {
        name.pop_back();
    }
    std::size_t start = 0;
    while (start < name.size() && space(name[start])) {
        ++start;
    }
    name.erase(0, start);
    return name;
}

/// Header level names are plain ids ("LEVEL0").  Tolerate a path-like
/// spelling with an extension; `load_level()` appends `.sbt`/`.sbw` itself.
/// The result is lower-cased: level ids are case-insensitive (the install
/// mixes `level0.sbt` and `Level10.sbc`) and the lower-case spelling is the
/// stable one a caller can print.
std::string normalize_level(std::string name) {
    name = trim_name(std::move(name));
    auto strip = [&name](std::string_view ext) {
        if (name.size() > ext.size() &&
            iequals_ascii(std::string_view(name).substr(name.size() - ext.size()), ext)) {
            name.resize(name.size() - ext.size());
        }
    };
    strip(".sbt");
    strip(".sbw");
    name = trim_name(std::move(name));
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}

bool all_digits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
}

std::size_t count_active_sector_stuff(const Save& save) {
    std::size_t active = 0;
    for (const SbtSectorSlot& slot : save.sector_stuff) {
        if (!slot.empty()) {
            ++active;
        }
    }
    return active;
}

/// The chassis of an initialised `SimPlayer`.
///
/// `SimPlayer::init()` is the only way to place a player, and it rebuilds the
/// `ControllerConfig` from a `ChassisSpec`; the save carries no chassis, so the
/// spec is derived from the config the caller already chose.  `init()` only
/// changes eye height, max speed and retention, so the re-init is exact when
/// `max_speed / 91 * 91 == max_speed`; the neighbouring doubles are tried for
/// the values where it is not (the shipped LIGHT 1.7 already round-trips).
ChassisSpec chassis_from_player(const SimPlayer& player) {
    const ControllerConfig& config = player.config();
    ChassisSpec spec;
    spec.name = "save-load";
    spec.eye_height = config.eye_height;
    spec.max_speed = config.max_speed / kPlayerTickHz;
    if (spec.max_speed * kPlayerTickHz != config.max_speed) {
        const double up =
            std::nextafter(spec.max_speed, std::numeric_limits<double>::infinity());
        const double down =
            std::nextafter(spec.max_speed, -std::numeric_limits<double>::infinity());
        if (up * kPlayerTickHz == config.max_speed) {
            spec.max_speed = up;
        } else if (down * kPlayerTickHz == config.max_speed) {
            spec.max_speed = down;
        }
    }
    return spec;
}

/// Save blocks 1/2/3/4/5/6/7/11/12/15/16/17/18/19/20 have no receiver in the
/// runtime model; every one of them is named here instead of being silently
/// dropped (`docs/SAVE_LOAD.md` §3).
std::string block_reason(const SaveBlock& block) {
    const std::string id = std::to_string(block.id);
    switch (block.id) {
    case 1:
        return "block 1 switches (" + std::to_string(block.count) +
               "): the runtime installs the level's ADD_SWITCH table";
    case 2:
        return "block 2 doors (" + std::to_string(block.count) +
               "): doors are built from the level .sbt, the saved state/phase is not replayed";
    case 3:
        return "block 3 things (" + std::to_string(block.count) +
               "): opaque 0x528 records, no proven field map";
    case 4:
        return "block 4 thing types (" + std::to_string(block.count) +
               "): level table, rebuilt by vm_state_from_level()";
    case 5:
        return "block 5 named items (" + std::to_string(block.count) +
               "): inventory, no runtime model";
    case 6:
        return "block 6 sprites (" + std::to_string(block.count) +
               "): no receiver in GameRuntime";
    case 7:
        return "block 7 sound sources (" + std::to_string(block.count) +
               "): no receiver in GameRuntime";
    case 11:
        return "block 11 script (" + std::to_string(block.count) +
               " bytes): compiled script, vm_state_from_level() builds the VM instead";
    case 12:
        return "block 12 sound table: rebuilt from the level .sbt";
    case 15:
        return "block 15 poly handles: asset handles, rebuilt from the level";
    case 16:
        return "block 16 object handles: asset handles, rebuilt from the level";
    case 17:
        return "block 17 sector handles: asset handles, rebuilt from the level";
    case 18:
        return "block 18 lifts (" + std::to_string(block.count) +
               "): lifts are built from the level .sbt";
    case 19:
        return "block 19 ITEM: item state, no runtime model";
    case 20:
        return "block 20 SOB table: asset table, rebuilt from the level";
    default:
        return "block " + id + ": no receiver in GameRuntime";
    }
}

} // namespace

std::optional<std::filesystem::path> resolve_save_path(
    const std::filesystem::path& game, std::string_view slot_or_path,
    std::string* error) {
    clear_error(error);
    if (slot_or_path.empty()) {
        fail(error, "--save-load wants a slot number 0..19 or a path");
        return std::nullopt;
    }
    std::error_code ec;
    if (all_digits(slot_or_path)) {
        long long slot = 0;
        try {
            slot = std::stoll(std::string(slot_or_path));
        } catch (const std::exception&) {
            fail(error, "save slot \"" + std::string(slot_or_path) + "\" is not a number");
            return std::nullopt;
        }
        if (slot < 0 || slot >= kSaveLoadSlots) {
            fail(error, "save slot " + std::to_string(slot) + " out of range [0, " +
                            std::to_string(kSaveLoadSlots - 1) + "]");
            return std::nullopt;
        }
        const std::string relative = "SAVEGAME/saved" + std::to_string(slot) + ".sbt";
        const auto path = resolve_ci(game, relative);
        if (!path) {
            fail(error, "save slot " + std::to_string(slot) + ": " + relative +
                            " not found under " + game.string());
            return std::nullopt;
        }
        return path;
    }

    const std::filesystem::path given(slot_or_path);
    if (std::filesystem::is_regular_file(given, ec)) {
        return given;
    }
    const std::filesystem::path joined = game / given;
    if (std::filesystem::is_regular_file(joined, ec)) {
        return joined;
    }
    fail(error, "save file not found: " + given.string());
    return std::nullopt;
}

std::string save_level_name(const Save& save) {
    return normalize_level(save.header.level_name);
}

bool apply_save_sector_stuff(const Save& save, VmState& vm, LoadedSave& out,
                             std::string* error) {
    clear_error(error);
    out.sector_stuff = save.sector_stuff.size();
    out.sector_stuff_active = count_active_sector_stuff(save);
    if (save.sector_stuff.empty()) {
        out.sector_stuff_applied = false;
        return true;
    }
    if (save.sector_stuff.size() != kSbtSectorSlots) {
        out.sector_stuff_applied = false;
        return fail(error, "SECTOR_STUFF: expected " + std::to_string(kSbtSectorSlots) +
                               " slots, got " + std::to_string(save.sector_stuff.size()));
    }
    // Proven loader (`0x46b7b0`, `docs/RECORDS.md` §2): the whole 0x5800 block
    // overwrites the runtime array 0x800b60.
    vm_load_sector_stuff(vm, save.sector_stuff);
    out.sector_stuff_applied = true;
    return true;
}

bool apply_save(const Save& save, GameRuntime& runtime, SimPlayer& player,
                LoadedSave& out, std::string* error) {
    clear_error(error);
    if (!player.initialized()) {
        return fail(error, "apply_save: SimPlayer::init() has not been called "
                           "(the capsule config comes from the player, the save has no chassis)");
    }
    if (!save.player) {
        return fail(error, "save has no player block (9)");
    }
    const SavePlayer& source = *save.player;
    if (!source.has_slot) {
        return fail(error, "player block has no present slot");
    }
    for (const double value : source.position) {
        if (!std::isfinite(value)) {
            return fail(error, "player position is not finite");
        }
    }
    for (const double value : source.angles) {
        if (!std::isfinite(value)) {
            return fail(error, "player angles are not finite");
        }
    }
    if (source.weapon_slot < 0 || source.weapon_slot >= kWeaponSlots) {
        return fail(error, "player weapon slot " + std::to_string(source.weapon_slot) +
                               " out of range [0, " + std::to_string(kWeaponSlots - 1) + "]");
    }

    // Gauges first: `SimPlayer::set_vitals()` is the documented validator
    // (`docs/PLAYER_FIELDS.md` §11) and it only mutates on success.  It also
    // becomes the starting set, so the `init()` below starts this very life
    // from the saved gauges (`alive` follows `armor > 0`).
    Vitals vitals;
    vitals.armor = source.armor;
    vitals.armor_max = source.armor_max;
    vitals.shield_max = source.shield_max;
    vitals.shields = source.shields;
    vitals.mult_plate = 1.0; // documented default (`0x452cd0`, docs/DAMAGE.md §2.3)
    vitals.mult_hull = 1.0;
    if (!player.set_vitals(vitals, error)) {
        return false;
    }

    const Vec3d feet{source.position[0], source.position[1], source.position[2]};
    const double yaw = source.angles[2]; // struct+0x28, `docs/SAVE_FIELDS.md` §9.2
    if (!player.init(feet, yaw, chassis_from_player(player), error)) {
        return false;
    }

    // The runtime owns the eye points and one gauge set per eye; installing
    // both is what makes the saved state live before the first tick.
    Vec3d eye = player.state().pos;
    eye.z += player.config().eye_height;
    const std::span<const Vec3d> eyes(&eye, 1);
    if (!runtime.set_player_eyes(eyes)) {
        return fail(error, "runtime rejected the saved eye point");
    }
    const std::span<const Vitals> runtime_vitals(&vitals, 1);
    if (!runtime.set_player_vitals(runtime_vitals)) {
        return fail(error, "runtime rejected the saved gauges");
    }

    Arsenal arsenal;
    arsenal.ammo = source.ammo;
    arsenal.mask = source.weapon_mask;
    arsenal.current = source.weapon_slot;
    runtime.player_arsenal() = arsenal;

    // --- what was written, what was not ------------------------------------
    const bool sector_applied = out.sector_stuff_applied;
    out = LoadedSave{};
    out.sector_stuff_applied = sector_applied;
    out.title = save.header.title;
    out.level = save_level_name(save);
    out.player_applied = true;
    out.vitals = vitals;
    out.feet = feet;
    out.yaw = yaw;
    out.angles = source.angles;
    out.arsenal = arsenal;
    out.blocks = save.blocks.size();
    out.sector_stuff = save.sector_stuff.size();
    out.sector_stuff_active = count_active_sector_stuff(save);

    out.unsupported.emplace_back(
        "header F0..F21 (level clock, mission, game mode, options): no runtime receiver");
    for (const SaveBlock& block : save.blocks) {
        if (block.id == 9) {
            continue; // applied above
        }
        if (block.id == 8 && out.sector_stuff_applied) {
            continue; // loaded by apply_save_sector_stuff()
        }
        if (block.id == 8) {
            out.unsupported.emplace_back(
                "block 8 SECTOR_STUFF (" + std::to_string(out.sector_stuff_active) +
                " active of " + std::to_string(out.sector_stuff) +
                "): no VmState in apply_save(); call apply_save_sector_stuff() or the "
                "VmState overload");
            continue;
        }
        out.unsupported.push_back(block_reason(block));
    }
    out.unsupported.emplace_back(
        "player history ring/history_index, attachments[16], object_id/exists/present: "
        "no runtime receiver");
    return true;
}

bool apply_save(const Save& save, VmState& vm, GameRuntime& runtime,
                SimPlayer& player, LoadedSave& out, std::string* error) {
    if (!apply_save_sector_stuff(save, vm, out, error)) {
        return false;
    }
    return apply_save(save, runtime, player, out, error);
}

} // namespace gm
