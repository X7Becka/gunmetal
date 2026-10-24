#pragma once

// Cockpit HUD icons (24x24) and the heading tape: id -> ``FRONTEND/BMP.IDX``.
//
// This is the data half of M4.4.5 slice 2.  It answers "which asset is the
// icon of weapon *n* / equipment *n*, and does the archive really hold it",
// plus the empirical compass geometry.  It does **not** draw anything: the
// board placement (23,170)/(201,170), the row direction of the two blits and
// the compositor belong to the caller (`docs/HUD_ICONS.md` §3).
//
// Proven (``docs/HUD_ICONS.md``; addresses are VA in ``gundx.exe``):
//   * the weapon icon is the third string of the ``DEFINE_WEAPONS`` record
//     (opcode 170): ``load("frontend", <icon>, record+0x50, 0, 24, 24)`` at
//     ``0x450c97``.  There is **no** id -> name formula: the name is data, and
//     every shipped weapon kind always carries the same asset (36 ``.sbt``,
//     684 records, 19 kinds, no conflict).  ``kWeaponIconTable`` is the shipped
//     ``level0.sbt`` order (19 records, proven by the C++ loader and the test).
//   * the equipment icon is ``sprintf(buf, "equip%d", i)`` (``0x48fa6f``,
//     format ``0x4be3a8``) over 12 HUD slots (``cmp eax,0xc``, ``0x492cc3``);
//     the archive also holds ``EQUIP12`` (``BALLAST``), outside the HUD window.
//   * both bitmaps are 24x24 indexed rasters (``0x240`` bytes, 24 bytes a row).
//
// Model (measured, not xref-backed - see ``docs/HUD_ICONS.md`` §5, §7.3):
//   ``x(bearing) = 102.62 + 0.2940 * (bearing - yaw)`` on the 51 px window
//   ``x 79..129``, ``y 2..15``; residual 1.08 px.  ``kCompassBasis`` records
//   that the base is a fit over three oracle frames, not proven code.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "res/bigfile.h"
#include "res/bmp.h"

namespace gm {

/// Which plate of the cockpit an icon belongs to.  ``weapon_alt`` is reserved:
/// the shipped record has exactly one bitmap (``record+0x50``), so an
/// "alternate" weapon icon is refused today rather than invented.
enum class IconKind { weapon, weapon_alt, equipment, ammo, unknown };

/// Stable lowercase name of *kind* (``"weapon"``, ``"equipment"``, ...).
const char* icon_kind_name(IconKind kind);

/// One resolved icon: the asset name as the caller spells it (the engine's
/// forms are ``WICON1`` from the script and ``equip0`` from ``sprintf``), the
/// real pixel size read from the BMP header (24x24 on the shipment, but the
/// reader reports what is there) and the 0-based position of the entry in the
/// archive index.
struct IconRef {
    IconKind kind = IconKind::unknown;
    std::string asset;
    int width = 0;
    int height = 0;
    int index_in_archive = -1;
};

/// Read-only, case-insensitive view of ``FRONTEND/BMP.IDX``.
///
/// ``Arch{}`` is a valid **empty** archive (``empty() == true``) so negatives
/// need no game files; ``open`` returns the error text when the index or its
/// ``.dat`` sibling is missing/malformed, exactly like ``BigFile::open``.
struct Arch {
    std::filesystem::path idx_path;
    std::shared_ptr<const BigFile> file;

    static std::optional<Arch> open(const std::filesystem::path& bmp_idx,
                                    std::string& error);

    bool empty() const;
    std::size_t size() const;

