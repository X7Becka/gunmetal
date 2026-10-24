// Depth fog (M4.1.6, slice 2): `z`, `level`, the `lighttable` and the
// per-frame LUT application of docs/FOG.md and docs/FOG_CPP.md.
//
// Anchors:
//   * `fog_level` numbers are docs/FOG.md §3 (`level = clamp(63-trunc(z),4,63)`,
//     `ftol` = `fistp` RC=11, `0x40e0e0`), not a guess;
//   * `fog_z` is `|(64/a0)*(e2.(p-eye))|` (docs/FOG.md §2) - `0x4804b0` is
//     `fabs`, there is no `fsqrt` in the rasteriser;
//   * the table goldens (FNV-1a64 of all 65 536 bytes, first white index, the
//     count and range of all-white rows, individual entries) come from an
//     independent Python reference: `tools/re/fog.py::build_lighttable` over
//     the real `FRONTEND/GAMEPAL` via `gm/palette.py`.  The palette itself is
//     read here with the C++ loader (`gm::installed_palette`, res/bmp.h, RGBX);
//     `gm/palette.py` also returns RGBX, but `tools/re/fog.py` re-reads it as
//     B,G,R, so its printed triples are R/B swapped.  The LUT is invariant
//     under that global permutation (checked below), so every index golden is
//     unaffected; the exact command is docs/FOG_CPP.md §5.
//   * every table entry is additionally compared with an independent
//     reimplementation of the `0x4201fe` formula and quantisation rule written
//     in this file (`reference_index`).
//
// The second half pins the frame application (`fbuf[i] = table[level*256 +
// fbuf[i]]`, `0x441b2d`..`0x441b5c`) on a 256-byte frame and on a 320x200 one,
// byte for byte against a direct `fog_pixel` walk, with Python FNV goldens.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "render/fog.h"
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

std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const std::uint8_t b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string num(double value) {
    std::ostringstream out;
    out.precision(12);
    out << value;
    return out.str();
}

/// A refusal must be `false` *and* name the offending input in the text.
void refused(bool result, const std::string& error, const std::string& needle,
             const std::string& what) {
    check(!result, what + ": refused");
    check(error.find(needle) != std::string::npos,
          what + ": error names \"" + needle + "\" (got: " + error + ")");
}

bool near(double a, double b, double epsilon = 1e-12) {
    return std::fabs(a - b) <= epsilon;
}

