// Cockpit icon asset resolution (`id -> FRONTEND/BMP.IDX`) and the compass
// model of M4.4.5 slice 2 (docs/HUD_ICONS_CPP.md).
//
// Synthetic half (no game files): an empty `gm::Arch`, a hand-built BIGFILE
// pair holding a 2x3 BMP, and the refusal texts for an id outside the table,
// `alt=true`, an empty archive, a missing asset and a non-BMP blob.
//
// Shipped half (read-only install): `FRONTEND/BMP.IDX` (143 entries) and
// `level0.sbt` (19 `DEFINE_WEAPONS` records).  Every id of the table is
// resolved and the asset is re-read and re-parsed directly through
// `gm::BigFile` + `gm::parse_bmp`, so the test does not trust the module's own
// lookup.  The numbers are printed; the doc pins them.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/weapons.h"
#include "render/hud_icons.h"
#include "res/bigfile.h"
#include "res/bmp.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

template <typename T, typename U>
void check_eq(const T& got, const U& want, const std::string& what) {
    ++checks;
    if (!(got == want)) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want
                  << ")\n";
    }
}

void check_eq(const std::string& got, const char* want, const std::string& what) {
    check_eq<std::string, std::string>(got, want, what);
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

/// A minimal 8-bit BMP, 2x3, all indices zero.  Only the header fields that
/// `parse_bmp` reads are meaningful.
std::vector<std::uint8_t> make_bmp(int width, int height) {
    const std::uint32_t palette_bytes = 256 * 4;
    const std::uint32_t offset = 14 + 40 + palette_bytes;
    const std::uint32_t stride = (static_cast<std::uint32_t>(width) + 3) & ~3u;
    std::vector<std::uint8_t> out = {'B', 'M'};
    put32(out, offset + stride * static_cast<std::uint32_t>(height));
    put16(out, 0);
    put16(out, 0);
    put32(out, offset);
    put32(out, 40);
    put32(out, static_cast<std::uint32_t>(width));
    put32(out, static_cast<std::uint32_t>(height));
    put16(out, 1);
    put16(out, 8);
    put32(out, 0); // BI_RGB
    put32(out, stride * static_cast<std::uint32_t>(height));
    put32(out, 2835);
    put32(out, 2835);
    put32(out, 256);
    put32(out, 0);
    for (int i = 0; i < 256; ++i) {
        out.push_back(static_cast<std::uint8_t>(i));
        out.push_back(static_cast<std::uint8_t>(i));
        out.push_back(static_cast<std::uint8_t>(i));
        out.push_back(0);
    }
    out.resize(offset + stride * static_cast<std::uint32_t>(height), 0);
    return out;
}

/// `BIGFILE` index + `.dat` pair with the given blobs; returns the index path.
fs::path make_archive(const fs::path& dir, const std::vector<std::string>& names,
                      const std::vector<std::vector<std::uint8_t>>& blobs) {
    fs::create_directories(dir);
    std::vector<std::uint8_t> dat;
    std::vector<std::uint8_t> idx = {'B', 'I', 'G', 'F', 'I', 'L', 'E',
                                     static_cast<std::uint8_t>(0x1A)};
    put32(idx, static_cast<std::uint32_t>(names.size()));
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string name = names[i];
        name.resize(8, '\0');
        idx.insert(idx.end(), name.begin(), name.end());
        put32(idx, static_cast<std::uint32_t>(dat.size()));
        dat.insert(dat.end(), blobs[i].begin(), blobs[i].end());
    }
    const fs::path idx_path = dir / "BMP.IDX";
    write_file(idx_path, idx);
    write_file(dir / "BMP.DAT", dat);
    return idx_path;
}

// ------------------------------------------------------------- synthetic

