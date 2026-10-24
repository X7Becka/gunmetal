#pragma once

// Save -> live runtime (M8.7/M9, slice 2; docs/SAVE_LOAD.md).
//
// Slice 1 (`res/save.*`, `ui/save_slots.*`) parses and prints a save; slice 2
// (`game/state.*`) turns one into a value object.  This module is the last
// step: it writes the subset of the save that has a proven **receiver** into a
// live `GameRuntime` + `SimPlayer` pair, and names everything it cannot write
// instead of inventing a mapping.
//
// Applied (field -> receiver):
//
//   * `SaveHeader::level_name`      -> `LoadedSave::level` (the caller loads it)
//   * player `+0x00..0x10` position -> `SimPlayer::state().pos` via
//     `SimPlayer::init()` (the feet point, `docs/SAVE_FIELDS.md` §9.2)
//   * player `+0x28` yaw            -> `SimPlayer` yaw (`angles[2]`)
//   * player `+0x3244..0x3260`      -> `SimPlayer::set_vitals()` and
//     `GameRuntime::set_player_vitals()` (armor/shields, save hundredths)
//   * player `+0x3294/0x3298/0x329c`-> `GameRuntime::player_arsenal()`
//     (selected slot, `1 << slot` mask, 12 ammo counters)
//   * block 8 `SECTOR_STUFF`        -> `VmState::sectors` through the proven
//     loader `vm_load_sector_stuff()` (`0x46b7b0`), VmState overload only
//
// Nothing in the save is written back and no game file is touched.
//
// Not applied (listed in `LoadedSave::unsupported` with the reason): the 22
// header counters F0..F21, the switch/door/thing/sprite/sound blocks (the
// runtime builds those from the level `.sbt`, it does not replay the save),
// the asset-table blocks (5/12/15/17/20), the ITEM block (19), and the player
// round-robin history / attachments / `object_id` / `exists`.  The full table
// is in docs/SAVE_LOAD.md §3.
//
// Limits (docs/SAVE_LOAD.md §5): the level is resolved from the header name
// through the install (`docs/BUILD.md`), not from the paired
// `saved<N>.sbw`; things, triggers, script state and the save's own door/switch
// state are a later slice.

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "game/sim_player.h" // SimPlayer
#include "game/runtime.h"    // GameRuntime
#include "game/weapons.h"    // Vitals, Arsenal
#include "render/collide.h"  // Vec3d
#include "res/save.h"        // Save
#include "res/vm_exec.h"     // VmState

namespace gm {

/// Number of save slots the front-end addresses (`SAVEGAME/saved0..19.sbt`,
/// `ui/save_slots.h`, `docs/SAVE_SLOTS.md` §1).
inline constexpr int kSaveLoadSlots = 20;

/// What `apply_save()` wrote into the runtime, plus what it could not write.
struct LoadedSave {
    std::string title;   ///< header title (`name[0x50]`, up to the first NUL)
    std::string level;   ///< header level name, trimmed ("LEVEL0" -> "LEVEL0")
    bool player_applied = false; ///< the player block had a present slot

    Vitals vitals{};               ///< gauges installed on both receivers
    Vec3d feet{};                  ///< player position (struct+0x00)
    double yaw = 0.0;              ///< struct+0x28 (Euler angle 3)
    std::array<double, 3> angles{};///< struct+0x18/0x20/0x28 verbatim
    Arsenal arsenal{};             ///< loadout installed on the runtime

    std::size_t blocks = 0;        ///< `save.blocks.size()`
    std::size_t sector_stuff = 0;  ///< block 8 slots in the save (0 or 256)
    std::size_t sector_stuff_active = 0; ///< non-zero slots (shipped: 13)
    bool sector_stuff_applied = false;   ///< the VmState overload loaded it

    /// One line per save feature with no receiver in the runtime model.
    std::vector<std::string> unsupported;
};

/// Resolve `--save-load`'s argument: a decimal slot number (`0..19`) becomes
/// `SAVEGAME/saved<N>.sbt` under *game* (case-insensitive per component, like
/// `load_level()`), anything else is a path (as given, else under *game*).
///
/// Returns nullopt and a one-line reason for an empty argument, an
/// out-of-range slot, a non-numeric string that names no file, or a missing
/// file.  Never touches the game install.
std::optional<std::filesystem::path> resolve_save_path(
    const std::filesystem::path& game, std::string_view slot_or_path,
    std::string* error = nullptr);

/// Header level name as the install spells it: trimmed of whitespace and of a
/// trailing `.sbt`/`.sbw`, lower-cased (`LEVEL0` -> `level0`), so it can be
/// passed to `load_level()` directly.  Empty when the header carries no level
/// name (a synthetic save).  Level ids are case-insensitive (`res/level.h`).
std::string save_level_name(const Save& save);

/// Load block 8 (`SECTOR_STUFF`) into the VM state with the proven loader/// `vm_load_sector_stuff()` (`0x46b7b0`, `res/vm_exec.h`).
///
/// `out.sector_stuff`/`sector_stuff_active` are refreshed either way;
/// `sector_stuff_applied` is set only when a block was really loaded.  An
/// empty block is a successful no-op (the state is left untouched); a block
/// that does not hold exactly `kSbtSectorSlots` (256) slots is refused with a
/// text (the runtime array has a fixed size).  Call it **before**
/// `GameRuntime::build()` if a build-time consumer of the VM state matters.
bool apply_save_sector_stuff(const Save& save, VmState& vm, LoadedSave& out,
                             std::string* error = nullptr);

/// Apply the save to a live runtime and player.
///
/// The player must already be initialised (`SimPlayer::init()`): its
/// `ControllerConfig` is the capsule/eye/retention source, because the save
/// carries no chassis record.  The call re-initialises the player at the
/// save's feet/yaw with a chassis reconstructed from that config (re-init keeps
/// the config bit for bit and clears the per-life counters, so a load is a
/// clean start), then installs the saved gauges (`set_vitals()` and
/// `runtime.set_player_vitals()`), the saved loadout
/// (`runtime.player_arsenal()`) and the eye point (`runtime.set_player_eyes()`,
/// `feet + eye_height`).
///
/// Block 8 is only *counted* here (there is no `VmState`); use
/// `apply_save_sector_stuff()` first or the `VmState` overload, otherwise the
/// block is added to `out.unsupported`.
///
/// Returns false and a one-line text (leaving the runtime and the player
/// untouched as far as the failed step allows) when the save has no player
/// block, no present slot, a non-finite position/angle, out-of-range gauges
/// (`SimPlayer::set_vitals()` rules, `docs/PLAYER_FIELDS.md` §11) or a weapon
/// slot outside `0..11`.
bool apply_save(const Save& save, GameRuntime& runtime, SimPlayer& player,
                LoadedSave& out, std::string* error = nullptr);

/// `apply_save_sector_stuff()` + `apply_save()` in the order a fresh runtime
/// needs: the block 8 slots reach the VM state, then the runtime is filled.
/// The caller still builds the runtime from that same `VmState`.
bool apply_save(const Save& save, VmState& vm, GameRuntime& runtime,
                SimPlayer& player, LoadedSave& out,
                std::string* error = nullptr);

} // namespace gm
