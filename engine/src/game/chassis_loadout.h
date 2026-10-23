// Chassis loadout (`M6.1.4`): the nine `DEFINE_PLAYER_TYPES` fields that the
// original applies to the player.
//
// `docs/CHASSIS.md` §3/§4: the purchase handler `0x47a980` copies op0..op5,
// op9, op10 and op11 into the player at `0x47ac1a`..`0x47af3e`; the remaining
// ~85 fields are physics blocks that are copied verbatim and have no proven
// consumer yet.  This module is the single place that names the nine fields,
// applies the two documented scalings and turns a record into a
// `ChassisMotion` (before this file the controller's defaults were the LIGHT
// numbers, hardcoded).
//
// Provenance per field (all from `tools/re/chassis.py --fields`):
//
//   op0  mass          -> `0x81c378` (weight limit of the speed penalty)
//   op1  weight penalty-> `0x81bd80`
//   op2  armour        -> `0x81c28c`, stored as `*100`
//   op3  armour limit  -> `0x81c284`, stored as `*100`
//   op4  max speed     -> `0x81bd70`
//   op5  model index   -> `player+0x3214` (SOB_TABLE index)
//   op9  eye height    -> `0x81bf90` (`player+0x2f50`, proven consumer)
//   op10 price         -> `player+0x3468`
//   op11 sound index   -> `player+0x33e0` (sample table; -1 = silent)
#pragma once

#include "game/player_types.h"
#include "player/controller.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gm {

/// Indices into `PlayerTypeDef::values` (the numeric signature, op0 first).
inline constexpr std::size_t kChassisMass = 0;
inline constexpr std::size_t kChassisWeightPenalty = 1;
inline constexpr std::size_t kChassisArmour = 2;
inline constexpr std::size_t kChassisArmourLimit = 3;
inline constexpr std::size_t kChassisMaxSpeed = 4;
inline constexpr std::size_t kChassisModel = 5;
inline constexpr std::size_t kChassisEyeHeight = 9;
inline constexpr std::size_t kChassisPrice = 10;
inline constexpr std::size_t kChassisSound = 11;

/// Smallest record that still carries every applied field (op0..op11).
inline constexpr std::size_t kChassisFieldsNeeded = 12;

/// `0x47ac2d`/`0x47ac41`: the two armour operands are stored `*100`.
inline constexpr double kChassisArmourScale = 100.0;

/// One chassis after the documented scalings.
struct ChassisSpec {
    std::string name;
    double mass = 0.0;             ///< op0 (`0x81c378`)
    double weight_penalty = 0.0;   ///< op1 (`0x81bd80`)
    double armour = 0.0;           ///< op2, already `*100`
    double armour_limit = 0.0;     ///< op3, already `*100`
    double max_speed = 0.0;        ///< op4 (`0x81bd70`)
    int model_index = -1;          ///< op5 (`player+0x3214`)
    double eye_height = 0.0;       ///< op9 (`0x81bf90`)
    double price = 0.0;            ///< op10 (`player+0x3468`)
    int sound_index = -1;          ///< op11 (`player+0x33e0`), -1 = silent

    /// `0x479dd0`: `price + 4 * armour * shield_price` where `shield_price` is
    /// `[0x818954]` (50 for 100 shields in all 36 levels).  LIGHT:
    /// `10000 + 4 * 80 * 50 = 26000`, the shipped shop price.
    double shop_price(double shield_price) const {
        return price + 4.0 * (armour / kChassisArmourScale) * shield_price;
    }
};

/// Map one proven record.  Fails with a text when a field is missing, not
/// finite, or outside the range the original can store (model 0..255, sound
/// -1..255, non-negative numbers).
std::optional<ChassisSpec> chassis_spec(const PlayerTypeDef& def,
                                        std::string* error = nullptr);

/// `ChassisMotion` for this chassis: the values `0x47ac1a`..`0x47af3e` write
/// (`weight_limit` = op0, `weight_penalty` = op1, `max_speed` = op4).
ChassisMotion chassis_motion(const ChassisSpec& spec);

/// All `DEFINE_PLAYER_TYPES` records of a decoded script, walking IF/ELSE
/// bodies (the shipped levels keep the campaign table inside the `else`
/// branch; `level0` adds a `GOD_MODE` branch with the fifth chassis).
/// Records are returned in script order, table by table flattened.
std::vector<ChassisSpec> collect_chassis(const std::vector<SbtInsn>& insns);

/// Parse one `.sbt` and collect every chassis record in it.
/// `error` gets the reason when the file or the script cannot be read.
std::optional<std::vector<ChassisSpec>> chassis_from_level(
    const std::filesystem::path& level_path, std::string* error = nullptr);

} // namespace gm
