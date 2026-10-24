// Composed cockpit frame (`M4.4.5` slice 3, docs/COCKPIT.md).
//
// Shipped half (read-only install):
//   * the full frame at armour 10000 / ammo 2000 / kills 3 / weapon 0 and 4:
//     elements_drawn, missing_assets == 0, ink_pixels, panel_pixels; two runs
//     are byte-identical and different weapons give different frames;
//   * the panel band is compared with an independent PANEL blit built in the
//     test from BigFile + parse_bmp + GAMEPAL (flipV, index 0 as the key), and
//     the rows above y = 122 must stay the opaque black background;
//   * armour 0 vs armour_max: the damage digits and the hexagon fills differ,
//     the fill length is checked by counting the fill colour;
//   * icons=false / numbers=false switch exactly their own pixels (outside the
//     icon rectangles the icon switch changes nothing), and the counted ink
//     follows numbers=false to 0.
//
// Synthetic half (no game files): a missing index, an index without PANEL, a
// PANEL that is not 320x78, a broken palette carrier and armor outside
// [0, armor_max] (clamped: -5 == 0, > max == max).
//
// Numbers are printed; docs/COCKPIT.md §5 pins them.

#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "core/path.h"
#include "render/cockpit.h"
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

/// Minimal 8-bit BMP of *width* x *height*, all indices zero, grey palette.
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
    put32(out, 0);
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

/// `BIGFILE` index + `.dat` pair with the given blobs.
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

constexpr std::uint32_t kBackground = 0x000000FFu;
constexpr int kWidth = 320;
constexpr int kHeight = 200;
constexpr int kPanelTop = 122;

/// One frame plus its numbers.
struct Rendered {
    bool ok = false;
    std::string error;
    std::vector<std::uint32_t> rgba;
    int width = 0;
    int height = 0;
    gm::CockpitStats stats;
};

Rendered render(const fs::path& game, const gm::CockpitState& state,
                const gm::CockpitOptions& options) {
    Rendered out;
    out.ok = gm::render_cockpit(game, state, options, out.rgba, out.width,
                                out.height, &out.error, &out.stats);
    return out;
}

std::uint32_t pixel(const std::vector<std::uint32_t>& rgba, int x, int y) {
    return rgba[static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)];
}

std::uint32_t rgb_of(const std::vector<std::uint8_t>& palette, int index) {
    const std::size_t offset = static_cast<std::size_t>(index) * 4;
    return (static_cast<std::uint32_t>(palette[offset + 0]) << 24) |
           (static_cast<std::uint32_t>(palette[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(palette[offset + 2]) << 8) | 0xFFu;
}

/// Pixels of *rgba* that differ from the opaque black background outside the
/// panel band.
int nonzero_outside_band(const std::vector<std::uint32_t>& rgba) {
    int count = 0;
    for (int y = 0; y < kPanelTop; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (pixel(rgba, x, y) != kBackground) {
                ++count;
            }
        }
    }
    return count;
}

/// Pixels that differ between two frames inside a rectangle.
int diff_in_rect(const std::vector<std::uint32_t>& a,
                 const std::vector<std::uint32_t>& b, int x, int y, int w, int h) {
    int count = 0;
    for (int row = y; row < y + h; ++row) {
        for (int column = x; column < x + w; ++column) {
            if (pixel(a, column, row) != pixel(b, column, row)) {
                ++count;
            }
        }
    }
    return count;
}

int diff_total(const std::vector<std::uint32_t>& a,
               const std::vector<std::uint32_t>& b) {
    int count = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i] != b[i]) {
            ++count;
        }
    }
    return count;
}

/// Fill pixels of one 3-row hexagon bar: the pixels equal to *colour* inside
/// the bar rectangle (the fill grows to the right, never overflows).
int bar_fill_pixels(const std::vector<std::uint32_t>& rgba, int x, int y,
                    std::uint32_t colour) {
    int count = 0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 17; ++column) {
            if (pixel(rgba, x + column, y + row) == colour) {
                ++count;
            }
        }
    }
    return count;
}

// --------------------------------------------------------------- shipped game

