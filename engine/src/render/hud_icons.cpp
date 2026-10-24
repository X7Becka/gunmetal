#include "render/hud_icons.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace gm {
namespace {

/// ASCII case folding only: BIGFILE names are 8-byte uppercase ASCII, and
/// locale-dependent `std::tolower` would be a trap.
char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool name_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// Normalize an angle to ``[0, 360)``.  ``fmod`` keeps the fractional part, so
/// the model's fractions survive; a result that rounds up to 360 after the
/// subtraction of the multiple is folded back.
double normalize_deg(double deg) {
    double value = std::fmod(deg, 360.0);
    if (value < 0.0) {
        value += 360.0;
    }
    if (value >= 360.0) {
        value -= 360.0;
    }
    return value;
}

void set_error(std::string* error, std::string text) {
    if (error != nullptr) {
        *error = std::move(text);
    }
}

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

} // namespace

const char* icon_kind_name(IconKind kind) {
    switch (kind) {
    case IconKind::weapon:
        return "weapon";
    case IconKind::weapon_alt:
        return "weapon_alt";
    case IconKind::equipment:
        return "equipment";
    case IconKind::ammo:
        return "ammo";
    case IconKind::unknown:
        break;
    }
    return "unknown";
}

// ------------------------------------------------------------------- Arch

std::optional<Arch> Arch::open(const std::filesystem::path& bmp_idx,
                               std::string& error) {
    auto file = BigFile::open(bmp_idx, error);
    if (!file) {
        return std::nullopt;
    }
    Arch arch;
    arch.idx_path = bmp_idx;
    arch.file = std::make_shared<const BigFile>(std::move(*file));
    error.clear();
    return arch;
}

bool Arch::empty() const {
    return !file || file->entries().empty();
}

std::size_t Arch::size() const {
    return file ? file->entries().size() : 0;
}