/// The `0x4201fe` formula + the documented quantisation, written here a second
/// time so the table is not only checked against itself.
std::uint8_t reference_index(const std::uint8_t pal6[256][3], int row, int col) {
    if (row >= gm::kFogRowIdentity) {
        return static_cast<std::uint8_t>(col);
    }
    int want[3];
    if (row >= gm::kFogRowWhite) {
        want[0] = want[1] = want[2] = 0x3F;
    } else {
        for (int channel = 0; channel < 3; ++channel) {
            const int c = pal6[col][channel];
            want[channel] = std::min(0x3F, (c * (0x40 - row) + row * 0x40) >> 6);
        }
    }
    for (int i = 0; i < 256; ++i) {
        if (pal6[i][0] == want[0] && pal6[i][1] == want[1] &&
            pal6[i][2] == want[2]) {
            return static_cast<std::uint8_t>(i);
        }
    }
    int best = 0;
    int best_distance = -1;
    for (int i = 0; i < 256; ++i) {
        const int dr = want[0] - static_cast<int>(pal6[i][0]);
        const int dg = want[1] - static_cast<int>(pal6[i][1]);
        const int db = want[2] - static_cast<int>(pal6[i][2]);
        const int distance = dr * dr + dg * dg + db * db;
        if (best_distance < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return static_cast<std::uint8_t>(best);
}

/// Read the engine palette through the C++ loader and shift it to 6 bits, the
/// way the BMP loader does at `0x47f0b4` (`shr dl,2`).
bool load_pal6(const fs::path& game, std::uint8_t pal6[256][3], std::string& error) {
    const auto rgba = gm::installed_palette(game, error);
    if (!rgba) {
        return false;
    }
    if (rgba->size() < 256 * 4) {
        error = "palette has " + std::to_string(rgba->size() / 4) +
                " entries, expected 256";
        return false;
    }
    for (int i = 0; i < 256; ++i) {
        for (int channel = 0; channel < 3; ++channel) {
            pal6[i][channel] =
                static_cast<std::uint8_t>((*rgba)[static_cast<std::size_t>(i) * 4 +
                                                   channel] >>
                                          2);
        }
    }
    return true;
}

} // namespace

int main() {
    // --- level: 63 - trunc(z), clamped to 4..63 (docs/FOG.md §3).
    //
    // `z = 0.5` gives 63, not 62: `ftol` is `fistp` with RC=11 (truncation
    // toward zero, 0x40e0ef), so 63 - trunc(0.5) = 63.  The 62 band starts at
    // z = 1.  `tools/re/fog.py --self-test` pins the same values
    // (ftol(0.5) == 0, ftol(0.99) == 0, ftol(-2.7) == -2).
    {
        const struct {
            double z;
            int level;
        } cases[] = {{0.0, 63},   {0.5, 63}, {1.0, 62},  {1.999, 62},
                     {2.0, 61},   {10.0, 53}, {58.999, 5}, {59.0, 4},
                     {60.0, 4},   {100.0, 4}, {-1.0, 63}};
        bool ok = true;
        std::cout << "fog_level (clamp(63-trunc(z),4,63)):";
        for (const auto& c : cases) {
            const int got = gm::fog_level(c.z);
            std::cout << " z=" << num(c.z) << "->" << got;
            ok = ok && got == c.level;
        }
        std::cout << "\n";
        check(ok, "fog_level matches docs/FOG.md §3 on every anchor");

        // The engine's clamp order: -1 gives 64 -> 63, 1e9 gives 4.
        check(gm::fog_level(-1.0) == gm::kFogLevelMax, "fog_level(-1) clamps to 63");
        check(gm::fog_level(1e9) == gm::kFogLevelMin, "fog_level(1e9) clamps to 4");
        check(gm::fog_level(0.0) == gm::kFogLevelMax, "fog_level(0) is white");
    }

    // --- z = |(64/a0)*(e2.(p-eye))| (docs/FOG.md §2).
    {
        const gm::FogParams params; // a0 = 256 -> 64/a0 = 0.25
        const gm::Vec3d eye{0.0, 0.0, 0.0};
        const gm::Vec3d e2{1.0, 0.0, 0.0};
        const struct {
            gm::Vec3d p;
            double z;
        } cases[] = {{{4.0, 0.0, 0.0}, 1.0},   // 0.25 * 4
                     {{0.0, 4.0, 0.0}, 0.0},   // off the view axis
                     {{-4.0, 0.0, 0.0}, 1.0},  // fabs
                     {{-4.0, -3.0, -2.0}, 1.0} // only the e2 component counts
        };
        bool ok = true;
        std::cout << "fog_z (a0=256, 64/a0=0.25, eye=(0,0,0), e2=(1,0,0)):";
        for (const auto& c : cases) {
            const double got = gm::fog_z(params, eye, e2, c.p);
            std::cout << " p=(" << num(c.p.x) << "," << num(c.p.y) << ","
                      << num(c.p.z) << ")->" << num(got);
            ok = ok && near(got, c.z);
        }
        std::cout << "\n";
        check(ok, "fog_z follows |(64/a0)*(e2.(p-eye))|");

        check(near(gm::fog_z(params, eye, gm::Vec3d{0.0, 1.0, 0.0},
                             gm::Vec3d{0.0, 4.0, 0.0}),
                   1.0),
              "fog_z with e2 = +y");
        check(near(gm::fog_z(gm::FogParams{128.0}, eye, e2, {4.0, 0.0, 0.0}), 2.0),
              "fog_z doubles when a0 halves (a0=128)");
        check(near(gm::fog_z(gm::FogParams{512.0}, eye, e2, {4.0, 0.0, 0.0}), 0.5),
              "fog_z halves when a0 doubles (a0=512)");
        check(near(gm::fog_z(params, {1.0, 2.0, 3.0}, e2, {5.0, 2.0, 3.0}), 1.0),
              "fog_z subtracts the eye first");
        // 236 units is the last level > 4 at a0=256 (docs/FOG.md §7).
        check(gm::fog_level(gm::fog_z(params, eye, e2, {236.0, 0.0, 0.0})) == 4 &&
                  gm::fog_level(gm::fog_z(params, eye, e2, {235.0, 0.0, 0.0})) == 5,
              "a0=256: depth 235 keeps level 5, 236 is the level-4 floor");

        // Plain functions are total: NaN instead of a crash.
        check(std::isnan(gm::fog_z(gm::FogParams{0.0}, eye, e2, {4.0, 0.0, 0.0})),
              "fog_z(a0=0) is NaN");
        check(std::isnan(gm::fog_z(params, eye, e2, {std::numeric_limits<double>::infinity(),
                                                     0.0, 0.0})),
              "fog_z(inf) is NaN");
        check(gm::fog_level(std::numeric_limits<double>::quiet_NaN()) ==
                  gm::kFogLevelMin,
              "fog_level(NaN) falls back to 4, never out of range");
    }

    // --- synthetic palette: bands, identity, formula parity, determinism.
    {
        std::uint8_t pal6[256][3]{};
        for (int i = 0; i < 256; ++i) {
            pal6[i][0] = static_cast<std::uint8_t>(i & 0x3F);
            pal6[i][1] = static_cast<std::uint8_t>((i * 3) & 0x3F);
            pal6[i][2] = static_cast<std::uint8_t>((i * 5) & 0x3F);
        }
        gm::FogTable table;
        std::string error = "stale";
        check(gm::build_fog_table(pal6, table, &error),
              "synthetic: build succeeds (" + error + ")");
        check(error.empty(), "synthetic: error cleared");

        bool colours = true;
        bool formula = true;
        for (int col = 0; col < 256; ++col) {
            const std::uint8_t got = table.data[col]; // row 0
            for (int channel = 0; channel < 3; ++channel) {
                colours = colours && pal6[got][channel] == pal6[col][channel];
            }
        }
        for (int row = 0; row < 256; ++row) {
            for (int col = 0; col < 256; ++col) {
                formula = formula &&
                          table.data[static_cast<std::size_t>(row) * 256 + col] ==
                              reference_index(pal6, row, col);
            }
        }
        check(colours, "synthetic: row 0 is the palette (colour identity)");
        check(formula, "synthetic: every entry equals the independent formula");

        // Global channel permutations leave the table unchanged: the lerp is
        // per channel, both bands are channel-symmetric and the quantiser uses
        // squared distance.  This is why the R/B-swapped palette view of
        // tools/re/fog.py produces the very same 65 536 bytes (docs/FOG_CPP.md §4).
        std::uint8_t swapped[256][3]{};
        for (int i = 0; i < 256; ++i) {
            swapped[i][0] = pal6[i][2];
            swapped[i][1] = pal6[i][1];
            swapped[i][2] = pal6[i][0];
        }
        gm::FogTable permuted;
        check(gm::build_fog_table(swapped, permuted, &error),
              "synthetic: build from the R/B-swapped palette");
        check(std::equal(table.data.begin(), table.data.end(), permuted.data.begin()),
              "synthetic: table is invariant under an R/B swap");

        gm::FogTable twice;
        check(gm::build_fog_table(pal6, twice, &error), "synthetic: second build");
        check(std::equal(table.data.begin(), table.data.end(), twice.data.begin()),
              "synthetic: two builds are byte-identical");
    }

    // --- real palette: bands and Python goldens.
    std::uint8_t pal6[256][3]{};
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    std::string palette_error;
    const bool have_palette = load_pal6(game, pal6, palette_error);
    if (!have_palette) {
        std::cout << "fog: game palette not found under " << game.string()
                  << " (" << palette_error << "), real-palette cases skipped\n";
    }

    gm::FogTable real;
    bool shipped_palette = false;
    if (have_palette) {
        const std::uint64_t palette_fnv = fnv1a64(
            std::span<const std::uint8_t>(&pal6[0][0], static_cast<std::size_t>(256 * 3)));
        shipped_palette = palette_fnv == 0x12941b858f0f2949ull;
        std::cout << "palette: fnv1a64=" << hex64(palette_fnv)
                  << (shipped_palette ? " (shipped 1.36 GAMEPAL)"
                                      : " (not the shipped 1.36 GAMEPAL)")
                  << "\n";
        if (shipped_palette) {
            // The first entries of FRONTEND/GAMEPAL, shifted to 6 bits.
            const std::uint8_t first[8][3] = {{0, 0, 0}, {0, 0, 0}, {5, 5, 5},
                                              {8, 8, 8}, {12, 12, 12}, {15, 15, 15},
                                              {18, 18, 18}, {21, 21, 21}};
            bool first_ok = true;
            for (int i = 0; i < 8; ++i) {
                for (int channel = 0; channel < 3; ++channel) {
                    first_ok = first_ok && pal6[i][channel] == first[i][channel];
                }
            }
            check(first_ok, "GAMEPAL: first 8 entries are 0,0 / 5,5,5 / ... (6-bit)");
            check(pal6[255][0] == 63 && pal6[255][1] == 63 && pal6[255][2] == 63,
                  "GAMEPAL: entry 255 is white");
        }

        std::string error = "stale";
        check(gm::build_fog_table(pal6, real, &error),
              "GAMEPAL: build succeeds (" + error + ")");
        check(error.empty(), "GAMEPAL: error cleared");

        const std::uint64_t table_fnv = fnv1a64(real.data);

        // Palette-agnostic invariants: row 0 is the palette in colour space,
        // rows >= 0x80 are the byte identity, and every entry equals the
        // independent formula below.
        bool row0_colours = true;
        for (int col = 0; col < 256; ++col) {
            const std::uint8_t got = real.data[col];
            for (int channel = 0; channel < 3; ++channel) {
                row0_colours = row0_colours && pal6[got][channel] == pal6[col][channel];
            }
        }
        check(row0_colours, "palette: row 0 is the palette in colour space");

        bool identity = true;
        for (const int row : {128, 255}) {
            for (int col = 0; col < 256; ++col) {
                identity = identity &&
                           real.data[static_cast<std::size_t>(row) * 256 + col] == col;
            }
        }
        check(identity, "palette: rows 128 and 255 are the byte identity");

        int mismatches = 0;
        for (int row = 0; row < 256; ++row) {
            for (int col = 0; col < 256; ++col) {
                if (real.data[static_cast<std::size_t>(row) * 256 + col] !=
                    reference_index(pal6, row, col)) {
                    ++mismatches;
                }
            }
        }
        std::cout << "lighttable: mismatches_vs_formula=" << mismatches << "/65536\n";
        check(mismatches == 0, "palette: table equals the independent formula");

        gm::FogTable again;
        check(gm::build_fog_table(pal6, again, &error), "palette: second build");
        check(std::equal(real.data.begin(), real.data.end(), again.data.begin()),
              "palette: two builds are byte-identical");

        if (shipped_palette) {
            std::cout << "lighttable: fnv1a64=" << hex64(table_fnv)
                      << " entries=" << real.data.size() << "\n";
            check(table_fnv == 0xea757b2e157e2905ull,
                  "GAMEPAL: table FNV-1a64 matches the Python reference (got " +
                      hex64(table_fnv) + ")");
            check(fnv1a64(again.data) == table_fnv,
                  "GAMEPAL: determinism of the hash");

            // Row 0: colour identity for all 256 columns; byte identity for 253,
            // because the palette repeats three colours ((0,0,0) at 0/1, (8,0,0)
            // at 48/65 and white at 96/255), and the quantiser picks the first.
            int row0_bytes = 0;
            for (int col = 0; col < 256; ++col) {
                row0_bytes += real.data[col] == col ? 1 : 0;
            }
            check(row0_bytes == 253,
                  "GAMEPAL: row 0 is a byte identity for 253/256 slots");
            check(real.data[0] == 0, "GAMEPAL: index 0 stays black in row 0");

            // The white band: row 0x3f (the last reachable level) and 0x40..0x7f
            // are all the first white palette entry (96, not 255: first exact
            // match).
            const int white_index = 96;
            bool rows_white = true;
            for (const int row : {63, 64, 127}) {
                for (int col = 0; col < 256; ++col) {
                    rows_white =
                        rows_white &&
                        real.data[static_cast<std::size_t>(row) * 256 + col] ==
                            white_index;
                }
            }
            check(rows_white,
                  "GAMEPAL: rows 63, 64 and 127 are all white (index 96)");

            // How many rows are entirely white after quantisation: rows
            // 62..127.  Row 62's colour is (62,62,62)..(63,63,63), whose nearest
            // palette entry is white as well, so the flat band is 66 rows.
            int white_rows = 0;
            int first_white_row = -1;
            int last_white_row = -1;
            for (int row = 0; row < 256; ++row) {
                bool flat = true;
                for (int col = 0; col < 256 && flat; ++col) {
                    flat = real.data[static_cast<std::size_t>(row) * 256 + col] ==
                           white_index;
                }
                if (flat) {
                    ++white_rows;
                    first_white_row = first_white_row < 0 ? row : first_white_row;
                    last_white_row = row;
                }
            }
            std::cout << "lighttable: white_index=" << white_index
                      << " white_rows=" << white_rows << " (rows "
                      << first_white_row << ".." << last_white_row
                      << "), row0_byte_identity=" << row0_bytes << "/256\n";
            check(white_rows == 66, "GAMEPAL: 66 rows are entirely white");
            check(first_white_row == 62 && last_white_row == 127,
                  "GAMEPAL: the flat white band is rows 62..127");

            // Individual entries pinned by the Python reference.
            check(real.data[1 * 256 + 0] == 64, "GAMEPAL: row 1 col 0 = 64");
            check(real.data[8 * 256 + 16] == 18, "GAMEPAL: row 8 col 16 = 18");
            check(real.data[16 * 256 + 16] == 6, "GAMEPAL: row 16 col 16 = 6");
            check(real.data[32 * 256 + 0] == 10, "GAMEPAL: row 32 col 0 = 10");
            check(real.data[32 * 256 + 16] == 11, "GAMEPAL: row 32 col 16 = 11");
            check(real.data[32 * 256 + 20] == 108, "GAMEPAL: row 32 col 20 = 108");
            check(real.data[32 * 256 + 176] == 12,
                  "GAMEPAL: row 32 col 176 = 12");
        } else {
            std::cout << "lighttable: shipped-GAMEPAL goldens skipped (palette "
                         "differs; invariants and formula parity still checked)\n";
        }
    }

    // --- fog_pixel and fog_frame.
    if (have_palette) {
        if (shipped_palette) {
            check(gm::fog_pixel(real, 63, 0) == 96, "fog_pixel(63, 0) is white");
        }
        check(gm::fog_pixel(real, 0, 7) == 7, "fog_pixel(0, 7) is the identity");
        check(gm::fog_pixel(real, 128, 7) == 7, "fog_pixel(128, 7) is the identity");
        check(gm::fog_pixel(real, -1, 7) == 7,
              "fog_pixel with a bad level is a no-op");

        // 256-byte frame: index i at byte i.
        for (const int level : {0, 63}) {
            std::vector<std::uint8_t> frame(256);
            for (int i = 0; i < 256; ++i) {
                frame[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
            }
            std::vector<std::uint8_t> expected = frame;
            for (std::uint8_t& pixel : expected) {
                pixel = gm::fog_pixel(real, level, pixel);
            }
            const auto before = frame;
            std::string error = "stale";
            check(gm::fog_frame(real, level, frame, &error),
                  "frame 256 level " + std::to_string(level) + ": applies");
            check(error.empty(), "frame 256 level " + std::to_string(level) +
                                     ": error cleared");
            check(frame == expected, "frame 256 level " + std::to_string(level) +
                                         ": byte-equal to fog_pixel");
            const std::uint64_t hash = fnv1a64(frame);
            std::size_t changed = 0;
            for (std::size_t i = 0; i < frame.size(); ++i) {
                changed += frame[i] != before[i] ? 1 : 0;
            }
            std::cout << "fog_frame 256 level=" << level << ": fnv1a64="
                      << hex64(hash) << " changed=" << changed << "/256\n";
            const std::uint64_t want =
                level == 0 ? 0x3652cec8557d1358ull : 0xf11b05f4478ab725ull;
            if (shipped_palette) {
                check(hash == want,
                      "frame 256 level " + std::to_string(level) +
                          ": FNV matches Python (got " + hex64(hash) + ")");
                check(changed == (level == 0 ? 3u : 255u),
                      "frame 256 level " + std::to_string(level) +
                          ": changed count");
            }
        }

        // 320x200 frame cycling 0..255: the real frame footprint.
        std::vector<std::uint8_t> frame(320 * 200);
        for (std::size_t i = 0; i < frame.size(); ++i) {
            frame[i] = static_cast<std::uint8_t>(i % 256);
        }
        for (const int level : {0, 4, 63}) {
            auto expected = frame;
            for (std::uint8_t& pixel : expected) {
                pixel = gm::fog_pixel(real, level, pixel);
            }
            auto got = frame;
            std::string error;
            check(gm::fog_frame(real, level, got, &error),
                  "frame 64000 level " + std::to_string(level) + ": applies");
            check(got == expected, "frame 64000 level " + std::to_string(level) +
                                       ": byte-equal to fog_pixel");
            const std::uint64_t hash = fnv1a64(got);
            std::size_t changed = 0;
            for (std::size_t i = 0; i < got.size(); ++i) {
                changed += got[i] != frame[i] ? 1 : 0;
            }
            std::cout << "fog_frame 64000 level=" << level << ": fnv1a64="
                      << hex64(hash) << " changed=" << changed << "/64000\n";
            const std::uint64_t want = level == 0   ? 0xdd9a27420d92a0c5ull
                                       : level == 4 ? 0x800c97fe8c5af3e1ull
                                                    : 0x2ca8fedaa4d2ab25ull;
            if (shipped_palette) {
                check(hash == want,
                      "frame 64000 level " + std::to_string(level) +
                          ": FNV matches Python (got " + hex64(hash) + ")");
            }
        }
    }

    // --- negatives: every refusal is a text, not a silent zero.
    {
        gm::FogTable table;
        std::string error;
        gm::Vec3d eye{0.0, 0.0, 0.0};
        const gm::Vec3d e2{1.0, 0.0, 0.0};
        const gm::Vec3d p{4.0, 0.0, 0.0};
        double z = -1.0;

        refused(gm::fog_z(gm::FogParams{0.0}, eye, e2, p, z, &error), error, "a0",
                "fog_z a0=0");
        check(z == -1.0, "fog_z a0=0 leaves z untouched");
        refused(gm::fog_z(gm::FogParams{-1.0}, eye, e2, p, z, &error), error, "a0",
                "fog_z a0=-1");
        refused(gm::fog_z(gm::FogParams{std::numeric_limits<double>::infinity()}, eye,
                          e2, p, z, &error),
                error, "a0", "fog_z a0=inf");
        refused(gm::fog_z(gm::FogParams{}, {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
                          e2, p, z, &error),
                error, "eye", "fog_z eye NaN");
        refused(gm::fog_z(gm::FogParams{}, eye,
                          {0.0, std::numeric_limits<double>::infinity(), 0.0}, p, z,
                          &error),
                error, "e2", "fog_z e2 inf");
        refused(gm::fog_z(gm::FogParams{}, eye, e2,
                          {4.0, 0.0, -std::numeric_limits<double>::infinity()}, z,
                          &error),
                error, "p", "fog_z p -inf");

        int level = -1;
        refused(gm::fog_level(std::numeric_limits<double>::quiet_NaN(), level, &error),
                error, "z", "fog_level NaN");
        refused(gm::fog_level(std::numeric_limits<double>::infinity(), level, &error),
                error, "z", "fog_level inf");
        check(level == -1, "fog_level refusal leaves the output untouched");

        std::uint8_t out = 7;
        refused(gm::fog_pixel(table, -1, 0, out, &error), error, "level",
                "fog_pixel level -1");
        refused(gm::fog_pixel(table, 256, 0, out, &error), error, "level",
                "fog_pixel level 256");
        check(out == 7, "fog_pixel refusal leaves the output untouched");

        std::vector<std::uint8_t> frame(16, 3);
        refused(gm::fog_frame(table, 256, frame, &error), error, "level",
                "fog_frame level 256");
        check(std::all_of(frame.begin(), frame.end(),
                          [](std::uint8_t b) { return b == 3; }),
              "fog_frame refusal leaves the frame untouched");

        // Palette bytes: the engine table is 6-bit, so 64 and 200 are errors.
        std::uint8_t bad[256][3]{};
        bad[12][1] = 64;
        refused(gm::build_fog_table(bad, table, &error), error, "pal6",
                "pal6 byte 64");
        bad[12][1] = 0;
        bad[200][2] = 200;
        refused(gm::build_fog_table(bad, table, &error), error, "pal6",
                "pal6 byte 200");
        refused(gm::build_fog_table(nullptr, table, &error), error, "pal6",
                "null palette");
    }

    if (failures == 0) {
        std::cout << "fog: " << checks << " checks, ok\n";
    }
    return failures == 0 ? 0 : 1;
}