gm::CockpitState shipped_state() {
    gm::CockpitState state;
    state.weapon_id = 0; // MACHINE GUN
    state.ammo = 2000;
    state.armor = 10000;
    state.armor_max = 10000;
    state.kills = 3;
    state.speed = 1.0;
    state.equipment_id = 0; // HEADLIGHTS
    state.yaw_deg = 344.61; // win2 sidecar, docs/HUD_ICONS.md §5
    return state;
}

bool test_shipped(const fs::path& game) {
    const auto index = gm::resolve_ci(game, "FRONTEND/BMP.IDX");
    if (!index) {
        std::cout << "cockpit: game not installed, shipped cases skipped\n";
        return false;
    }

    std::string error;
    auto archive = gm::BigFile::open(*index, error);
    check(archive.has_value(), "shipped: BMP.IDX opens (" + error + ")");
    if (!archive) {
        return false;
    }
    const gm::BigEntry* panel_entry = nullptr;
    for (const gm::BigEntry& entry : archive->entries()) {
        std::string name = entry.name;
        for (char& ch : name) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (name == "PANEL") {
            panel_entry = &entry;
            break;
        }
    }
    check(panel_entry != nullptr, "shipped: PANEL is in BMP.IDX");
    if (panel_entry == nullptr) {
        return false;
    }
    std::string read_error;
    const auto panel_blob = archive->read(*panel_entry, read_error);
    check(panel_blob.has_value(), "shipped: PANEL reads (" + read_error + ")");
    if (!panel_blob) {
        return false;
    }
    std::string parse_error;
    const auto panel = gm::parse_bmp(*panel_blob, parse_error);
    check(panel.has_value(), "shipped: PANEL parses (" + parse_error + ")");
    if (!panel) {
        return false;
    }
    check_eq(panel->width, kWidth, "shipped: PANEL width");
    check_eq(panel->height, 78, "shipped: PANEL height");

    std::string palette_error;
    const auto palette = gm::installed_palette(game, palette_error);
    check(palette.has_value(), "shipped: GAMEPAL loads (" + palette_error + ")");
    if (!palette) {
        return false;
    }

    // 1. the full frame of two different weapons
    const gm::CockpitOptions options;
    const auto weapon0 = render(game, shipped_state(), options);
    std::cout << "cockpit: weapon=0 elements=" << weapon0.stats.elements_drawn
              << " missing=" << weapon0.stats.missing_assets
              << " ink=" << weapon0.stats.ink_pixels
              << " panel=" << weapon0.stats.panel_pixels << "\n";
    check(weapon0.ok, "shipped: weapon 0 composes (" + weapon0.error + ")");
    check_eq(weapon0.width, kWidth, "shipped: frame width");
    check_eq(weapon0.height, kHeight, "shipped: frame height");
    check_eq(weapon0.rgba.size(), std::size_t{64000}, "shipped: 320x200 pixels");
    check_eq(weapon0.stats.elements_drawn, 11, "shipped: elements_drawn");
    check_eq(weapon0.stats.missing_assets, 0, "shipped: missing_assets");
    check_eq(weapon0.stats.ink_pixels, 765, "shipped: ink_pixels");
    check_eq(weapon0.stats.panel_pixels, 21196, "shipped: panel_pixels");

    const auto weapon0_again = render(game, shipped_state(), options);
    check(weapon0_again.ok, "shipped: second run composes (" + weapon0_again.error + ")");
    check(weapon0.rgba == weapon0_again.rgba,
          "shipped: two runs are byte for byte identical");

    gm::CockpitState grenade = shipped_state();
    grenade.weapon_id = 4; // GRENADE, a different icon and name
    const auto weapon4 = render(game, grenade, options);
    std::cout << "cockpit: weapon=4 elements=" << weapon4.stats.elements_drawn
              << " missing=" << weapon4.stats.missing_assets
              << " ink=" << weapon4.stats.ink_pixels << "\n";
    check(weapon4.ok, "shipped: weapon 4 composes (" + weapon4.error + ")");
    check_eq(weapon4.stats.missing_assets, 0, "shipped: weapon 4 assets");
    check(weapon4.rgba != weapon0.rgba,
          "shipped: different weapons give different frames");
    const auto weapon4_again = render(game, grenade, options);
    check(weapon4_again.ok && weapon4_again.rgba == weapon4.rgba,
          "shipped: weapon 4 is deterministic too");

    // 2. the panel band against an independent blit
    gm::CockpitState panel_only_state = shipped_state();
    panel_only_state.speed = 0.0; // no throttle fill
    gm::CockpitOptions panel_only;
    panel_only.icons = false;
    panel_only.numbers = false;
    panel_only.compass = false;
    const auto band = render(game, panel_only_state, panel_only);
    check(band.ok, "band: panel-only composes (" + band.error + ")");
    if (band.ok) {
        check_eq(band.stats.ink_pixels, 0, "band: no text ink");
        check_eq(band.stats.elements_drawn, 1, "band: panel is the only element");

        std::vector<std::uint32_t> direct(
            static_cast<std::size_t>(kWidth) * kHeight, kBackground);
        int direct_pixels = 0;
        for (int row = 0; row < panel->height; ++row) {
            const int y = kPanelTop + (panel->height - 1 - row); // engine order
            for (int column = 0; column < panel->width; ++column) {
                const std::uint8_t index =
                    panel->pixels[static_cast<std::size_t>(row) * panel->width +
                                  static_cast<std::size_t>(column)];
                if (index == 0) {
                    continue; // key
                }
                direct[static_cast<std::size_t>(y) * kWidth +
                       static_cast<std::size_t>(column)] = rgb_of(*palette, index);
                ++direct_pixels;
            }
        }
        const int mismatch = diff_total(band.rgba, direct);
        std::cout << "cockpit: panel band pixels=" << direct_pixels
                  << " mismatches=" << mismatch << " outside="
                  << nonzero_outside_band(band.rgba) << "\n";
        check_eq(direct_pixels, band.stats.panel_pixels,
                 "band: independent blit counts the same pixels");
        check_eq(mismatch, 0, "band: matches the independent PANEL blit");
        check_eq(nonzero_outside_band(band.rgba), 0,
                 "band: rows above y=122 are untouched background");
    }

    // 3. the armour scale boundaries
    gm::CockpitState empty_tank = shipped_state();
    empty_tank.armor = 0; // damage 100 %
    gm::CockpitState full_tank = shipped_state();
    full_tank.armor = full_tank.armor_max; // damage 0 %
    const auto damaged = render(game, empty_tank, options);
    const auto intact = render(game, full_tank, options);
    check(damaged.ok && intact.ok, "scale: both ends compose");
    if (damaged.ok && intact.ok) {
        const auto white = rgb_of(*palette, 0xFF);
        const int damaged_fill =
            bar_fill_pixels(damaged.rgba, 148, 151, white) +
            bar_fill_pixels(damaged.rgba, 148, 177, white);
        const int intact_fill =
            bar_fill_pixels(intact.rgba, 148, 151, white) +
            bar_fill_pixels(intact.rgba, 148, 177, white);
        std::cout << "cockpit: armor=0 ink=" << damaged.stats.ink_pixels
                  << " elements=" << damaged.stats.elements_drawn
                  << " fill=" << damaged_fill << "; armor=max ink="
                  << intact.stats.ink_pixels << " elements="
                  << intact.stats.elements_drawn << " fill=" << intact_fill
                  << "\n";
        check_eq(damaged.stats.ink_pixels, 845,
                 "scale: armour 0 draws the 100 % digits");
        check_eq(intact.stats.ink_pixels, 765,
                 "scale: full armour draws the 0 % digit");
        check_eq(damaged.stats.elements_drawn, 13,
                 "scale: two hexagon fills are drawn");
        check_eq(intact.stats.elements_drawn, 11,
                 "scale: no hexagon fill at 0 %");
        check_eq(damaged_fill, 102, "scale: 17 px x 3 rows x 2 bars");
        check_eq(intact_fill, 0, "scale: empty fill at full armour");
        check(damaged.rgba != intact.rgba, "scale: the frames differ");
    }

    // 4. the icon / numbers switches
    gm::CockpitOptions no_icons = options;
    no_icons.icons = false;
    gm::CockpitOptions no_numbers = options;
    no_numbers.numbers = false;
    const auto without_icons = render(game, shipped_state(), no_icons);
    const auto without_numbers = render(game, shipped_state(), no_numbers);
    check(without_icons.ok && without_numbers.ok, "options: both variants compose");
    if (without_icons.ok && without_numbers.ok) {
        const int left = diff_in_rect(weapon0.rgba, without_icons.rgba, 23, 162, 24, 24);
        const int right =
            diff_in_rect(weapon0.rgba, without_icons.rgba, 201, 162, 24, 24);
        const int icons_total = diff_total(weapon0.rgba, without_icons.rgba);
        const int numbers_diff = diff_total(weapon0.rgba, without_numbers.rgba);
        std::cout << "cockpit: --icons=false diff=" << icons_total
                  << " (left " << left << ", right " << right << ")"
                  << "; --numbers=false diff=" << numbers_diff
                  << " ink=" << without_numbers.stats.ink_pixels
                  << " elements=" << without_icons.stats.elements_drawn
                  << "/" << without_numbers.stats.elements_drawn << "\n";
        check_eq(icons_total, left + right,
                 "options: icons only change their two rectangles");
        check_eq(left, 549, "options: weapon icon rectangle differs");
        check_eq(right, 571, "options: equipment icon rectangle differs");
        check_eq(without_icons.stats.ink_pixels, 765,
                 "options: text ink survives icons=false");
        check_eq(without_icons.stats.elements_drawn, 9,
                 "options: icons are two elements");
        check_eq(without_numbers.stats.ink_pixels, 0,
                 "options: numbers=false leaves no text ink");
        check_eq(without_numbers.stats.elements_drawn, 4,
                 "options: seven elements left without the text layer");
        check(numbers_diff == 764 && numbers_diff <= weapon0.stats.ink_pixels,
              "options: numbers=false removes the text pixels (" +
                  std::to_string(numbers_diff) + " of " +
                  std::to_string(weapon0.stats.ink_pixels) + ")");
    }

    // 5. the compass (MODEL) writes only inside its window
    gm::CockpitOptions with_compass = options;
    with_compass.compass = true;
    const auto taped = render(game, shipped_state(), with_compass);
    const auto taped_other = [&] {
        gm::CockpitState other = shipped_state();
        other.yaw_deg = 100.0;
        return render(game, other, with_compass);
    }();
    check(taped.ok && taped_other.ok, "compass: both headings compose");
    if (taped.ok && taped_other.ok) {
        int outside = 0;
        int inside = 0;
        for (int y = 0; y < 20; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const bool changed = pixel(taped.rgba, x, y) != pixel(weapon0.rgba, x, y);
                if (changed) {
                    ++inside;
                    if (x < 79 || x > 129) {
                        ++outside;
                    }
                }
            }
        }
        const int moved = diff_total(taped.rgba, taped_other.rgba);
        std::cout << "cockpit: compass pixels=" << inside << " outside window="
                  << outside << " yaw delta pixels=" << moved
                  << " elements=" << taped.stats.elements_drawn << "\n";
        check(inside > 0, "compass: the tape marks pixels");
        check_eq(outside, 0, "compass: everything stays in the measured window");
        check(moved > 0, "compass: a different yaw moves the tape");
        check_eq(taped.stats.elements_drawn, 12, "compass: one more element");
    }

    return true;
}