void test_synthetic() {
    std::string error = "stale";

    // An empty archive is a valid object; every lookup is a text refusal.
    gm::Arch empty;
    check(empty.empty(), "synthetic: default Arch is empty");
    check_eq(empty.size(), std::size_t{0}, "synthetic: empty archive size");
    check(empty.find("WICON1") == nullptr, "synthetic: empty archive finds nothing");
    check_eq(empty.index_of("WICON1"), -1, "synthetic: empty archive index");

    check(!gm::weapon_icon(0, false, empty, &error), "synthetic: empty archive refuses weapon");
    check(contains(error, "empty"), "synthetic: empty archive text: " + error);

    // Bad ids are refused before the archive is touched.
    error.clear();
    check(!gm::weapon_icon(-1, false, empty, &error), "synthetic: weapon id -1 refused");
    check(contains(error, "outside"), "synthetic: weapon -1 text: " + error);
    error.clear();
    check(!gm::weapon_icon(999, false, empty, &error), "synthetic: weapon id 999 refused");
    check(contains(error, "outside"), "synthetic: weapon 999 text: " + error);
    error.clear();
    check(!gm::weapon_icon(0, true, empty, &error), "synthetic: alt icon refused");
    check(contains(error, "alternate"), "synthetic: alt text: " + error);
    error.clear();
    check(!gm::equipment_icon(-1, empty, &error), "synthetic: equipment -1 refused");
    check(contains(error, "outside"), "synthetic: equipment -1 text: " + error);
    error.clear();
    check(!gm::equipment_icon(13, empty, &error), "synthetic: equipment 13 refused");
    check(contains(error, "outside"), "synthetic: equipment 13 text: " + error);

    // A hand-built archive: one valid 2x3 BMP named HELLO.
    const fs::path root = fs::temp_directory_path() / "gm_hud_icons_tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const auto bmp_bytes = make_bmp(2, 3);
    const fs::path idx = make_archive(root, {"HELLO"}, {bmp_bytes});

    std::string open_error;
    auto arch = gm::Arch::open(idx, open_error);
    check(arch.has_value(), "synthetic: BIGFILE pair opens (" + open_error + ")");
    if (!arch) {
        return;
    }
    check(!arch->empty(), "synthetic: archive is not empty");
    check_eq(arch->size(), std::size_t{1}, "synthetic: one entry");
    check(arch->find("hello") != nullptr, "synthetic: lookup is case-insensitive");
    check_eq(arch->index_of("HeLLo"), 0, "synthetic: index of HELLO");

    error.clear();
    const auto hello = gm::icon_asset("hello", gm::IconKind::unknown, *arch, &error);
    check(hello.has_value(), "synthetic: HELLO resolves (" + error + ")");
    if (hello) {
        check_eq(hello->asset, "hello", "synthetic: the requested spelling is kept");
        check_eq(hello->width, 2, "synthetic: width from the BMP header");
        check_eq(hello->height, 3, "synthetic: height from the BMP header");
        check_eq(hello->index_in_archive, 0, "synthetic: archive index");
        check_eq(std::string(gm::icon_kind_name(hello->kind)), "unknown",
                 "synthetic: kind name");
    }

    // A missing asset names the asset and the archive.
    error.clear();
    check(!gm::weapon_icon(0, false, *arch, &error),
          "synthetic: WICON1 is absent");
    check(contains(error, "WICON1") && contains(error, "not in"),
          "synthetic: missing asset text: " + error);
    error.clear();
    check(!gm::equipment_icon(0, *arch, &error), "synthetic: equip0 is absent");
    check(contains(error, "equip0") && contains(error, "not in"),
          "synthetic: missing equip text: " + error);

    // A blob that is not a BMP is a parse refusal, not a size 0 icon.
    const fs::path bad_idx =
        make_archive(root / "bad", {"WICON1"}, {std::vector<std::uint8_t>(16, 0xAB)});
    std::string bad_open;
    if (auto bad = gm::Arch::open(bad_idx, bad_open)) {
        error.clear();
        check(!gm::weapon_icon(0, false, *bad, &error),
              "synthetic: non-BMP WICON1 refused");
        check(contains(error, "BMP"), "synthetic: non-BMP text: " + error);
    } else {
        check(false, "synthetic: bad pair opens (" + bad_open + ")");
    }

    // A missing index file keeps BigFile's text.
    error.clear();
    check(!gm::Arch::open(root / "nope" / "BMP.IDX", error),
          "synthetic: missing index refused");
    check(contains(error, "cannot open index"), "synthetic: missing index text: " + error);

    fs::remove_all(root, ec);
}

// ------------------------------------------------------------------ compass