    /// Entry whose name equals *name* ignoring ASCII case, or nullptr.
    const BigEntry* find(std::string_view name) const;
    /// 0-based index of *name* in the index, or -1.
    int index_of(std::string_view name) const;
    /// Decode *name* as an 8-bit BMP; fills *error* when absent/broken.
    std::optional<BmpImage> image(std::string_view name, std::string& error) const;
};

/// One shipped weapon kind: the display name of ``DEFINE_WEAPONS`` and the
/// icon asset the same record names.
struct WeaponIconSlot {
    const char* name;
    const char* asset;
};

/// ``level0.sbt`` ``DEFINE_WEAPONS`` order: 19 kinds.  Data, not a formula -
/// see the header comment.  The C++ ``Weapons`` loader reads the same records,
/// and the test compares this table against the real ``level0.sbt``.
std::span<const WeaponIconSlot> weapon_icon_table();

inline constexpr int kWeaponIconCount = 19;
/// HUD equipment slots (``cmp eax,0xc``); the archive has one more.
inline constexpr int kEquipmentHudCount = 12;
/// ``equip0``..``equip12`` in ``FRONTEND/BMP.IDX`` (12 = ``BALLAST``).
inline constexpr int kEquipmentArchiveCount = 13;

/// Resolve a known asset name (the engine's spelling, any case) against
/// *bmp*.  Refuses, with text, an empty name, an empty archive and an asset the
/// archive does not hold; the bitmap must decode as an 8-bit BMP.
std::optional<IconRef> icon_asset(std::string_view asset, IconKind kind,
                                  const Arch& bmp, std::string* error);

/// Weapon icon by table slot (``level0.sbt`` order, 0..18).
///
/// * ``alt == false`` is the only supported form: the record owns one bitmap.
/// * ``alt == true`` is refused by text - ``WICON*``/``WCON*`` are different
///   weapons' icons, not two icons of one weapon.
/// * an id outside ``0..18``, an empty archive and a missing asset are refused
///   by text (never a silent ``nullopt`` without *error*).
///
/// The name comes from ``DEFINE_WEAPONS`` in the level's ``.sbt``; the table
/// here is the shipped level0 order because that level carries all 19 kinds.
std::optional<IconRef> weapon_icon(int weapon_id, bool alt, const Arch& bmp,
                                   std::string* error);

/// Equipment icon ``equip<id>`` (``0x48fa6f``).  ``0..12`` are accepted:
/// ``0..11`` are the HUD slots, 12 is ``BALLAST`` (present in the archive but
/// outside ``cmp eax,0xc``, ``0x492cc3``).  Out-of-range/absent is refused.
std::optional<IconRef> equipment_icon(int equip_id, const Arch& bmp,
                                      std::string* error);

// ---------------------------------------------------------------- compass

/// Basis marker for the compass numbers: fitted on ``win1..win3``, the drawing
/// code was not found (``docs/HUD_ICONS.md`` §5, §7.3).  Print it next to any
/// compass output so a MODEL value is not mistaken for a proven one.
inline constexpr const char* kCompassBasis =
    "MODEL (fit over win1..win3, docs/HUD_ICONS.md §5)";

/// Ribbon window measured in the 320x200 frame: ``x 79..129`` (51 px),
/// ``y 2..15``; long (cardinal) marks reach ``y 5..8``, letters ``y 9..14``.
inline constexpr int kCompassWindowLeft = 79;
inline constexpr int kCompassWindowRight = 129;
inline constexpr int kCompassWindowTop = 2;
inline constexpr int kCompassWindowBottom = 15;

/// ``x = origin_x + px_per_deg * (bearing - yaw)``: 0.2940 px/deg means a
/// 90 deg cardinal period of 26.46 px and a 173.5 deg window.
inline constexpr double kCompassOriginX = 102.62;
inline constexpr double kCompassPxPerDeg = 0.2940;

/// Returned in ``CompassPose::x``/``y`` for a non-finite input.
inline constexpr int kCompassInvalid = -2147483647 - 1;

/// Tape geometry.  ``yaw_deg`` is the player's heading (``player+0x0028``,
/// ``0x819068``); the ribbon moves **left** as yaw grows.  ``y`` is the top
/// row of the window - the pose names a column, the ribbon's vertical extent
/// is fixed.
struct CompassGeometry {
    double origin_x = kCompassOriginX;
    double px_per_deg = kCompassPxPerDeg;
    double yaw_deg = 0.0;
    int y = kCompassWindowTop;
    int window_left = kCompassWindowLeft;
    int window_right = kCompassWindowRight;
};

/// One bearing on the tape: ``bearing_deg`` is the normalized heading
/// (``[0, 360)``), ``x`` the rounded screen column.  ``x`` may lie outside the
/// window: the caller clips (the window is only ~173.5 deg wide), and the
/// nearest copy of the bearing is used (the delta is wrapped to
/// ``[-180, 180)``).  Non-finite input yields ``kCompassInvalid`` for x and y.
struct CompassPose {
    double bearing_deg = 0.0;
    int x = 0;
    int y = 0;
};

/// ``compass_pos`` for *bearing_deg* under *geometry*, with the bearing
/// normalized into ``[0, 360)`` (``450 -> 90``, ``-90 -> 270``).
CompassPose compass_pos(double bearing_deg, const CompassGeometry& geometry);

/// True when *x* falls inside the measured ribbon window.
bool compass_in_window(int x, const CompassGeometry& geometry);

} // namespace gm