const BigEntry* Arch::find(std::string_view name) const {
    if (!file || name.empty()) {
        return nullptr;
    }
    for (const BigEntry& entry : file->entries()) {
        if (name_equals(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

int Arch::index_of(std::string_view name) const {
    if (!file) {
        return -1;
    }
    const auto& entries = file->entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (name_equals(entries[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<BmpImage> Arch::image(std::string_view name,
                                    std::string& error) const {
    const BigEntry* entry = find(name);
    if (entry == nullptr) {
        error = "asset '" + std::string(name) + "' is not in " +
                (idx_path.empty() ? std::string("the archive") : idx_path.string());
        return std::nullopt;
    }
    std::string read_error;
    auto blob = file->read(*entry, read_error);
    if (!blob) {
        error = "asset '" + entry->name + "': " + read_error;
        return std::nullopt;
    }
    std::string parse_error;
    auto image = parse_bmp(std::span<const std::uint8_t>(*blob), parse_error);
    if (!image) {
        error = "asset '" + entry->name + "' is not a readable 8-bit BMP: " +
                parse_error;
        return std::nullopt;
    }
    error.clear();
    return image;
}

// ------------------------------------------------------------ icon tables

namespace {

// Shipped ``level0.sbt`` ``DEFINE_WEAPONS`` order (all 19 kinds).  The doc's
// name -> asset table is docs/HUD_ICONS.md §1.3; the slot order is what the
// level loader increments into ``0x80fa00 + slot*0x2a8``.  Cross-checked
// against ``gm::Weapons::load(level0.sbt)`` by the test, and against
// tools/re/hud_icons.py --icons (684 records, no conflicts).
constexpr WeaponIconSlot kWeaponIconTable[] = {
    {"MACHINE GUN", "WICON1"},
    {"CANNON", "WICON3"},
    {"AUTOCANNON", "WICON4"},
    {"RAILGUN", "WICON5"},
    {"GRENADE", "WICON6"},
    {"VIRAL AGENT", "WCON41"},
    {"FLAMETHROWER", "WICON7"},
    {"ROCKET", "WCON18"},
    {"RGM", "WCON20"},
    {"TAC NUKE", "WICON8"},
    {"MRL", "WCON43"},
    {"LASER", "WICON9"},
    {"FUSIONBEAM", "WCON10"},
    {"PLASMA CANNON", "WCON16"},
    {"MICROWAVE GUN", "WCON22"},
    {"EMP GUN", "WCON24"},
    {"DISRUPTORFIELD", "WCON25"},
    {"DISINTEGRATOR", "WCON11"},
    {"MESON GUN", "WCON23"},
};
static_assert(sizeof(kWeaponIconTable) / sizeof(kWeaponIconTable[0]) ==
                  kWeaponIconCount,
              "weapon icon table must hold every shipped kind");

} // namespace

std::span<const WeaponIconSlot> weapon_icon_table() {
    return std::span<const WeaponIconSlot>(kWeaponIconTable,
                                           kWeaponIconCount);
}

std::optional<IconRef> icon_asset(std::string_view asset, IconKind kind,
                                  const Arch& bmp, std::string* error) {
    if (asset.empty()) {
        set_error(error, "icon asset name is empty");
        return std::nullopt;
    }
    if (bmp.empty()) {
        set_error(error, "BMP archive is empty (no index entries): cannot look up '" +
                             std::string(asset) + "'");
        return std::nullopt;
    }
    const BigEntry* entry = bmp.find(asset);
    if (entry == nullptr) {
        set_error(error, "asset '" + std::string(asset) + "' is not in " +
                             bmp.idx_path.string() + " (" +
                             std::to_string(bmp.size()) + " entries)");
        return std::nullopt;
    }
    std::string image_error;
    const auto image = bmp.image(entry->name, image_error);
    if (!image) {
        set_error(error, image_error);
        return std::nullopt;
    }
    if (image->width <= 0 || image->height <= 0) {
        set_error(error, "asset '" + entry->name + "' has an empty bitmap (" +
                             std::to_string(image->width) + "x" +
                             std::to_string(image->height) + ")");
        return std::nullopt;
    }
    IconRef ref;
    ref.kind = kind;
    ref.asset = std::string(asset); // the requested spelling: WICON1 / equip0
    ref.width = image->width;
    ref.height = image->height;
    ref.index_in_archive = bmp.index_of(entry->name);
    clear_error(error);
    return ref;
}

std::optional<IconRef> weapon_icon(int weapon_id, bool alt, const Arch& bmp,
                                   std::string* error) {
    if (weapon_id < 0 || weapon_id >= kWeaponIconCount) {
        set_error(error, "weapon id " + std::to_string(weapon_id) +
                             " is outside the shipped level0 DEFINE_WEAPONS "
                             "table 0.." +
                             std::to_string(kWeaponIconCount - 1));
        return std::nullopt;
    }
    if (alt) {
        set_error(error,
                  "weapon " + std::to_string(weapon_id) +
                      " has no alternate icon: DEFINE_WEAPONS carries one "
                      "24x24 bitmap at record+0x50 (docs/HUD_ICONS.md §1.2, §3)");
        return std::nullopt;
    }
    const WeaponIconSlot& slot = kWeaponIconTable[weapon_id];
    return icon_asset(slot.asset, IconKind::weapon, bmp, error);
}

std::optional<IconRef> equipment_icon(int equip_id, const Arch& bmp,
                                      std::string* error) {
    if (equip_id < 0 || equip_id >= kEquipmentArchiveCount) {
        set_error(error, "equipment id " + std::to_string(equip_id) +
                             " is outside equip0..equip" +
                             std::to_string(kEquipmentArchiveCount - 1) +
                             " (HUD draws 0.." +
                             std::to_string(kEquipmentHudCount - 1) +
                             ", cmp eax,0xc at 0x492cc3)");
        return std::nullopt;
    }
    const std::string asset = "equip" + std::to_string(equip_id);
    return icon_asset(asset, IconKind::equipment, bmp, error);
}

// ---------------------------------------------------------------- compass

CompassPose compass_pos(double bearing_deg, const CompassGeometry& geometry) {
    CompassPose pose;
    if (!std::isfinite(bearing_deg) || !std::isfinite(geometry.yaw_deg) ||
        !std::isfinite(geometry.origin_x) || !std::isfinite(geometry.px_per_deg)) {
        pose.bearing_deg = bearing_deg;
        pose.x = kCompassInvalid;
        pose.y = kCompassInvalid;
        return pose;
    }
    const double bearing = normalize_deg(bearing_deg);
    pose.bearing_deg = bearing;
    pose.y = geometry.y;
    // The window shows less than 180 deg, so the bearing is drawn once: use
    // the nearest copy of the tape and let the caller clip by the window.
    double delta = normalize_deg(bearing - geometry.yaw_deg);
    if (delta >= 180.0) {
        delta -= 360.0;
    }
    const double x = geometry.origin_x + geometry.px_per_deg * delta;
    if (!std::isfinite(x) || x <= static_cast<double>(std::numeric_limits<int>::min()) + 1.0 ||
        x >= static_cast<double>(std::numeric_limits<int>::max())) {
        pose.x = kCompassInvalid;
        return pose;
    }
    pose.x = static_cast<int>(std::lround(x));
    return pose;
}

bool compass_in_window(int x, const CompassGeometry& geometry) {
    return x >= geometry.window_left && x <= geometry.window_right;
}

} // namespace gm