void test_compass() {
    gm::CompassGeometry geometry;
    geometry.yaw_deg = 344.61; // win2 sidecar, docs/HUD_ICONS.md §5

    const int expected[4] = {107, 134, 54, 81}; // 0, 90, 180, 270
    const double bearings[4] = {0.0, 90.0, 180.0, 270.0};
    int inside = 0;
    for (int i = 0; i < 4; ++i) {
        const auto pose = gm::compass_pos(bearings[i], geometry);
        std::cout << "compass: yaw=344.61 bearing=" << bearings[i]
                  << " -> x=" << pose.x << " y=" << pose.y
                  << " (" << (gm::compass_in_window(pose.x, geometry) ? "inside"
                                                                      : "outside")
                  << ")\n";
        check_eq(pose.x, expected[i], "compass: win2 x for " +
                                         std::to_string(bearings[i]));
        check_eq(pose.y, 2, "compass: ribbon row");
        if (gm::compass_in_window(pose.x, geometry)) {
            ++inside;
        }
    }
    check_eq(inside, 2, "compass: two of four cardinals in the 51 px window");
    check(gm::kCompassWindowRight - gm::kCompassWindowLeft + 1 == 51,
          "compass: window is 51 px");
    check(std::abs(gm::kCompassPxPerDeg * 90.0 - 26.46) < 0.01,
          "compass: 90 deg period is 26.46 px");

    // Normalization: the same heading is the same column.
    const auto zero = gm::compass_pos(0.0, geometry);
    const auto full = gm::compass_pos(360.0, geometry);
    const auto plus = gm::compass_pos(450.0, geometry);
    const auto minus = gm::compass_pos(-90.0, geometry);
    const auto ninety = gm::compass_pos(90.0, geometry);
    const auto two_seventy = gm::compass_pos(270.0, geometry);
    check_eq(full.x, zero.x, "compass: 360 == 0");
    check_eq(full.bearing_deg, 0.0, "compass: 360 normalizes to 0");
    check_eq(plus.x, ninety.x, "compass: 450 == 90");
    check_eq(plus.bearing_deg, 90.0, "compass: 450 normalizes to 90");
    check_eq(minus.x, two_seventy.x, "compass: -90 == 270");
    check_eq(minus.bearing_deg, 270.0, "compass: -90 normalizes to 270");
    check_eq(gm::compass_pos(720.0 + 45.0, geometry).bearing_deg, 45.0,
             "compass: 765 normalizes to 45");

    // The yaw is a parameter, not a global: yaw 0 moves the tape right.
    auto yaw0 = geometry;
    yaw0.yaw_deg = 0.0;
    check_eq(gm::compass_pos(0.0, yaw0).x, 103, "compass: yaw 0, bearing 0 -> 103");
    check_eq(gm::compass_pos(90.0, yaw0).x, 129, "compass: yaw 0, bearing 90 -> 129");
    check_eq(gm::compass_pos(180.0, yaw0).x, 50, "compass: yaw 0, bearing 180 -> 50");
    check_eq(gm::compass_pos(270.0, yaw0).x, 76, "compass: yaw 0, bearing 270 -> 76");

    // Non-finite input is a sentinel, never UB through lround/cast.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    check_eq(gm::compass_pos(nan, geometry).x, gm::kCompassInvalid,
             "compass: NaN is invalid");
    check_eq(gm::compass_pos(inf, geometry).x, gm::kCompassInvalid,
             "compass: +inf is invalid");
}

// -------------------------------------------------------------- shipped game

