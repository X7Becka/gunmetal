// Chassis table (`M5.1.6` leftover / `M6.1.4`): `DEFINE_PLAYER_TYPES`.
//
// The level script declares the four chassis with opcode 172
// (`docs/FORMATS.md` §12.3): three strings plus 0x17c bytes of numbers.  The
// only field with a proven consumer is the **eye height**:
//
//   * `docs/CAMERA.md`: `player+0x2F50` is the eye height, read as
//     `eye = (x, y, z + player[0x2F50])`, and it is filled from this table;
//   * the saved game `saved0.sbt` has 10.0 there for `LIGHT CHASSIS`;
//   * across all 36 levels field **9** (a double) is exactly 10.0 for LIGHT,
//     11.0 for MEDIUM, 11.0 for STEALTH and 12.0 for HEAVY — the values the doc
//     lists, with no exceptions.
//
// `57 CHEVY` (14.0) is not in any level table: it comes from the chassis
// selection, so this table only carries the four campaign chassis.
#pragma once

#include "res/sbtvm.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

/// Signature of one `DEFINE_PLAYER_TYPES` record after the three strings
/// (`gm/sbtvm.py::TABLE_SIGS[172]`; the text loader feeds the same letters to
/// 0x461bc0, so the sizes are authoritative).
inline constexpr std::string_view kPlayerTypeSignature =
    "LDLLDODDDDLNVNVEEDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDEE";

/// Doubles are the wide letters of the signature.
constexpr bool player_type_field_is_real(std::size_t field) {
    if (field >= kPlayerTypeSignature.size()) {
        return false;
    }
    const char letter = kPlayerTypeSignature[field];
    return letter == 'D' || letter == 'A' || letter == 'B';
}

/// `player+0x2F50`: the eye height is the tenth numeric field.
inline constexpr std::size_t kEyeHeightField = 9;

/// One chassis record.
struct PlayerTypeDef {
    std::string name;            ///< first string (`LIGHT CHASSIS`, ...)
    std::vector<std::string> strings; ///< all strings, name first
    /// Every numeric field in signature order (integers widened to double).
    std::vector<double> values;

    double eye_height() const {
        return kEyeHeightField < values.size() ? values[kEyeHeightField] : 0.0;
    }
};

/// The chassis table of one level (four records, eight in `level0`).
struct PlayerTypeTable {
    /// The store is sized for 32 rows (`docs/FORMATS.md` §12.3 has no smaller
    /// limit); shipped levels carry 4.
    static constexpr std::size_t kMaxRecords = 32;
    /// Numeric fields in one record = 0x17c bytes.
    static constexpr std::size_t kFieldCount = kPlayerTypeSignature.size();

    std::vector<PlayerTypeDef> defs;

    /// Build from the VM's decoded records; fails on too many rows, a wrong
    /// field count or a non-numeric field.
    static std::optional<PlayerTypeTable> from_records(
        const std::vector<SbtTableRecord>& records, std::string* error = nullptr);

    /// Record by name, or nullptr.
    const PlayerTypeDef* find(std::string_view name) const;
    bool empty() const { return defs.empty(); }
    std::size_t size() const { return defs.size(); }
};

} // namespace gm