// ---------------------------------------------------------------- synthetic

void test_synthetic() {
    const fs::path root = fs::temp_directory_path() / "gm_cockpit_tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    gm::CockpitState state;
    state.weapon_id = 0;
    state.ammo = 10;
    state.armor = 5000;
    state.armor_max = 10000;
    state.kills = 1;
    state.speed = 0.5;
    gm::CockpitOptions options;

    // Missing index: the text names BMP.IDX, the buffer is cleared.
    {
        const fs::path empty = root / "empty";
        fs::create_directories(empty, ec);
        const auto out = render(empty, state, options);
        std::cout << "cockpit: empty dir -> " << out.error << "\n";
        check(!out.ok, "negative: empty directory refused");
        check(contains(out.error, "BMP.IDX"), "negative: empty dir text: " + out.error);
        check(out.rgba.empty(), "negative: empty dir clears rgba");
        check_eq(out.width, 0, "negative: empty dir width");
        check_eq(out.height, 0, "negative: empty dir height");
    }

    // An index with GAMEPAL but no PANEL.
    {
        const fs::path no_panel = root / "no_panel";
        make_archive(no_panel / "FRONTEND", {"GAMEPAL"}, {make_bmp(1, 1)});
        const auto out = render(no_panel, state, options);
        std::cout << "cockpit: no PANEL -> " << out.error << "\n";
        check(!out.ok, "negative: missing PANEL refused");
        check(contains(out.error, "PANEL"), "negative: missing PANEL text: " + out.error);
    }

    // A PANEL of the wrong size cannot be a 320x200 cockpit frame.
    {
        const fs::path small = root / "small";
        make_archive(small / "FRONTEND", {"GAMEPAL", "PANEL"},
                     {make_bmp(1, 1), make_bmp(2, 3)});
        const auto out = render(small, state, options);
        std::cout << "cockpit: small PANEL -> " << out.error << "\n";
        check(!out.ok, "negative: 2x3 PANEL refused");
        check(contains(out.error, "320x78"), "negative: small PANEL text: " + out.error);
        check(contains(out.error, "2x3"), "negative: small PANEL names the size: " +
                                             out.error);
    }

    // A broken palette carrier: no archive, one loose BMP that is not a BMP.
    {
        const fs::path bad_palette = root / "bad_palette";
        write_file(bad_palette / "FLAG" / "naUSA.BMP",
                   std::vector<std::uint8_t>(64, 0xAB));
        make_archive(bad_palette / "FRONTEND", {"PANEL"}, {make_bmp(320, 78)});
        const auto out = render(bad_palette, state, options);
        std::cout << "cockpit: broken palette -> " << out.error << "\n";
        check(!out.ok, "negative: broken palette refused");
        check(contains(out.error, "palette"), "negative: palette text: " + out.error);
    }

    // armor_max <= 0 is a state error, not a divide by zero.
    {
        gm::CockpitState bad = state;
        bad.armor_max = 0;
        const auto out = render(root / "empty", bad, options);
        std::cout << "cockpit: armor_max=0 -> " << out.error << "\n";
        check(!out.ok, "negative: armor_max 0 refused");
        check(contains(out.error, "armor_max"), "negative: armor_max text: " + out.error);
    }

    fs::remove_all(root, ec);
}

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }

    test_synthetic();

    if (test_shipped(game)) {
        // The clamp only makes sense with real assets, so it lives here.
        // -5 == 0 and > max == max: the frame must not change.
        const gm::CockpitOptions options;
        gm::CockpitState low;
        low.weapon_id = 0;
        low.ammo = 2000;
        low.armor = 0;
        low.armor_max = 10000;
        low.kills = 3;
        low.speed = 1.0;
        gm::CockpitState below = low;
        below.armor = -5;
        gm::CockpitState high = low;
        high.armor = 10000;
        gm::CockpitState above = high;
        above.armor = 20000;

        const auto clamped_low = render(game, below, options);
        const auto zero = render(game, low, options);
        const auto clamped_high = render(game, above, options);
        const auto full = render(game, high, options);
        check(clamped_low.ok && zero.ok && clamped_high.ok && full.ok,
              "clamp: all four frames compose");
        if (clamped_low.ok && zero.ok && clamped_high.ok && full.ok) {
            std::cout << "cockpit: clamp armor=-5 ink=" << clamped_low.stats.ink_pixels
                      << " same_as_0=" << (clamped_low.rgba == zero.rgba)
                      << "; armor=20000 ink=" << clamped_high.stats.ink_pixels
                      << " same_as_max=" << (clamped_high.rgba == full.rgba)
                      << "\n";
            check_eq(clamped_low.stats.ink_pixels, zero.stats.ink_pixels,
                     "clamp: armor=-5 draws the same digits as 0");
            check(clamped_low.rgba == zero.rgba, "clamp: armor=-5 is clamped to 0");
            check_eq(clamped_high.stats.ink_pixels, full.stats.ink_pixels,
                     "clamp: armor>max draws the same digits as max");
            check(clamped_high.rgba == full.rgba,
                  "clamp: armor>max is clamped to armor_max");
        }
    } else {
        std::cout << "cockpit: game install incomplete, shipped numbers skipped\n";
    }

    if (failures == 0) {
        std::cout << "cockpit: " << checks << " checks, 0 failed\n";
        std::cout << "cockpit: OK (frame from PANEL/icons/fonts, compass MODEL)\n";
        return 0;
    }
    std::cerr << "cockpit: " << failures << " of " << checks << " check(s) failed\n";
    return 1;
}