bool shipped(const fs::path& game) {
    std::error_code ec;
    const fs::path idx = game / "FRONTEND" / "BMP.IDX";
    if (!fs::is_regular_file(idx, ec)) {
        std::cout << "hud_icons: game not installed, shipped cases skipped\n";
        return false;
    }
    std::string error;
    auto arch = gm::Arch::open(idx, error);
    check(arch.has_value(), "shipped: BMP.IDX opens (" + error + ")");
    if (!arch) {
        return false;
    }
    std::cout << "shipped: archive=" << idx.string() << " entries=" << arch->size()
              << "\n";
    check_eq(arch->size(), std::size_t{143}, "shipped: BMP.IDX has 143 entries");

    // ---- weapons: the table must equal the real level0 DEFINE_WEAPONS block.
    const fs::path level = game / "level0.sbt";
    std::string weapons_error;
    auto weapons = gm::Weapons::load(level, &weapons_error);
    check(weapons.has_value(), "shipped: level0 weapons load (" + weapons_error + ")");
    if (weapons) {
        check_eq(weapons->size(), std::size_t{19}, "shipped: 19 DEFINE_WEAPONS records");
        const auto table = gm::weapon_icon_table();
        check_eq(table.size(), std::size_t{19}, "shipped: icon table size");
        for (int i = 0; i < gm::kWeaponIconCount; ++i) {
            const gm::Weapon* weapon = weapons->at(i);
            check(weapon != nullptr, "shipped: weapon " + std::to_string(i));
            if (weapon == nullptr) {
                continue;
            }
            check_eq(std::string(table[i].name), weapon->name,
                     "shipped: table name " + std::to_string(i));
            check_eq(std::string(table[i].asset), weapon->icon,
                     "shipped: table asset " + std::to_string(i));
        }
    }

    int weapon_resolved = 0;
    int weapon_24 = 0;
    std::vector<int> weapon_unresolved;
    for (int id = 0; id < gm::kWeaponIconCount; ++id) {
        error.clear();
        const auto icon = gm::weapon_icon(id, false, *arch, &error);
        if (!icon) {
            weapon_unresolved.push_back(id);
            std::cerr << "FAIL: shipped: weapon " << id << ": " << error << "\n";
            ++failures;
            continue;
        }
        ++weapon_resolved;
        if (icon->width == 24 && icon->height == 24) {
            ++weapon_24;
        }
        // Independent re-read: the entry exists and parses to the same size.
        const gm::BigEntry* entry = arch->find(icon->asset);
        check(entry != nullptr, "shipped: weapon asset is in the index: " + icon->asset);
        if (entry != nullptr) {
            std::string read_error;
            check(arch->file->read(*entry, read_error).has_value(),
                  "shipped: weapon blob reads: " + icon->asset + " (" + read_error + ")");
            std::string parse_error;
            const auto blob = arch->file->read(*entry, parse_error);
            const auto image =
                blob ? gm::parse_bmp(*blob, parse_error) : std::nullopt;
            check(image.has_value(), "shipped: weapon asset parses: " +
                                         icon->asset + " (" + parse_error + ")");
            if (image) {
                check_eq(icon->width, image->width,
                         "shipped: width matches the reader: " + icon->asset);
                check_eq(icon->height, image->height,
                         "shipped: height matches the reader: " + icon->asset);
            }
        }
        check(icon->kind == gm::IconKind::weapon, "shipped: weapon kind");
        if (id < 5) {
            const std::string name =
                weapons && weapons->at(id) ? weapons->at(id)->name : std::string("?");
            std::cout << "weapon: id=" << id << " name=\"" << name
                      << "\" asset=" << icon->asset << " " << icon->width << "x"
                      << icon->height << " archive_index=" << icon->index_in_archive
                      << "\n";
        }
    }
    std::cout << "weapons: resolved=" << weapon_resolved << "/19 24x24=" << weapon_24
              << " unresolved=" << weapon_unresolved.size() << " [";
    for (std::size_t i = 0; i < weapon_unresolved.size(); ++i) {
        std::cout << (i ? "," : "") << weapon_unresolved[i];
    }
    std::cout << "]\n";
    check_eq(weapon_resolved, 19, "shipped: every weapon icon resolves");
    check_eq(weapon_24, 19, "shipped: every weapon icon is 24x24");
    check(weapon_unresolved.empty(), "shipped: no unresolved weapons");

    // ---- equipment: 0..11 are the HUD slots, 12 is archive-only BALLAST.
    int equip_resolved = 0;
    int equip_24 = 0;
    std::vector<int> equip_unresolved;
    for (int id = 0; id < gm::kEquipmentHudCount; ++id) {
        error.clear();
        const auto icon = gm::equipment_icon(id, *arch, &error);
        if (!icon) {
            equip_unresolved.push_back(id);
            std::cerr << "FAIL: shipped: equipment " << id << ": " << error << "\n";
            ++failures;
            continue;
        }
        ++equip_resolved;
        if (icon->width == 24 && icon->height == 24) {
            ++equip_24;
        }
        check_eq(icon->asset, "equip" + std::to_string(id),
                 "shipped: equipment asset formula");
        check(icon->kind == gm::IconKind::equipment, "shipped: equipment kind");
        check_eq(icon->index_in_archive, arch->index_of(icon->asset),
                 "shipped: equipment archive index");
        if (id < 3) {
            std::cout << "equipment: id=" << id << " asset=" << icon->asset << " "
                      << icon->width << "x" << icon->height
                      << " archive_index=" << icon->index_in_archive << "\n";
        }
    }
    std::cout << "equipment: resolved=" << equip_resolved
              << "/12 24x24=" << equip_24
              << " unresolved=" << equip_unresolved.size() << "\n";
    check_eq(equip_resolved, gm::kEquipmentHudCount,
             "shipped: every HUD equipment icon resolves");
    check_eq(equip_24, gm::kEquipmentHudCount,
             "shipped: every HUD equipment icon is 24x24");

    // BALLAST: in the archive and in DEFINE_EQUIPMENT, outside the HUD window.
    error.clear();
    const auto ballast = gm::equipment_icon(12, *arch, &error);
    check(ballast.has_value(), "shipped: equip12 (BALLAST) resolves (" + error + ")");
    if (ballast) {
        check_eq(ballast->asset, "equip12", "shipped: equip12 name");
        check_eq(ballast->width, 24, "shipped: equip12 width");
        check_eq(ballast->height, 24, "shipped: equip12 height");
        std::cout << "equipment: id=12 asset=equip12 " << ballast->width << "x"
                  << ballast->height << " (archive-only, outside 0x492cc3)\n";
    }

    // ---- refusals against the real archive.
    error.clear();
    check(!gm::weapon_icon(-1, false, *arch, &error), "shipped: weapon -1 refused");
    check(contains(error, "outside"), "shipped: weapon -1 text: " + error);
    error.clear();
    check(!gm::weapon_icon(999, false, *arch, &error), "shipped: weapon 999 refused");
    check(contains(error, "outside"), "shipped: weapon 999 text: " + error);
    error.clear();
    check(!gm::weapon_icon(0, true, *arch, &error), "shipped: alt weapon refused");
    check(contains(error, "alternate"), "shipped: alt text: " + error);
    error.clear();
    check(!gm::equipment_icon(13, *arch, &error), "shipped: equipment 13 refused");
    check(contains(error, "outside"), "shipped: equipment 13 text: " + error);
    error.clear();
    check(!gm::icon_asset("NOSUCH", gm::IconKind::unknown, *arch, &error),
          "shipped: unknown asset refused");
    check(contains(error, "NOSUCH") && contains(error, "not in"),
          "shipped: unknown asset text: " + error);

    // ---- determinism: the same call twice is the same IconRef.
    error.clear();
    const auto a = gm::weapon_icon(17, false, *arch, &error);
    const auto b = gm::weapon_icon(17, false, *arch, &error);
    check(a.has_value() && b.has_value(), "shipped: determinism resolves");
    if (a && b) {
        check_eq(a->asset, b->asset, "shipped: determinism asset");
        check_eq(a->width, b->width, "shipped: determinism width");
        check_eq(a->height, b->height, "shipped: determinism height");
        check_eq(a->index_in_archive, b->index_in_archive,
                 "shipped: determinism index");
        std::cout << "determinism: id=17 asset=" << a->asset
                  << " index=" << a->index_in_archive << " twice equal\n";
    }
    if (a) {
        check_eq(a->asset, "WCON11", "shipped: id 17 is DISINTEGRATOR's icon");
    }
    return true;
}

} // namespace

int main() {
    test_synthetic();
    test_compass();

    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_GAME")) {
        game = env;
    }
    shipped(game);

    std::cout << "hud_icons: " << checks << " checks, " << failures << " failed\n";
    if (failures == 0) {
        std::cout << "hud_icons: OK (icons from FRONTEND/BMP.IDX, compass MODEL)\n";
        return 0;
    }
    return 1;
}
