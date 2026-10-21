#pragma once

// Single C++ bridge between the on-disk save (`res/save.*`), the runtime
// player (`player/controller.*`) and the runtime door state (`world/doors.*`).
//
// `GameState` is a value object: it never owns platform/render state and has no
// globals.  It is built from a parsed `Save`, can be applied to a `PlayerState`
// and a `WorldState`, and can be written back into a `Save` without touching
// the blocks it does not understand.  The mapping and the proven/hypothesis
// split live in docs/GAME_STATE.md.
//
// Typical load:
//
//     std::optional<gm::Save> save = gm::parse_save(bytes, error);
//     auto state = gm::GameState::from_save(std::move(*save), "level0", error);
//     if (state) {
//         state->apply_to_player(player);
//         state->apply_to_doors(world);
//     }
//
// Typical save:
//
//     std::optional<gm::Save> save = gm::parse_save(bytes, error); // template
//     state->update_save(*save, &error);        // or state->to_save(error)
//     std::optional<std::vector<std::uint8_t>> out = save->to_bytes(error);

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/weapons.h"      // Vitals
#include "player/controller.h" // PlayerState
#include "res/save.h"          // Save, SaveDoor, SavePlayer
#include "world/doors.h"       // WorldState

namespace gm {

/// Gameplay state of one save slot: the player block (9), the runtime door
/// array (2), the opaque thing records (3), the level id and the header
/// counters.
///
/// Unknown save blocks (8/11/12/15..20, ...) are not interpreted.  They stay in
/// `Save::blocks` and are copied verbatim by `Save::to_bytes`, so the only
/// blocks this class rewrites are 9 (player), 2 (doors) and 3 (things).
struct GameState {
    /// Level identity from the save header (`LEVEL0` in both shipped saves).
    /// Matching via `from_save`/`update_save` is case-insensitive and ignores
    /// a path or an `.sbt` extension.
    std::string level_name;

    /// Verbatim header counters F0..F21 (docs/SAVE_FIELDS.md §2).  `F1` is the
    /// level clock; `level_clock` is a convenience copy of it.  On write
    /// `level_clock` wins and is copied back into `fields[1]`.
    std::array<std::uint32_t, kSaveFieldCount> fields{};
    std::uint32_t level_clock = 0;

    // --- player block (9) ---------------------------------------------------
    bool has_player = false;
    std::array<double, 3> position{}; ///< world X/Y/Z, struct+0x00 (feet point)
    std::array<double, 3> angles{};   ///< Euler pitch/roll/yaw, struct+0x18
    std::int32_t history_index = 0;   ///< struct+0x3204
    std::uint32_t exists = 0;         ///< struct+0x3178
    std::int32_t status_present = 0;  ///< struct+0x3210
    std::int32_t object_id = 0;       ///< struct+0x3214
    std::int32_t armor_max = 0;       ///< hundredths of a point
    std::int32_t armor = 0;           ///< hundredths of a point
    std::int32_t shield_max = 0;      ///< hundredths of a point
    std::array<std::int32_t, 4> shields{}; ///< front/right/back/left, hundredths
    std::int32_t weapon_slot = 0;     ///< 0..11
    std::uint32_t weapon_mask = 0;    ///< `1 << slot` per available slot
    std::array<std::uint32_t, 12> ammo{}; ///< struct+0x329c

    // --- door block (2) -----------------------------------------------------
    /// Full door records in file order, including fields the runtime does not
    /// read (constant, param5/param6) so an untouched save round-trips.
    std::vector<SaveDoor> doors;

    // --- thing block (3) ----------------------------------------------------
    /// Runtime things are opaque in the save format (no proven field map); they
    /// are loaded and written back verbatim so no record is lost.  The runtime
    /// (`world/things.*`) has no proven save mapping, so no `apply_to_things`
    /// exists yet.
    std::array<bool, kSaveThingSlots> thing_present{};
    std::vector<SaveThing> things;

    /// Build from a parsed save and a requested level id.
    ///
    /// Fails (nullopt + `error`) when the save level does not match `level`
    /// (`level0`, `LEVEL0` and `level0.sbt` are equivalent), when block 9 is
    /// missing, when it has no present slot, or when `validate()` rejects the
    /// values.  The save is kept as the template for `to_save()`.
    static std::optional<GameState> from_save(Save save, std::string_view level,
                                              std::string& error);

    /// Structural/value check: player present, finite position/angles, gauges
    /// within their maxima, weapon slot/mask in range, every door's
    /// sector/object/state/phase in range.  No level/world lookup is done, so
    /// a save can be validated before a `.sbw` is loaded.
    bool validate(std::string* error = nullptr) const;

    /// Write the player fields into a runtime controller state.
    ///
    /// `pos` becomes the saved world point (the feet point, the same semantics
    /// as `make_state`), `yaw` becomes `angles[2]`, velocity is zeroed and
    /// `on_ground` is cleared.  Capsule height/eye height stay owned by the
    /// caller's `ControllerConfig`.
    bool apply_to_player(PlayerState& player, std::string* error = nullptr) const;

    /// Runtime player gauges in the damage model's units (hundredths).
    /// The save has no burn/hitstun/status fields, so those stay zero and
    /// `mult_plate`/`mult_hull` keep their documented defaults.
    Vitals vitals() const;

    /// Copy gauges from the damage model back into the save-shaped state.
    /// Rejects out-of-range values without modifying this object.
    bool apply_vitals(const Vitals& vitals, std::string* error = nullptr);

    /// Apply the saved door runtime state to `world`.
    ///
    /// Doors are matched by the proven `(sector, object)` address; every match
    /// gets `flags/ids/closed/open/size/step/state/phase` from the save
    /// (`phase` = `phase_next`).  Runtime-only fields (`locked`, `timer`,
    /// `hold_ticks`) are not in the save and stay untouched.  Returns the
    /// number of matched doors; doors present in the save but not in the level
    /// are skipped.  On a validation failure returns 0 and fills `error`.
    std::size_t apply_to_doors(WorldState& world, std::string* error = nullptr) const;

    /// Write this state into an existing `Save`, preserving its block chain.
    ///
    /// The save must carry the same level and a player block; a door/thing
    /// block is required when the state has doors/things.  Only block 9 fields
    /// and the typed door/thing vectors are changed, all other blocks keep
    /// their raw bytes.
    bool update_save(Save& save, std::string* error = nullptr) const;

    /// Copy the source save, patch it via `update_save()` and return it.
    /// Fails when the state was not created by `from_save()`.
    std::optional<Save> to_save(std::string& error) const;

private:
    Save template_{};
    bool has_template_ = false;
};

} // namespace gm
